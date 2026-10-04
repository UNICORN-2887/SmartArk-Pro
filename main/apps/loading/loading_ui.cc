/** 全屏加载动画 — lv_layer_top 覆盖层：lv_bar 真实进度 + lv_arc 自制旋转环。
 *  旋转动画由 lv_anim 驱动，跑在 LVGL 任务内，与 SD I/O 加载任务天然解耦。
 */
#include "loading_ui.h"
#include <string.h>
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

#define LOADING_TIMEOUT_US (15 * 1000000LL)  // 15s 超时自动隐藏，防加载挂起后遮罩永驻

static lv_obj_t *s_ov = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_stage_lbl = NULL;
static lv_obj_t *s_pct_lbl = NULL;
static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_spin = NULL;
static const lv_font_t *s_font = NULL;
static volatile bool s_active = false;
static int64_t s_start_us = 0;

static void arc_rotate_cb(void *var, int32_t v) {
    lv_arc_set_rotation((lv_obj_t *)var, (uint16_t)v);
}

static const lv_font_t *efont(void) { return s_font ? s_font : LV_FONT_DEFAULT; }

bool loading_show(const char *title) {
    if (s_active) {  // 复用：更新标题 + 重置超时
        s_start_us = esp_timer_get_time();
        if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
            if (s_title_lbl && title) lv_label_set_text(s_title_lbl, title);
            lvgl_port_unlock();
        }
        return true;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return false;

    s_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ov, 480, 800);
    lv_obj_set_pos(s_ov, 0, 0);
    /* 2026-09-26 纯黑不透明(用户拍板:全项目统一纯黑底,与 WiFi 页/角色选择页一致;
       原 90% 半透明黑在浅色底上透出白色,观感如"白底叠半透明黑层") */
    lv_obj_set_style_bg_color(s_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_set_style_pad_all(s_ov, 0, 0);
    lv_obj_clear_flag(s_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_CLICKABLE);  // 模态：吃掉触摸防误触

    // 标题
    s_title_lbl = lv_label_create(s_ov);
    lv_obj_set_style_text_font(s_title_lbl, efont(), 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_white(), 0);
    lv_label_set_text(s_title_lbl, title ? title : "加载中...");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, -120);

    // 旋转环：lv_arc 300° 弧 + lv_anim 旋转（CONFIG_LV_USE_ARC=y 已启用）
    s_spin = lv_arc_create(s_ov);
    lv_obj_set_size(s_spin, 44, 44);
    lv_obj_remove_style(s_spin, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(s_spin, 0, 300);
    lv_arc_set_rotation(s_spin, 0);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x4A7BFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spin, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 4, LV_PART_MAIN);
    lv_obj_align(s_spin, LV_ALIGN_CENTER, 0, -40);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_spin);
    lv_anim_set_exec_cb(&a, arc_rotate_cb);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_time(&a, 800);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    // 进度条（确定阶段显示，与旋转环互斥）
    s_bar = lv_bar_create(s_ov);
    lv_obj_set_size(s_bar, 320, 8);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    // 阶段文字
    s_stage_lbl = lv_label_create(s_ov);
    lv_obj_set_style_text_font(s_stage_lbl, efont(), 0);
    lv_obj_set_style_text_color(s_stage_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(s_stage_lbl, "");
    lv_obj_align(s_stage_lbl, LV_ALIGN_CENTER, 0, 10);

    // 百分比
    s_pct_lbl = lv_label_create(s_ov);
    lv_obj_set_style_text_font(s_pct_lbl, efont(), 0);
    lv_obj_set_style_text_color(s_pct_lbl, lv_color_white(), 0);
    lv_label_set_text(s_pct_lbl, "");
    lv_obj_align(s_pct_lbl, LV_ALIGN_CENTER, 0, 45);
    lv_obj_add_flag(s_pct_lbl, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
    s_active = true;
    s_start_us = esp_timer_get_time();
    return true;
}

void loading_set_stage(const char *text, int percent) {
    if (!s_active) return;
    if (esp_timer_get_time() - s_start_us > LOADING_TIMEOUT_US) {
        loading_hide();  // 超时兜底
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    if (s_stage_lbl && text) lv_label_set_text(s_stage_lbl, text);
    if (percent >= 0) {
        if (percent > 100) percent = 100;
        if (s_bar) {
            lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(s_bar, percent, LV_ANIM_OFF);
        }
        if (s_spin) lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
        if (s_pct_lbl) {
            lv_obj_remove_flag(s_pct_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(s_pct_lbl, "%d%%", percent);
        }
    } else {  // 不确定阶段：旋转环
        if (s_bar) lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        if (s_spin) lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
        if (s_pct_lbl) lv_obj_add_flag(s_pct_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void loading_hide(void) {
    if (!s_active) return;
    s_active = false;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_spin) lv_anim_delete(s_spin, arc_rotate_cb);  // 防对象已删后动画回调踩空
        if (s_ov) lv_obj_del(s_ov);
        s_ov = s_title_lbl = s_stage_lbl = s_pct_lbl = s_bar = s_spin = NULL;
        lvgl_port_unlock();
    }
}

void loading_raise(void) {
    if (!s_active || !s_ov) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
        lv_obj_move_foreground(s_ov);
        lvgl_port_unlock();
    }
}

bool loading_is_active(void) { return s_active; }

void loading_set_font(const void *font) {
    s_font = (const lv_font_t *)font;
}
