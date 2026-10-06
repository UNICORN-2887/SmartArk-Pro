/** 全屏菜单页（lv_layer_top，480×800）：2 列网格大按钮（200×80），
 *  收纳次要功能：一起拼豆/蟑螂派对/语音记录/背景音乐/Live2D测试/弹出键盘。
 *  按钮点击 → 先关菜单页 → 打开目标功能（回调函数由 ImageDisplay.cpp 提供，extern 链接）。
 */
#include "menu_ui.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "apps/loading/loading_ui.h"

#define TAG "MenuUI"

// ImageDisplay.cpp 中提供（原 static 函数已开放链接）
extern void profile_show(void);
extern void voice_ui_show(void);
extern void music_ui_show(void);
extern void keyboard_ui_show(void);
extern void lv2_test_show(void);
extern void pd_test_show(void);
extern void beads_main_show(void);
extern void beads_set_font(const lv_font_t*);
extern void anim_test_show(void);   // Q 版动作测试页（横屏菜单入口）
extern void ppd_interaction_stop_for_app(void);     // 资源互斥全屏应用（纸偶测试）打开前退出 PPD 交互
extern void ppd_interaction_suspend_for_app(void);  // 无冲突全屏应用打开前暂停 PPD 直写（返回后回 PPD 待机）
extern void role_downloader_show(void);             // 角色下载页(云端角色包 → SD 卡)

static lv_obj_t *s_overlay = NULL;
static bool s_cover_mode = false;
static const lv_font_t *s_font = NULL;

static const lv_font_t *efont(void) { return s_font ? s_font : LV_FONT_DEFAULT; }

void menu_ui_init(const lv_font_t *font) {
    s_font = font;
}

bool menu_ui_is_open(void) { return s_overlay != NULL; }

static void open_feature(void (*fn)(void)) {
    menu_ui_hide();  // 先关菜单页（含 loading 兜底），再开目标功能
    if (fn) fn();
}

static void make_menu_btn(lv_obj_t *parent, const char *text, int x, int y,
                          void (*fn)(void), bool special) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 200, 80);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(special ? 0x885588 : 0x3A3A3A), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, efont(), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        void (*handler)(void) = (void (*)(void))lv_event_get_user_data(e);
        open_feature(handler);
    }, LV_EVENT_CLICKED, (void*)fn);
}

void menu_ui_show(bool cover_mode, bool landscape) {
    if (s_overlay) return;
    s_cover_mode = cover_mode;

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    if (landscape) {
        // 横屏菜单：800×480 虚拟页面绕屏幕中心顺时针转 90°（与横屏按钮组同向，
        // LVGL 输入映射自动跟随 transform，点击坐标无需换算）
        lv_obj_set_size(s_overlay, 800, 480);
        lv_obj_set_pos(s_overlay, -160, 160);   // 中心 (240,400) = 屏幕中心
        lv_obj_set_style_transform_pivot_x(s_overlay, 400, 0);
        lv_obj_set_style_transform_pivot_y(s_overlay, 240, 0);
        lv_obj_set_style_transform_rotation(s_overlay, 900, 0);
    } else {
        lv_obj_set_size(s_overlay, 480, 800);
        lv_obj_set_pos(s_overlay, 0, 0);
    }

    // ── 标题栏 ──
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_set_size(bar, landscape ? 800 : 480, 44);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "菜单");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, efont(), 0);
    lv_obj_center(title);

    // ── 关闭按钮 ──
    lv_obj_t *close_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(close_btn, 60, 30);
    /* 2026-10-05 竖屏关闭按钮左移到标题栏左侧(x=405 被右上角电量胶囊遮挡:
       lv_layer_sys 在 layer_top 之上) */
    lv_obj_set_pos(close_btn, landscape ? 725 : 17, 7);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(close_btn, 6, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "关闭");
    lv_obj_set_style_text_font(close_lbl, efont(), 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t *e) { menu_ui_hide(); }, LV_EVENT_CLICKED, NULL);

    if (landscape) {
        // ── 横屏 2×2 网格（200×80，列 x=180/420，行 y=130/260）；动作展示按钮在互动画面左侧 ──
        make_menu_btn(s_overlay, "一起拼豆", 180, 130, []() {
            ppd_interaction_suspend_for_app();   // 暂停 PPD 直写 + 横屏立牌（返回后恢复）
            beads_set_font(s_font);
            beads_main_show();
        }, true);
        make_menu_btn(s_overlay, "蟑螂派对！", 420, 130, []() {
            ppd_interaction_suspend_for_app();
            profile_show();
        }, false);
        make_menu_btn(s_overlay, "语音记录", 180, 260, voice_ui_show, false);
        make_menu_btn(s_overlay, "背景音乐", 420, 260, music_ui_show, false);
    } else {
        // ── 2 列网格（200×80，列 x=20/260，行 y=100/204/308）──
        // 行1：拼豆（特色配色） | 蟑螂派对
        make_menu_btn(s_overlay, "一起拼豆", 20, 100, []() {
            ppd_interaction_suspend_for_app();   // 暂停 PPD 直写（返回后回 PPD 待机）
            beads_set_font(s_font);
            beads_main_show();
        }, true);
        make_menu_btn(s_overlay, "蟑螂派对！", 260, 100, []() {
            ppd_interaction_suspend_for_app();
            profile_show();
        }, false);

        // 行2：语音记录 | 背景音乐（overlay 面板：pd_anim 的 ui_open 检测会暂停直写，无需退出）
        make_menu_btn(s_overlay, "语音记录", 20, 204, voice_ui_show, false);
        make_menu_btn(s_overlay, "背景音乐", 260, 204, music_ui_show, false);

        // 行3：Live2D测试（弹出键盘已有右上按钮，菜单不重复提供）
        make_menu_btn(s_overlay, "Live2D测试", 20, 308, []() {
            ppd_interaction_suspend_for_app();
            lv2_test_show();
        }, false);

        // 行4：纸偶测试（sprite+仿射渲染器；与 PPD 共用 fb/canvas，须退出交互）
        make_menu_btn(s_overlay, "纸偶测试", 20, 412, []() {
            ppd_interaction_stop_for_app();
            pd_test_show();
        }, false);
        make_menu_btn(s_overlay, "角色下载", 260, 412, role_downloader_show, false);
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Menu UI shown (%s%s)", cover_mode ? "cover" : "expression",
             landscape ? ", landscape" : "");
}

void menu_ui_hide(void) {
    if (!s_overlay) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Menu UI hidden");
}
