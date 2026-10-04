/** 设置页全屏 overlay（lv_layer_top，480×800）：
 *  转发到手机 / 转发时本地静音（lv_switch）、音量 / 亮度（lv_slider）、
 *  设备 IP 与手机连接状态（1s lv_timer 刷新）、重新搜索手机按钮。
 *  打开时不隐藏主界面按钮——全屏遮罩本身阻断下层交互。
 */
#include "settings_ui.h"

#include <cstdio>
#include <string>

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "wifi_station.h"

#define TAG "SettingsUI"

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_sw_fwd = NULL;
static lv_obj_t *s_sw_mute = NULL;
static lv_obj_t *s_sw_mcast = NULL;
static lv_obj_t *s_sw_vp = NULL;
static lv_obj_t *s_vol_lbl = NULL;
static lv_obj_t *s_brt_lbl = NULL;
static lv_obj_t *s_dev_ip_lbl = NULL;
static lv_obj_t *s_phone_lbl = NULL;
static lv_obj_t *s_stat_lbl = NULL;
static lv_timer_t *s_timer = NULL;
static const lv_font_t *s_font = NULL;

static const lv_font_t *efont(void) { return s_font ? s_font : LV_FONT_DEFAULT; }

void settings_ui_init(const lv_font_t *font) {
    s_font = font;
}

bool settings_ui_is_open(void) { return s_overlay != NULL; }

static void refresh_status(lv_timer_t *timer) {
    if (!s_overlay) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    auto &audio = Application::GetInstance().GetAudioService();

    std::string dev_ip = WifiStation::GetInstance().GetIpAddress();
    lv_label_set_text_fmt(s_dev_ip_lbl, "设备 IP: %s", dev_ip.empty() ? "未联网" : dev_ip.c_str());

    std::string phone = audio.ForwarderTargetIp();
    if (phone.empty()) {
        lv_label_set_text(s_phone_lbl, "手机: 未连接（等待 App）");
    } else if (audio.ForwarderHasTarget()) {
        lv_label_set_text_fmt(s_phone_lbl, "手机: %s 在线", phone.c_str());
    } else {
        lv_label_set_text_fmt(s_phone_lbl, "手机: %s 离线", phone.c_str());
    }

    lv_label_set_text_fmt(s_stat_lbl, "发包: %lu  丢包: %lu",
                          (unsigned long)audio.ForwarderPacketsSent(),
                          (unsigned long)audio.ForwarderPacketsDropped());
    lvgl_port_unlock();
}

static void fwd_sw_cb(lv_event_t *e) {
    auto &audio = Application::GetInstance().GetAudioService();
    bool on = lv_obj_has_state(s_sw_fwd, LV_STATE_CHECKED);
    audio.SetForwarderEnabled(on);
    // 联动：转发关闭时静音开关置灰
    if (s_sw_mute) {
        if (on) lv_obj_remove_state(s_sw_mute, LV_STATE_DISABLED);
        else lv_obj_add_state(s_sw_mute, LV_STATE_DISABLED);
    }
}

static void mute_sw_cb(lv_event_t *e) {
    auto &audio = Application::GetInstance().GetAudioService();
    audio.SetForwarderMuteLocal(lv_obj_has_state(s_sw_mute, LV_STATE_CHECKED));
}

static void mcast_sw_cb(lv_event_t *e) {
    auto &audio = Application::GetInstance().GetAudioService();
    audio.SetForwarderMulticast(lv_obj_has_state(s_sw_mcast, LV_STATE_CHECKED));
}

static void vp_sw_cb(lv_event_t *e) {
    // 2026-09-30 声纹过滤:写 NVS,下次连接服务器时随 hello 上报
    bool on = lv_obj_has_state(s_sw_vp, LV_STATE_CHECKED);
    Settings("voiceprint", true).SetInt("enabled", on ? 1 : 0);
    ESP_LOGI(TAG, "voiceprint enabled=%d", on ? 1 : 0);
}

static void vol_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(s_vol_lbl, "%d", v);
    Board::GetInstance().GetAudioCodec()->SetOutputVolume(v);
}

static void brt_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(s_brt_lbl, "%d", v);
    Backlight *backlight = Board::GetInstance().GetBacklight();
    if (backlight) backlight->SetBrightness(v, true);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, efont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

static lv_obj_t *make_switch(lv_obj_t *parent, int x, int y, bool checked, bool disabled,
                             lv_event_cb_t cb) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 65, 30);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x333333), 0);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (disabled) lv_obj_add_state(sw, LV_STATE_DISABLED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *make_slider(lv_obj_t *parent, int x, int y, int value, lv_obj_t **value_lbl,
                             lv_event_cb_t cb, int min_val = 0) {
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, 270, 10);
    lv_obj_set_pos(slider, x, y);
    lv_slider_set_range(slider, min_val, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x4A7BFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xDDDDDD), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);

    *value_lbl = make_label(parent, "", x + 285, y - 8);
    return slider;
}

void settings_ui_show(void) {
    if (s_overlay) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 480, 800);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // ── 标题栏 ──
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_set_size(bar, 480, 44);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, efont(), 0);
    lv_obj_center(title);

    // ── 关闭按钮 ──
    lv_obj_t *close_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(close_btn, 60, 30);
    lv_obj_set_pos(close_btn, 405, 7);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(close_btn, 6, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "关闭");
    lv_obj_set_style_text_font(close_lbl, efont(), 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t *e) { settings_ui_hide(); }, LV_EVENT_CLICKED, NULL);

    auto &audio = Application::GetInstance().GetAudioService();

    // ── 转发到手机 ──
    make_label(s_overlay, "转发到手机", 30, 95);
    s_sw_fwd = make_switch(s_overlay, 380, 90, audio.ForwarderEnabled(), false, fwd_sw_cb);

    // ── 转发时本地静音 ──
    make_label(s_overlay, "转发时本地静音", 30, 150);
    s_sw_mute = make_switch(s_overlay, 380, 145, audio.ForwarderMuteLocal(),
                            !audio.ForwarderEnabled(), mute_sw_cb);

    // ── 组播模式（热点实验）──
    make_label(s_overlay, "组播模式(热点实验)", 30, 205);
    s_sw_mcast = make_switch(s_overlay, 380, 200, audio.ForwarderMulticast(), false, mcast_sw_cb);

    // ── 声纹过滤（只听我；2026-09-30，默认关闭）──
    make_label(s_overlay, "声纹过滤(只听我)", 30, 260);
    s_sw_vp = make_switch(s_overlay, 380, 255,
                          Settings("voiceprint", false).GetInt("enabled", 0) != 0,
                          false, vp_sw_cb);

    // ── 音量 ──
    make_label(s_overlay, "音量", 30, 318);
    make_slider(s_overlay, 95, 320, Board::GetInstance().GetAudioCodec()->output_volume(),
                &s_vol_lbl, vol_slider_cb);
    lv_label_set_text_fmt(s_vol_lbl, "%d", Board::GetInstance().GetAudioCodec()->output_volume());

    // ── 亮度 ──
    make_label(s_overlay, "亮度", 30, 378);
    Backlight *backlight = Board::GetInstance().GetBacklight();
    int brightness = backlight ? backlight->brightness() : 75;
    make_slider(s_overlay, 95, 380, brightness, &s_brt_lbl, brt_slider_cb, 5);   // 亮度下限 5：太低连设置界面都看不见
    lv_label_set_text_fmt(s_brt_lbl, "%d", brightness);

    // ── 状态区 ──
    s_dev_ip_lbl = make_label(s_overlay, "设备 IP: --", 30, 450);
    s_phone_lbl = make_label(s_overlay, "手机: 未连接（等待 App）", 30, 493);
    s_stat_lbl = make_label(s_overlay, "发包: 0  丢包: 0", 30, 536);

    // ── 重新搜索手机 ──
    lv_obj_t *rescan_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(rescan_btn, 160, 40);
    lv_obj_set_pos(rescan_btn, 160, 585);
    lv_obj_set_style_bg_color(rescan_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(rescan_btn, 6, 0);
    lv_obj_t *rescan_lbl = lv_label_create(rescan_btn);
    lv_label_set_text(rescan_lbl, "重新搜索手机");
    lv_obj_set_style_text_font(rescan_lbl, efont(), 0);
    lv_obj_center(rescan_lbl);
    lv_obj_add_event_cb(rescan_btn, [](lv_event_t *e) {
        Application::GetInstance().GetAudioService().ForwarderResetTarget();
    }, LV_EVENT_CLICKED, NULL);

    // ── 设备绑定码(用户网页绑定用,6 位数字) ──
    std::string bind_code = SystemInfo::GetBindCode();
    lv_obj_t *code_lbl = make_label(s_overlay, ("设备绑定码: " + bind_code).c_str(), 30, 630);
    lv_obj_set_style_text_color(code_lbl, lv_color_hex(0x7fd), 0);

    // ── 提示 ──
    lv_obj_t *hint = make_label(s_overlay, "提示：声纹过滤开启后只响应主人声音（网页注册声纹后生效）", 30, 675);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x999999), 0);

    s_timer = lv_timer_create(refresh_status, 1000, NULL);
    lvgl_port_unlock();
    refresh_status(NULL);  // 立即刷一次
    ESP_LOGI(TAG, "Settings UI shown");
}

void settings_ui_hide(void) {
    if (!s_overlay) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    s_sw_fwd = NULL;
    s_sw_mute = NULL;
    s_sw_mcast = NULL;
    s_sw_vp = NULL;
    s_vol_lbl = NULL;
    s_brt_lbl = NULL;
    s_dev_ip_lbl = NULL;
    s_phone_lbl = NULL;
    s_stat_lbl = NULL;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Settings UI hidden");
}
