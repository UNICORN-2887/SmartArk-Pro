/* PPDQ 设备端渲染实现(2026-09-14)。
 * 数据格式见云端 compile_ppdq.py:header+纹理表+attachment 表+动画表+数据块。
 * 顶点 q12.4 定点(int16,渲染时 *1/16);uvs uint16 像素坐标;三角 uint16 索引。
 * 光栅:重心坐标插值 + RGBA 最近邻采样 + alpha blend(normal/additive)。
 * 性能目标:~1000 三角形/帧 @ 21fps;先 float 实现实测,不够再定点化。 */
#include "pdq_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "PDQ"
#define PDQ_MAGIC 0x51445050u   // 字节 50 50 44 51 = "PPDQ" 小端读(与 compile_ppdq b'PPDQ' 一致;
                                 // 2026-09-17 修复:旧值 0x51515044 字节序反,mesh 从未加载成功)

typedef struct {
    uint16_t tex_idx, blend, alpha, vc, tc;
    uint32_t uvs_off, tris_off;
} pdq_att_t;

typedef struct {
    char name[40];
    uint16_t dur_ms, fcount;
    uint8_t loop;
    uint32_t frames_off;
} pdq_anim_t;

struct pdq_model {
    int tex_count;
    uint8_t **tex; int *tex_w, *tex_h;
    pdq_att_t *atts; int att_count;
    int16_t *uvs; uint16_t *tris;
    pdq_anim_t *anims; int anim_count;
    uint8_t *frames_blob;
    int16_t *verts_blob;
    uint8_t *mem;   // 文件镜像(整体释放)
};

static inline uint16_t pdq_rgba565(uint32_t c) {
    /* 2026-09-17:c 为 RGBA 字节流小端读出 = A<<24|B<<16|G<<8|R。
       旧实现按 RGB 布局解析,R/B 错位 → 人物变蓝 */
    uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t pdq_ablend(uint32_t s, uint16_t d) {
    uint32_t sa = (s >> 24) & 0xFF;
    if (sa >= 255) return pdq_rgba565(s);
    uint32_t sr = s & 0xFF, sg = (s >> 8) & 0xFF, sb = (s >> 16) & 0xFF;
    uint32_t dr = ((d >> 11) & 0x1F) * 255 / 31;
    uint32_t dg = ((d >> 5) & 0x3F) * 255 / 63;
    uint32_t db = (d & 0x1F) * 255 / 31;
    uint32_t r = (sr * sa + dr * (255 - sa)) / 255;
    uint32_t g = (sg * sa + dg * (255 - sa)) / 255;
    uint32_t b = (sb * sa + db * (255 - sa)) / 255;
    return (uint16_t)(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
}

static uint16_t pdq_rd16(uint8_t *p, uint32_t *off) {
    uint16_t v; memcpy(&v, p + *off, 2); *off += 2; return v;
}

static uint32_t pdq_rd32(uint8_t *p, uint32_t *off) {
    uint32_t v; memcpy(&v, p + *off, 4); *off += 4; return v;
}

static int pdq_find_anim(PdqModel *m, const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < m->anim_count; i++)
        if (strcmp(m->anims[i].name, name) == 0) return i;
    return -1;
}

PdqModel *pdq_load(const char *dir) {
    char path[300];
    snprintf(path, sizeof(path), "%s/mesh.ppdq", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 16) { fclose(f); return NULL; }
    uint8_t *mem = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!mem) { ESP_LOGE(TAG, "alloc %ld B failed", len); fclose(f); return NULL; }
    if (fread(mem, 1, len, f) != (size_t)len) { fclose(f); heap_caps_free(mem); return NULL; }
    fclose(f);

    uint32_t off = 0;
    uint32_t magic = pdq_rd32(mem, &off);
    if (magic != PDQ_MAGIC) {
        ESP_LOGE(TAG, "bad magic(0x%08lx) @ %s → 删除坏文件,等待自动重下",
                 (unsigned long)magic, path);
        heap_caps_free(mem);
        remove(path);   // 2026-09-17:坏文件删除,后台修复任务会重新下载
        return NULL;
    }
    uint16_t ver = (uint16_t)pdq_rd16(mem, &off);
    (void)ver;
    uint16_t cw = (uint16_t)pdq_rd16(mem, &off), ch = (uint16_t)pdq_rd16(mem, &off);
    uint16_t tn = (uint16_t)pdq_rd16(mem, &off);
    uint16_t an = (uint16_t)pdq_rd16(mem, &off);
    uint16_t ann = (uint16_t)pdq_rd16(mem, &off);
    ESP_LOGI(TAG, "load %s: %dx%d tex=%d att=%d anim=%d", path, cw, ch, tn, an, ann);

    PdqModel *m = (PdqModel *)calloc(1, sizeof(PdqModel));
    m->mem = mem;
    m->tex_count = tn;
    m->tex = (uint8_t **)calloc(tn, sizeof(uint8_t *));
    m->tex_w = (int *)calloc(tn, sizeof(int));
    m->tex_h = (int *)calloc(tn, sizeof(int));

    char namebuf[128];
    for (int i = 0; i < tn; i++) {
        uint16_t w = (uint16_t)pdq_rd16(mem, &off);
        uint16_t h = (uint16_t)pdq_rd16(mem, &off);
        uint8_t nl = mem[off++];
        memcpy(namebuf, mem + off, nl); namebuf[nl] = 0; off += nl;
        m->tex_w[i] = w; m->tex_h[i] = h;
        char rp[340];
        snprintf(rp, sizeof(rp), "%s/%s", dir, namebuf);
        FILE *rf = fopen(rp, "rb");
        if (rf) {
            m->tex[i] = (uint8_t *)heap_caps_malloc((size_t)w * h * 4, MALLOC_CAP_SPIRAM);
            if (m->tex[i] && fread(m->tex[i], 1, (size_t)w * h * 4, rf) != (size_t)w * h * 4) {
                heap_caps_free(m->tex[i]); m->tex[i] = NULL;
            }
            fclose(rf);
        }
        if (!m->tex[i]) ESP_LOGW(TAG, "tex %s 缺失", namebuf);
    }

    m->att_count = an;
    m->atts = (pdq_att_t *)calloc(an, sizeof(pdq_att_t));
    for (int i = 0; i < an; i++) {
        m->atts[i].tex_idx = (uint16_t)pdq_rd16(mem, &off);
        m->atts[i].blend = mem[off++];
        m->atts[i].alpha = mem[off++];
        m->atts[i].vc = (uint16_t)pdq_rd16(mem, &off);
        m->atts[i].tc = (uint16_t)pdq_rd16(mem, &off);
        m->atts[i].uvs_off = pdq_rd32(mem, &off);
        m->atts[i].tris_off = pdq_rd32(mem, &off);
    }

    m->anim_count = ann;
    m->anims = (pdq_anim_t *)calloc(ann, sizeof(pdq_anim_t));
    for (int i = 0; i < ann; i++) {
        uint8_t nl = mem[off++];
        memcpy(m->anims[i].name, mem + off, nl < 39 ? nl : 39);
        m->anims[i].name[nl < 39 ? nl : 39] = 0;
        off += nl;
        m->anims[i].dur_ms = (uint16_t)pdq_rd16(mem, &off);
        m->anims[i].loop = mem[off++];
        m->anims[i].fcount = (uint16_t)pdq_rd16(mem, &off);
        m->anims[i].frames_off = pdq_rd32(mem, &off);
    }

    /* 数据块指针(uvs/tris 在帧数据之前;由编译器布局:uvs_blob+tris_blob+frames_blob+verts_blob) */
    m->uvs = (int16_t *)(mem + off);
    /* tris 紧随 uvs:计算 uvs 总长(所有 attachment uvs 之和)——编译器顺序保证
       uvs 与 tris 紧邻,这里按 attachment 表反推 */
    uint32_t uvs_total = 0, tris_total = 0;
    for (int i = 0; i < an; i++) {
        uvs_total += (uint32_t)m->atts[i].vc * 2;
        /* 2026-09-17:tc=三角形数,tris 索引元素数 = tc×3(每三角形 3 个 u16 索引);
           旧代码只加 tc → tris 段长度少 3 倍 → frames/verts 定位错位野指针崩溃 */
        tris_total += (uint32_t)m->atts[i].tc * 3;
    }
    m->tris = (uint16_t *)(m->uvs + uvs_total);
    m->frames_blob = mem + off + uvs_total * 2 + tris_total * 2;
    /* verts_blob 长度 = 所有动画所有帧所有 draw 顶点之和——编译时未存总长,由末尾反推:
       帧数据段起始已知,末尾=文件尾;verts 起点需解析帧……简化:编译器在帧段末尾记录了
       verts 起始偏移吗?没有。改为:在 header 后追加 uvs/tris/frames 段长度?
       为兼容已编译产物,用扫描法定位:frames_blob 开始解析到第一个非法 draw 数为止不可靠。
       → 直接利用:verts_blob = frames_blob + (文件尾 - frames_blob) 中按结构解析;
       这里用简单方案:逐动画逐帧扫描 frames_blob 计算其总长,verts_blob 紧随其后。 */
    {
        uint8_t *p = m->frames_blob;
        uint8_t *end = mem + len;
        for (int i = 0; i < ann && p + 6 <= end; i++) {
            uint32_t fo = m->anims[i].frames_off;
            p = m->frames_blob + fo;
            for (int j = 0; j < m->anims[i].fcount && p + 6 <= end; j++) {
                uint16_t dn; memcpy(&dn, p, 2); p += 2;
                p += 4;                       // items_off 跳过
                p += (uint32_t)dn * 6;        // 每 item = att_idx(2)+verts_off(4)
            }
        }
        m->verts_blob = (int16_t *)p;
    }
    return m;
}

void pdq_free(PdqModel *m) {
    if (!m) return;
    for (int i = 0; i < m->tex_count; i++)
        if (m->tex[i]) heap_caps_free(m->tex[i]);
    free(m->tex); free(m->tex_w); free(m->tex_h);
    free(m->atts); free(m->anims);
    if (m->mem) heap_caps_free(m->mem);
    free(m);
}

int pdq_has_anim(PdqModel *m, const char *anim) {
    return m && pdq_find_anim(m, anim) >= 0;
}

int pdq_anim_is_loop(PdqModel *m, const char *anim) {
    int i = m ? pdq_find_anim(m, anim) : -1;
    return i >= 0 ? m->anims[i].loop : 0;
}

int pdq_anim_count(PdqModel *m) {
    return m ? m->anim_count : 0;
}

const char *pdq_anim_name(PdqModel *m, int i) {
    if (!m || i < 0 || i >= m->anim_count) return NULL;
    return m->anims[i].name;
}

static inline int min3i(int a, int b, int c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
static inline int max3i(int a, int b, int c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }

void pdq_render(PdqModel *m, uint16_t *fb, int fw, int fh, const char *anim, uint32_t t_ms, bool force_loop) {
    if (!m || !fb) return;
    int ai = pdq_find_anim(m, anim);
    if (ai < 0) {
        ai = pdq_find_anim(m, "Relax");
        if (ai < 0 && m->anim_count > 0) ai = 0;
        if (ai < 0) return;
    }
    pdq_anim_t *an = &m->anims[ai];
    /* force_loop 由调用方(pd_set_anim 兜底)传入,与本地空帧兜底合并 */
    if (an->fcount == 0) {
        /* 2026-09-27:目标动画空帧(变奏/背面/基建等形态的 Idle 导出为 0 帧,
           pd_render 曾在此直接 return → 画面永久静止)——
           兜底:换第一个有帧的动画(循环动画优先) */
        int fb = -1;
        for (int i = 0; i < m->anim_count; i++) {
            if (m->anims[i].fcount > 0 && m->anims[i].loop) { fb = i; break; }
        }
        if (fb < 0) {
            for (int i = 0; i < m->anim_count; i++) {
                if (m->anims[i].fcount > 0) { fb = i; break; }
            }
        }
        if (fb < 0) return;
        an = &m->anims[fb];
        if (!an->loop) force_loop = true;   /* 2026-09-27:空帧兜底选中非循环 → 强制循环 */
    }
    int frame = (int)(t_ms / (an->dur_ms / an->fcount));
    if ((an->loop || force_loop) && an->fcount > 0) frame %= an->fcount;   /* 2026-09-26:循环动画取模——
       曾 frame 无限增长(482/80),帧定位越界读顶点数据当帧头 → 画面静止 */
    if (!an->loop && !force_loop && frame >= an->fcount) frame = an->fcount - 1;
    if (frame < 0) frame = 0;
    /* 2026-09-26 诊断:动画帧号每 2s 打印(定位"人物静态"——帧号增长=数据在变)。
       2026-09-27:t_ms 是相对时间(每次切动画重置),曾用 t_ms 计时 → 切换后
       t_ms < s_dbg_last 永远打不出;改 m->last_ms(绝对时间) */
    static uint32_t s_dbg_last = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_dbg_last >= 2000) {
        s_dbg_last = now_ms;
        ESP_LOGI("PDQ", "anim=%s t=%u frame=%d/%u dur=%u loop=%d force=%d",
                 an->name, (unsigned)now_ms, frame, an->fcount, an->dur_ms, an->loop, force_loop);
    }

    uint8_t *p = m->frames_blob + an->frames_off;
    for (int j = 0; j < frame && p + 6 <= (uint8_t *)m->verts_blob; j++) {
        uint16_t dn; memcpy(&dn, p, 2);
        p += 6 + (uint32_t)dn * 6;
    }
    uint16_t dn; uint32_t items_off;
    if (p + 6 > (uint8_t *)m->verts_blob) return;
    memcpy(&dn, p, 2); memcpy(&items_off, p + 2, 4);
    uint8_t *items = m->frames_blob + items_off;

    for (int k = 0; k < dn; k++) {
        uint16_t att_i;
        uint32_t vo;   /* 顶点偏移 4 字节(与 compile_ppdq 打包格式一致) */
        memcpy(&att_i, items + k * 6, 2);
        memcpy(&vo, items + k * 6 + 2, 4);
        if (att_i >= (uint16_t)m->att_count) continue;
        pdq_att_t *a = &m->atts[att_i];
        int ti = a->tex_idx;
        if (ti < 0 || ti >= m->tex_count || !m->tex[ti]) continue;
        uint8_t *tex = m->tex[ti];
        int tw = m->tex_w[ti], th = m->tex_h[ti];
        /* 2026-09-17:uvs_off/tris_off/verts_off 是编译器的字节偏移,
           int16/uint16 指针直接加会把偏移放大 2 倍 → 野指针崩溃 */
        int16_t *uvs = (int16_t *)((uint8_t *)m->uvs + a->uvs_off);
        uint16_t *tris = (uint16_t *)((uint8_t *)m->tris + a->tris_off);
        int16_t *verts = (int16_t *)((uint8_t *)m->verts_blob + vo);
        uint8_t ga = a->alpha;
        int additive = a->blend == 1;

        for (int t = 0; t + 2 < a->tc * 3; t += 3) {   /* 2026-09-17:tc×3 个索引 */
            int i0 = tris[t], i1 = tris[t + 1], i2 = tris[t + 2];
            if (i0 >= a->vc || i1 >= a->vc || i2 >= a->vc) continue;
            /* q13.3 解码 */
            float x0 = verts[i0 * 2] * 0.125f, y0 = verts[i0 * 2 + 1] * 0.125f;
            float x1 = verts[i1 * 2] * 0.125f, y1 = verts[i1 * 2 + 1] * 0.125f;
            float x2 = verts[i2 * 2] * 0.125f, y2 = verts[i2 * 2 + 1] * 0.125f;
            float u0 = uvs[i0 * 2], v0 = uvs[i0 * 2 + 1];
            float u1 = uvs[i1 * 2], v1 = uvs[i1 * 2 + 1];
            float u2 = uvs[i2 * 2], v2 = uvs[i2 * 2 + 1];
            int bmnx = min3i((int)x0, (int)x1, (int)x2), bmxx = max3i((int)x0, (int)x1, (int)x2);
            int bmny = min3i((int)y0, (int)y1, (int)y2), bmxy = max3i((int)y0, (int)y1, (int)y2);
            if (bmnx < 0) bmnx = 0;
            if (bmxx >= fw) bmxx = fw - 1;
            if (bmny < 0) bmny = 0;
            if (bmxy >= fh) bmxy = fh - 1;
            if (bmnx >= bmxx || bmny >= bmxy) continue;
            float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (fabsf(area) < 1e-6f) continue;
            float inv = 1.0f / area;
            for (int py = bmny; py <= bmxy; py++) {
                for (int px = bmnx; px <= bmxx; px++) {
                    float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
                    float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
                    float tu = u0 * w0 + u1 * w1 + u2 * w2;
                    float tv = v0 * w0 + v1 * w1 + v2 * w2;
                    int tx = (int)tu, ty = (int)tv;
                    if (tx < 0 || tx >= tw || ty < 0 || ty >= th) continue;
                    uint32_t c = ((uint32_t *)tex)[ty * tw + tx];
                    uint32_t a8 = (c >> 24) & 0xFF;
                    if (!a8) continue;
                    if (ga < 255) a8 = a8 * ga / 255;
                    int pi = py * fw + px;
                    if (additive) {
                        /* additive: out = dst + src*a(饱和) */
                        uint16_t d = fb[pi];
                        uint32_t r = (((d >> 11) & 0x1F) + (((c & 0xFF) * a8) >> 11)) & 0x1F;
                        uint32_t g = (((d >> 5) & 0x3F) + ((((c >> 8) & 0xFF) * a8) >> 10)) & 0x3F;
                        uint32_t b = ((d & 0x1F) + ((((c >> 16) & 0xFF) * a8) >> 11)) & 0x1F;
                        fb[pi] = (uint16_t)((r << 11) | (g << 5) | b);
                    } else if (a8 >= 255) {
                        fb[pi] = pdq_rgba565(c);
                    } else {
                        fb[pi] = pdq_ablend(c, fb[pi]);
                    }
                }
            }
        }
    }
}
