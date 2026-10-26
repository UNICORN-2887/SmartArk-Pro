/* 交友雷达页(2026-10-22):军事风绿色圆形扫描雷达
 * 同心圆 = 距离档(贴脸/近/中/远),白线扫描旋转,点位 = 附近用户
 * (距离=RSSI 真实;方位=uid 伪随机固定角,单天线无 AoA,标注说明)。
 * 点击点位 → 信息卡。数据源 = radar_ble 的 nearby 列表。 */
#include "radar_ui.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "radar_ble.h"
#include "board.h"
#include "display.h"
#include "application.h"

#define TAG "RadarUI"

#define R_SIZE 320          /* 雷达盘像素边长 */
#define R_CX   (R_SIZE / 2)
#define R_CY   (R_SIZE / 2)
#define R_MAX  (R_SIZE / 2 - 12)   /* 最外环半径 */
/* 2026-10-24 右侧用户列表:画布左移(80→10),右侧 x330-470 放列表 */
#define RADAR_CANVAS_X 10
#define RADAR_LIST_X 330
#define RADAR_LIST_W 140

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_canvas = NULL;
static lv_timer_t *s_timer = NULL;
static uint16_t *s_buf = NULL;
static lv_obj_t *s_info = NULL;
static lv_obj_t *s_count_lbl = NULL;
static lv_obj_t *s_me_lbl = NULL;
static lv_obj_t *s_update_lbl = NULL;   /* 更新率指示(2026-10-24) */
static uint32_t s_upd_ms = 0;
static char s_me_shown[32] = {0};   /* 已显示过的 me 文本,变了才刷新标签 */
static bool s_ww_was_on = false;    /* 进入雷达页前唤醒检测是否开启(退出按此恢复) */

static radar_user_t s_users[16];
static int s_user_n = 0;
static float s_scan_deg = 0;
static const lv_font_t *radar_font(void) {
    auto display = Board::GetInstance().GetDisplay();
    return display ? display->GetTextFont() : LV_FONT_DEFAULT;
}

/* ── 像素画线(整数 Bresenham) ── */
static void px(uint16_t *b, int x, int y, uint16_t c) {
    if (x < 0 || x >= R_SIZE || y < 0 || y >= R_SIZE) return;
    b[y * R_SIZE + x] = c;
}
static void line(uint16_t *b, int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(b, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void circle(uint16_t *b, int cx, int cy, int r, uint16_t c) {
    int x = 0, y = r, d = 3 - 2 * r;
    while (y >= x) {
        px(b, cx + x, cy + y, c); px(b, cx - x, cy + y, c);
        px(b, cx + x, cy - y, c); px(b, cx - x, cy - y, c);
        px(b, cx + y, cy + x, c); px(b, cx - y, cy + x, c);
        px(b, cx + y, cy - x, c); px(b, cx - y, cy - x, c);
        x++;
        if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
        else d = d + 4 * x + 6;
    }
}
static void dot(uint16_t *b, int cx, int cy, int r, uint16_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) px(b, cx + x, cy + y, c);
}

/* ── 点位半径:AP 定位距离(dist_m)优先;否则 BLE 距离档 ── */
static int dist_radius(const radar_user_t *u) {
    if (u->dist_m > 0) {
        /* AP 定位距离映射(2026-10-22 测试模式):<=50m 内环 / 100m 半 / 200m 3/4 / 外环 */
        if (u->dist_m <= 50) return R_MAX * 25 / 100;
        if (u->dist_m <= 100) return R_MAX * 50 / 100;
        if (u->dist_m <= 200) return R_MAX * 75 / 100;
        return R_MAX * 92 / 100;
    }
    if (strcmp(u->dist, "贴脸") == 0) return R_MAX * 25 / 100;
    if (strcmp(u->dist, "近") == 0) return R_MAX * 50 / 100;
    if (strcmp(u->dist, "中") == 0) return R_MAX * 75 / 100;
    return R_MAX * 92 / 100;   /* 远 */
}

/* ── 点位方位角(度):WiFi 定位 dir(北=0,顺时针,转屏幕坐标北朝上)优先;
   否则 side 双机测向(-1 偏我左半 / +1 偏队友右半);
   无任何方位数据时固定顶部(-90):不再哈希散布——三边数据时有时无
   (第三台设备掉线)时两种模式切换造成"随机跳"的错觉(2026-10-23) */
static float dot_angle(const radar_user_t *u) {
    if (u->dir >= 0) return (float)(u->dir - 90);
    if (u->side < 0) return -60.0f;
    if (u->side > 0) return 60.0f;
    return -90.0f;   /* 固定顶部:无方位信息,按距离档排同心圆 */
}

/* ── 点位画法优先级:三边定位相对坐标(真实) > AP 定位方位+距离 > 距离档+伪角 ── */
#define REL_SCALE 8.0f   /* 像素/米:雷达半径 148px ≈ 18.5m 满盘 */

static void dot_pos(const radar_user_t *u, int *ox, int *oy) {
    if (u->has_rel) {
        /* 三边定位:我=圆心;2026-10-24 行走朝向:盘面上方=行走方向
           (车头朝上)—— 走路时身体朝向=位移方向,转弯后自动跟 */
        extern float radar_me_heading(void);
        float x = u->rel_x, y = u->rel_y;
        float hd = radar_me_heading();
        if (hd >= 0) {
            float th = hd * 3.1415926f / 180.0f;
            float sn = sinf(th), cs = cosf(th);
            float x2 = x * sn - y * cs;   /* 全局旋转:行走方向 → 屏幕上方 */
            float y2 = x * cs + y * sn;
            x = x2;
            y = y2;
        }
        float sx = R_CX + x * REL_SCALE;
        float sy = R_CY - y * REL_SCALE;
        float dx = sx - R_CX, dy = sy - R_CY;
        float rmax = R_MAX - 10.0f;
        float rr = sqrtf(dx * dx + dy * dy);
        if (rr > rmax) { sx = R_CX + dx * rmax / rr; sy = R_CY + dy * rmax / rr; }
        *ox = (int)sx;
        *oy = (int)sy;
        return;
    }
    int r = dist_radius(u);
    float ang = dot_angle(u) * 3.1415926f / 180.0f;
    *ox = R_CX + (int)(cosf(ang) * r);
    *oy = R_CY + (int)(sinf(ang) * r);
}

/* ── 重绘一帧 ── */
static void radar_redraw(void) {
    if (!s_buf) return;
    uint16_t green = 0x07E0, dimg = 0x0320, white = 0xFFFF, yel = 0xFFE0;
    memset(s_buf, 0, sizeof(uint16_t) * R_SIZE * R_SIZE);

    /* 同心圆(距离档) */
    circle(s_buf, R_CX, R_CY, R_MAX, green);
    circle(s_buf, R_CX, R_CY, R_MAX * 75 / 100, dimg);
    circle(s_buf, R_CX, R_CY, R_MAX * 50 / 100, dimg);
    circle(s_buf, R_CX, R_CY, R_MAX * 25 / 100, dimg);
    /* 十字线 */
    line(s_buf, R_CX - R_MAX, R_CY, R_CX + R_MAX, R_CY, dimg);
    line(s_buf, R_CX, R_CY - R_MAX, R_CX, R_CY + R_MAX, dimg);
    /* 刻度(每 30°) */
    for (int a = 0; a < 360; a += 30) {
        float rad = a * 3.1415926f / 180.0f;
        int x1 = R_CX + (int)(cosf(rad) * (R_MAX - 14));
        int y1 = R_CY + (int)(sinf(rad) * (R_MAX - 14));
        int x2 = R_CX + (int)(cosf(rad) * (R_MAX - 4));
        int y2 = R_CY + (int)(sinf(rad) * (R_MAX - 4));
        line(s_buf, x1, y1, x2, y2, dimg);
    }
    /* 扫描线(白) */
    {
        float rad = s_scan_deg * 3.1415926f / 180.0f;
        int x = R_CX + (int)(cosf(rad) * (R_MAX - 6));
        int y = R_CY + (int)(sinf(rad) * (R_MAX - 6));
        line(s_buf, R_CX, R_CY, x, y, white);
    }
    /* 点位(附近用户):三边定位真实坐标优先,回退距离档+角度 */
    for (int i = 0; i < s_user_n; i++) {
        radar_user_t *u = &s_users[i];
        int x, y;
        dot_pos(u, &x, &y);
        /* 三边定位点=青色;AP 定位点=亮青;靠近/远离颜色优先 */
        uint16_t c2 = yel;
        if (u->has_rel) c2 = 0x07FF;
        if (u->dir >= 0 && !u->has_rel) c2 = 0x07FF;
        if (u->trend > 0) c2 = 0x06E0;      /* 靠近 = 亮绿 */
        else if (u->trend < 0) c2 = 0xF800;  /* 远离 = 红 */
        dot(s_buf, x, y, 5, c2);
        dot(s_buf, x, y, 2, white);
    }

    if (s_canvas && lv_obj_is_valid(s_canvas))
        lv_obj_invalidate(s_canvas);
}

/* 右侧用户列表(2026-10-24:点不中雷达点时从列表点) */
static lv_obj_t *s_list_btns[16];
static lv_obj_t *s_list_lbls[16];
static uint32_t s_list_refresh_ms = 0;

static void radar_df_open(void);   /* 测向模式(定义在文件后部) */

static void radar_list_refresh(void) {
    for (int i = 0; i < 16; i++) {
        if (!s_list_btns[i]) continue;
        if (i < s_user_n) {
            lv_obj_clear_flag(s_list_btns[i], LV_OBJ_FLAG_HIDDEN);
            radar_user_t *u = &s_users[i];
            char txt[64];
            if (u->has_rel) {
                float d = sqrtf(u->rel_x * u->rel_x + u->rel_y * u->rel_y);
                snprintf(txt, sizeof(txt), "%s\n%.1f米", u->name, (double)d);
            } else if (u->dist_m > 0) {
                snprintf(txt, sizeof(txt), "%s\n约%u米", u->name, (unsigned)u->dist_m);
            } else {
                snprintf(txt, sizeof(txt), "%s\n%s", u->name, u->dist);
            }
            lv_label_set_text(s_list_lbls[i], txt);
        } else {
            lv_obj_add_flag(s_list_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void radar_timer_cb(lv_timer_t *t) {
    (void)t;
    s_scan_deg += 6.0f;   /* 30ms × 6° ≈ 200°/s 扫描 */
    if (s_scan_deg >= 360) s_scan_deg -= 360;
    /* 刷新 nearby(周期数据由 radar_ble 任务更新) */
    s_user_n = radar_nearby_list(s_users, 16);
    if (s_count_lbl && lv_obj_is_valid(s_count_lbl))
        lv_label_set_text_fmt(s_count_lbl, "附近 %d 人", s_user_n);
    /* 右侧列表:1s 节流刷新(2026-10-24) */
    if (lv_tick_get() - s_list_refresh_ms > 1000) {
        s_list_refresh_ms = lv_tick_get();
        radar_list_refresh();
    }
    /* 更新率指示(2026-10-24):数据新鲜度 + 朝向角,移动中可直观看是否在更新 */
    if (s_update_lbl && lv_obj_is_valid(s_update_lbl) &&
        lv_tick_get() - s_upd_ms > 500) {
        s_upd_ms = lv_tick_get();
        extern float radar_nearby_age(void);
        extern float radar_me_heading(void);
        float age = radar_nearby_age();
        float hd = radar_me_heading();
        if (hd >= 0)
            lv_label_set_text_fmt(s_update_lbl, "更新 %.1fs 前\n朝向 %u°", (double)age, (unsigned)hd);
        else
            lv_label_set_text_fmt(s_update_lbl, "更新 %.1fs 前\n静止·朝向未知", (double)age);
        lv_obj_set_style_text_color(s_update_lbl,
                                    lv_color_hex(age < 8 ? 0x4dff6a : 0xd0b000), 0);
    }
    /* 当前用户:me 可能在本页打开后才到达(60s 一次 nearby),变了才刷 */
    const char *mn = radar_me_name();
    if (s_me_lbl && lv_obj_is_valid(s_me_lbl) && strcmp(mn, s_me_shown) != 0) {
        snprintf(s_me_shown, sizeof(s_me_shown), "%s", mn);
        lv_label_set_text_fmt(s_me_lbl, "当前用户: %s", mn[0] ? mn : "未绑定");
    }
    radar_redraw();
}

static void radar_show_info(radar_user_t *u) {
    if (s_info && lv_obj_is_valid(s_info)) lv_obj_del(s_info);
    s_info = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_info, radar_font(), 0);
    const char *td = u->trend > 0 ? "正在靠近" : (u->trend < 0 ? "正在远离" : "距离稳定");
    if (u->has_rel) {
        /* 三边定位:相对位置(2026-10-24 行走朝向旋转后,前=行走方向) */
        extern float radar_me_heading(void);
        float x = u->rel_x, y = u->rel_y;
        float hd = radar_me_heading();
        if (hd >= 0) {
            float th = hd * 3.1415926f / 180.0f;
            float sn = sinf(th), cs = cosf(th);
            float x2 = x * sn - y * cs;
            float y2 = x * cs + y * sn;
            x = x2;
            y = y2;
        }
        float d = sqrtf(x * x + y * y);
        const char *fb = y >= 0.8f ? "前" : (y <= -0.8f ? "后" : "");
        const char *lr = x >= 0.8f ? "右" : (x <= -0.8f ? "左" : "");
        lv_label_set_text_fmt(s_info, "昵称: %s\n相对位置: %s%s 约 %.1f 米\n%s",
                              u->name, fb, lr, (double)d, td);
    } else if (u->dist_m > 0 && u->dir >= 0) {
        /* WiFi 定位:真实方位 + 距离(米) */
        static const char *dirs[8] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
        const char *dn = dirs[((u->dir + 22) % 360) / 45];
        lv_label_set_text_fmt(s_info, "昵称: %s\n方位: %s %u°\n距离: 约 %u 米\n%s",
                              u->name, dn, (unsigned)u->dir, (unsigned)u->dist_m, td);
    } else {
        lv_label_set_text_fmt(s_info, "昵称: %s\n距离: %s (RSSI %d)\n%s",
                              u->name, u->dist, (int)u->rssi, td);
    }
    lv_obj_set_style_bg_color(s_info, lv_color_hex(0x0a1a0a), 0);
    lv_obj_set_style_bg_opa(s_info, LV_OPA_90, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(0x4dff6a), 0);
    lv_obj_set_style_pad_all(s_info, 14, 0);
    lv_obj_set_style_radius(s_info, 8, 0);
    lv_obj_set_style_border_width(s_info, 1, 0);
    lv_obj_set_style_border_color(s_info, lv_color_hex(0x07E0), 0);
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, -30);
}

static int s_last_hit_idx = -1;      /* 重叠点轮换:上次命中的点 */
static uint32_t s_last_hit_ms = 0;

static void radar_page_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        /* 2026-10-24 按下即响应(旧逻辑等 CLICKED 松手才弹,延迟感);
           点空白处删除信息面板 */
        lv_point_t p;
        lv_indev_get_point(lv_indev_get_act(), &p);
        /* 画布在屏幕上的偏移:画布左上 (RADAR_CANVAS_X, 180) */
        int cx = RADAR_CANVAS_X, cy = 180;
        int px2 = p.x - cx, py2 = p.y - cy;
        if (px2 < 0 || py2 < 0 || px2 >= R_SIZE || py2 >= R_SIZE) return;
        /* 命中:收集 24px 内所有点;重叠时轮换(2026-10-24:
           两个点重叠时旧逻辑永远命中 index 小的,点不到第二个人) */
        int cand[16];
        int cand_n = 0;
        for (int i = 0; i < s_user_n && cand_n < 16; i++) {
            int dx, dy;
            dot_pos(&s_users[i], &dx, &dy);
            float d2 = (float)((px2 - dx) * (px2 - dx) + (py2 - dy) * (py2 - dy));
            if (d2 <= 24.0f * 24.0f) cand[cand_n++] = i;
        }
        if (cand_n == 0) {
            if (s_info && lv_obj_is_valid(s_info)) lv_obj_del(s_info);
            s_info = NULL;
            return;
        }
        int pick = cand[0];
        uint32_t now = lv_tick_get();
        if (cand_n > 1 && s_last_hit_idx >= 0 && now - s_last_hit_ms < 3000) {
            /* 3 秒内再次点击同一位置:轮换到下一个重叠点 */
            for (int i = 0; i < cand_n; i++) {
                if (cand[i] == s_last_hit_idx) { pick = cand[(i + 1) % cand_n]; break; }
            }
        }
        s_last_hit_idx = pick;
        s_last_hit_ms = now;
        radar_show_info(&s_users[pick]);
    }
}

int radar_ui_show(void) {
    if (s_page) return 0;   /* 已显示 */
    if (!lvgl_port_lock(pdMS_TO_TICKS(5000))) return -1;
    /* 2026-10-22 雷达页动画+BLE 期间挂起唤醒词 NN(防 task_wdt);
       记录进入前状态:从对话模式进 → 退出恢复;从索引页进(检测本就关)
       → 退出保持关,由 cover 流程统一开。
       2026-10-23 修复:仅当检测运行中才挂起——未初始化时 Stop 会阻塞 */
    s_ww_was_on = Application::GetInstance().GetAudioService().IsWakeWordRunning();
    if (s_ww_was_on)
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);

    s_page = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_page, 480, 800);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);

    /* 左上角:当前用户(2026-10-22 便于辨认设备身份;timer 里随 me 到达自动刷新) */
    {
        s_me_lbl = lv_label_create(s_page);
        s_me_shown[0] = 0;
        lv_label_set_text_fmt(s_me_lbl, "当前用户: %s", radar_me_name()[0] ? radar_me_name() : "未绑定");
        lv_obj_set_style_text_font(s_me_lbl, radar_font(), 0);
        lv_obj_set_style_text_color(s_me_lbl, lv_color_hex(0x4dff6a), 0);
        lv_obj_align(s_me_lbl, LV_ALIGN_TOP_LEFT, 12, 12);
    }

    /* 标题 + 计数 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "交友雷达");
    lv_obj_set_style_text_font(title, radar_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x07E0), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    s_count_lbl = lv_label_create(s_page);
    lv_label_set_text(s_count_lbl, "附近 0 人");
    lv_obj_set_style_text_font(s_count_lbl, radar_font(), 0);
    lv_obj_set_style_text_color(s_count_lbl, lv_color_hex(0x4dff6a), 0);
    lv_obj_align(s_count_lbl, LV_ALIGN_TOP_MID, 0, 60);

    /* 更新率 + 朝向(2026-10-24,右上角) */
    s_update_lbl = lv_label_create(s_page);
    lv_label_set_text(s_update_lbl, "更新 --s 前\n静止·朝向未知");
    lv_obj_set_style_text_font(s_update_lbl, radar_font(), 0);
    lv_obj_set_style_text_color(s_update_lbl, lv_color_hex(0x4dff6a), 0);
    lv_obj_set_style_text_align(s_update_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_update_lbl, LV_ALIGN_TOP_RIGHT, -8, 46);   /* 避开电量胶囊 */

    /* 画布 320×320 左侧(2026-10-24 左移腾出右侧列表;左上 10,180) */
    s_buf = (uint16_t *)heap_caps_malloc(R_SIZE * R_SIZE * 2, MALLOC_CAP_SPIRAM);
    if (!s_buf) { lv_obj_del(s_page); s_page = NULL; lvgl_port_unlock(); return -1; }
    s_canvas = lv_canvas_create(s_page);
    lv_canvas_set_buffer(s_canvas, s_buf, R_SIZE, R_SIZE, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, RADAR_CANVAS_X, 180);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);   /* canvas 默认不接收点击,必须显式加 */
    lv_obj_add_event_cb(s_canvas, radar_page_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_canvas, radar_page_event, LV_EVENT_PRESSED, NULL);

    /* 右侧用户列表(2026-10-24:点不中画面中的点时,点列表查看信息) */
    for (int i = 0; i < 16; i++) {
        lv_obj_t *b = lv_btn_create(s_page);
        lv_obj_set_size(b, RADAR_LIST_W, 42);
        lv_obj_set_pos(b, RADAR_LIST_X, 180 + i * 46);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x0a1a0a), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_font(l, radar_font(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x4dff6a), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
            if (i < s_user_n) radar_show_info(&s_users[i]);
        }, LV_EVENT_CLICKED, NULL);
        s_list_btns[i] = b;
        s_list_lbls[i] = l;
    }

    /* 测向按钮(2026-10-24:三向采样找对方方位,内置校准教程) */
    {
        lv_obj_t *dbtn = lv_btn_create(s_page);
        lv_obj_set_size(dbtn, 130, 40);
        lv_obj_align(dbtn, LV_ALIGN_BOTTOM_LEFT, 40, -20);
        lv_obj_set_style_bg_color(dbtn, lv_color_hex(0x0a3a0a), 0);
        lv_obj_t *dl = lv_label_create(dbtn);
        lv_label_set_text(dl, "测向");
        lv_obj_set_style_text_font(dl, radar_font(), 0);
        lv_obj_set_style_text_color(dl, lv_color_hex(0x4dff6a), 0);
        lv_obj_center(dl);
        lv_obj_add_event_cb(dbtn, [](lv_event_t *e) {
            (void)e;
            radar_df_open();
        }, LV_EVENT_CLICKED, NULL);
    }

    /* 翻转左右按钮(2026-10-24:镜像歧义一键纠正) */
    {
        lv_obj_t *fbtn = lv_btn_create(s_page);
        lv_obj_set_size(fbtn, 130, 40);
        lv_obj_align(fbtn, LV_ALIGN_BOTTOM_RIGHT, -40, -20);
        lv_obj_set_style_bg_color(fbtn, lv_color_hex(0x0a3a0a), 0);
        lv_obj_t *fl = lv_label_create(fbtn);
        lv_label_set_text(fl, "翻转左右");
        lv_obj_set_style_text_font(fl, radar_font(), 0);
        lv_obj_set_style_text_color(fl, lv_color_hex(0x4dff6a), 0);
        lv_obj_center(fl);
        lv_obj_add_event_cb(fbtn, [](lv_event_t *e) {
            (void)e;
            extern void radar_flip_request(void);
            radar_flip_request();
        }, LV_EVENT_CLICKED, NULL);
    }

    /* 退出按钮 */
    lv_obj_t *btn = lv_btn_create(s_page);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0a3a0a), 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "退出");
    lv_obj_set_style_text_font(bl, radar_font(), 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x4dff6a), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        radar_ui_hide();
    }, LV_EVENT_CLICKED, NULL);

    /* 方位说明 */
    lv_obj_t *hint = lv_label_create(s_page);
    lv_obj_set_style_text_font(hint, radar_font(), 0);
    lv_label_set_text(hint, "距离精确 · 方位需 3 台设备(三边定位)\n点绿点查看对方信息");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x2a7a3a), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -70);

    s_user_n = radar_nearby_list(s_users, 16);
    radar_redraw();
    s_timer = lv_timer_create(radar_timer_cb, 30, NULL);

    lvgl_port_unlock();
    return 0;
}

/* ════════════ 测向模式(2026-10-24:三向采样,人体遮挡差分) ════════════
 * 原理:人体遮挡 2.4GHz → 面对对方 RSSI 最强。前/右/后三向采样:
 * φ = atan2(2·r右 - r前 - r后, r前 - r后),完整 360° 方位。
 * 对方无需配合(移动只影响公共基线,差分消掉)。 */
extern int radar_esnow_set_fast(int on);

static lv_obj_t *s_df_panel = NULL;
static lv_obj_t *s_df_lbl = NULL;
static lv_obj_t *s_df_btn = NULL;
static lv_obj_t *s_df_btn_lbl = NULL;
static lv_timer_t *s_df_timer = NULL;
static int s_df_step = 0;          /* 0=未开始;1..3=采样步骤 */
static int s_df_target = 0;
static int8_t s_df_rssi[3] = {0};
static int s_df_vals[40];
static int s_df_vals_n = 0;
static int s_df_ticks = 0;
static int s_df_done = 0;

static int df_target_uid(int8_t *out_rssi) {
    extern int radar_esnow_peers(uint32_t *uids, int8_t *rssis, int max);
    uint32_t uids[8];
    int8_t rs[8];
    int n = radar_esnow_peers(uids, rs, 8);
    if (n <= 0) return 0;
    int best = 0;
    for (int i = 1; i < n; i++) if (rs[i] > rs[best]) best = i;
    if (out_rssi) *out_rssi = rs[best];
    return (int)uids[best];
}

static const char *df_target_name(int uid) {
    for (int i = 0; i < s_user_n; i++)
        if (s_users[i].uid == (uint32_t)uid) return s_users[i].name;
    return "";
}

static void df_cleanup(void) {
    radar_esnow_set_fast(0);
    if (s_df_timer) { lv_timer_del(s_df_timer); s_df_timer = NULL; }
    if (s_df_panel && lv_obj_is_valid(s_df_panel)) lv_obj_del(s_df_panel);
    s_df_panel = NULL;
    s_df_lbl = NULL;
    s_df_btn = NULL;
    s_df_btn_lbl = NULL;
    s_df_step = 0;
    s_df_done = 0;
}

static void df_show_result(void) {
    radar_esnow_set_fast(0);
    s_df_done = 1;
    int r1 = s_df_rssi[0], r2 = s_df_rssi[1], r3 = s_df_rssi[2];
    float y = 2.0f * r2 - r1 - r3;
    float x = (float)(r1 - r3);
    float deg = atan2f(y, x) * 180.0f / 3.1415926f;
    float ad = deg;
    if (ad < 0) ad += 360;
    char degtxt[32];
    if (ad > 180) snprintf(degtxt, sizeof(degtxt), "偏左 %.0f°", 360.0f - ad);
    else snprintf(degtxt, sizeof(degtxt), "偏右 %.0f°", ad);
    int8_t rr;
    df_target_uid(&rr);
    const char *dstr = rr >= -50 ? "贴脸" : (rr >= -65 ? "近" : (rr >= -80 ? "中" : "远"));
    const char *nm = df_target_name(s_df_target);
    lv_label_set_text_fmt(s_df_lbl,
                          "%s 在你%s\n(相对你第一步站姿的前方)\n距离: %s (RSSI %d)",
                          nm[0] ? nm : "对方", degtxt, dstr, (int)rr);
    lv_label_set_text(s_df_btn_lbl, "完成");
}

static void df_sample_tick(lv_timer_t *t) {
    (void)t;
    int8_t r;
    int uid = df_target_uid(&r);
    if (uid == s_df_target && s_df_vals_n < 40)
        s_df_vals[s_df_vals_n++] = (int)r;
    if (++s_df_ticks >= 30) {   /* 100ms × 30 = 3 秒 */
        if (s_df_timer) { lv_timer_del(s_df_timer); s_df_timer = NULL; }
        /* 中位数(简单排序,40 元素量级无碍) */
        for (int i = 0; i < s_df_vals_n; i++)
            for (int j = i + 1; j < s_df_vals_n; j++)
                if (s_df_vals[j] < s_df_vals[i]) {
                    int x2 = s_df_vals[j]; s_df_vals[j] = s_df_vals[i]; s_df_vals[i] = x2;
                }
        int med = s_df_vals_n > 0 ? s_df_vals[s_df_vals_n / 2] : -100;
        s_df_rssi[s_df_step - 1] = (int8_t)med;
        ESP_LOGI(TAG, "测向第 %d 步: %d 样本,中位数 %d", s_df_step, s_df_vals_n, med);
        if (s_df_step < 3) {
            lv_label_set_text_fmt(s_df_lbl, "第 %d/3 步完成\n左转 90° 站好\n点『下一步』", s_df_step);
        } else {
            df_show_result();
        }
    }
}

static void df_next_clicked(void) {
    if (s_df_done) { df_cleanup(); return; }
    if (s_df_step < 3) {
        s_df_step++;
        s_df_vals_n = 0;
        s_df_ticks = 0;
        lv_label_set_text(s_df_btn_lbl, "下一步");
        static const char *dirs[3] = {"前方", "右方(左转 90° 后)", "后方(再左转 90° 后)"};
        lv_label_set_text_fmt(s_df_lbl, "第 %d/3 步\n面朝%s站好\n正在采样 3 秒…",
                              s_df_step, dirs[s_df_step - 1]);
        s_df_timer = lv_timer_create(df_sample_tick, 100, NULL);
    }
}

static void df_start(void) {
    s_df_target = df_target_uid(NULL);
    if (s_df_target == 0) {
        lv_label_set_text(s_df_lbl, "附近没有检测到对方\n(需两台设备都在线)\n点『取消』关闭");
        lv_label_set_text(s_df_btn_lbl, "取消");
        return;
    }
    radar_esnow_set_fast(1);
    s_df_step = 0;
    s_df_done = 0;
    df_next_clicked();
}

static void radar_df_open(void) {
    if (s_df_panel && lv_obj_is_valid(s_df_panel)) return;
    s_df_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_df_panel, 440, 300);
    lv_obj_align(s_df_panel, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_bg_color(s_df_panel, lv_color_hex(0x0a1a0a), 0);
    lv_obj_set_style_border_width(s_df_panel, 2, 0);
    lv_obj_set_style_border_color(s_df_panel, lv_color_hex(0x07E0), 0);
    s_df_lbl = lv_label_create(s_df_panel);
    /* 校准教程:每次进入都显示(2026-10-24 用户要求设备内置教程) */
    lv_label_set_text(s_df_lbl,
                      "校准教程\n\n"
                      "1. 站在原地,面朝任意方向\n"
                      "2. 保持 3 秒,按提示左转 90°\n"
                      "3. 再左转 90° 一次\n\n"
                      "对方移动无影响,你不用动\n"
                      "完成后显示对方方位");
    lv_obj_set_style_text_font(s_df_lbl, radar_font(), 0);
    lv_obj_set_style_text_color(s_df_lbl, lv_color_hex(0x4dff6a), 0);
    lv_obj_set_style_text_align(s_df_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_df_lbl, LV_ALIGN_TOP_MID, 0, 14);
    s_df_btn = lv_btn_create(s_df_panel);
    lv_obj_set_size(s_df_btn, 150, 44);
    lv_obj_align(s_df_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(s_df_btn, lv_color_hex(0x0a3a0a), 0);
    s_df_btn_lbl = lv_label_create(s_df_btn);
    lv_label_set_text(s_df_btn_lbl, "开始测向");
    lv_obj_set_style_text_font(s_df_btn_lbl, radar_font(), 0);
    lv_obj_set_style_text_color(s_df_btn_lbl, lv_color_hex(0x4dff6a), 0);
    lv_obj_center(s_df_btn_lbl);
    lv_obj_add_event_cb(s_df_btn, [](lv_event_t *e) {
        (void)e;
        if (s_df_step == 0 && !s_df_done) df_start();
        else df_next_clicked();
    }, LV_EVENT_CLICKED, NULL);
    /* 右上角关闭 */
    lv_obj_t *x = lv_btn_create(s_df_panel);
    lv_obj_set_size(x, 36, 36);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, "X");
    lv_obj_set_style_text_color(xl, lv_color_hex(0x4dff6a), 0);
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, [](lv_event_t *e) { (void)e; df_cleanup(); }, LV_EVENT_CLICKED, NULL);
}

void radar_ui_hide(void) {
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) return;
    df_cleanup();   /* 测向模式残留清理(panel 在 layer_top,fast 心跳复位) */
    /* 仅当进入雷达页前检测是开的(对话模式)才恢复;索引页进入时保持关 */
    if (s_ww_was_on) {
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
        s_ww_was_on = false;
    }
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_info && lv_obj_is_valid(s_info)) lv_obj_del(s_info);
    s_info = NULL;
    if (s_page && lv_obj_is_valid(s_page)) lv_obj_del(s_page);
    s_page = NULL;
    s_canvas = NULL;
    s_count_lbl = NULL;
    s_me_lbl = NULL;
    s_update_lbl = NULL;
    for (int i = 0; i < 16; i++) { s_list_btns[i] = NULL; s_list_lbls[i] = NULL; }
    if (s_buf) { free(s_buf); s_buf = NULL; }
    lvgl_port_unlock();
}
