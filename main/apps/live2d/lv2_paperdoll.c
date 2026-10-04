/* 纸偶渲染器实现 —— 与 PC 仿真器 static/sim.js 逐行对应（v34）
 *
 * 渲染数学完全照搬 sim.js render()/demoParams()/applyPat()/applyTracking()：
 *   全局 z 序逐层；身组（bob 呼吸）；脖子 0.85 阻尼；头组全变换（sx=cos(ax*1.15) 压扁
 *   + 剪切透视 + ry=ay*0.5 + rz=az）；脸轴张嘴矩阵 B·S·Bᵀ；统一眨眼；前发眼区打洞；
 *   待机整体晃动（绕 (640,1260)）+ 呼吸起伏；摸头 1.4s 包络；视线追踪 5/s 缓动。
 */
#include "lv2_paperdoll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "driver/ppa.h"

static const char *TAG = "paperdoll";

/* PPA 硬件混合总开关：1=身体大图层走硬件（整数坐标，sway 只能近似为平移）
   0=全 CPU 真旋转（与仿真器一致；呼吸层本来就要求 CPU 浮点，PPA 收益有限）
   2026-08-25 重开：对话模式下 CPU 预算 = 渲染 + AFE + 音频 + LVGL 上屏，56ms 全 CPU 渲染
   会饿死 IDLE 触发 watchdog；PPA 接走 legwear/back hair/footwear 等大层省 ~15-20ms。
   缺 .ppa.raw 的角色自动回退 CPU（has_ppa=false），零风险。 */
#define PD_USE_PPA 1

/* ---- PPA 硬件混合客户端（BLEND 引擎，阻塞式；与 PPACompositor 同配置） ---- */
static ppa_client_handle_t s_ppa_client = NULL;
static SemaphoreHandle_t s_ppa_mtx = NULL;   /* 双核并发提交串行化（max_pending=1，并发会 ESP_FAIL） */

static void ppa_init(void) {
    if (s_ppa_client) return;
    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&cfg, &s_ppa_client) != ESP_OK) {
        s_ppa_client = NULL;
        ESP_LOGW(TAG, "PPA BLEND client register failed, CPU fallback");
    } else {
        s_ppa_mtx = xSemaphoreCreateMutex();
        ESP_LOGI(TAG, "PPA BLEND client ready");
    }
}

/* 帧缓冲上的 PPA 混合（bg=out=fb 原地叠加，fg=预缩放 BGRA 纹理；[cy0,cy1)=行带裁切） */
static void ppa_blend_layer(uint16_t *fb, int fw, int fh, int x, int y,
                            const pd_tex_t *fg, int cy0, int cy1) {
    if (!s_ppa_client || !fg->rgba) return;
    int bx = 0, by = 0, bw = fg->w, bh = fg->h;
    if (x < 0) { bx = -x; bw += x; x = 0; }
    if (y < 0) { by = -y; bh += y; y = 0; }
    if (y < cy0) { by += cy0 - y; bh -= cy0 - y; y = cy0; }
    if (y + bh > cy1) bh = cy1 - y;
    if (x + bw > fw) bw = fw - x;
    if (bw <= 0 || bh <= 0) return;

    ppa_in_pic_blk_config_t bg_cfg = {};
    bg_cfg.buffer = fb;
    bg_cfg.pic_w = fw; bg_cfg.pic_h = fh;
    bg_cfg.block_w = bw; bg_cfg.block_h = bh;
    bg_cfg.block_offset_x = x; bg_cfg.block_offset_y = y;
    bg_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    ppa_in_pic_blk_config_t fg_cfg = {};
    fg_cfg.buffer = fg->rgba;
    fg_cfg.pic_w = fg->w; fg_cfg.pic_h = fg->h;
    fg_cfg.block_w = bw; fg_cfg.block_h = bh;
    fg_cfg.block_offset_x = bx; fg_cfg.block_offset_y = by;
    fg_cfg.blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888;

    ppa_out_pic_blk_config_t out_cfg = {};
    out_cfg.buffer = fb;
    out_cfg.buffer_size = (uint32_t)fw * fh * 2;
    out_cfg.pic_w = fw; out_cfg.pic_h = fh;
    out_cfg.block_offset_x = x; out_cfg.block_offset_y = y;
    out_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    ppa_blend_oper_config_t cfg = {};
    cfg.in_bg = bg_cfg;
    cfg.in_fg = fg_cfg;
    cfg.out = out_cfg;
    cfg.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    cfg.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    cfg.bg_ck_en = false;
    cfg.fg_ck_en = false;
    cfg.mode = PPA_TRANS_MODE_BLOCKING;
    if (s_ppa_mtx) xSemaphoreTake(s_ppa_mtx, portMAX_DELAY);
    esp_err_t ret = ppa_do_blend(s_ppa_client, &cfg);
    if (s_ppa_mtx) xSemaphoreGive(s_ppa_mtx);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "ppa_do_blend fail");
}

#define PSRAM MALLOC_CAP_SPIRAM
#define DEV_W 480
#define DEV_H 800
#define ANGLE_MAX 10.0f
#define PI_F 3.14159265358979f

/* 15 参数下标（与 sim.js PARAM_KEYS 一致） */
enum {
    P_AX, P_AY, P_AZ, P_EYEL, P_EYER, P_MOUTH,
    P_SMILEL, P_SMILER, P_FORM, P_BALLX, P_BALLY,
    P_BROWYL, P_BROWYR, P_BROWAL, P_BROWAR
};

/* ---- 2x3 仿射：x' = a x + c y + e ; y' = b x + d y + f（canvas 同构） ---- */
typedef struct { float a, b, c, d, e, f; } mat_t;

static inline mat_t mat_id(void) { return (mat_t){1, 0, 0, 1, 0, 0}; }
static inline mat_t mat_T(float x, float y) { return (mat_t){1, 0, 0, 1, x, y}; }
static inline mat_t mat_R(float r) {
    float c = cosf(r), s = sinf(r);
    return (mat_t){c, s, -s, c, 0, 0};
}
static inline mat_t mat_S(float sx, float sy) { return (mat_t){sx, 0, 0, sy, 0, 0}; }
/* ctx.transform(sx, shx, 0, 1)：x'=sx*x ; y'=shx*x + y */
static inline mat_t mat_sh(float sx, float shx) { return (mat_t){sx, shx, 0, 1, 0, 0}; }

/* r = r * m（canvas 后乘语义） */
static void mat_mul(mat_t *r, const mat_t *m) {
    float a = r->a * m->a + r->c * m->b;
    float b = r->b * m->a + r->d * m->b;
    float c = r->a * m->c + r->c * m->d;
    float d = r->b * m->c + r->d * m->d;
    float e = r->a * m->e + r->c * m->f + r->e;
    float f = r->b * m->e + r->d * m->f + r->f;
    r->a = a; r->b = b; r->c = c; r->d = d; r->e = e; r->f = f;
}

static void mat_inv(const mat_t *m, mat_t *o) {
    float det = m->a * m->d - m->b * m->c;
    if (det == 0) { *o = mat_id(); return; }
    float inv = 1.0f / det;
    o->a = m->d * inv;  o->c = -m->c * inv;
    o->b = -m->b * inv; o->d = m->a * inv;
    o->e = (m->c * m->f - m->d * m->e) * inv;
    o->f = (m->b * m->e - m->a * m->f) * inv;
}

static void mat_pt(const mat_t *m, float x, float y, float *ox, float *oy) {
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

/* ---- RGB565 ---- */
static inline uint16_t pack565(uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static inline uint16_t blend565(uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa, uint16_t dst) {
    if (sa >= 255) return pack565(sr, sg, sb);
    if (sa == 0) return dst;
    uint32_t dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    dr = (dr << 3) | (dr >> 2); dg = (dg << 2) | (dg >> 4); db = (db << 3) | (db >> 2);
    uint32_t inv = 255 - sa;
    sr = (sr * sa + dr * inv + 127) / 255;
    sg = (sg * sa + dg * inv + 127) / 255;
    sb = (sb * sa + db * inv + 127) / 255;
    return pack565(sr, sg, sb);
}

/* ---- 文件读取（分块 + 让步，镜像 lv2_load） ---- */
#define PD_CHUNK (256 * 1024)
static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "open fail: %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    uint8_t *buf = heap_caps_malloc((size_t)len, PSRAM);
    if (!buf) { ESP_LOGE(TAG, "PSRAM alloc fail %ld: %s", len, path); fclose(f); return NULL; }
    size_t off = 0;
    while (off < (size_t)len) {
        size_t n = fread(buf + off, 1,
                         ((size_t)len - off) > PD_CHUNK ? PD_CHUNK : ((size_t)len - off), f);
        if (n == 0) break;
        off += n;
        vTaskDelay(1);
    }
    fclose(f);
    if (off != (size_t)len) { heap_caps_free(buf); return NULL; }
    *out_len = (size_t)len;
    return buf;
}

static bool load_raw_tex(const char *path, pd_tex_t *t) {
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "open fail: %s", path); return false; }
    uint8_t hdr[4];
    if (fread(hdr, 1, 4, f) != 4) { fclose(f); return false; }
    t->w = (uint16_t)(hdr[0] | (hdr[1] << 8));
    t->h = (uint16_t)(hdr[2] | (hdr[3] << 8));
    size_t sz = (size_t)t->w * t->h * 4;
    t->rgba = heap_caps_malloc(sz, PSRAM);
    if (!t->rgba) {
        /* 全尺寸分配失败 → 半分辨率降级(2x2 均值,内存 1/4),图层保留而非整层消失 */
        uint32_t hw = (uint32_t)t->w / 2, hh = (uint32_t)t->h / 2;
        size_t hsz = (size_t)hw * hh * 4;
        t->rgba = heap_caps_malloc(hsz, PSRAM);
        if (!t->rgba) {
            ESP_LOGE(TAG, "PSRAM alloc fail %u(half): %s", (unsigned)sz, path);
            fclose(f);
            return false;
        }
        size_t rowbytes = (size_t)t->w * 4;
        uint8_t *rows = heap_caps_malloc(rowbytes * 2, PSRAM);
        if (!rows) { heap_caps_free(t->rgba); t->rgba = NULL; fclose(f); return false; }
        bool ok = true;
        for (uint32_t yy = 0; yy < hh; yy++) {
            if (fread(rows, 1, rowbytes, f) != rowbytes ||
                fread(rows + rowbytes, 1, rowbytes, f) != rowbytes) { ok = false; break; }
            uint8_t *dst = (uint8_t *)t->rgba + (size_t)yy * hw * 4;
            for (uint32_t x = 0; x < hw; x++) {
                const uint8_t *p00 = rows + (size_t)(x * 2) * 4;
                const uint8_t *p10 = rows + (size_t)(x * 2 + 1) * 4;
                const uint8_t *p01 = rows + rowbytes + (size_t)(x * 2) * 4;
                const uint8_t *p11 = rows + rowbytes + (size_t)(x * 2 + 1) * 4;
                for (int c = 0; c < 4; c++)
                    dst[c] = (uint8_t)(((unsigned)p00[c] + p10[c] + p01[c] + p11[c]) >> 2);
                dst += 4;
            }
        }
        heap_caps_free(rows);
        fclose(f);
        if (!ok) { heap_caps_free(t->rgba); t->rgba = NULL; return false; }
        t->w = (uint16_t)hw;
        t->h = (uint16_t)hh;
        ESP_LOGW(TAG, "half-res fallback %ux%u: %s", hw, hh, path);
    } else {
        size_t off = 0;
        while (off < sz) {
            size_t n = fread((uint8_t *)t->rgba + off, 1, (sz - off) > PD_CHUNK ? PD_CHUNK : (sz - off), f);
            if (n == 0) break;
            off += n;
            vTaskDelay(1);
        }
        fclose(f);
        if (off != sz) { heap_caps_free(t->rgba); t->rgba = NULL; return false; }
    }
    /* 大纹理：逐行内容跨度表（对角线内容的大图层 AABB 里一半是空像素，逐行裁切可省 40%+） */
    t->has_cb = false;
    t->span = NULL;
    if ((size_t)t->w * t->h > 65536) {
        t->span = heap_caps_malloc((size_t)t->h * 2 * sizeof(uint16_t), PSRAM);
        uint32_t x0 = t->w, y0 = t->h, x1 = 0, y1 = 0;
        if (t->span) {
            for (uint32_t y = 0; y < t->h; y++) {
                const uint32_t *row = t->rgba + y * t->w;
                uint32_t rmin = t->w, rmax = 0;
                for (uint32_t x = 0; x < t->w; x++) {
                    if ((row[x] >> 24) > 8) {
                        if (x < rmin) rmin = x;
                        if (x > rmax) rmax = x;
                    }
                }
                if (rmin <= rmax) {
                    t->span[y * 2] = rmin;
                    t->span[y * 2 + 1] = rmax;
                    if (rmin < x0) x0 = rmin;
                    if (rmax > x1) x1 = rmax;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                } else {
                    t->span[y * 2] = 0xFFFF;
                    t->span[y * 2 + 1] = 0xFFFF;
                }
            }
        }
        if (x1 >= x0 && y1 >= y0) {
            t->cbx0 = x0; t->cby0 = y0; t->cbx1 = x1; t->cby1 = y1;
            t->has_cb = true;
        }
    }
    return true;
}

/* ---- 仿射贴图（双线性采样 + alpha 混合，M 把纹理坐标 (0..w,0..h) 映射到屏幕） ---- */
static void blit(uint16_t *fb, int fw, int fh, const pd_tex_t *t, const mat_t *M,
                 int cx0, int cy0, int cx1, int cy1) {
    if (!t->rgba) return;
    float x0t = 0, y0t = 0, x1t = t->w, y1t = t->h;
    if (t->has_cb) {   /* 内容 bbox 裁切（透明边距不画） */
        x0t = t->cbx0; y0t = t->cby0;
        x1t = t->cbx1 + 1; y1t = t->cby1 + 1;
    }
    float cx[4], cy[4];
    mat_pt(M, x0t, y0t, &cx[0], &cy[0]);
    mat_pt(M, x1t, y0t, &cx[1], &cy[1]);
    mat_pt(M, x1t, y1t, &cx[2], &cy[2]);
    mat_pt(M, x0t, y1t, &cx[3], &cy[3]);
    float x0 = cx[0], x1 = cx[0], y0 = cy[0], y1 = cy[0];
    for (int i = 1; i < 4; i++) {
        if (cx[i] < x0) x0 = cx[i];
        if (cx[i] > x1) x1 = cx[i];
        if (cy[i] < y0) y0 = cy[i];
        if (cy[i] > y1) y1 = cy[i];
    }
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0);
    int ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > fw) ix1 = fw;
    if (iy1 > fh) iy1 = fh;
    if (ix0 < cx0) ix0 = cx0;   /* 裁剪（行带分工 + 瞳孔眼睑遮盖） */
    if (ix1 > cx1) ix1 = cx1;
    if (iy0 < cy0) iy0 = cy0;
    if (iy1 > cy1) iy1 = cy1;
    if (ix0 >= ix1 || iy0 >= iy1) return;

    mat_t inv;
    mat_inv(M, &inv);
    /* 16.16 定点逆映射步进（内循环只有两次定点加法） */
    int32_t dux = (int32_t)(inv.a * 65536.0f), dvx = (int32_t)(inv.b * 65536.0f);
    float u00 = inv.a * ix0 + inv.c * iy0 + inv.e;
    float v00 = inv.b * ix0 + inv.d * iy0 + inv.f;
    const uint32_t wmax = (uint32_t)t->w * 65536u;
    const uint32_t hmax = (uint32_t)t->h * 65536u;
    const uint32_t *px = t->rgba;
    /* 小图层（五官等）双线性保细节，大图层（衣服/头发）最近邻提速 */
    const bool bilin = (size_t)t->w * t->h <= 4096;
    const uint16_t *span = t->span;

    for (int y = iy0; y < iy1; y++) {
        int32_t u = (int32_t)(u00 * 65536.0f), v = (int32_t)(v00 * 65536.0f);
        int xa = ix0, xb = ix1;
        if (span && dux != 0) {
            /* 逐行跨度裁切：本行采样的纹理行范围 → 内容起止并集 → 映射回屏幕 x 区间 */
            int32_t vend = v + dvx * (ix1 - ix0);
            int r0 = (v < vend ? v : vend) >> 16;
            int r1 = (v < vend ? vend : v) >> 16;
            if (r0 < 0) r0 = 0;
            if (r1 > (int)t->h - 1) r1 = (int)t->h - 1;
            uint32_t smin = 0xFFFFFFFF, smax = 0;
            for (int r = r0; r <= r1; r++) {
                uint32_t a = span[r * 2], b = span[r * 2 + 1];
                if (a != 0xFFFF) {
                    if (a < smin) smin = a;
                    if (b > smax) smax = b;
                }
            }
            if (smin > smax) { u00 += inv.c; v00 += inv.d; continue; }  /* 整行无内容 */
            float uf = (float)u / 65536.0f;
            float duf = (float)dux / 65536.0f;
            float xa_f = (float)ix0 + ((float)smin - 1.5f - uf) / duf;
            float xb_f = (float)ix0 + ((float)smax + 1.5f - uf) / duf;
            int xa2 = (int)ceilf(fminf(xa_f, xb_f)) - 1;
            int xb2 = (int)floorf(fmaxf(xa_f, xb_f)) + 2;
            if (xa2 < ix0) xa2 = ix0;
            if (xb2 > ix1) xb2 = ix1;
            if (xa2 >= xb2) { u00 += inv.c; v00 += inv.d; continue; }
            xa = xa2; xb = xb2;
        }
        u += dux * (xa - ix0);
        v += dvx * (xa - ix0);
        uint16_t *row = &fb[y * fw + xa];
        for (int x = xa; x < xb; x++) {
            if ((uint32_t)u < wmax && (uint32_t)v < hmax) {
                int ui = u >> 16, vi = v >> 16;
                if (bilin) {
                    uint32_t uf = (u >> 8) & 255, vf = (v >> 8) & 255;
                    if (ui >= (int)t->w - 1) uf = 0;
                    if (vi >= (int)t->h - 1) vf = 0;
                    const uint32_t *b = px + vi * t->w + ui;
                    uint32_t p00 = b[0], p10 = b[1], p01 = b[t->w], p11 = b[t->w + 1];
                    uint32_t iuf = 256 - uf, ivf = 256 - vf;
                    /* raw = 小端 RGBA（byte0=R, byte2=B） */
                    uint32_t r = (((p00 & 255) * iuf + (p10 & 255) * uf) * ivf +
                                  ((p01 & 255) * iuf + (p11 & 255) * uf) * vf) >> 16;
                    uint32_t g = ((((p00 >> 8) & 255) * iuf + ((p10 >> 8) & 255) * uf) * ivf +
                                  (((p01 >> 8) & 255) * iuf + ((p11 >> 8) & 255) * uf) * vf) >> 16;
                    uint32_t bb = ((((p00 >> 16) & 255) * iuf + ((p10 >> 16) & 255) * uf) * ivf +
                                   (((p01 >> 16) & 255) * iuf + ((p11 >> 16) & 255) * uf) * vf) >> 16;
                    uint32_t aa = ((((p00 >> 24)) * iuf + ((p10 >> 24)) * uf) * ivf +
                                   (((p01 >> 24)) * iuf + ((p11 >> 24)) * uf) * vf) >> 16;
                    if (aa >= 255) *row = pack565(r, g, bb);          /* 不透明直写（省一次 fb 读改写） */
                    else if (aa) *row = blend565(r, g, bb, aa, *row);
                } else {
                    uint32_t p = px[vi * t->w + ui];
                    uint32_t aa = p >> 24;
                    if (aa >= 255) *row = pack565(p & 255, (p >> 8) & 255, (p >> 16) & 255);
                    else if (aa) *row = blend565(p & 255, (p >> 8) & 255, (p >> 16) & 255, aa, *row);
                }
            }
            u += dux; v += dvx;
            row++;
        }
        u00 += inv.c; v00 += inv.d;
    }
}

/* 口腔黑椭圆（画在嘴层下方；L 线性部分把场景偏移映射到屏幕，含脸轴旋转） */
static void fill_ellipse(uint16_t *fb, int fw, int fh, float cx, float cy,
                         float rx, float ry, const mat_t *L, int cy0, int cy1) {
    mat_t inv;
    mat_inv(L, &inv);
    /* 保守外接盒：k = Frobenius 上界 */
    float k = sqrtf(L->a * L->a + L->b * L->b + L->c * L->c + L->d * L->d);
    float rad = k * (rx > ry ? rx : ry) + 1;
    int x0 = (int)floorf(cx - rad), x1 = (int)ceilf(cx + rad);
    int y0 = (int)floorf(cy - rad), y1 = (int)ceilf(cy + rad);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    if (y0 < cy0) y0 = cy0;   /* 行带裁切（双核分工） */
    if (y1 > cy1) y1 = cy1;
    uint16_t col = pack565(0x2a, 0x13, 0x18);
    for (int y = y0; y < y1; y++) {
        uint16_t *row = &fb[y * fw];
        for (int x = x0; x < x1; x++) {
            float dx = inv.a * (x - cx) + inv.c * (y - cy);
            float dy = inv.b * (x - cx) + inv.d * (y - cy);
            if ((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) <= 1.0f) row[x] = col;
        }
    }
}

/* ---- 取景/头部区域 ---- */
static void fit_to_screen(pd_model_t *m) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < m->n_layers; i++) {
        pd_layer_t *L = &m->layers[i];
        if (L->bbox[0] < x0) x0 = L->bbox[0];
        if (L->bbox[1] < y0) y0 = L->bbox[1];
        if (L->bbox[2] > x1) x1 = L->bbox[2];
        if (L->bbox[3] > y1) y1 = L->bbox[3];
    }
    float uw = x1 - x0, uh = y1 - y0;
    m->base_scale = fminf((DEV_W - 36) / uw, (DEV_H - 120) / uh);
    m->cX = (x0 + x1) / 2;
    m->cY = (y0 + y1) / 2;
    /* 全身默认：在完整适配基础上放大 1.3×（外饰允许轻微越界），pan 归零居中 */
    m->zoom = 1.3f;
    m->pan_y = 0;
    m->scale = 0.8f;  /* 全局 80%：像素量 64%（帧率收益） */
}

static bool head_part(const char *name) {
    static const char *names[] = {"face", "front hair", "headwear",
                                  "eyelash-l", "eyelash-r", "mouth", "nose"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return true;
    /* Q 版（Spine 转换）图层名前缀：F_Head/F_Front_Hair/F_L_Hair/F_R_Hair/F_Ear/F_Eye/
       F_Eyebrow/F_Mouth/F_Ponytail/F_Hood（scene.json 的 group 字段已正确标头组，
       但本函数供头部组判定/渲染使用——Q 版名不匹配会导致头部部件不走头组变换） */
    if (strncmp(name, "F_Head", 6) == 0 || strncmp(name, "F_Front_Hair", 12) == 0 ||
        strncmp(name, "F_L_Hair", 8) == 0 || strncmp(name, "F_R_Hair", 8) == 0 ||
        strncmp(name, "F_Ear", 5) == 0 || strncmp(name, "F_Eye", 5) == 0 ||
        strncmp(name, "F_Eyebrow", 9) == 0 || strncmp(name, "F_Mouth", 7) == 0 ||
        strncmp(name, "F_Ponytail", 10) == 0 || strncmp(name, "F_Hood", 6) == 0)
        return true;
    return false;
}

/* 摸头核心区判定：只含脸部核心图层（不含耳朵/马尾/兜帽等外围件） */
static bool pat_part(const char *name) {
    static const char *core[] = {"face", "front hair", "mouth", "nose"};
    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++)
        if (strcmp(name, core[i]) == 0) return true;
    if (strncmp(name, "F_Head", 6) == 0 || strncmp(name, "F_Front_Hair", 12) == 0 ||
        strncmp(name, "F_Mouth", 7) == 0 || strncmp(name, "F_Eye", 5) == 0 ||
        strncmp(name, "F_Eyebrow", 9) == 0)
        return true;
    return false;
}

static pd_layer_t *find_layer(pd_model_t *m, const char *name) {
    for (int i = 0; i < m->n_layers; i++)
        if (strcmp(m->layers[i].name, name) == 0) return &m->layers[i];
    return NULL;
}

/* ---- 加载 ---- */
static int layer_cmp(const void *a, const void *b) {
    float d = ((const pd_layer_t *)a)->z - ((const pd_layer_t *)b)->z;
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}

static uint8_t special_of(const char *s) {
    if (!s) return 0;
    if (!strcmp(s, "mouth")) return 1;
    if (!strcmp(s, "eye-l")) return 2;
    if (!strcmp(s, "eye-r")) return 3;
    if (!strcmp(s, "mouth-open")) return 4;
    if (!strcmp(s, "arm-l")) return 5;
    if (!strcmp(s, "arm-r")) return 6;
    return 0;
}

/* cJSON 默认 malloc 走内部 RAM(仅 ~150KB),大 anims.json(凯尔希基建 1.3MB)
   解析树要几十 MB → 解析失败静默 0 动画。全局 hook 到 PSRAM(16MB),
   所有 cJSON 解析(scene.json/anims.json/forms.json/manifest)统一受益。 */
static bool s_cjson_psram = false;
static void *cjson_psram_malloc(size_t sz) {
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}
static void cjson_psram_free(void *p) {
    heap_caps_free(p);
}
static void pd_ensure_cjson_psram(void) {
    if (s_cjson_psram) return;
    cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = cjson_psram_free };
    cJSON_InitHooks(&hooks);
    s_cjson_psram = true;
}

/* ---- anims.json 手写流式解析器：cJSON 节点树开销 ≈ 20× 文件体积
       (基建 367KB → ~6MB PSRAM，模型加载后余量不足 → 曾静默 0 动画)。
       流式解析零树开销，内存 = 文件文本 + 目标 keys。 ---- */
typedef struct {
    const char *p, *end;
} aj_rd_t;

static void aj_ws(aj_rd_t *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
        r->p++;
}
static bool aj_ch(aj_rd_t *r, char c) {
    aj_ws(r);
    if (r->p < r->end && *r->p == c) { r->p++; return true; }
    return false;
}
static bool aj_str(aj_rd_t *r, char *out, int cap) {
    aj_ws(r);
    if (r->p >= r->end || *r->p != '"') return false;
    r->p++;
    int n = 0;
    while (r->p < r->end && *r->p != '"' && n < cap - 1)
        out[n++] = *r->p++;
    if (r->p >= r->end || *r->p != '"') return false;
    r->p++;
    out[n] = 0;
    return true;
}
static bool aj_num(aj_rd_t *r, float *v) {
    aj_ws(r);
    if (r->p >= r->end) return false;
    char *e = NULL;
    *v = strtof(r->p, &e);
    if (e == r->p) return false;
    r->p = e;
    return true;
}
static bool aj_bool(aj_rd_t *r, bool *v) {
    aj_ws(r);
    if (r->end - r->p >= 4 && !strncmp(r->p, "true", 4)) { *v = true; r->p += 4; return true; }
    if (r->end - r->p >= 5 && !strncmp(r->p, "false", 5)) { *v = false; r->p += 5; return true; }
    return false;
}
/* 跳过当前 JSON 值(对象/数组用深度计数;标量读到分隔符) */
static bool aj_skip(aj_rd_t *r) {
    aj_ws(r);
    if (r->p >= r->end) return false;
    if (*r->p == '"') {
        r->p++;
        while (r->p < r->end && *r->p != '"') r->p++;
        if (r->p >= r->end) return false;
        r->p++;
        return true;
    }
    if (*r->p == '[' || *r->p == '{') {
        int d = 0;
        bool ins = false;
        while (r->p < r->end) {
            char c = *r->p;
            if (ins) { if (c == '"') ins = false; r->p++; continue; }
            if (c == '"') { ins = true; r->p++; continue; }
            if (c == '[' || c == '{') d++;
            else if (c == ']' || c == '}') { d--; r->p++; if (d == 0) return true; continue; }
            r->p++;
        }
        return false;
    }
    while (r->p < r->end && *r->p != ',' && *r->p != '}' && *r->p != ']') r->p++;
    return true;
}

/* 2026-09-17 减载模式:轻量扫描 anims.json 收集被动画引用的层名
   (流式,不建轨道;最多 maxn 个,去重)。返回收集数。 */
static int collect_used_layers(const char *path, char (*names)[64], int maxn) {
    FILE *af = fopen(path, "rb");
    if (!af) return 0;
    fseek(af, 0, SEEK_END);
    long asz = ftell(af);
    fseek(af, 0, SEEK_SET);
    char *abuf = heap_caps_malloc(asz + 1, MALLOC_CAP_SPIRAM);
    if (!abuf || fread(abuf, 1, asz, af) != (size_t)asz) {
        if (abuf) heap_caps_free(abuf);
        fclose(af);
        return 0;
    }
    abuf[asz] = 0;
    int n = 0;
    aj_rd_t r = {abuf, abuf + asz};
    if (aj_ch(&r, '{')) {
        for (;;) {   /* 顶层动画对象 */
            char nm[64];
            aj_ws(&r);
            if (r.p >= r.end || *r.p == '}') break;
            if (!aj_str(&r, nm, sizeof(nm)) || !aj_ch(&r, ':') || !aj_ch(&r, '{')) break;
            for (;;) {   /* 动画键 */
                char k[24];
                aj_ws(&r);
                if (r.p >= r.end || *r.p == '}') { r.p++; break; }
                if (!aj_str(&r, k, sizeof(k)) || !aj_ch(&r, ':')) break;
                if (!strcmp(k, "layers")) {
                    if (!aj_ch(&r, '{')) break;
                    for (;;) {   /* 层 */
                        char lnm[64];
                        aj_ws(&r);
                        if (r.p >= r.end || *r.p == '}') { r.p++; break; }
                        if (!aj_str(&r, lnm, sizeof(lnm)) || !aj_ch(&r, ':')) break;
                        int dup = 0;
                        for (int i = 0; i < n; i++)
                            if (!strcmp(names[i], lnm)) { dup = 1; break; }
                        if (!dup && n < maxn) snprintf(names[n++], 64, "%s", lnm);
                        aj_skip(&r);
                        aj_ws(&r);
                        if (r.p < r.end && *r.p == ',') { r.p++; continue; }
                        if (r.p < r.end && *r.p == '}') r.p++;
                        break;
                    }
                } else {
                    aj_skip(&r);
                }
                aj_ws(&r);
                if (r.p < r.end && *r.p == ',') { r.p++; continue; }
                if (r.p < r.end && *r.p == '}') r.p++;
                break;
            }
            aj_ws(&r);
            if (r.p < r.end && *r.p == ',') { r.p++; continue; }
            break;
        }
    }
    heap_caps_free(abuf);
    fclose(af);
    return n;
}

/* 读一个关键帧 [dx,dy,rot] / [dx,dy,rot,vis] / [dx,dy,rot,vis,sx,sy] /
   [dx,dy,rot,vis,sx,sy,a,b,c,d] */
static bool aj_key(aj_rd_t *r, float k[10], int *nval) {
    for (int i = 0; i < 10; i++) k[i] = 0.0f;
    k[3] = 1.0f;
    k[4] = 1.0f;
    k[5] = 1.0f;
    k[6] = 1.0f;
    k[9] = 1.0f;
    *nval = 0;
    if (!aj_ch(r, '[')) return false;
    for (int i = 0; i < 10; i++) {
        if (!aj_num(r, &k[i])) return false;
        *nval = i + 1;
        aj_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        break;
    }
    return aj_ch(r, ']');
}

pd_model_t *pd_load(const char *dir, int mode) {   /* 2026-09-17:0=旧全量 1=mesh_only 2=旧路径减载 */
    char path[256];
    snprintf(path, sizeof(path), "%s/scene.json", dir);
    ESP_LOGI(TAG, "pd_load start: PSRAM free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(PSRAM),
             (unsigned)heap_caps_get_largest_free_block(PSRAM));
    pd_ensure_cjson_psram();   // 必须在任何 cJSON_Parse 之前
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) return NULL;
    cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
    if (!root) { heap_caps_free(buf); return NULL; }

    pd_model_t *m = calloc(1, sizeof(pd_model_t));
    if (!m) { cJSON_Delete(root); heap_caps_free(buf); return NULL; }
    m->demo = true;

    const cJSON *name = cJSON_GetObjectItem(root, "name");
    if (name && cJSON_IsString(name))
        snprintf(m->name, sizeof(m->name), "%s", name->valuestring);
    else
        snprintf(m->name, sizeof(m->name), "%s", strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir);

#define GETF(o, d) ((o) ? (float)(o)->valuedouble : (d))   /* 0 是合法值，不能当缺失判据 */
    const cJSON *hp = cJSON_GetObjectItem(root, "head_pivot");
    const cJSON *mp = cJSON_GetObjectItem(root, "mouth_pivot");
    if (cJSON_IsArray(hp) && cJSON_GetArraySize(hp) == 2) {
        m->head_pivot[0] = (float)cJSON_GetArrayItem(hp, 0)->valuedouble;
        m->head_pivot[1] = (float)cJSON_GetArrayItem(hp, 1)->valuedouble;
    } else { m->head_pivot[0] = 640; m->head_pivot[1] = 320; }
    if (cJSON_IsArray(mp) && cJSON_GetArraySize(mp) == 2) {
        m->mouth_pivot[0] = (float)cJSON_GetArrayItem(mp, 0)->valuedouble;
        m->mouth_pivot[1] = (float)cJSON_GetArrayItem(mp, 1)->valuedouble;
    } else { m->mouth_pivot[0] = m->head_pivot[0]; m->mouth_pivot[1] = m->head_pivot[1]; }
    m->face_tilt = GETF(cJSON_GetObjectItem(root, "face_tilt"), 0);
    m->eye_tilt = GETF(cJSON_GetObjectItem(root, "eye_tilt"), m->face_tilt);   // 缺省回退脸轴
    m->mouth_tilt = GETF(cJSON_GetObjectItem(root, "mouth_tilt"), m->face_tilt);   // 缺省回退脸轴
    m->landscape = cJSON_IsTrue(cJSON_GetObjectItem(root, "landscape")) ? 1 : 0;
    m->no_cavity = cJSON_IsTrue(cJSON_GetObjectItem(root, "no_cavity")) ? 1 : 0;
    m->mouth_compress = cJSON_IsTrue(cJSON_GetObjectItem(root, "mouth_compress")) ? 1 : 0;
    m->mouth_gain = GETF(cJSON_GetObjectItem(root, "mouth_gain"), 1.10f);
    m->mouth_floor = GETF(cJSON_GetObjectItem(root, "mouth_floor"), 0.45f);
    m->mouth_width_open = GETF(cJSON_GetObjectItem(root, "mouth_width_open"), 0.0f);
    m->mouth_open_gain = GETF(cJSON_GetObjectItem(root, "mouth_open_gain"), 0.55f);
    m->hair_factor = GETF(cJSON_GetObjectItem(root, "hair_factor"), 0.7f);
#undef GETF

    /* 全局背景（u16w+u16h+RGB565，cover 铺满屏幕）：统一一套背景，只读
       /sdcard/Arknights/main/background/bg.raw（角色目录不支持专属背景）。
       必须在图层纹理之前分配——PSRAM 大块优先：纹理逐层加载后碎片化
       （实测能天使 18 层加载完 largest 仅 64KB），768KB 连续分配必失败 */
    {
        const char *bgpath = "/sdcard/Arknights/main/background/bg.raw";
        FILE *f = fopen(bgpath, "rb");
        if (f) {
            uint16_t w = 0, h = 0;
            if (fread(&w, 2, 1, f) == 1 && fread(&h, 2, 1, f) == 1 && w > 0 && h > 0) {
                uint32_t sz = (uint32_t)w * h * 2;
                m->bg = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
                if (m->bg) {
                    m->bg_w = w; m->bg_h = h;
                    size_t rd = fread(m->bg, 1, sz, f);
                    if (rd != sz) { heap_caps_free(m->bg); m->bg = NULL; m->bg_w = m->bg_h = 0; }
                    else ESP_LOGI(TAG, "bg.raw loaded: %ux%u (%s)", w, h, bgpath);
                } else {
                    ESP_LOGW(TAG, "bg.raw %ux%u alloc fail (%u bytes)", w, h, (unsigned)sz);
                }
            }
            fclose(f);
        } else {
            ESP_LOGW(TAG, "bg.raw open fail: %s", bgpath);
        }
    }

    const cJSON *layers = cJSON_GetObjectItem(root, "layers");
    if (!cJSON_IsArray(layers)) { cJSON_Delete(root); heap_caps_free(buf); pd_free(m); return NULL; }
    /* 2026-09-17:mesh 优先——ppdq 加载在层纹理之前(纹理 15MB+ 会挤爆 PSRAM
       导致 mesh atlas 2.6MB 分配失败);mesh_only 且成功则跳过层纹理/anims。 */
    if (mode == 1) {
        extern void *pdq_load(const char *dir);
        m->pdq = pdq_load(dir);
        if (m->pdq) ESP_LOGI(TAG, "PPDQ 已启用(mesh_only, 跳过层纹理)");
    }
    /* 2026-09-17 减载模式:先轻量扫描 anims.json 收集动画引用的层名,
       纹理只加载「可见 ∪ 被引用」的层(248 层里 152 层静态隐藏,仅 96 可见) */
    char used_names[256][64];
    int used_n = 0;
    if (mode == 2) {
        char apath[192];
        snprintf(apath, sizeof(apath), "%s/anims.json", dir);
        used_n = collect_used_layers(apath, used_names, 256);
        ESP_LOGI(TAG, "减载: anims 引用 %d 层", used_n);
    }
    /* 2026-09-26:纯 mesh 角色(如黑键)scene.json 的 layers 为空——几何全在
       mesh.ppdq;mesh_only 且 mesh 加载成功时允许空 layers(calloc(0) 返回
       NULL 曾使 pd_load 直接失败)。 */
    if (cJSON_GetArraySize(layers) == 0 && mode == 1 && m->pdq) {
        m->n_layers = 0;
        m->layers = NULL;
    } else {
        m->n_layers = cJSON_GetArraySize(layers);
        m->layers = heap_caps_calloc(m->n_layers, sizeof(pd_layer_t), PSRAM);
        if (!m->layers) { cJSON_Delete(root); heap_caps_free(buf); pd_free(m); return NULL; }
    }

    for (int i = 0; i < m->n_layers; i++) {
        const cJSON *it = cJSON_GetArrayItem(layers, i);
        pd_layer_t *L = &m->layers[i];
        const cJSON *j;
        if ((j = cJSON_GetObjectItem(it, "name")) && cJSON_IsString(j))
            snprintf(L->name, sizeof(L->name), "%s", j->valuestring);
        L->x = (int16_t)cJSON_GetObjectItem(it, "x")->valuedouble;
        L->y = (int16_t)cJSON_GetObjectItem(it, "y")->valuedouble;
        L->w = (uint16_t)cJSON_GetObjectItem(it, "w")->valuedouble;
        L->h = (uint16_t)cJSON_GetObjectItem(it, "h")->valuedouble;
        L->cx = (float)cJSON_GetObjectItem(it, "cx")->valuedouble;
        L->cy = (float)cJSON_GetObjectItem(it, "cy")->valuedouble;
        const cJSON *bb = cJSON_GetObjectItem(it, "bbox");
        if (cJSON_IsArray(bb) && cJSON_GetArraySize(bb) == 4)
            for (int k = 0; k < 4; k++) L->bbox[k] = (float)cJSON_GetArrayItem(bb, k)->valuedouble;
        L->z = (float)cJSON_GetObjectItem(it, "z")->valuedouble;
        /* 骨骼点(动作旋转中心;旧数据无此字段=bbox 中心) */
        {
            const cJSON *jb = cJSON_GetObjectItem(it, "bone_px");
            L->bone_px = (jb && cJSON_IsNumber(jb)) ? (float)jb->valuedouble : (float)L->w / 2;
            jb = cJSON_GetObjectItem(it, "bone_py");
            L->bone_py = (jb && cJSON_IsNumber(jb)) ? (float)jb->valuedouble : (float)L->h / 2;
        }
        if ((j = cJSON_GetObjectItem(it, "group")) && cJSON_IsString(j))
            L->group = strcmp(j->valuestring, "head") == 0 ? 1 : 0;
        L->special = special_of(cJSON_GetObjectItem(it, "special") &&
                                cJSON_IsString(cJSON_GetObjectItem(it, "special"))
                                    ? cJSON_GetObjectItem(it, "special")->valuestring : NULL);
        {
            const cJSON *jv = cJSON_GetObjectItem(it, "visible");
            L->visible = jv ? cJSON_IsTrue(jv) : true;   // 隐藏槽默认不渲染
        }
        snprintf(path, sizeof(path), "%s/%s.raw", dir, L->name);
        if (mode == 1 && m->pdq) {
            /* 2026-09-17:mesh 渲染不需要层纹理,只保留几何元数据 */
            L->tex.w = L->tex.h = 0;
            L->tex.rgba = NULL;
            continue;
        }
        if (mode == 2 && !L->visible) {
            /* 2026-09-17 减载:静态隐藏且不被动画引用的层不加载纹理 */
            int used = 0;
            for (int u = 0; u < used_n; u++)
                if (!strcmp(used_names[u], L->name)) { used = 1; break; }
            if (!used) {
                L->tex.w = L->tex.h = 0;
                L->tex.rgba = NULL;
                continue;
            }
        }
        if (!load_raw_tex(path, &L->tex)) {
            ESP_LOGW(TAG, "层纹理缺失: %s", path);
            L->tex.w = L->tex.h = 0;
            L->tex.rgba = NULL;
        } else if (strncmp(L->name, "eyelash", 7) == 0) {
            /* 诊断：睫毛层 alpha 统计（验证 SD 数据是否为最新） */
            uint32_t mx = 0, sum = 0, cnt = 0;
            for (uint32_t i = 0; i < (uint32_t)L->tex.w * L->tex.h; i++) {
                uint32_t al = L->tex.rgba[i] >> 24;
                if (al > mx) mx = al;
                if (al > 0) { sum += al; cnt++; }
            }
            ESP_LOGI(TAG, "lash %s: %dx%d maxA=%u meanA=%u", L->name, L->tex.w, L->tex.h,
                     (unsigned)mx, cnt ? (unsigned)(sum / cnt) : 0);
        }
    }
    if (m->n_layers > 1) qsort(m->layers, m->n_layers, sizeof(pd_layer_t), layer_cmp);   /* 2026-09-26:纯 mesh 角色 n_layers=0(NULL 指针 qsort 为 UB) */

    /* PPA 预缩放纹理与烘焙位置（PD_USE_PPA=0 时跳过，省 PSRAM 与加载时间） */
#if PD_USE_PPA
    ppa_init();
    m->ppa_zoom = -1;
    m->ppa_scale = -1;
    const cJSON *ppa = cJSON_GetObjectItem(root, "ppa");
    if (cJSON_IsObject(ppa)) {
        m->ppa_zoom = (float)cJSON_GetObjectItem(ppa, "zoom")->valuedouble;
        m->ppa_scale = (float)cJSON_GetObjectItem(ppa, "scale")->valuedouble;
        const cJSON *pl = cJSON_GetObjectItem(ppa, "layers");
        if (cJSON_IsObject(pl)) {
            cJSON *it = NULL;
            cJSON_ArrayForEach(it, pl) {
                pd_layer_t *L = find_layer(m, it->string);
                if (!L) continue;
                snprintf(path, sizeof(path), "%s/%s.ppa.raw", dir, L->name);
                if (load_raw_tex(path, &L->ppa_tex)) {
                    L->ppa_x = (int16_t)cJSON_GetObjectItem(it, "x")->valuedouble;
                    L->ppa_y = (int16_t)cJSON_GetObjectItem(it, "y")->valuedouble;
                    L->has_ppa = true;
                }
            }
        }
    }
#else
    m->ppa_zoom = -1;
    m->ppa_scale = -1;
#endif

    /* 眼睛掩膜（裁剪后导出：scene.json 带 eye_mask {x,y,w,h}） */
    const cJSON *em = cJSON_GetObjectItem(root, "eye_mask");
    if (cJSON_IsObject(em)) {
        snprintf(path, sizeof(path), "%s/eye_mask.raw", dir);
        if (load_raw_tex(path, &m->eye_mask)) {
            m->em_x = (int16_t)cJSON_GetObjectItem(em, "x")->valuedouble;
            m->em_y = (int16_t)cJSON_GetObjectItem(em, "y")->valuedouble;
            m->has_mask = true;
            ESP_LOGI(TAG, "eye_mask ok: %dx%d @(%d,%d)", m->eye_mask.w, m->eye_mask.h,
                     m->em_x, m->em_y);
        } else {
            ESP_LOGW(TAG, "eye_mask.raw 加载失败: %s（hair=0 将退回矩形兜底）", path);
        }
    } else {
        ESP_LOGW(TAG, "scene.json 无 eye_mask 字段（旧导出？）→ hair=0 将退回矩形兜底");
    }
    cJSON_Delete(root);
    heap_caps_free(buf);

    /* 前发打洞（hair_factor）：destination-out 等效。优先眼部掩膜形状（精确，不多清刘海），
       hair=0 且无掩膜时才退回矩形通杀兜底 */
    pd_layer_t *fh = find_layer(m, "front hair");
    if (fh && fh->tex.rgba && m->has_mask) {
        /* 眼部掩膜形状打洞（精确，不多清刘海）。eye_mask 是 1.0 画布坐标，
           hair raw 是缩放纹理（draw 端用 mat_S(L.w/tex.w) 拉伸），索引前必须乘缩放 */
        size_t sz = (size_t)fh->tex.w * fh->tex.h;
        m->hair_punched.rgba = heap_caps_malloc(sz * 4, PSRAM);
        if (m->hair_punched.rgba) {
            m->hair_punched.w = fh->tex.w;
            m->hair_punched.h = fh->tex.h;
            memcpy(m->hair_punched.rgba, fh->tex.rgba, sz * 4);
            const uint32_t *mpx = m->eye_mask.rgba;
            float sx = (float)fh->tex.w / fh->w, sy = (float)fh->tex.h / fh->h;
            uint32_t punched = 0;
            for (int my = 0; my < (int)m->eye_mask.h; my++) {
                for (int mx = 0; mx < (int)m->eye_mask.w; mx++) {
                    uint32_t ma = mpx[my * m->eye_mask.w + mx] >> 24;
                    if (!ma) continue;
                    int hx = (int)(((m->em_x + mx) - fh->x) * sx + 0.5f);
                    int hy = (int)(((m->em_y + my) - fh->y) * sy + 0.5f);
                    if (hx < 0 || hx >= (int)fh->tex.w || hy < 0 || hy >= (int)fh->tex.h) continue;
                    uint32_t p = m->hair_punched.rgba[hy * fh->tex.w + hx];
                    float f;
                    if (m->hair_factor <= 0.001f) f = 0;   /* hair=0：掩膜区全清（不按强度折算） */
                    else f = 1.0f - (ma / 255.0f) * (1.0f - m->hair_factor);
                    uint32_t a = (uint32_t)((p >> 24) * f + 0.5f);
                    m->hair_punched.rgba[hy * fh->tex.w + hx] = (p & 0x00FFFFFF) | (a << 24);
                    punched++;
                }
            }
            m->hair_done = true;
            ESP_LOGI(TAG, "hair punch: mask %dx%d@(%d,%d) hair %dx%d@(%d,%d) factor=%.2f 命中 %u px",
                     m->eye_mask.w, m->eye_mask.h, m->em_x, m->em_y,
                     fh->tex.w, fh->tex.h, fh->x, fh->y, m->hair_factor, (unsigned)punched);
        }
    } else if (fh && fh->tex.rgba && m->hair_factor <= 0.001f) {
        /* hair=0 且无掩膜：眼部矩形通杀兜底（眼相关层 bbox 并集 + 6px 余量） */
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        for (int i = 0; i < m->n_layers; i++) {
            const char *n = m->layers[i].name;
            if (strncmp(n, "eyewhite", 8) == 0 || strncmp(n, "irides", 6) == 0 ||
                strncmp(n, "eyelash", 7) == 0 || strncmp(n, "eyebrow", 7) == 0 ||
                strcmp(n, "eye-white") == 0) {
                if (m->layers[i].bbox[0] < x0) x0 = (int)m->layers[i].bbox[0];
                if (m->layers[i].bbox[1] < y0) y0 = (int)m->layers[i].bbox[1];
                if (m->layers[i].bbox[2] > x1) x1 = (int)m->layers[i].bbox[2];
                if (m->layers[i].bbox[3] > y1) y1 = (int)m->layers[i].bbox[3];
            }
        }
        x0 -= 6; y0 -= 6; x1 += 6; y1 += 6;
        size_t sz = (size_t)fh->tex.w * fh->tex.h;
        /* 同掩膜分支：场景坐标 → 纹理坐标必须乘导出缩放，否则矩形错位 1/0.6 倍 */
        float sx = (float)fh->tex.w / fh->w, sy = (float)fh->tex.h / fh->h;
        m->hair_punched.rgba = heap_caps_malloc(sz * 4, PSRAM);
        if (m->hair_punched.rgba) {
            memcpy(m->hair_punched.rgba, fh->tex.rgba, sz * 4);
            int hx0 = (int)((x0 - fh->x) * sx + 0.5f), hy0 = (int)((y0 - fh->y) * sy + 0.5f);
            int hx1 = (int)((x1 - fh->x) * sx + 0.5f), hy1 = (int)((y1 - fh->y) * sy + 0.5f);
            if (hx0 < 0) hx0 = 0;
            if (hy0 < 0) hy0 = 0;
            if (hx1 > (int)fh->tex.w) hx1 = fh->tex.w;
            if (hy1 > (int)fh->tex.h) hy1 = fh->tex.h;
            for (int hy = hy0; hy < hy1; hy++)
                for (int hx = hx0; hx < hx1; hx++)
                    m->hair_punched.rgba[hy * fh->tex.w + hx] &= 0x00FFFFFF;
            m->hair_punched.w = fh->tex.w;
            m->hair_punched.h = fh->tex.h;
            m->hair_done = true;
            ESP_LOGI(TAG, "hair=0 无掩膜兜底: 眼区矩形通杀 [%d,%d]-[%d,%d] (hair局部 %d,%d-%d,%d)",
                     x0, y0, x1, y1, hx0, hy0, hx1, hy1);
        }
    } else {
        ESP_LOGW(TAG, "hair punch SKIPPED: fh=%d mask=%d", fh && fh->tex.rgba ? 1 : 0,
                 m->has_mask ? 1 : 0);
    }

    fit_to_screen(m);
    m->hfb = heap_caps_malloc(240 * 400 * 2, PSRAM);   /* 半分辨率渲染缓冲 */

    /* ---- Spine 原生动画（anims.json）：{ "<名>": {"duration":s,"loop":bool,
             "layers":{"<槽名>":[[dx,dy,drot,vis],...]}} } 可选，无则动作面板为空。
             手写流式解析：cJSON 节点树内存 ≈ 20× 文件体积（基建 367KB→~6MB，
             模型加载后余量不足 → 曾静默 0 动画）——流式零树开销。 ---- */
    m->anims = NULL; m->n_anims = 0; m->anim_idx = -1;
    if (!(mode == 1 && m->pdq)) {   /* 2026-09-17:mesh 模式动画在 mesh.ppdq 里,免载大体积 anims.json */
    {
        char apath[192];   // 中文形态路径长(曾 96 截断 → fopen 失败 → 形态无动画按钮)
        snprintf(apath, sizeof(apath), "%s/anims.json", dir);
        FILE *af = fopen(apath, "rb");
        if (af) {
            fseek(af, 0, SEEK_END);
            long asz = ftell(af);
            fseek(af, 0, SEEK_SET);
            char *abuf = heap_caps_malloc(asz + 1, MALLOC_CAP_SPIRAM);
            if (abuf && fread(abuf, 1, asz, af) == (size_t)asz) {
                abuf[asz] = 0;
                /* 第一遍：数顶层动画数 */
                int cnt = 0;
                {
                    aj_rd_t r = {abuf, abuf + asz};
                    if (aj_ch(&r, '{')) {
                        for (;;) {
                            char nm[64];
                            aj_ws(&r);
                            if (r.p >= r.end || *r.p == '}') break;
                            if (!aj_str(&r, nm, sizeof(nm)) || !aj_ch(&r, ':') || !aj_skip(&r)) break;
                            cnt++;
                            aj_ws(&r);
                            if (r.p < r.end && *r.p == ',') { r.p++; continue; }
                            break;
                        }
                    }
                }
                if (cnt > 0)
                    m->anims = heap_caps_calloc(cnt, sizeof(pd_anim_t), MALLOC_CAP_SPIRAM);
                /* 第二遍：完整解析 */
                if (m->anims) {
                    aj_rd_t r = {abuf, abuf + asz};
                    float tkeys[80 * 11];   // 单层关键帧临时缓冲(每层帧数 ≤ 80)
                    int ai = 0;
                    bool ok = aj_ch(&r, '{');
                    while (ok && ai < cnt) {
                        char nm[64];
                        aj_ws(&r);
                        if (r.p >= r.end || *r.p == '}') break;
                        if (!aj_str(&r, nm, sizeof(nm)) || !aj_ch(&r, ':') || !aj_ch(&r, '{')) { ok = false; break; }
                        pd_anim_t *A = &m->anims[ai];
                        snprintf(A->name, sizeof(A->name), "%.31s", nm);
                        A->duration = 1.0f; A->loop = false; A->n_tracks = 0;
                        for (;;) {   // 动画对象键循环
                            char k[24];
                            aj_ws(&r);
                            if (r.p >= r.end || *r.p == '}') { r.p++; break; }
                            if (!aj_str(&r, k, sizeof(k)) || !aj_ch(&r, ':')) { ok = false; break; }
                            if (!strcmp(k, "duration")) {
                                if (!aj_num(&r, &A->duration)) { ok = false; break; }
                            } else if (!strcmp(k, "loop")) {
                                if (!aj_bool(&r, &A->loop)) { ok = false; break; }
                            } else if (!strcmp(k, "layers")) {
                                if (!aj_ch(&r, '{')) { ok = false; break; }
                                for (;;) {   // 层循环
                                    char lnm[64];
                                    aj_ws(&r);
                                    if (r.p >= r.end || *r.p == '}') { r.p++; break; }
                                    if (!aj_str(&r, lnm, sizeof(lnm)) || !aj_ch(&r, ':')) { ok = false; break; }
                                    pd_layer_t *L = find_layer(m, lnm);
                                    int nk = 0;
                                    if (!aj_ch(&r, '[')) { ok = false; break; }
                                    for (;;) {   // 帧循环
                                        float kk[10];
                                        int kn = 0;
                                        aj_ws(&r);
                                        if (r.p >= r.end || *r.p == ']') { r.p++; break; }
                                        if (!aj_key(&r, kk, &kn)) { ok = false; break; }
                                        if (nk < 80) {
                                            tkeys[nk*11] = kk[0]; tkeys[nk*11+1] = kk[1];
                                            tkeys[nk*11+2] = kk[2]; tkeys[nk*11+3] = kk[3];
                                            tkeys[nk*11+4] = kk[4]; tkeys[nk*11+5] = kk[5];
                                            tkeys[nk*11+6] = kk[6]; tkeys[nk*11+7] = kk[7];
                                            tkeys[nk*11+8] = kk[8]; tkeys[nk*11+9] = kk[9];
                                            tkeys[nk*11+10] = kn >= 10 ? 1.0f : 0.0f;
                                        }
                                        nk++;
                                        aj_ws(&r);
                                        if (r.p < r.end && *r.p == ',') { r.p++; continue; }
                                        if (r.p < r.end && *r.p == ']') r.p++;   // 消耗帧数组尾(曾漏:break 后停 ']' 上,链条错乱只解析 1 层)
                                        break;
                                    }
                                    if (!ok) break;
                                    if (L && nk >= 2 && A->n_tracks < 256) {
                                        pd_anim_key_t *ks =
                                            heap_caps_malloc(nk * sizeof(pd_anim_key_t), MALLOC_CAP_SPIRAM);
                                        if (ks) {
                                            for (int q = 0; q < nk; q++) {
                                                ks[q].dx = tkeys[q*11]; ks[q].dy = tkeys[q*11+1];
                                                ks[q].rot = tkeys[q*11+2]; ks[q].vis = tkeys[q*11+3];
                                                ks[q].sx = tkeys[q*11+4]; ks[q].sy = tkeys[q*11+5];
                                                ks[q].a = tkeys[q*11+6]; ks[q].b = tkeys[q*11+7];
                                                ks[q].c = tkeys[q*11+8]; ks[q].d = tkeys[q*11+9];
                                                ks[q].affine = tkeys[q*11+10] > 0.5f;
                                            }
                                            A->tracks[A->n_tracks].layer_idx = (int16_t)(L - m->layers);
                                            A->tracks[A->n_tracks].n_keys = (int16_t)nk;
                                            A->tracks[A->n_tracks].keys = ks;
                                            A->n_tracks++;
                                        }
                                    }
                                    aj_ws(&r);
                                    if (r.p < r.end && *r.p == ',') { r.p++; continue; }
                                    if (r.p < r.end && *r.p == '}') r.p++;   // 消耗层对象尾(同上)
                                    break;
                                }
                                if (!ok) break;
                            } else {
                                if (!aj_skip(&r)) { ok = false; break; }
                            }
                            aj_ws(&r);
                            if (r.p < r.end && *r.p == ',') { r.p++; continue; }
                            if (r.p < r.end && *r.p == '}') r.p++;   // 消耗动画对象尾(同上)
                            break;
                        }
                        if (!ok) break;
                        m->n_anims = ++ai;
                        aj_ws(&r);
                        if (r.p < r.end && *r.p == ',') { r.p++; continue; }
                        break;
                    }
                    if (!ok)
                        ESP_LOGW(TAG, "anims.json 流式解析中断 @%d(%ld 字节)",
                                 (int)(r.p - abuf), asz);
                }
                heap_caps_free(abuf);
            }
            fclose(af);
            ESP_LOGI(TAG, "anims.json: %d 动画", m->n_anims);
        }
    }
    }   /* 2026-09-17:mesh_only && pdq 时跳过 anims.json 块 */

    /* 摸头判定区域（场景坐标）：只含脸部核心图层（face/前发/嘴/鼻/眼/眉），
       不含耳朵/马尾/兜帽等外围件——摸头区应小于视线追踪区（用户反馈） */
    {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < m->n_layers; i++) {
            pd_layer_t *L = &m->layers[i];
            if (!pat_part(L->name)) continue;
            if (L->bbox[0] < x0) x0 = L->bbox[0];
            if (L->bbox[1] < y0) y0 = L->bbox[1];
            if (L->bbox[2] > x1) x1 = L->bbox[2];
            if (L->bbox[3] > y1) y1 = L->bbox[3];
        }
        m->head_rect[0] = x0; m->head_rect[1] = y0;
        m->head_rect[2] = x1; m->head_rect[3] = y1;
        pd_layer_t *f = find_layer(m, "face");
        if (f) { m->head_center[0] = f->cx; m->head_center[1] = f->cy; }
        else { m->head_center[0] = (x0 + x1) / 2; m->head_center[1] = (y0 + y1) / 2; }
    }
    ESP_LOGI(TAG, "loaded %s: %d layers, hair=%.2f, tilt=%.1f", m->name, m->n_layers,
             m->hair_factor, m->face_tilt);
    ESP_LOGI(TAG, "pd_load end: PSRAM free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(PSRAM),
             (unsigned)heap_caps_get_largest_free_block(PSRAM));
    /* PPDQ(2026-09-14):仅旧全量模式在此加载;mesh_only 已前移,减载模式不加载 */
    if (mode == 0) {
        extern void *pdq_load(const char *dir);
        m->pdq = pdq_load(dir);
        if (m->pdq) ESP_LOGI(TAG, "PPDQ 已启用");
    }
    return m;
}

void pd_free(pd_model_t *m) {
    if (!m) return;
    for (int i = 0; i < m->n_layers; i++) {
        if (m->layers[i].tex.rgba) heap_caps_free(m->layers[i].tex.rgba);
        if (m->layers[i].tex.span) heap_caps_free(m->layers[i].tex.span);
        if (m->layers[i].ppa_tex.rgba) heap_caps_free(m->layers[i].ppa_tex.rgba);
    }
    if (m->eye_mask.rgba) heap_caps_free(m->eye_mask.rgba);
    if (m->hair_punched.rgba) heap_caps_free(m->hair_punched.rgba);
    if (m->bg) heap_caps_free(m->bg);
    if (m->hfb) heap_caps_free(m->hfb);
    if (m->pdq) { extern void pdq_free(void *pdm); pdq_free(m->pdq); m->pdq = NULL; }
    for (int i = 0; i < m->n_anims; i++)
        for (int k = 0; k < m->anims[i].n_tracks; k++)
            if (m->anims[i].tracks[k].keys) heap_caps_free(m->anims[i].tracks[k].keys);
    if (m->anims) heap_caps_free(m->anims);
    if (m->layers) heap_caps_free(m->layers);
    free(m);
}

/* ---- 交互 ---- */
static float sX(const pd_model_t *m, float x, int fw) {
    return (x - m->cX) * m->base_scale * m->zoom * m->scale + fw / 2.0f;
}
static float sY(const pd_model_t *m, float y, int fh) {
    return (y - m->cY) * m->base_scale * m->zoom * m->scale + fh / 2.0f + m->pan_y * m->scale;
}

void pd_touch(pd_model_t *m, int sx, int sy, bool down) {
    if (!m) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (down) {
        m->demo = false;
        float *P = m->P;
        if (!m->press_set) {
            m->press_set = true;
            m->press_x = sx; m->press_y = sy;
            m->last_x = sx; m->last_y = sy;
        }
        /* 拖动判定：位移超阈值（14px）才转拖动（短触/按住不视为拖动） */
        if (!m->dragging) {
            int ddx = sx - m->press_x, ddy = sy - m->press_y;
            if (ddx * ddx + ddy * ddy > 14 * 14) m->dragging = true;
        }
        if (m->dragging) {
            /* 拖动：转头；开始滑动即取消摸头（摸头=按住不动专属） */
            if (m->pat_active) m->pat_active = false;
            float dx = (float)(sx - m->last_x), dy = (float)(sy - m->last_y);
            P[P_AX] += dx * 0.25f;
            P[P_AY] += dy * 0.25f;
            if (P[P_AX] > ANGLE_MAX) P[P_AX] = ANGLE_MAX;
            if (P[P_AX] < -ANGLE_MAX) P[P_AX] = -ANGLE_MAX;
            if (P[P_AY] > ANGLE_MAX) P[P_AY] = ANGLE_MAX;
            if (P[P_AY] < -ANGLE_MAX) P[P_AY] = -ANGLE_MAX;
        } else {
            /* 未拖动（按住不动/微移）：摸头判定（head_rect = 脸部核心区联合，
               已小于视线追踪区域（全屏）——区分"按住摸头"与"滑动转头/视线"；
               曾缩到中心 50% 导致头右半落空（F_Head 偏右），回滚为完整核心区）
               + 视线跟踪 */
            if (!m->pat_active) {
                float x0 = sX(m, m->head_rect[0], DEV_W), x1 = sX(m, m->head_rect[2], DEV_W);
                float y0 = sY(m, m->head_rect[1], DEV_H), y1 = sY(m, m->head_rect[3], DEV_H);
                if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) {
                    m->pat_active = true;
                    m->pat_t0 = now;
                }
            }
            /* 视线目标：仅非拖拽时更新（拖拽是手动转头，视线保持，与仿真器一致） */
            float dx = (sx - sX(m, m->head_center[0], DEV_W)) / 70.0f;
            float dy = (sy - sY(m, m->head_center[1], DEV_H)) / 70.0f;
            m->track_x = dx > 1 ? 1 : (dx < -1 ? -1 : dx);
            m->track_y = dy > 1 ? 1 : (dy < -1 ? -1 : dy);
            m->track_set = true;
        }
        m->last_x = sx;
        m->last_y = sy;
    } else {
        /* 松开：回待机（与 Live2D 引擎一致——触摸/视线追踪结束后恢复演示） */
        m->dragging = false;
        m->press_set = false;
        m->track_set = false;          // 视线回待机游走
        m->track_x = 0; m->track_y = 0;
        if (m->expr == 0) m->demo = true;   // 无表情时恢复待机演示（表情模式保持表情）
    }
}

void pd_set_demo(pd_model_t *m, bool on) {
    if (!m) return;
    m->demo = on;
}

/* 播放原生动画（anims.json 增量时间轴）；NULL/空 = 停止回待机 */
void pd_set_anim(pd_model_t *m, const char *name) {
    if (!m) return;
    /* 2026-09-17:mesh 模式动画在 mesh.ppdq 里——记录名+起始时刻,渲染分流使用 */
    if (m->pdq) {
        extern int pdq_has_anim(void *pdm, const char *anim);
        extern int pdq_anim_count(void *pdm);
        extern int pdq_anim_is_loop(void *pdm, const char *anim);
        m->pdq_anim[0] = 0;
        m->pdq_force_loop = false;
        if (name && name[0]) {
            if (pdq_has_anim(m->pdq, name)) {
                snprintf(m->pdq_anim, sizeof(m->pdq_anim), "%s", name);
                m->pdq_anim_t0 = m->last_ms;
                ESP_LOGI(TAG, "pdq anim play: %s", name);
            } else {
                ESP_LOGW(TAG, "pdq anim not found: %s", name);
                /* 2026-09-27 兜底(用户拍板):无 Idle → Relax;Relax 非循环 → 强制循环播。
                   都没有 → 第一个动画+强制循环 */
                extern const char *pdq_anim_name(void *pdm, int i);
                const char *fb = NULL;
                if (pdq_has_anim(m->pdq, "Relax")) {
                    fb = "Relax";
                    if (!pdq_anim_is_loop(m->pdq, "Relax"))
                        m->pdq_force_loop = true;   /* 基建 Relax 非循环(播4秒停)→ 强制循环 */
                }
                if (!fb && pdq_anim_count(m->pdq) > 0) {
                    fb = pdq_anim_name(m->pdq, 0);
                    m->pdq_force_loop = true;
                }
                if (fb) {
                    snprintf(m->pdq_anim, sizeof(m->pdq_anim), "%s", fb);
                    m->pdq_anim_t0 = m->last_ms;
                    ESP_LOGI(TAG, "pdq anim fallback: %s (force_loop=%d)", fb, m->pdq_force_loop);
                }
            }
        }
        m->anim_idx = -1;
        return;
    }
    m->anim_idx = -1;
    if (!name || !name[0]) {
        if (m->expr == 0) m->demo = true;   // 停止：回待机演示
        return;
    }
    for (int i = 0; i < m->n_anims; i++) {
        if (strcmp(m->anims[i].name, name) == 0) {
            m->anim_idx = i;
            /* 时间基准必须与渲染 t_ms 一致（相对时钟）：不能用系统时间——
               曾用 esp_timer_get_time() 导致 t-t0 恒负、动画卡第 0 帧（画面无变化） */
            m->anim_t0 = m->last_ms;
            m->anim_speed = 0.6f;   // 慢放便于看清动作（用户反馈）
            m->demo = false;
            ESP_LOGI(TAG, "anim play: %s (%.2fs %s)", name, m->anims[i].duration,
                     m->anims[i].loop ? "loop" : "once");
            return;
        }
    }
    ESP_LOGW(TAG, "anim not found: %s", name);
}

/* ---- 参数驱动 ---- */
static void demo_params(pd_model_t *m, uint32_t t_ms) {
    float t0 = t_ms / 1000.0f;
    float *P = m->P;
    /* 转头角平滑收敛（用户拖动松手回待机时从大角度渐回摆动，不硬跳） */
    float dax = 3.0f * sinf(2 * PI_F * t0 / 7.3f) - P[P_AX];
    float day = 1.2f * sinf(2 * PI_F * t0 / 6.1f + 1) - P[P_AY];
    P[P_AX] += dax * 0.15f;
    P[P_AY] += day * 0.15f;
    P[P_AZ] = 1.0f * sinf(2 * PI_F * t0 / 5.2f);
    P[P_BALLX] = 0.4f * sinf(2 * PI_F * t0 / 4.0f);
    float ph = fmodf(t0, 2.8f) / 2.8f;
    float blink = ph < 0.10f ? fmaxf(0.05f, 1 - sinf(ph / 0.10f * PI_F) * 0.95f) : 1;
    P[P_EYEL] = blink;
    P[P_EYER] = blink;
    float mp2 = fmodf(t0, 6) / 6;
    P[P_MOUTH] = (mp2 > 0.82f && mp2 < 0.95f) ? sinf((mp2 - 0.82f) / 0.13f * PI_F) : 0;
    P[P_SMILEL] = 0;
    P[P_SMILER] = 0;
    P[P_FORM] = 0.2f * sinf(2 * PI_F * t0 / 9);
    P[P_BALLY] = 0;
    P[P_BROWYL] = 0;
    P[P_BROWYR] = 0;
    P[P_BROWAL] = 0;
    P[P_BROWAR] = 0;
}

/* 表情库（与 PC 仿真器 sim.js EXPRESSIONS 表逐行对应：参数顺序同 PARAM_KEYS；
   osc_p<0 表示无振荡，否则 osc_amp·sin(2π·t·osc_freq) 叠加到该参数） */
typedef struct { float v[15]; int8_t osc_p; float osc_amp, osc_freq; } pd_expr_t;
static const pd_expr_t PD_EXPR[13] = {
    {{0}},                                                       /* 0 无表情 */
    {{0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, -1, 0, 0},  /* 1 说话（嘴型 speak_mouth 循环） */
    {{0, 0, 0, 0.40f, 0.40f, 0, -0.50f, -0.50f, -0.30f, 0, 1.0f, -1, -1, 0, 0}, 9, 0.60f, 0.25f}, /* 2 害羞 */
    {{0, 0, 0, 1, 1, 1.0f, 0, 0, 1.0f, 0, 0, 4, 4, 0, 0}, -1, 0, 0},       /* 3 惊讶 */
    {{0, 0, 0, 1.0f, 1, 0.75f, 0, 0, 0.30f, 0, 0, 1, 1, 0, 0}, -1, 0, 0},  /* 4 开心 */
    {{0, 0, 0, 0.83f, 0.81f, 0.00f, 1.00f, 0.52f, 1.00f, 1.00f, 1.00f, 6.00f, 4.69f, 0.00f, 0.00f}, -1, 0, 0}, /* 5 生气（用户定稿） */
    {{0, 0, 0, 0.37f, 0.95f, 0.00f, 0, 0, 1.00f, 0.00f, 0.00f, 0.00f, -1.00f, 3.59f, 5.38f}, 9, 0.30f, 0.30f}, /* 6 俏皮（用户定稿） */
    {{10, 4, 0, 0.77f, 0.80f, 0.14f, 1.0f, 1.0f, -0.09f, -0.18f, 1.0f, 3.36f, 1.21f, 5.60f, 4.37f}, 9, 0.25f, 0.35f}, /* 7 悲伤（用户定稿） */
    {{0, 0, 0, 0.66f, 0.66f, 0.20f, 0, 0, 0.30f, 1.0f, 1.0f, 6.0f, 0, 0, 0}, 9, 0.30f, 0.30f}, /* 8 疑惑（用户定稿） */
    {{0, 0, 0, 0.30f, 0.30f, 0.34f, 0, 0, 0, 0, 1.0f, 0, 0, 0, 0}, 9, 0.30f, 0.25f}, /* 9 困倦 */
    {{0, 0, 0, 0.92f, 1.00f, 1.00f, 0.64f, 0.64f, 1.00f, 1.00f, 1.00f, 1.46f, 1.00f, 0.00f, 0.00f}, 9, 0.30f, 0.30f}, /* 10 微笑（用户定稿） */
    {{0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, -1, 0, 0},             /* 11 待机 */
    {{0, 0, 0, 1, 1, 0.75f, 0, 0, 0.60f, 0, 0, 0, 0, 0, 0}, -1, 0, 0},    /* 12 自信 */
};

/* 说话嘴型循环：0→1→0.5→1→0（周期 1s，与 sim.js 一致） */
static float speak_mouth(uint32_t t_ms) {
    float T = 1.0f;
    float p = fmodf(t_ms / 1000.0f, T) / T;
    if (p < 0.3f) return p / 0.3f;
    if (p < 0.5f) return 1 - 0.5f * (p - 0.3f) / 0.2f;
    if (p < 0.8f) return 0.5f + 0.5f * (p - 0.5f) / 0.3f;
    return 1 - (p - 0.8f) / 0.2f;
}

static void apply_expr(pd_model_t *m, uint32_t t_ms) {
    if (m->expr < 1 || m->expr > 12) return;
    const pd_expr_t *e = &PD_EXPR[m->expr];
    /* 表情参数平滑收敛（每帧 30%）：LLM 每句回复切换表情时不再骤变——
       曾硬设 P 导致表情/角度跳变，视觉上"人物闪烁" */
    for (int i = 0; i < 15; i++) {
        if (i == e->osc_p) continue;   // 振荡参数独立设置（动态）
        m->P[i] += (e->v[i] - m->P[i]) * 0.30f;
    }
    if (e->osc_p >= 0) {
        float t0 = t_ms / 1000.0f;
        m->P[e->osc_p] = e->v[e->osc_p] + e->osc_amp * sinf(2 * PI_F * t0 * e->osc_freq);
    }
    /* 表情保持时叠加正常眨眼（与演示同款 2.8s 周期） */
    float t0 = t_ms / 1000.0f;
    float ph = fmodf(t0, 2.8f) / 2.8f;
    float blink = ph < 0.10f ? fmaxf(0.05f, 1 - sinf(ph / 0.10f * PI_F) * 0.95f) : 1;
    m->P[P_EYEL] *= blink;
    m->P[P_EYER] *= blink;
    if (m->expr == 1) m->P[P_MOUTH] = speak_mouth(t_ms - m->speak_t0);   /* 说话嘴型循环（相位从开始说话起） */
}

void pd_set_expression(pd_model_t *m, int ex) {
    if (!m) return;
    if (ex < 0 || ex > 12) ex = 0;
    /* 新进入说话表情：嘴型循环从闭合相位起步（全局相位会随机跳 → 更新一句话闪一下） */
    if (ex == 1 && m->expr != 1) m->speak_t0 = m->last_ms;
    m->expr = ex;
    m->demo = (ex == 0);   /* 无表情恢复演示；有表情停演示 */
}

static void apply_pat(pd_model_t *m, uint32_t t_ms) {
    float p = (t_ms - m->pat_t0) / 1400.0f;
    if (p >= 1) { m->pat_active = false; return; }
    float w = sinf(PI_F * (p > 1 ? 1 : p));   /* 0→1→0 包络 */
    float *P = m->P;
    P[P_SMILEL] = 1.5f * w;   /* 眯眼笑（1.5：眼白压到 0.55；与 sim.js 一致） */
    P[P_SMILER] = 1.5f * w;   /* 眯眼笑（1.5：眼白压到 0.55；与 sim.js 一致） */
    P[P_FORM] = 0.2f * w;
    P[P_EYEL] = P[P_EYER] = 1 - 0.45f * w;
    P[P_AY] = 2.5f * sinf(p * 4 * PI_F) * w;
    P[P_AZ] = 1.5f * sinf(p * 4 * PI_F) * w;
    P[P_AX] = 0;
    P[P_BROWYL] = 0;
    P[P_BROWYR] = 0;
    P[P_BROWAL] = 0;
    P[P_BROWAR] = 0;
}

/* ---- 渲染 ---- */
/* ---- 双核并行：worker 渲染下半带，主任务渲染上半带（按行带切分保持 z 序） ---- */
static TaskHandle_t s_worker = NULL;
static SemaphoreHandle_t s_go = NULL, s_done = NULL;
static pd_model_t *s_wm = NULL;
static uint16_t *s_wdst = NULL;
static int s_wdw = 0, s_wdh = 0, s_gf = -1;
static uint32_t s_wt = 0;

static void draw_band(pd_model_t *m, uint16_t *dst, int dw, int dh, uint32_t t_ms,
                      int cy0, int cy1, bool fill_bg, int gfilter);

static void worker_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        draw_band(s_wm, s_wdst, s_wdw, s_wdh, s_wt, s_wdh / 2, s_wdh, true, s_gf);
        xSemaphoreGive(s_done);
    }
}

/* 参数更新（每帧一次，仅主任务；worker 只读 P/sway_amp） */
static void update_params(pd_model_t *m, uint32_t t_ms) {
    float dt;
    if (m->last_ms == 0) dt = 1.0f / 60.0f;
    else {
        dt = (t_ms - m->last_ms) / 1000.0f;
        if (dt > 0.25f) dt = 0.25f;
    }
    m->last_ms = t_ms;
    /* 原生动画播放：一次性动画播完回待机（循环动画由渲染侧取模） */
    if (m->anim_idx >= 0 && m->anim_idx < m->n_anims) {
        pd_anim_t *A = &m->anims[m->anim_idx];
        if (!A->loop && (t_ms - m->anim_t0) * m->anim_speed / 1000.0f >= A->duration) {
            m->anim_idx = -1;
            if (m->expr == 0) m->demo = true;
        }
    }
    /* 优先级：摸头 > 表情 > 演示（与 sim.js 一致） */
    if (m->pat_active) apply_pat(m, t_ms);
    else if (m->expr > 0) apply_expr(m, t_ms);
    else if (m->demo) demo_params(m, t_ms);
    if (m->track_set) {
        float k = fminf(1.0f, 5.0f * dt);
        m->P[P_BALLX] += (m->track_x - m->P[P_BALLX]) * k;
        m->P[P_BALLY] += (m->track_y - m->P[P_BALLY]) * k;
        /* 头部随视线微随动：眼睛主导，头辅随（±4°/±3°） */
        m->hf_x += (m->track_x * 4.0f - m->hf_x) * k;
        m->hf_y += (m->track_y * 3.0f - m->hf_y) * k;
    } else {
        /* 视线回中：松手后眼球与头部微随动渐回 0（回待机） */
        float k = fminf(1.0f, 5.0f * dt);
        m->P[P_BALLX] += (0 - m->P[P_BALLX]) * k;
        m->P[P_BALLY] += (0 - m->P[P_BALLY]) * k;
        m->hf_x += (0 - m->hf_x) * k;
        m->hf_y += (0 - m->hf_y) * k;
    }
    m->sway_amp += ((((m->demo || m->expr > 0) && !m->pat_active) ? 1.0f : 0.0f) - m->sway_amp) * 0.05f;
}

/* 渲染 [cy0, cy1) 行带（双核各画一半，z 序安全）
   gfilter: -1=全部层, 0=仅身体组, 1=仅头组（混合分辨率两趟渲染用） */
static void draw_band(pd_model_t *m, uint16_t *dst, int dw, int dh, uint32_t t_ms,
                      int cy0, int cy1, bool fill_bg, int gfilter) {
    /* 背景（仅本带；第二趟头组叠加时不填背景）：有 bg 则 cover 铺满，否则深色。
       性能红线：逐像素浮点采样会吃满 CPU（38 万 px × 浮点 ≈ 70ms/帧 → watchdog），
       只用 1:1 memcpy 快路径 + 整数 8.8 定点下采样（fast 模式 240×400）。 */
    if (fill_bg) {
        if (m->long_bg && m->long_bg_w > 0 && m->long_bg_w >= dw) {
            /* 长背景偏移渲染（横屏长图竖屏切片，外部注入）：
               视口 = 480×800 窗口在可见宽内平移，off_x=视口左缘偏移。
               dw==480 全分辨率 → 每行 1:1 memcpy；
               dw==240 fast 半分辨率 → 视口 480→240 定点近邻（每输出列 2 源列）。 */
            int off = m->long_bg_off_x;
            if (off < 0) off = 0;
            if (off > m->long_bg_w - 480) off = m->long_bg_w - 480;   // off 是 480 视口坐标(与 dw 无关)
            if (dw == 480) {
                for (int y = cy0; y < cy1; y++)
                    memcpy(dst + (size_t)y * dw,
                           m->long_bg + (size_t)y * m->long_bg_stride + off, (size_t)dw * 2);
            } else {
                const uint16_t bcol = pack565(0x0b, 0x0d, 0x12);
                int sx_step = (int)((480u << 8) / dw);   // 视口 480 → dw
                int sy_step = (int)((800u << 8) / dh);   // 视口 800 → dh
                int fx0 = off << 8;
                for (int y = cy0; y < cy1; y++) {
                    int sy = (y * sy_step) >> 8;
                    uint16_t *drow = dst + (size_t)y * dw;
                    if (sy < 0 || sy >= m->long_bg_h) {
                        for (int x = 0; x < dw; x++) drow[x] = bcol;
                        continue;
                    }
                    const uint16_t *srow = m->long_bg + (size_t)sy * m->long_bg_stride;
                    int fx = fx0;
                    for (int x = 0; x < dw; x++, fx += sx_step)
                        drow[x] = srow[fx >> 8];
                }
            }
        } else if (m->bg && m->bg_w > 0 && m->bg_h > 0) {
            if (m->bg_w == dw && m->bg_h == dh) {
                /* 1:1 快路径（bg.raw 标准 480×800）：每行 memcpy */
                for (int y = cy0; y < cy1; y++)
                    memcpy(dst + (size_t)y * dw, m->bg + (size_t)y * m->bg_w, (size_t)dw * 2);
            } else {
                /* 下采样（fast 模式 240×400）：8.8 定点近邻 */
                const uint16_t bcol = pack565(0x0b, 0x0d, 0x12);
                int sx_step = (int)(((uint32_t)m->bg_w << 8) / dw);
                int sy_step = (int)(((uint32_t)m->bg_h << 8) / dh);
                for (int y = cy0; y < cy1; y++) {
                    int sy = (y * sy_step) >> 8;
                    uint16_t *drow = dst + (size_t)y * dw;
                    if (sy < 0 || sy >= m->bg_h) {
                        for (int x = 0; x < dw; x++) drow[x] = bcol;
                        continue;
                    }
                    const uint16_t *srow = m->bg + (size_t)sy * m->bg_w;
                    int fx = 0;
                    for (int x = 0; x < dw; x++, fx += sx_step)
                        drow[x] = srow[fx >> 8];
                }
            }
        } else {
            uint16_t bg = pack565(0x0b, 0x0d, 0x12);
            for (int y = cy0; y < cy1; y++)
                for (int x = 0; x < dw; x++) dst[y * dw + x] = bg;
        }
    }

    float *P = m->P;
    float ax = (P[P_AX] + m->hf_x) * PI_F / 180, ay = (P[P_AY] + m->hf_y) * PI_F / 180,
          az = P[P_AZ] * PI_F / 180;
    float sx = cosf(ax * 1.15f), shx = -0.28f * sinf(ax * 1.15f);
    float ry = ay * 0.5f, rz = az;
    float tiltRad = m->face_tilt * PI_F / 180;
    float eyeTiltRad = m->eye_tilt * PI_F / 180;   // 眼轴（眨眼压缩沿眼睛竖直方向；侧脸单眼 ≠ 脸轴）
    float mouthTiltRad = m->mouth_tilt * PI_F / 180;   // 嘴轴（张嘴/嘴型沿嘴的垂直方向；侧脸 ≠ 脸轴）
    float vscale = m->base_scale * m->zoom * m->scale;
    float hp0 = m->head_pivot[0], hp1 = m->head_pivot[1];

    /* 待机晃动（整身绕底部中心）+ 呼吸 */
    float t0 = t_ms / 1000.0f;
    float sway = m->sway_amp * 0.7f * sinf(2 * PI_F * t0 / 5.8f) * PI_F / 180;   /* 0.7°（sim.js 同款度转弧度） */
    float breathPx = m->sway_amp * 1.8f * sinf(2 * PI_F * t0 / 3.5f);
    float spx = sX(m, 640, dw), spy = sY(m, 1260, dh);

    /* PPA 帧：sway 旋转近似为整身统一平移（头/身/颈同位移，零相对层移）；
       非 PPA 帧（特写/快模式）：保持真旋转与仿真器一致 */
    bool ppa_frame = (PD_USE_PPA && s_ppa_client && m->zoom == m->ppa_zoom &&
                      m->scale == m->ppa_scale && m->pan_y == 0);
    float sway_dx = 0, sway_dy = 0;
    int sway_dx_q = 0, sway_dy_q = 0;
    mat_t Msway;
    if (ppa_frame) {
        /* 整身统一平移并整数量化：PPA 层是整数坐标，CPU 层同步量化到同一整数网格，
           消除"衣服动3次头动6次"的量化相位差（层间仅存各自的常数小数基差，不随时间变化，不可见） */
        sway_dx = -sway * (400.0f - spy);
        sway_dy = sway * (240.0f - spx);
        sway_dx_q = (int)lrintf(sway_dx);
        sway_dy_q = (int)lrintf(sway_dy);
        Msway = mat_T((float)sway_dx_q, (float)sway_dy_q);
    } else {
        Msway = mat_T(spx, spy);
        mat_t r = mat_R(sway), t = mat_T(-spx, -spy);
        mat_mul(&Msway, &r);
        mat_mul(&Msway, &t);
    }

    /* 头组基变换（不含 sway）：T(hp)·R(ry)·R(rz)·Sh·S(vscale)·T(-hp) */
    mat_t Mhead = mat_id();
    {
        mat_t t1 = ppa_frame
                       ? mat_T(roundf(sX(m, hp0, dw)), roundf(sY(m, hp1, dh)) + breathPx)
                       : mat_T(sX(m, hp0, dw), sY(m, hp1, dh) + breathPx);
        mat_t r1 = mat_R(ry), r2 = mat_R(rz);
        mat_t sh = mat_sh(sx, shx * 0.15f);
        mat_t sc = mat_S(vscale, vscale);
        mat_t t2 = mat_T(-hp0, -hp1);
        mat_mul(&Mhead, &t1);
        mat_mul(&Mhead, &r1);
        mat_mul(&Mhead, &r2);
        mat_mul(&Mhead, &sh);
        mat_mul(&Mhead, &sc);
        mat_mul(&Mhead, &t2);
    }
    /* 口腔椭圆的线性部分：R(sway)·R(ry)·R(rz)·Sh·S(vscale)·R(mouthTilt)（PPA 帧 sway 已平移化）。
       口腔是嘴的一部分，沿嘴轴倾斜（侧脸立绘 ≠ 脸轴） */
    mat_t Lcav = ppa_frame ? mat_id() : mat_R(sway);
    {
        mat_t r1 = mat_R(ry), r2 = mat_R(rz);
        mat_t sh = mat_sh(sx, shx * 0.15f);
        mat_t sc = mat_S(vscale, vscale);
        mat_t rt = mat_R(mouthTiltRad);
        mat_mul(&Lcav, &r1);
        mat_mul(&Lcav, &r2);
        mat_mul(&Lcav, &sh);
        mat_mul(&Lcav, &sc);
        mat_mul(&Lcav, &rt);
    }

    for (int i = 0; i < m->n_layers; i++) {
        pd_layer_t *L = &m->layers[i];
        if (!L->tex.rgba) continue;
        /* 可见性：无动画用默认 visible（隐藏槽不渲染）；动画播放中由可见性轨道插值覆盖 */
        {
            bool anim_visible = L->visible;
            if (m->anim_idx >= 0 && m->anim_idx < m->n_anims) {
                pd_anim_t *AV = &m->anims[m->anim_idx];
                float atv = (t_ms - m->anim_t0) * m->anim_speed / 1000.0f;
                if (!AV->loop && AV->duration > 0) {
                    if (atv > AV->duration) atv = AV->duration;
                    if (atv < 0) atv = 0;
                } else if (AV->loop && AV->duration > 0) {
                    atv = fmodf(atv, AV->duration);
                }
                for (int k = 0; k < AV->n_tracks; k++) {
                    if (AV->tracks[k].layer_idx != i) continue;
                    int nk = AV->tracks[k].n_keys;
                    float pos = AV->duration > 0 ? atv / AV->duration * (nk - 1) : 0;
                    int i0 = (int)pos;
                    float frac = pos - i0;
                    if (i0 >= nk - 1) { i0 = nk - 2; frac = 1; }
                    if (i0 < 0) { i0 = 0; frac = 0; }
                    float v0 = AV->tracks[k].keys[i0].vis;
                    float v1 = AV->tracks[k].keys[i0 + 1].vis;
                    float visv = v0 + (v1 - v0) * frac;
                    anim_visible = visv >= 0.5f;
                    break;
                }
            }
            if (!anim_visible) continue;
        }
        if (gfilter == 0) {
            /* 身体趟：body 组 + z 序压在身体之下的头件（back hair z=4 / eyewear z=2 / earwear z=3） */
            if (L->group != 0 && strcmp(L->name, "back hair") != 0 &&
                strcmp(L->name, "eyewear") != 0 && strcmp(L->name, "earwear") != 0) continue;
        } else if (gfilter == 1) {
            /* 头趟：z≥15 的头件（不含压身体的 back hair/eyewear/earwear） */
            if (L->group != 1 || strcmp(L->name, "back hair") == 0 ||
                strcmp(L->name, "eyewear") == 0 || strcmp(L->name, "earwear") == 0) continue;
        }
        mat_t M = Msway;
        float px = hp0, py = hp1;   /* 特殊部件枢轴（头组分支内重定向；瞳孔裁剪在层尾使用） */

        if (L->group == 0 && strcmp(L->name, "neck") != 0) {
            /* 呼吸层（topwear/手）：始终 CPU 浮点渲染（亚像素平滑呼吸，与头/颈连续）；
               其余身体大图层走 PPA 硬件（整数坐标） */
            static const char *bobset[] = {"topwear", "handwear-l", "handwear-r"};
            bool breathe = false;
            for (size_t k = 0; k < 3; k++)
                if (strcmp(L->name, bobset[k]) == 0) { breathe = true; break; }
            float bob = breathe ? breathPx : 0;
            /* PPA 硬件混合：全身帧 + 1:1 预缩放纹理（sway 旋转近似为整数平移）。
               取景/缩放匹配才启用；快模式（zoom 折半）与特写自动回退 CPU。
               原生动画播放中禁用（动画增量无法应用在硬件层上，走 CPU 全层渲染） */
            if (s_ppa_client && L->has_ppa && !breathe && m->anim_idx < 0 &&
                m->zoom == m->ppa_zoom && m->scale == m->ppa_scale && m->pan_y == 0) {
                /* 整身统一平移（与头/颈同源整数，零相对层移） */
                int dx = sway_dx_q;
                int dy = sway_dy_q;
                int64_t t0p = esp_timer_get_time();
                ppa_blend_layer(dst, dw, dh, L->ppa_x + dx, L->ppa_y + dy, &L->ppa_tex, cy0, cy1);
                int64_t t1p = esp_timer_get_time();
                L->prof_us += (uint32_t)(t1p - t0p);
                L->prof_px += (uint32_t)L->ppa_tex.w * L->ppa_tex.h;
                continue;
            }
            /* CPU 兜底：身组 bob 呼吸（topwear/手）；腿脚不动 */
            mat_t t = ppa_frame
                          ? mat_T(roundf(sX(m, 0, dw)), roundf(sY(m, 0, dh)) + bob)
                          : mat_T(sX(m, 0, dw), sY(m, 0, dh) + bob);
            mat_t sc = mat_S(vscale, vscale);
            mat_mul(&M, &t);
            mat_mul(&M, &sc);
        } else if (strcmp(L->name, "neck") == 0) {
            /* 脖子 0.85 阻尼 */
            mat_t t1 = ppa_frame
                       ? mat_T(roundf(sX(m, hp0, dw)), roundf(sY(m, hp1, dh)) + breathPx)
                       : mat_T(sX(m, hp0, dw), sY(m, hp1, dh) + breathPx);
            mat_t r = mat_R((ry + rz) * 0.85f);
            mat_t sc1 = mat_S(1 + (sx - 1) * 0.85f, 1);
            mat_t sc = mat_S(vscale, vscale);
            mat_t t2 = mat_T(-hp0, -hp1);
            mat_mul(&M, &t1);
            mat_mul(&M, &r);
            mat_mul(&M, &sc1);
            mat_mul(&M, &sc);
            mat_mul(&M, &t2);
        } else {
            /* 头组全变换 */
            mat_t tmp = Mhead;
            M = Msway;
            mat_mul(&M, &tmp);

            /* 口腔：画在嘴层下方（模板嘴 no_cavity 时跳过） */
            if (!m->no_cavity && strcmp(L->name, "mouth") == 0 && P[P_MOUTH] > 0.01f) {
                float cx, cy;
                mat_pt(&M, m->mouth_pivot[0], m->mouth_pivot[1], &cx, &cy);
                fill_ellipse(dst, dw, dh, cx, cy,
                             fmaxf(4, L->w * 0.28f),
                             fmaxf(3, L->h * 0.35f * P[P_MOUTH]), &Lcav, cy0, cy1);
            }

            /* 各特殊部件变换（嘴只做标准缩放动画，贴图不做任何处理） */
            px = hp0; py = hp1;
            if (L->special == 1 || L->special == 4) { px = m->mouth_pivot[0]; py = m->mouth_pivot[1]; }
            if (L->special == 2) { px = L->cx; py = L->cy; }
            if (L->special == 3) { px = L->cx; py = L->cy; }
            mat_t t1 = mat_T(px, py);
            mat_t t2 = mat_T(-px, -py);
            mat_mul(&M, &t1);

            if (L->special == 1 || L->special == 4) {
                /* 脸轴张嘴矩阵 B·S·Bᵀ */
                float sy = m->mouth_compress
                               ? m->mouth_floor + m->mouth_gain * P[P_MOUTH]
                               : 1 + P[P_MOUTH] * m->mouth_open_gain;
                float sxf = 1 + P[P_FORM] * 0.25f + P[P_MOUTH] * m->mouth_width_open;
                float ft = mouthTiltRad;   // 嘴轴（张嘴沿嘴的垂直方向，侧脸立绘 ≠ 脸轴）
                float ux = cosf(ft), uy = sinf(ft);
                float vx = -uy, vy = ux;
                mat_t B = {ux, uy, vx, vy, 0, 0};
                mat_t Bi = {ux, vx, uy, vy, 0, 0};
                mat_t S2 = mat_S(sxf, sy);
                mat_t bm = B;
                mat_mul(&bm, &S2);
                mat_mul(&bm, &Bi);
                mat_mul(&M, &bm);
            } else if (L->special == 2 || L->special == 3) {
                float s = (L->special == 2 ? P[P_EYEL] : P[P_EYER]) *
                          (1 - 0.3f * (L->special == 2 ? P[P_SMILEL] : P[P_SMILER]));
                /* F_Eye_L/R（Q 版拆分的单眼层）：眨眼压向眼中心偏下 0.2h——
                   上眼皮下移 0.7h 主导（从上往下闭的方向感），下眼睑仅上移 0.3h
                   （真实眨眼下睑也有轻微上移），闭合线≈原眼中心 = 原地闭眼。
                   中心锚点（0）：上下对称夹"一上一下"怪感；眼底锚点（0.5）：眼睛整体下移。
                   T(p')·S·T(-p') = T(p)·[T(0,+ao)·S·T(0,-ao)]·T(-p)，
                   ao = 0.2·层高；与 PC 仿真器 eyePivot 一致。
                   竖屏压 y（视觉竖直）；横屏场景（角色侧躺）视觉竖直 = 画布 x → 压 x
                   （横屏 Q 版维持原中心锚点 ao=0，另一套 tune 勿动） */
                if (strncmp(L->name, "eyelash", 7) == 0 || strncmp(L->name, "eyewhite", 8) == 0 ||
                    strncmp(L->name, "F_Eye", 5) == 0) {
                    float ao = m->landscape ? 0.0f : L->h * 0.2f;
                    /* r1/r2 用眼轴（eye_tilt）：眨眼压缩沿眼睛竖直方向
                       （侧脸单眼立绘眼裂方向 ≠ face 主轴估算的脸轴） */
                    mat_t r1 = mat_R(eyeTiltRad), o1 = mat_T(0, ao),
                          sc = m->landscape
                                   ? mat_S(fmaxf(0.05f, s), 1)
                                   : mat_S(1, fmaxf(0.05f, s)),
                          o2 = mat_T(0, -ao), r2 = mat_R(-eyeTiltRad);
                    mat_mul(&M, &r1);
                    mat_mul(&M, &o1);
                    mat_mul(&M, &sc);
                    mat_mul(&M, &o2);
                    mat_mul(&M, &r2);
                } else if (strncmp(L->name, "irides", 6) == 0) {
                    /* 瞳孔随眼白同一变换压缩（标准 Live2D 眨眼画法：眼睛组整体压扁），
                       与眼皮完全同步——裁剪带方案在眼轴倾斜时瞳孔漏出/慢半拍，已废弃。
                       视线追踪平移保留（缩放前应用，平移量不随眨眼变） */
                    float ao = m->landscape ? 0.0f : L->h * 0.2f;
                    mat_t r1 = mat_R(eyeTiltRad),
                          tr = mat_T(P[P_BALLX] * 3.2f, P[P_BALLY] * 2.2f),
                          o1 = mat_T(0, ao),
                          sc = m->landscape ? mat_S(fmaxf(0.05f, s), 1)
                                            : mat_S(1, fmaxf(0.05f, s)),
                          o2 = mat_T(0, -ao),
                          r2 = mat_R(-eyeTiltRad);
                    mat_mul(&M, &r1);
                    mat_mul(&M, &tr);
                    mat_mul(&M, &o1);
                    mat_mul(&M, &sc);
                    mat_mul(&M, &o2);
                    mat_mul(&M, &r2);
                } else if (strncmp(L->name, "eyebrow", 7) == 0) {
                    /* 眉毛独立参数：高（px）+ 斜（度，正=内端向下=生气皱眉）。
                       眉毛跟脸轴（眉高/眉斜按脸轴 tune） */
                    bool is_l = (L->special == 2);
                    float by = is_l ? P[P_BROWYL] : P[P_BROWYR];
                    float ba = (is_l ? P[P_BROWAL] : -P[P_BROWAR]) * PI_F / 180;
                    mat_t r1 = mat_R(tiltRad);
                    mat_t tr = mat_T(0, by);
                    mat_t rr = mat_R(ba);
                    mat_t r2 = mat_R(-tiltRad);
                    mat_mul(&M, &r1);
                    mat_mul(&M, &tr);
                    mat_mul(&M, &rr);
                    mat_mul(&M, &r2);
                }
            }
            mat_mul(&M, &t2);
        }

        /* 图层左上角平移到层内坐标，纹理拉伸到图层矩形（导出可降采样，几何不变） */
        mat_t tl = mat_T(L->x, L->y);
        mat_mul(&M, &tl);
        /* 原生动画增量（anims.json）：绕层中心旋转 + 平移（关键帧线性插值）。
           一次性动画乘 sin 包络（0→1→0）：姿势渐入渐出——Spine 该模型的动作
           是"换链瞬变姿势"（中间过程在隐藏链/mesh 形变，PPD 表达不了），
           包络提供过程感（待机→姿势→待机），避免瞬切两帧 */
        if (m->anim_idx >= 0 && m->anim_idx < m->n_anims) {
            pd_anim_t *A = &m->anims[m->anim_idx];
            float at = (t_ms - m->anim_t0) * m->anim_speed / 1000.0f;
            float env = 1.0f;   /* 无包络:轨道忠实还原 spine 时间轴(与 sim.js 一致) */
            if (A->loop && A->duration > 0) {
                at = fmodf(at, A->duration);
            }
            for (int k = 0; k < A->n_tracks; k++) {
                if (A->tracks[k].layer_idx != i) continue;
                int nk = A->tracks[k].n_keys;
                float pos = A->duration > 0 ? at / A->duration * (nk - 1) : 0;
                int i0 = (int)pos;
                float frac = pos - i0;
                if (i0 >= nk - 1) { i0 = nk - 2; frac = 1; }
                if (i0 < 0) { i0 = 0; frac = 0; }
                pd_anim_key_t *k0 = &A->tracks[k].keys[i0];
                pd_anim_key_t *k1 = &A->tracks[k].keys[i0 + 1];
                float adx = (k0->dx + (k1->dx - k0->dx) * frac) * env;
                float ady = (k0->dy + (k1->dy - k0->dy) * frac) * env;
                float adrot = (k0->rot + (k1->rot - k0->rot) * frac) * env * PI_F / 180;
                float asx = k0->sx + (k1->sx - k0->sx) * frac;
                float asy = k0->sy + (k1->sy - k0->sy) * frac;
                bool use_affine = k0->affine && k1->affine;
                if (adx != 0 || ady != 0 || adrot != 0 || asx != 1.0f || asy != 1.0f || use_affine) {
                    float ax = L->bone_px, ay = L->bone_py;
                    mat_t t1 = mat_T(ax + adx, ay + ady);
                    mat_t t2 = mat_T(-ax, -ay);
                    mat_mul(&M, &t1);
                    if (use_affine) {
                        mat_t aa = {
                            k0->a + (k1->a - k0->a) * frac,
                            k0->b + (k1->b - k0->b) * frac,
                            k0->c + (k1->c - k0->c) * frac,
                            k0->d + (k1->d - k0->d) * frac,
                            0, 0
                        };
                        mat_mul(&M, &aa);
                    } else {
                        mat_t rr = mat_R(adrot);
                        mat_t ss = mat_S(asx, asy);
                        mat_mul(&M, &rr);
                        mat_mul(&M, &ss);
                    }
                    mat_mul(&M, &t2);
                }
                break;
            }
        }

        const pd_tex_t *tex = &L->tex;
        if (m->hair_done && strcmp(L->name, "front hair") == 0) tex = &m->hair_punched;
        mat_t sc = mat_S((float)L->w / tex->w, (float)L->h / tex->h);
        mat_mul(&M, &sc);
        /* 瞳孔裁剪带已废弃：irides 与眼白走同一变换（见上方 special 分支），
           眼皮 z 序在瞳孔之上（build_scene 解剖修正），眨眼时天然同步遮盖 */
        int bcx0 = 0, bcy0 = cy0, bcx1 = dw, bcy1 = cy1;
        int64_t t0p = esp_timer_get_time();
        blit(dst, dw, dh, tex, &M, bcx0, bcy0, bcx1, bcy1);
        int64_t t1p = esp_timer_get_time();
        L->prof_us += (uint32_t)(t1p - t0p);
        {
            /* 像素数估算：变换后内容 bbox 面积 */
            float px0, py0, px1, py1;
            float bx0 = tex->has_cb ? tex->cbx0 : 0, by0 = tex->has_cb ? tex->cby0 : 0;
            float bx1 = tex->has_cb ? tex->cbx1 + 1 : tex->w, by1 = tex->has_cb ? tex->cby1 + 1 : tex->h;
            mat_pt(&M, bx0, by0, &px0, &py0);
            mat_pt(&M, bx1, by1, &px1, &py1);
            L->prof_px += (uint32_t)fmaxf(0, fabsf(px1 - px0) * fabsf(py1 - py0));
        }
    }
}

/* 双核跑一条行带任务（gfilter: -1=全部, 0=身体, 1=头） */
static void run_bands(pd_model_t *m, uint16_t *dst, int dw, int dh, uint32_t t_ms,
                      int gfilter) {
    if (s_worker && s_go && s_done) {
        s_wm = m; s_wdst = dst; s_wdw = dw; s_wdh = dh; s_wt = t_ms; s_gf = gfilter;
        xSemaphoreGive(s_go);
        draw_band(m, dst, dw, dh, t_ms, 0, dh / 2, true, gfilter);
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_wm = NULL;
    } else {
        draw_band(m, dst, dw, dh, t_ms, 0, dh, true, gfilter);
    }
}

/* RGB565 双像素平均（经典位技巧：高 5/6/5 位右移一位 + 绿低位修正） */
static inline uint16_t avg2_565(uint16_t a, uint16_t b) {
    return (uint16_t)(((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1) + (a & b & 0x0821));
}
static inline uint16_t avg4_565(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    return (uint16_t)(((a & 0xF7DE) >> 2) + ((b & 0xF7DE) >> 2) +
                      ((c & 0xF7DE) >> 2) + ((d & 0xF7DE) >> 2) +
                      ((a & b & c & d) & 0x0821));
}

/* hfb(半分辨率) 2× 双线性放大到 fb（三角滤波，边缘平滑不块状） */
static void upscale2x(pd_model_t *m, uint16_t *fb, int fw, int fh) {
    int dw = fw / 2, dh = fh / 2;
    const uint16_t *s = m->hfb;
    for (int y = 0; y < dh; y++) {
        int yp = y > 0 ? y - 1 : 0, yn = y + 1 < dh ? y + 1 : dh - 1;
        uint16_t *d0 = &fb[(2 * y) * fw];
        uint16_t *d1 = &fb[(2 * y + 1) * fw];
        const uint16_t *ru = &s[yp * dw], *rc = &s[y * dw], *rd = &s[yn * dw];
        for (int x = 0; x < dw; x++) {
            int xp = x > 0 ? x - 1 : 0, xn = x + 1 < dw ? x + 1 : dw - 1;
            uint16_t c = rc[x];
            uint16_t r = rc[xn], b = rd[x], d = rd[xn];
            d0[2 * x] = c;
            d0[2 * x + 1] = avg2_565(c, r);
            d1[2 * x] = avg2_565(c, b);
            d1[2 * x + 1] = avg4_565(c, r, b, d);
        }
    }
}

void pd_render(pd_model_t *m, uint16_t *fb, int fw, int fh, uint32_t t_ms) {
    if (!m || !fb) return;
    /* PPDQ 分流(2026-09-14):mesh.ppdq 存在时走三角形渲染 */
    if (m->pdq) {
        extern void pdq_render(void *pdm, uint16_t *fb, int fw, int fh, const char *anim, uint32_t t_ms, bool force_loop);
        /* 2026-09-17:背景铺真实 bg.raw(曾铺深色 0x0b0d12 → 背景全黑) */
        if (m->bg && m->bg_w == fw && m->bg_h == fh) {
            memcpy(fb, m->bg, (size_t)fw * fh * 2);
        } else {
            const uint16_t bcol = pack565(0x0b, 0x0d, 0x12);
            for (int i = 0; i < fw * fh; i++) fb[i] = bcol;
        }
        const char *an = m->pdq_anim[0] ? m->pdq_anim : NULL;
        /* 2026-09-26:pdq 分支提前 return,last_ms 从未更新——pd_set_anim 的 t0
           恒为 0,动画起始时刻错位(点击时 t 已数秒 → 帧立即越界跳终态/循环错位) */
        m->last_ms = t_ms;
        uint32_t t_rel = (m->pdq_anim[0] && t_ms >= m->pdq_anim_t0) ? (t_ms - m->pdq_anim_t0) : 0;
        pdq_render(m->pdq, fb, fw, fh, an, t_rel, m->pdq_force_loop);
        return;
    }
    update_params(m, t_ms);

    if (!s_worker) {
        s_go = xSemaphoreCreateBinary();
        s_done = xSemaphoreCreateBinary();
        /* 钉在 CPU 0：纸偶渲染（pd_anim+pd_worker）必须与 AFE/音频错核——
           Live2D 单任务渲染天然让出另一个核给 AFE；纸偶双核渲染会把两核占满，
           AFE 检测拿不到节拍 → ringbuffer full → IDLE 饿死 → watchdog */
        if (s_go && s_done) xTaskCreatePinnedToCore(worker_task, "pd_worker", 8192, NULL, 4, &s_worker, 1);  /* 渲染专用核 CPU 1；CPU 0 留给 LVGL+AFE（改到 core 0） */  // 8192 栈（渲染链深，4096 曾溢出踩坏 LVGL）+ 不 pin 让调度器避开 AFE（固定 CPU 1）
    }

    bool fast = m->hfb && m->half_res;   /* 快：全场景半分辨率 */
    if (fast) {
        float saved = m->zoom;
        m->zoom *= 0.5f;
        run_bands(m, m->hfb, fw / 2, fh / 2, t_ms, -1);
        m->zoom = saved;
        upscale2x(m, fb, fw, fh);
    } else if (m->hfb) {
        /* 高画质：全分辨率单趟（瓦片预取渲染，画质与仿真器一致） */
        run_bands(m, fb, fw, fh, t_ms, -1);
    } else {
        draw_band(m, fb, fw, fh, t_ms, 0, fh, true, -1);  /* 无 hfb 兜底：全分辨率 */
    }

    /* 剖面：每 120 帧打印各层耗时/像素 top5 */
    static uint32_t prof_frames = 0;
    if (++prof_frames >= 120) {
        prof_frames = 0;
        int top[5] = {-1, -1, -1, -1, -1};
        for (int i = 0; i < m->n_layers; i++) {
            for (int k = 0; k < 5; k++) {
                if (top[k] < 0 || m->layers[i].prof_us > m->layers[top[k]].prof_us) {
                    for (int j = 4; j > k; j--) top[j] = top[j - 1];
                    top[k] = i;
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "profile(120帧):");
        for (int k = 0; k < 5 && top[k] >= 0; k++) {
            pd_layer_t *L = &m->layers[top[k]];
            ESP_LOGI(TAG, "  %-14s %6.1f ms/f  %6.1f kpx/f", L->name,
                     L->prof_us / 120000.0f, L->prof_px / 120000.0f);
        }
        for (int i = 0; i < m->n_layers; i++) {
            m->layers[i].prof_us = 0;
            m->layers[i].prof_px = 0;
        }
    }
}
