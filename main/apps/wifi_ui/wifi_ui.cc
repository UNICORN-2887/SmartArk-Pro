/* 屏幕 WiFi 列表页(2026-09-20):连接失败/无已存时弹出,手机式交互。
 * 页面状态机:列表 → 密码 → 连接中 → 成功/失败(重试回密码,返回回列表)。
 * 扫描/连接放独立任务(esp_wifi 阻塞调用不进 LVGL 任务);UI 操作持 LVGL 锁。
 * 已存 SSID 置顶标 ✓;信号三档 ●●●;加密/开放文字标注(字体不含 emoji)。 */
#include "wifi_ui.h"

#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "wifi_station.h"
#include "ssid_manager.h"
#include "board.h"
#include "display/display.h"   /* Display 完整定义(GetTextFont) */

#define TAG "WifiUi"

static const lv_font_t *s_font = NULL;
static const lv_font_t *efont(void) { return s_font ? s_font : LV_FONT_DEFAULT; }

static lv_obj_t *s_page = NULL;          // 全屏页面(lv_layer_top)
static lv_obj_t *s_list = NULL;          // 列表容器
static std::vector<WifiApRecord> s_aps;  // 扫描结果(任务与 UI 间经锁)
static char s_sel_ssid[40];              // 当前选中 SSID
static char s_conn_pwd[64];              // 连接用密码
static bool s_sel_open = false;
static bool s_connecting = false;

static char s_pwd_buf[64];               // 密码输入缓冲
static int s_pwd_len = 0;
static lv_obj_t *s_pwd_ta = NULL;
static lv_obj_t *s_kb = NULL;             // 键盘容器(kb_rebuild 用)
static bool s_shift = false;

static EventGroupHandle_t s_done_evt = NULL;
#define BIT_CONNECTED BIT0
#define BIT_SKIP      BIT1

static void show_pwd_page(void);
static void show_connecting(void);
static void show_error(void);
static void show_list_page(void);
static void start_connect(const char *pwd);
static void pwd_append(char c);
static void pwd_backspace(void);
static void kb_rebuild(void);

/* ── 工具 ── */
static void set_font(lv_obj_t *o) { lv_obj_set_style_text_font(o, efont(), 0); }

static std::string sig_bars(int8_t rssi) {
    if (rssi >= -55) return "●●●";   /* ●●● 强 */
    if (rssi >= -70) return "●●○";   /* ●●○ 中 */
    return "●○○";                    /* ●○○ 弱 */
}

static bool ssid_saved(const char *ssid) {
    for (auto &it : SsidManager::GetInstance().GetSsidList())
        if (it.ssid == ssid) return true;
    return false;
}

static std::string saved_pwd(const char *ssid) {
    for (auto &it : SsidManager::GetInstance().GetSsidList())
        if (it.ssid == ssid) return it.password;
    return "";
}

/* ── 连接(独立任务;结果经事件组) ── */
static void conn_task(void *arg) {
    auto &w = WifiStation::GetInstance();
    w.ConnectTo(s_sel_ssid, s_conn_pwd);
    if (w.WaitForConnected(20000)) {
        xEventGroupSetBits(s_done_evt, BIT_CONNECTED);
    } else if (lvgl_port_lock(pdMS_TO_TICKS(3000))) {
        s_connecting = false;
        /* 页面可能已被跳过路径删除(s_page=NULL),不再绘制错误页 */
        if (s_page && lv_obj_is_valid(s_page)) show_error();
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void start_connect(const char *pwd) {
    snprintf(s_conn_pwd, sizeof(s_conn_pwd), "%s", pwd ? pwd : "");
    show_connecting();
    s_connecting = true;
    xTaskCreate(conn_task, "wifi_conn", 8192, NULL, 3, NULL);
}

/* ── 扫描(独立任务) ── */
static bool s_scanning = false;   /* 防重入:避免连点「刷新」并发两个 ScanOnce */

static void scan_task(void *arg) {
    auto &ws = WifiStation::GetInstance();
    std::vector<WifiApRecord> aps;   /* 局部缓冲,ScanOnce 期间不碰共享 s_aps */
    bool ok = ws.ScanOnce(aps);
    /* 2026-09-26 首次扫描失败自动重试一次(开机自动连接失败后 LP 扫描引擎
       恢复需数秒,首轮常空;免去用户手动点「刷新」第二次) */
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        aps.clear();
        ok = ws.ScanOnce(aps);
    }
    if (lvgl_port_lock(pdMS_TO_TICKS(3000))) {
        s_aps.swap(aps);   /* 锁内交换,与 UI 侧读写互斥 */
        if (s_list && lv_obj_is_valid(s_list)) {
            lv_obj_clean(s_list);
            if (!ok) {
                lv_obj_t *l = lv_label_create(s_list);
                lv_label_set_text(l, "未发现 WiFi(点「刷新」重试)");
                set_font(l);
                lv_obj_set_style_text_color(l, lv_color_hex(0x888888), 0);
                lv_obj_center(l);
            } else {
                /* 已存置顶,其余按信号降序 */
                std::sort(s_aps.begin(), s_aps.end(),
                          [](const WifiApRecord &a, const WifiApRecord &b) {
                    bool sa = ssid_saved(a.ssid.c_str()), sb = ssid_saved(b.ssid.c_str());
                    if (sa != sb) return sa;
                    return a.rssi > b.rssi;
                });
                for (size_t i = 0; i < s_aps.size(); i++) {
                    const WifiApRecord &ap = s_aps[i];
                    std::string text =
                        (ssid_saved(ap.ssid.c_str()) ? "√ " : "  ") + ap.ssid +
                        "  " + sig_bars(ap.rssi) +
                        (ap.authmode == WIFI_AUTH_OPEN ? "  开放"   /* 开放 */
                                                       : "  加密");  /* 加密 */
                    lv_obj_t *row = lv_btn_create(s_list);
                    lv_obj_set_size(row, 448, 46);
                    lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2e37), 0);
                    lv_obj_set_style_radius(row, 8, 0);
                    lv_obj_t *l = lv_label_create(row);
                    lv_label_set_text(l, text.c_str());
                    lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
                    lv_obj_set_width(l, 430);
                    set_font(l);
                    lv_obj_set_style_text_color(l, lv_color_hex(0xDDDDDD), 0);
                    lv_obj_center(l);
                    lv_obj_add_event_cb(row, [](lv_event_t *e) {
                        int idx = (int)(intptr_t)lv_event_get_user_data(e);
                        if (idx < 0 || idx >= (int)s_aps.size()) return;
                        snprintf(s_sel_ssid, sizeof(s_sel_ssid), "%s", s_aps[idx].ssid.c_str());
                        s_sel_open = (s_aps[idx].authmode == WIFI_AUTH_OPEN);
                        std::string pwd = saved_pwd(s_sel_ssid);
                        if (s_sel_open || !pwd.empty()) {
                            start_connect(s_sel_open ? "" : pwd.c_str());
                        } else {
                            s_pwd_len = 0;
                            s_pwd_buf[0] = 0;
                            s_shift = false;
                            show_pwd_page();
                        }
                    }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                }
            }
        }
        lvgl_port_unlock();
    }
    s_scanning = false;
    vTaskDelete(NULL);
}

/* ── 键盘 ── */
static const char *KB_ROWS[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm._",
};

static void kb_rebuild(void) {
    if (!s_kb || !lv_obj_is_valid(s_kb)) return;
    lv_obj_clean(s_kb);
    for (int r = 0; r < 4; r++) {
        const char *row = KB_ROWS[r];
        for (int c = 0; row[c]; c++) {
            char ch = row[c];
            if (s_shift && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
            lv_obj_t *b = lv_btn_create(s_kb);
            lv_obj_set_size(b, 44, 40);
            lv_obj_set_pos(b, c * 46, r * 44);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x3a3f4b), 0);
            lv_obj_set_style_radius(b, 6, 0);
            lv_obj_t *l = lv_label_create(b);
            char lbl[2] = {ch, 0};
            lv_label_set_text(l, lbl);
            set_font(l);
            lv_obj_center(l);
            lv_obj_add_event_cb(b, [](lv_event_t *e) {
                char c = (char)(intptr_t)lv_event_get_user_data(e);
                pwd_append(c);
            }, LV_EVENT_CLICKED, (void *)(intptr_t)ch);   /* 字符值直传,不挂栈指针 */
        }
    }
}

static void pwd_append(char c) {
    if (s_pwd_len >= (int)sizeof(s_pwd_buf) - 1) return;
    s_pwd_buf[s_pwd_len++] = c;
    s_pwd_buf[s_pwd_len] = 0;
    if (s_pwd_ta && lv_obj_is_valid(s_pwd_ta)) {
        std::string mask;
        for (int i = 0; i < s_pwd_len; i++) mask += "●";
        lv_textarea_set_text(s_pwd_ta, mask.c_str());
    }
}

static void pwd_backspace(void) {
    if (s_pwd_len <= 0) return;
    s_pwd_buf[--s_pwd_len] = 0;
    if (s_pwd_ta && lv_obj_is_valid(s_pwd_ta)) {
        std::string mask;
        for (int i = 0; i < s_pwd_len; i++) mask += "●";
        lv_textarea_set_text(s_pwd_ta, mask.c_str());
    }
}

/* ── 页面 ── */
static void page_reset(void) {
    if (!s_page) return;
    lv_obj_clean(s_page);
    s_pwd_ta = NULL;
}

static void show_list_page(void) {
    page_reset();
    lv_obj_set_style_bg_color(s_page, lv_color_black(), 0);   /* 2026-09-26 纯黑背景(用户拍板:与角色选择页一致,不再叠半透明层) */

    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "选择 WiFi");   /* 选择 WiFi */
    set_font(title);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);

    lv_obj_t *btn_refresh = lv_btn_create(s_page);
    lv_obj_set_size(btn_refresh, 92, 38);
    lv_obj_align(btn_refresh, LV_ALIGN_TOP_RIGHT, -104, 38);   /* 2026-10-02 下移:避电量胶囊 */
    lv_obj_set_style_bg_color(btn_refresh, lv_color_hex(0x2b6cb0), 0);
    lv_obj_t *bl = lv_label_create(btn_refresh);
    lv_label_set_text(bl, "刷新");   /* 刷新 */
    set_font(bl);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn_refresh, [](lv_event_t *e) {
        if (s_scanning) return;   /* 扫描进行中,忽略(避免与 ScanOnce 并发写 s_aps) */
        s_aps.clear();
        lv_obj_clean(s_list);
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, "扫描中...");   /* 扫描中... */
        set_font(l);
        lv_obj_center(l);
        s_scanning = true;
        xTaskCreate(scan_task, "wifi_scan", 10240, NULL, 3, NULL);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_skip = lv_btn_create(s_page);
    lv_obj_set_size(btn_skip, 92, 38);
    lv_obj_align(btn_skip, LV_ALIGN_TOP_RIGHT, -6, 38);        /* 2026-10-02 下移 */
    lv_obj_set_style_bg_color(btn_skip, lv_color_hex(0x3a3f4b), 0);
    bl = lv_label_create(btn_skip);
    lv_label_set_text(bl, "跳过");   /* 跳过 */
    set_font(bl);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn_skip, [](lv_event_t *e) {
        xEventGroupSetBits(s_done_evt, BIT_SKIP);
    }, LV_EVENT_CLICKED, NULL);

    /* 2026-09-26 离线模式按钮移进配网页(用户拍板:不进连接后弹,配网时即可选):
       点击=设离线标志+跳过配网,不进 AP 兜底,直接进离线展示页(对话不可用) */
    lv_obj_t *btn_offline = lv_btn_create(s_page);
    lv_obj_set_size(btn_offline, 110, 38);
    lv_obj_align(btn_offline, LV_ALIGN_TOP_RIGHT, -202, 38);   /* 2026-10-02 下移 */   /* 最左:离线(-202) · 刷新(-104) · 跳过(-6) */
    lv_obj_set_style_bg_color(btn_offline, lv_color_hex(0x6d3a8a), 0);
    bl = lv_label_create(btn_offline);
    lv_label_set_text(bl, "离线模式");   /* 离线模式 */
    set_font(bl);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn_offline, [](lv_event_t *e) {
        extern void application_set_offline_mode(void);
        application_set_offline_mode();
        xEventGroupSetBits(s_done_evt, BIT_SKIP);
    }, LV_EVENT_CLICKED, NULL);

    s_list = lv_obj_create(s_page);
    lv_obj_set_size(s_list, 464, 700);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x14161c), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, 6, 0);
}

static void show_pwd_page(void) {
    page_reset();
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text_fmt(title, "%s%s", s_sel_ssid, s_sel_open ? "(开放)"
                                                                : "(加密)");
    set_font(title);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);

    lv_obj_t *btn_back = lv_btn_create(s_page);
    lv_obj_set_size(btn_back, 92, 38);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -6, 38);        /* 2026-10-02 下移 */
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x3a3f4b), 0);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, "返回");   /* 返回 */
    set_font(bl);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn_back, [](lv_event_t *e) {
        s_pwd_len = 0;
        s_pwd_buf[0] = 0;
        show_list_page();
        if (!s_scanning) {
            s_scanning = true;
            xTaskCreate(scan_task, "wifi_scan", 10240, NULL, 3, NULL);
        }
    }, LV_EVENT_CLICKED, NULL);

    /* 密码框(掩码显示) */
    s_pwd_ta = lv_textarea_create(s_page);
    lv_obj_set_size(s_pwd_ta, 448, 44);
    lv_obj_align(s_pwd_ta, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(0x2a2e37), 0);
    lv_obj_set_style_border_color(s_pwd_ta, lv_color_hex(0x4d9fff), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pwd_ta, lv_color_white(), 0);
    set_font(s_pwd_ta);
    lv_textarea_set_placeholder_text(s_pwd_ta, "请输入密码");   /* 请输入密码 */
    lv_obj_set_style_text_color(s_pwd_ta, lv_color_hex(0x888888), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_max_length(s_pwd_ta, 63);
    lv_obj_add_flag(s_pwd_ta, LV_OBJ_FLAG_IGNORE_LAYOUT);

    /* 键盘容器 */
    s_kb = lv_obj_create(s_page);
    lv_obj_set_size(s_kb, 464, 190);
    lv_obj_align(s_kb, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(0x14161c), 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    lv_obj_set_style_pad_all(s_kb, 4, 0);

    /* 操作行:⇧ 空格 ⌫ 连接 */
    lv_obj_t *ops = lv_obj_create(s_page);
    lv_obj_set_size(ops, 464, 56);
    lv_obj_align(ops, LV_ALIGN_TOP_MID, 0, 332);
    lv_obj_set_style_bg_color(ops, lv_color_hex(0x14161c), 0);
    lv_obj_set_style_border_width(ops, 0, 0);
    lv_obj_set_style_pad_all(ops, 4, 0);

    auto mk_op = [ops](const char *txt, int x, int w, lv_event_cb_t cb) {
        lv_obj_t *b = lv_btn_create(ops);
        lv_obj_set_size(b, w, 48);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, 2);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3a3f4b), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        set_font(l);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        return b;
    };
    mk_op("↑", 0, 64, [](lv_event_t *e) {   /* ↑(原⇧) */
        s_shift = !s_shift;
        kb_rebuild();
    });
    mk_op("空格", 70, 90, [](lv_event_t *e) {   /* 空格 */
        pwd_append(' ');
    });
    mk_op("←", 166, 64, [](lv_event_t *e) {   /* ←(原⌫) */
        pwd_backspace();
    });
    lv_obj_t *b = mk_op("连接", 236, 224, NULL);   /* 连接 */
    lv_obj_set_style_bg_color(b, lv_color_hex(0x00AA55), 0);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
        if (s_pwd_len == 0) return;
        start_connect(s_pwd_buf);
    }, LV_EVENT_CLICKED, NULL);

    kb_rebuild();
}

static void show_connecting(void) {
    page_reset();
    lv_obj_t *l = lv_label_create(s_page);
    lv_label_set_text_fmt(l, "正在连接 %s ...", s_sel_ssid);   /* 正在连接 */
    set_font(l);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    lv_obj_t *sp = lv_arc_create(s_page);   /* LVGL 9 无 spinner,arc 旋转替代 */
    lv_obj_set_size(sp, 60, 60);
    lv_arc_set_bg_angles(sp, 0, 360);
    lv_arc_set_angles(sp, 0, 120);
    lv_obj_remove_style(sp, NULL, LV_PART_KNOB);
    lv_obj_remove_style(sp, NULL, LV_PART_INDICATOR);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(sp, 300);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, 80);
}

static void show_error(void) {
    page_reset();
    lv_obj_t *l = lv_label_create(s_page);
    lv_label_set_text(l, "连接失败:密码错误或信号弱");   /* 连接失败:密码错误或信号弱 */
    set_font(l);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFF6B6B), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -90);

    lv_obj_t *b1 = lv_btn_create(s_page);
    lv_obj_set_size(b1, 200, 46);
    lv_obj_align(b1, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(b1, lv_color_hex(0x00AA55), 0);
    lv_obj_t *t1 = lv_label_create(b1);
    lv_label_set_text(t1, "重试");   /* 重试 */
    set_font(t1);
    lv_obj_center(t1);
    lv_obj_add_event_cb(b1, [](lv_event_t *e) {
        if (s_sel_open) start_connect("");
        else if (s_pwd_len > 0) start_connect(s_pwd_buf);
        else show_pwd_page();
    }, LV_EVENT_CLICKED, NULL);

    /* 2026-10-07 用户要求:密码输错后能改密码重连(曾只有重试/返回列表,
       改不了密码只能返回列表再进,密码还残留) */
    lv_obj_t *b1b = lv_btn_create(s_page);
    lv_obj_set_size(b1b, 200, 46);
    lv_obj_align(b1b, LV_ALIGN_CENTER, 0, 44);
    lv_obj_set_style_bg_color(b1b, lv_color_hex(0x2b6cb0), 0);
    lv_obj_t *t1b = lv_label_create(b1b);
    lv_label_set_text(t1b, "重新输入密码");   /* 重新输入密码 */
    set_font(t1b);
    lv_obj_center(t1b);
    lv_obj_add_event_cb(b1b, [](lv_event_t *e) {
        s_pwd_len = 0;   // 清掉旧密码,重新输入
        show_pwd_page();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b2 = lv_btn_create(s_page);
    lv_obj_set_size(b2, 200, 46);
    lv_obj_align(b2, LV_ALIGN_CENTER, 0, 108);
    lv_obj_set_style_bg_color(b2, lv_color_hex(0x3a3f4b), 0);
    lv_obj_t *t2 = lv_label_create(b2);
    lv_label_set_text(t2, "返回列表");   /* 返回列表 */
    set_font(t2);
    lv_obj_center(t2);
    lv_obj_add_event_cb(b2, [](lv_event_t *e) {
        show_list_page();
        if (!s_scanning) {
            s_scanning = true;
            xTaskCreate(scan_task, "wifi_scan", 10240, NULL, 3, NULL);
        }
    }, LV_EVENT_CLICKED, NULL);
}

/* ── 入口 ── */
void wifi_ui_init(const lv_font_t *font) {
    s_font = font;
    if (!s_done_evt) s_done_evt = xEventGroupCreate();
}

bool wifi_ui_show(void) {
    /* 2026-09-20:配网(StartNetwork)早于 image_display_init,wifi_ui_init 尚未被调——
       从板级直接取中文字体兜底(否则 LVGL 默认字体无中文 → 全屏方框) */
    if (!s_font) {
        auto display = Board::GetInstance().GetDisplay();
        if (display) s_font = display->GetTextFont();
    }
    if (!s_done_evt) s_done_evt = xEventGroupCreate();
    xEventGroupClearBits(s_done_evt, BIT_CONNECTED | BIT_SKIP);
    s_connecting = false;
    s_scanning = false;   /* 残留 scan_task(若有)写局部缓冲,不冲突;它退出时再置 false 无碍 */

    if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
        s_aps.clear();   /* 共享 s_aps 只在锁内读写 */
        /* 2026-09-26 底图层完全照抄角色选择页(用户拍板:同样的创建序列/样式):
           create → size → pos → 纯黑 → 不透明 → 无边框 → 无内边距 */
        s_page = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_page, 480, 800);
        lv_obj_set_pos(s_page, 0, 0);
        lv_obj_set_style_bg_color(s_page, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_page, 0, 0);
        lv_obj_set_style_pad_all(s_page, 0, 0);
        lvgl_port_unlock();
    } else {
        return false;
    }

    /* 确保 station 模式。2026-09-21:P4 LP 核 Stop→Start 会永久卡死(扫描恒
       12289)——若站点已运行(启动自动连接失败后)直接复用,绝不重启 */
    auto &ws = WifiStation::GetInstance();
    if (!ws.IsStarted()) {
        ws.Start();
        vTaskDelay(pdMS_TO_TICKS(1500));   /* P4 RPC 启动就绪需时间 */
    }
    ws.SetManualConfig(true);   /* 浏览期间不自动连已存 SSID,把选择权交给用户 */
    ws.PauseScanTimer(true);   /* 列表页扫描与组件定时扫描互斥,暂停之 */

    /* 2026-09-21:show_list_page 必须在 lvgl_port_lock 内——此板 LVGL 渲染跑在
       独立任务(prio 4 > main),渲染期间 rendering_in_progress=true;main 裸调
       LVGL API 会被抢占后触发 LV_ASSERT(Invalidate during rendering)死循环 */
    if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
        show_list_page();
        lvgl_port_unlock();
    }
    if (!s_scanning) {
        s_scanning = true;
        xTaskCreate(scan_task, "wifi_scan", 10240, NULL, 3, NULL);
    }

    EventBits_t bits = xEventGroupWaitBits(s_done_evt, BIT_CONNECTED | BIT_SKIP,
                                           pdTRUE, pdFALSE, portMAX_DELAY);
    bool connected = (bits & BIT_CONNECTED) != 0;

    if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
        if (s_page && lv_obj_is_valid(s_page)) lv_obj_del(s_page);
        s_page = NULL;
        s_list = NULL;
        s_pwd_ta = NULL;
        lvgl_port_unlock();
    }
    /* 跳过时停 station(释放 netif,AP 兜底接管);连接成功保持并恢复定时扫描 */
    if (!connected) ws.Stop();
    else ws.PauseScanTimer(false);
    return connected;
}
