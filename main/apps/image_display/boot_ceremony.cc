/* 开机仪式(2026-09-29 用户拍板):
 * 开机 → 背景轮播 + 加载条「正在尝试与服务器取得神经连接」
 *      → 加载完成 → 黄色半透明「开始唤醒」按钮(上下跳动+呼吸发光)
 *      → 用户点击 → 进入 WiFi 配置页/主流程。
 * 独立编译单元(布局敏感教训:新 LVGL 代码不并入 ImageDisplay.cpp)。 */
#include <string.h>
#include <stdio.h>

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#define TAG "BootCeremony"

static const lv_font_t *s_font = NULL;   // 中文文本字体(对话框/WiFi 页同款)

static lv_obj_t *s_loading_label = NULL;
static lv_obj_t *s_touch_layer = NULL;   // 全屏透明触摸层(任意处触摸进入)
static lv_obj_t *s_wake_btn = NULL;
static lv_obj_t *s_wake_lbl = NULL;
static lv_obj_t *s_wake_lbl2 = NULL;   // 加粗副层(1px 偏移叠加)
static lv_anim_t s_bob_anim;
static lv_anim_t s_glow_anim;
static volatile int s_clicked = 0;

static void wake_btn_click_cb(lv_event_t *e) {
    s_clicked = 1;
}

/* 动画 wrapper(lv_anim_exec_xcb_t 签名匹配,消除 -Wcast-function-type) */
static void anim_translate_y(void *var, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}
static void anim_opa(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

void boot_ceremony_loading(const lv_font_t *font) {
    s_font = font;   // 2026-09-29 中文显示:LV_FONT_DEFAULT 无中文(方框),用系统中文文本字体
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    s_loading_label = lv_label_create(lv_layer_top());
    lv_label_set_text(s_loading_label, "正在尝试与服务器取得神经连接");
    lv_obj_set_style_text_color(s_loading_label, lv_color_hex(0xFFD27F), 0);
    lv_obj_set_style_text_font(s_loading_label, s_font ? s_font : LV_FONT_DEFAULT, 0);
    lv_obj_align(s_loading_label, LV_ALIGN_BOTTOM_MID, 0, -120);
    // 半透明黑底条(整宽)
    lv_obj_set_style_bg_color(s_loading_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_loading_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_loading_label, 8, 0);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "loading shown");
}

void boot_ceremony_ready(void) {
    /* 加载完成:移除加载条,显示「开始唤醒」按钮(黄字半透明,上下跳动+呼吸) */
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_loading_label) {
        lv_obj_del(s_loading_label);
        s_loading_label = NULL;
    }
    if (!s_wake_btn) {
        s_wake_btn = lv_btn_create(lv_layer_top());
        lv_obj_set_style_bg_color(s_wake_btn, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_wake_btn, LV_OPA_40, 0);
        lv_obj_set_style_border_width(s_wake_btn, 0, 0);
        lv_obj_set_style_radius(s_wake_btn, 30, 0);
        lv_obj_set_style_pad_hor(s_wake_btn, 44, 0);   // 2026-09-29 放大
        lv_obj_set_style_pad_ver(s_wake_btn, 20, 0);
        lv_obj_align(s_wake_btn, LV_ALIGN_BOTTOM_MID, 0, -40);   // 2026-09-29 再向下 10px   // 2026-09-29 再向下 15px   // 2026-09-29 再降低 30px   // 2026-09-29 再降低 30px   // 2026-09-29 再下移 30px   // 2026-09-29 微调:太高,下移 20px
        lv_obj_add_event_cb(s_wake_btn, wake_btn_click_cb, LV_EVENT_CLICKED, NULL);

        s_wake_lbl = lv_label_create(s_wake_btn);
        lv_label_set_text(s_wake_lbl, "开始唤醒");
        lv_obj_set_style_text_color(s_wake_lbl, lv_color_hex(0xFFD27F), 0);
        lv_obj_set_style_text_font(s_wake_lbl, s_font ? s_font : LV_FONT_DEFAULT, 0);
        lv_obj_center(s_wake_lbl);
        // 2026-09-29 模拟加粗:本版 LVGL 无 text_shadow API、中文字体无粗体变体,
        // 叠一层同文字偏移 1px 的副 label(视觉加粗)
        s_wake_lbl2 = lv_label_create(s_wake_btn);
        lv_label_set_text(s_wake_lbl2, "开始唤醒");
        lv_obj_set_style_text_color(s_wake_lbl2, lv_color_hex(0xFFD27F), 0);
        lv_obj_set_style_text_font(s_wake_lbl2, s_font ? s_font : LV_FONT_DEFAULT, 0);
        lv_obj_center(s_wake_lbl2);
        lv_obj_set_pos(s_wake_lbl2, lv_obj_get_x(s_wake_lbl2) + 1, lv_obj_get_y(s_wake_lbl2) + 1);
    } else {
        lv_obj_clear_flag(s_wake_btn, LV_OBJ_FLAG_HIDDEN);
    }
    s_clicked = 0;

    // 2026-09-29 全屏透明触摸层:用户触摸屏幕任意处即进入(按钮仅作视觉提示)
    if (!s_touch_layer) {
        s_touch_layer = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_touch_layer, 480, 800);
        lv_obj_set_pos(s_touch_layer, 0, 0);
        lv_obj_set_style_bg_opa(s_touch_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_touch_layer, 0, 0);
        lv_obj_clear_flag(s_touch_layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_touch_layer, wake_btn_click_cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_clear_flag(s_touch_layer, LV_OBJ_FLAG_HIDDEN);
    }

    // 上下跳动动画(translate 0↔-8:不动 align 位置,此前 lv_obj_set_y 覆盖 align)
    lv_anim_init(&s_bob_anim);
    lv_anim_set_var(&s_bob_anim, s_wake_btn);
    lv_anim_set_exec_cb(&s_bob_anim, anim_translate_y);
    lv_anim_set_values(&s_bob_anim, 0, -8);
    lv_anim_set_time(&s_bob_anim, 700);
    lv_anim_set_playback_time(&s_bob_anim, 700);
    lv_anim_set_repeat_count(&s_bob_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_bob_anim);

    // 呼吸发光(透明度 30%↔80%)
    lv_anim_init(&s_glow_anim);
    lv_anim_set_var(&s_glow_anim, s_wake_btn);
    lv_anim_set_exec_cb(&s_glow_anim, anim_opa);
    lv_anim_set_values(&s_glow_anim, LV_OPA_30, LV_OPA_80);
    lv_anim_set_time(&s_glow_anim, 900);
    lv_anim_set_playback_time(&s_glow_anim, 900);
    lv_anim_set_repeat_count(&s_glow_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_glow_anim);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "wake button ready");
}

int boot_ceremony_wait(int timeout_ms) {
    /* 阻塞等待用户点击(调用线程=主线程);超时返回 0 */
    int waited = 0;
    while (!s_clicked && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_clicked ? 1 : 0;
}

void boot_ceremony_hide(void) {
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_wake_btn) {
        lv_anim_del(s_wake_btn, anim_translate_y);
        lv_anim_del(s_wake_btn, anim_opa);
        lv_obj_del(s_wake_btn);
        s_wake_btn = NULL;
        s_wake_lbl = NULL;
        s_wake_lbl2 = NULL;
    }
    if (s_touch_layer) {
        lv_obj_del(s_touch_layer);
        s_touch_layer = NULL;
    }
    if (s_loading_label) {
        lv_obj_del(s_loading_label);
        s_loading_label = NULL;
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "ceremony hidden");
}
