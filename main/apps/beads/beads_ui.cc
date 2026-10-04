/** 拼豆编辑器 UI — 竖屏 480×800。
 *  一级：24×24 画布 + 36 色板 + 工具 + 存取。
 *  二级/三级：3×3 合成板 + 右侧作品列表（点选→点格放入）。
 */
#include "beads.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#define TAG "BEADS_UI"

// ── 全局 UI 状态 ──
static lv_obj_t* s_overlay = NULL;
static BeadArt* s_edit_art = NULL;      // 当前编辑画作
static int s_edit_level = 1;            // 1/2/3
static int s_tool = 0;                  // 0=色笔 1=橡皮
static int s_color = 1;                 // 当前色号 1..36
static uint16_t* s_board_buf = NULL;    // 画布 RGB565
static lv_obj_t* s_canvas = NULL;
static char s_file_list[128][64];  // 128: N=216 导入有 81 个 B1 文件，64 列不全
static int s_file_count = 0;
static int s_selected_file = -1;        // 合成模式选中的文件
static lv_obj_t* s_pal_btns[BEADS_COLORS] = {NULL};  // 色卡按钮（高亮刷新用）
static const lv_font_t* s_font = NULL;

// ── 工具函数 ──
static void draw_pixel(BeadArt* art, int x, int y, int color) {
    if (!art || x < 0 || x >= art->w || y < 0 || y >= art->h) return;
    art->pixels[y * art->w + x] = (uint8_t)color;
}

// 色卡高亮刷新：仅当前选中色号显示红色外框
static void palette_refresh_highlight(void) {
    for (int i = 0; i < BEADS_COLORS; i++) {
        if (!s_pal_btns[i]) continue;
        lv_obj_set_style_border_width(s_pal_btns[i], (i + 1 == s_color) ? 2 : 0, 0);
        lv_obj_invalidate(s_pal_btns[i]);
    }
}

// 画布刷新：把 BeadArt 渲染到 s_board_buf（缓冲与部件同尺寸，避免偏移）
static void refresh_canvas(void) {
    if (!s_canvas || !s_edit_art) return;
    int cell = 456 / s_edit_art->w;  // L1:19, L2:6, L3:2
    if (cell < 1) cell = 1;
    int cw = s_edit_art->w * cell;
    int ch = s_edit_art->h * cell;
    for (int i = 0; i < cw * ch; i++) s_board_buf[i] = 0xFFFF;  // 白底
    // 像素
    for (int y = 0; y < s_edit_art->h; y++) {
        for (int x = 0; x < s_edit_art->w; x++) {
            int c = s_edit_art->pixels[y * s_edit_art->w + x];
            if (c == 0) continue;
            uint16_t rgb = g_beads_palette[c - 1];
            for (int dy = 0; dy < cell; dy++) {
                for (int dx = 0; dx < cell; dx++) {
                    int px = x * cell + dx;
                    int py = y * cell + dy;
                    if (px < cw && py < ch) s_board_buf[py * cw + px] = rgb;
                }
            }
        }
    }
    // 网格线（L1: 每格一条浅灰线；L2/L3: 3×3 分隔黑线）
    if (s_edit_level == 1 && cell >= 4) {
        for (int gy = 1; gy < s_edit_art->h; gy++) {
            for (int p = 0; p < cw; p++) s_board_buf[gy * cell * cw + p] = 0xE71C;
        }
        for (int gx = 1; gx < s_edit_art->w; gx++) {
            for (int p = 0; p < ch; p++) s_board_buf[p * cw + gx * cell] = 0xE71C;
        }
    } else if (s_edit_level > 1) {
        int cell_px = s_edit_art->w / 3 * cell;
        for (int i = 1; i < 3; i++) {
            int line = i * cell_px;
            for (int p = 0; p < ch; p++) s_board_buf[p * cw + line] = 0x0000;
            for (int p = 0; p < cw; p++) s_board_buf[line * cw + p] = 0x0000;
        }
    }
    // 缓冲与部件同尺寸，居中显示
    int off_x = (480 - cw) / 2;
    lv_canvas_set_buffer(s_canvas, (uint8_t*)s_board_buf, cw, ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, off_x, 28);
    lv_obj_invalidate(s_canvas);
}

// ── 文件选择器 ──
static lv_obj_t* s_picker = NULL;
static int s_picker_level = 1;         // 当前打开的选择器级别（删除后刷新用）

static void picker_close(void) {
    if (s_picker) { lv_obj_del(s_picker); s_picker = NULL; }
    // 恢复 compose 右侧子级文件列表（picker_open 会覆盖 s_file_list，
    // 不恢复的话合成板点格会错拿同级别文件）
    if (s_edit_level > 1) {
        s_file_count = beads_list_files(s_edit_level - 1, s_file_list, 128);
    }
}

static void picker_open(int level, void (*on_select)(const char* filename)) {
    picker_close();
    s_picker_level = level;
    s_selected_file = -1;  // 清掉合成板点选状态，防加载后误放块
    int n = beads_list_files(level, s_file_list, 128);
    s_file_count = n;

    s_picker = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_picker, 480, 800);
    lv_obj_set_pos(s_picker, 0, 0);
    lv_obj_set_style_bg_color(s_picker, lv_color_hex(0x80000000), 0);
    lv_obj_set_style_pad_all(s_picker, 0, 0);

    // 内容面板
    lv_obj_t* panel = lv_obj_create(s_picker);
    lv_obj_set_size(panel, 360, 560);
    lv_obj_set_pos(panel, 60, 120);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x2A2A30), 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "选择文件");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t* list = lv_obj_create(panel);
    lv_obj_set_size(list, 344, 430);
    lv_obj_set_pos(list, 0, 32);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x222228), 0);
    lv_obj_set_style_pad_all(list, 4, 0);

    for (int i = 0; i < n; i++) {
        // 加载按钮（左侧）
        lv_obj_t* lb = lv_btn_create(list);
        lv_obj_set_size(lb, 250, 34);
        lv_obj_set_pos(lb, 0, i * 38);
        lv_obj_set_style_bg_color(lb, lv_color_hex(0x33333A), 0);
        lv_obj_t* ll = lv_label_create(lb);
        lv_label_set_text(ll, s_file_list[i]);
        lv_obj_set_style_text_color(ll, lv_color_white(), 0);
        lv_obj_set_style_text_font(ll, s_font, 0);
        lv_obj_center(ll);
        int fi = i;
        lv_obj_add_event_cb(lb, [](lv_event_t* e) {
            int idx = (int)(uintptr_t)lv_event_get_user_data(e);
            char fn[64];
            strncpy(fn, s_file_list[idx], 63);
            fn[63] = '\0';
            picker_close();
            // 调用选择回调
            extern void beads_picker_on_select(const char*);
            beads_picker_on_select(fn);
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)fi);
        // 删除按钮（右侧）
        lv_obj_t* db = lv_btn_create(list);
        lv_obj_set_size(db, 74, 34);
        lv_obj_set_pos(db, 260, i * 38);
        lv_obj_set_style_bg_color(db, lv_color_hex(0x884444), 0);
        lv_obj_t* dl = lv_label_create(db);
        lv_label_set_text(dl, "删");
        lv_obj_set_style_text_color(dl, lv_color_white(), 0);
        lv_obj_set_style_text_font(dl, s_font, 0);
        lv_obj_center(dl);
        lv_obj_add_event_cb(db, [](lv_event_t* e) {
            int idx = (int)(uintptr_t)lv_event_get_user_data(e);
            char fn[64];
            strncpy(fn, s_file_list[idx], 63);
            fn[63] = '\0';
            if (beads_delete(fn)) {
                int lv = s_picker_level;
                picker_close();
                picker_open(lv, NULL);  // 重建列表刷新
            }
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)fi);
    }

    // 取消按钮
    lv_obj_t* cancel = lv_btn_create(panel);
    lv_obj_set_size(cancel, 100, 40);
    lv_obj_set_pos(cancel, 130, 510);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x444455), 0);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, s_font, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, [](lv_event_t* e) { picker_close(); }, LV_EVENT_CLICKED, NULL);
}

// 选择回调（L1 加载用）
void beads_picker_on_select(const char* filename) {
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/User/Beads/%s", filename);
    BeadArt* loaded = beads_load(path);
    if (loaded && s_edit_art && loaded->w == s_edit_art->w) {
        beads_free(s_edit_art);
        s_edit_art = loaded;
        refresh_canvas();
        ESP_LOGI(TAG, "loaded %s", filename);
    } else if (loaded) {
        beads_free(loaded);
    }
}

// ── 英文命名键盘 ──
static lv_obj_t* s_name_kb = NULL;
static lv_obj_t* s_name_label = NULL;
static char s_name_buf[32] = {0};
static int s_name_len = 0;

// 按键工厂（样式同对话模式 26 键键盘）
static lv_obj_t* name_kb_btn(lv_obj_t* parent, const char* text,
                             int x, int y, int w, int h,
                             lv_event_cb_t cb, void* ud) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, s_font, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void name_kb_update_label(void) {
    if (s_name_label) lv_label_set_text(s_name_label, s_name_buf);
}

static void name_kb_close(void) {
    if (s_name_kb) { lv_obj_del(s_name_kb); s_name_kb = NULL; s_name_label = NULL; }
}

static void name_kb_type(char c) {
    if (s_name_len < 20) {
        s_name_buf[s_name_len++] = c;
        s_name_buf[s_name_len] = '\0';
        name_kb_update_label();
    }
}

static void name_kb_backspace(void) {
    if (s_name_len > 0) {
        s_name_len--;
        s_name_buf[s_name_len] = '\0';
        name_kb_update_label();
    }
}

static void name_kb_open(void) {
    name_kb_close();
    s_name_len = 0;
    s_name_buf[0] = '\0';

    // 面板：与对话模式键盘一致（480×420，底部弹出，90% 黑底）
    s_name_kb = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_name_kb, 480, 420);
    lv_obj_set_pos(s_name_kb, 0, 380);
    lv_obj_set_style_bg_color(s_name_kb, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_name_kb, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_name_kb, 0, 0);
    lv_obj_set_style_pad_all(s_name_kb, 0, 0);
    lv_obj_clear_flag(s_name_kb, LV_OBJ_FLAG_SCROLLABLE);

    // 输入预览框
    lv_obj_t* preview = lv_obj_create(s_name_kb);
    lv_obj_set_size(preview, 460, 36);
    lv_obj_set_pos(preview, 10, 4);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_radius(preview, 4, 0);
    lv_obj_set_style_pad_all(preview, 4, 0);
    s_name_label = lv_label_create(preview);
    lv_label_set_text(s_name_label, "");
    lv_obj_set_style_text_color(s_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_name_label, s_font, 0);
    lv_obj_center(s_name_label);

    // 26 键 QWERTY 布局（键距/行距同对话模式键盘）
    #define KX(c)  (8+(c)*45)
    #define KY(r)  (88+(r)*43)
    #define KW 43
    #define KH 39
    // 行1: 数字 1..9,0
    for (int d = 0; d < 10; d++) {
        char ch = (d == 9) ? '0' : (char)('1' + d);
        char label[2] = {ch, '\0'};
        name_kb_btn(s_name_kb, label, KX(d), KY(0), KW, KH, [](lv_event_t* e){
            name_kb_type((char)(uintptr_t)lv_event_get_user_data(e));
        }, (void*)(uintptr_t)ch);
    }
    // 行2: q..p
    name_kb_btn(s_name_kb, "q", KX(0), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('q'); }, NULL);
    name_kb_btn(s_name_kb, "w", KX(1), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('w'); }, NULL);
    name_kb_btn(s_name_kb, "e", KX(2), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('e'); }, NULL);
    name_kb_btn(s_name_kb, "r", KX(3), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('r'); }, NULL);
    name_kb_btn(s_name_kb, "t", KX(4), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('t'); }, NULL);
    name_kb_btn(s_name_kb, "y", KX(5), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('y'); }, NULL);
    name_kb_btn(s_name_kb, "u", KX(6), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('u'); }, NULL);
    name_kb_btn(s_name_kb, "i", KX(7), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('i'); }, NULL);
    name_kb_btn(s_name_kb, "o", KX(8), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('o'); }, NULL);
    name_kb_btn(s_name_kb, "p", KX(9), KY(1), KW, KH, [](lv_event_t* e){ name_kb_type('p'); }, NULL);
    // 行3: a..l + 确定（右侧宽键）
    name_kb_btn(s_name_kb, "a", KX(0), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('a'); }, NULL);
    name_kb_btn(s_name_kb, "s", KX(1), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('s'); }, NULL);
    name_kb_btn(s_name_kb, "d", KX(2), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('d'); }, NULL);
    name_kb_btn(s_name_kb, "f", KX(3), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('f'); }, NULL);
    name_kb_btn(s_name_kb, "g", KX(4), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('g'); }, NULL);
    name_kb_btn(s_name_kb, "h", KX(5), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('h'); }, NULL);
    name_kb_btn(s_name_kb, "j", KX(6), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('j'); }, NULL);
    name_kb_btn(s_name_kb, "k", KX(7), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('k'); }, NULL);
    name_kb_btn(s_name_kb, "l", KX(8), KY(2), KW, KH, [](lv_event_t* e){ name_kb_type('l'); }, NULL);
    int ok_x = KX(9) + 2, ok_w = 480 - 8 - KX(9) - 2;
    name_kb_btn(s_name_kb, "确定", ok_x, KY(2), ok_w, KH, [](lv_event_t* e){
        if (s_name_len > 0 && s_edit_art) {
            beads_save(s_edit_art, s_name_buf, s_edit_level);
            ESP_LOGI(TAG, "saved B%d_%s", s_edit_level, s_name_buf);
        }
        name_kb_close();
    }, NULL);
    // 行4: z..m（左移一列）+ 退格（右侧宽键）
    name_kb_btn(s_name_kb, "z", KX(1), KY(3), KW, KH, [](lv_event_t* e){ name_kb_type('z'); }, NULL);
    name_kb_btn(s_name_kb, "x", KX(2), KY(3), KW, KH, [](lv_event_t* e){ name_kb_type('x'); }, NULL);
    name_kb_btn(s_name_kb, "c", KX(3), KY(3), KW, KH, [](lv_event_t* e){ name_kb_type('c'); }, NULL);
    name_kb_btn(s_name_kb, "v", KX(4), KY(3), KW, KH, [](lv_event_t* e){ name_kb_type('v'); }, NULL);
    name_kb_btn(s_name_kb, "b", KX(5), KY(3), KW, KH, [](lv_event_t* e){ name_kb_type('b'); }, NULL);
    name_kb_btn(s_name_kb, "n", KX(6), KY(3), KW, KH, [](lv_event_t* e){ name_kb_type('n'); }, NULL);
    name_kb_btn(s_name_kb, "m", KX(7), KY(3), KW, KH, [](lv_event_t* e){ name_kb_type('m'); }, NULL);
    int bs_x = KX(8) + 2, bs_w = 480 - 8 - KX(8) - 2;
    name_kb_btn(s_name_kb, "退格", bs_x, KY(3), bs_w, KH, [](lv_event_t* e){ name_kb_backspace(); }, NULL);
    // 行5: 功能键
    name_kb_btn(s_name_kb, "取消", 8, 260, 90, KH, [](lv_event_t* e){ name_kb_close(); }, NULL);
    name_kb_btn(s_name_kb, "空格", 101, 260, 90, KH, [](lv_event_t* e){ name_kb_type(' '); }, NULL);
    name_kb_btn(s_name_kb, "_", 194, 260, 90, KH, [](lv_event_t* e){ name_kb_type('_'); }, NULL);
    name_kb_btn(s_name_kb, "-", 287, 260, 90, KH, [](lv_event_t* e){ name_kb_type('-'); }, NULL);
    name_kb_btn(s_name_kb, "清空", 382, 260, 90, KH, [](lv_event_t* e){
        s_name_len = 0;
        s_name_buf[0] = '\0';
        name_kb_update_label();
    }, NULL);
    #undef KX
    #undef KY
    #undef KW
    #undef KH
}

// ── 保存（命名后保存）──
static void save_current(void) {
    name_kb_open();
}

// ── 一级编辑器 UI ──
static void l1_editor_show(void) {
    s_edit_level = 1;
    if (s_edit_art) { beads_free(s_edit_art); s_edit_art = NULL; }
    s_edit_art = beads_create(BEADS_L1_SIZE, BEADS_L1_SIZE);
    if (!s_edit_art) return;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 480, 800);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x1E1E22), 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(s_overlay);
    lv_label_set_text(title, "拼豆 24×24");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // 画布：456px 最大，每格 19px（缓冲在 refresh_canvas 中按实际尺寸设置）
    s_board_buf = (uint16_t*)heap_caps_malloc(456 * 456 * 2, MALLOC_CAP_SPIRAM);
    s_canvas = lv_canvas_create(s_overlay);
    lv_obj_set_style_bg_color(s_canvas, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);  // 接收触摸
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    refresh_canvas();

    // 画布触摸 → 画格子
    lv_obj_add_event_cb(s_canvas, [](lv_event_t* e) {
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        lv_obj_t* cv = lv_event_get_target_obj(e);
        lv_area_t a;
        lv_obj_get_coords(cv, &a);
        int rel_x = pt.x - a.x1;
        int rel_y = pt.y - a.y1;
        int cell = 456 / s_edit_art->w;
        int gx = rel_x / cell;
        int gy = rel_y / cell;
        if (s_tool == 0) draw_pixel(s_edit_art, gx, gy, s_color);
        else draw_pixel(s_edit_art, gx, gy, 0);
        refresh_canvas();
    }, LV_EVENT_CLICKED, NULL);

    // 调色板 6×6
    int sw = 26;  // 色块大小
    int gap = 4;
    int pal_x = (480 - 6 * (sw + gap)) / 2;
    int pal_y = 500;
    for (int i = 0; i < BEADS_COLORS; i++) {
        int cx = i % 6, cy = i / 6;
        lv_obj_t* sw_btn = lv_btn_create(s_overlay);
        lv_obj_set_size(sw_btn, sw, sw);
        lv_obj_set_pos(sw_btn, pal_x + cx * (sw + gap), pal_y + cy * (sw + gap));
        uint16_t c565 = g_beads_palette[i];
        uint32_t r8 = (uint32_t)(((c565 >> 11) & 0x1F) << 3);
        uint32_t g8 = (uint32_t)(((c565 >> 5) & 0x3F) << 2);
        uint32_t b8 = (uint32_t)((c565 & 0x1F) << 3);
        lv_obj_set_style_bg_color(sw_btn,
            lv_color_hex((r8 << 16) | (g8 << 8) | b8), 0);
        lv_obj_set_style_radius(sw_btn, 3, 0);
        lv_obj_set_style_border_color(sw_btn, lv_color_hex(0xFF4444), 0);
        s_pal_btns[i] = sw_btn;
        int color_idx = i + 1;
        lv_obj_add_event_cb(sw_btn, [](lv_event_t* e) {
            s_color = (int)(uintptr_t)lv_event_get_user_data(e);
            palette_refresh_highlight();
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)color_idx);
    }
    palette_refresh_highlight();

    // 工具行（与底部行对齐：x=45 + i*140）
    const char* tool_names[] = {"色笔", "橡皮", "清空"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* tb = lv_btn_create(s_overlay);
        lv_obj_set_size(tb, 110, 36);
        lv_obj_set_pos(tb, 45 + i * 140, 706);
        lv_obj_set_style_bg_color(tb, s_tool == i ? lv_color_hex(0x4477AA) : lv_color_hex(0x333338), 0);
        lv_obj_t* tl = lv_label_create(tb);
        lv_label_set_text(tl, tool_names[i]);
        lv_obj_set_style_text_color(tl, lv_color_white(), 0);
        lv_obj_set_style_text_font(tl, s_font, 0);
        lv_obj_center(tl);
        int tool_idx = i;
        lv_obj_add_event_cb(tb, [](lv_event_t* e) {
            s_tool = (int)(uintptr_t)lv_event_get_user_data(e);
            if (s_tool == 2) {  // 清空
                memset(s_edit_art->pixels, 0, (size_t)s_edit_art->w * s_edit_art->h);
                refresh_canvas();
            }
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)tool_idx);
    }

    // 底部行（与工具行对齐：x=45 + i*140）
    const char* act_names[] = {"保存", "加载", "返回"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* ab = lv_btn_create(s_overlay);
        lv_obj_set_size(ab, 110, 40);
        lv_obj_set_pos(ab, 45 + i * 140, 748);
        lv_obj_set_style_bg_color(ab, lv_color_hex(0x333338), 0);
        lv_obj_t* al = lv_label_create(ab);
        lv_label_set_text(al, act_names[i]);
        lv_obj_set_style_text_color(al, lv_color_white(), 0);
        lv_obj_set_style_text_font(al, s_font, 0);
        lv_obj_center(al);
        int act = i;
        lv_obj_add_event_cb(ab, [](lv_event_t* e) {
            int a = (int)(uintptr_t)lv_event_get_user_data(e);
            if (a == 0) save_current();
            else if (a == 1) picker_open(1, NULL);  // 打开文件选择器
            else if (a == 2) beads_main_show();  // 返回主页面（重建）
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)act);
    }
}

// ── 合成模式 UI（二级/三级共用）──
static void compose_show(int level) {
    s_edit_level = level;
    if (s_edit_art) { beads_free(s_edit_art); s_edit_art = NULL; }
    int size = (level == 2) ? BEADS_L2_SIZE : BEADS_L3_SIZE;
    s_edit_art = beads_create(size, size);
    if (!s_edit_art) return;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 480, 800);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x1E1E22), 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    char title_txt[32];
    snprintf(title_txt, sizeof(title_txt), "拼豆 %d级合成", level);
    lv_obj_t* title = lv_label_create(s_overlay);
    lv_label_set_text(title, title_txt);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // 中央 3×3 板子（456px 最大，缓冲在 refresh_canvas 中按实际尺寸设置）
    s_board_buf = (uint16_t*)heap_caps_malloc(456 * 456 * 2, MALLOC_CAP_SPIRAM);
    s_canvas = lv_canvas_create(s_overlay);
    lv_obj_set_style_bg_color(s_canvas, lv_color_hex(0xF0F0F0), 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);  // 接收触摸
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    refresh_canvas();

    // 板子触摸：把选中作品放进 3×3 格子
    lv_obj_add_event_cb(s_canvas, [](lv_event_t* e) {
        if (s_selected_file < 0) return;
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        lv_obj_t* cv = lv_event_get_target_obj(e);
        lv_area_t a;
        lv_obj_get_coords(cv, &a);
        int rel_x = pt.x - a.x1;
        int rel_y = pt.y - a.y1;
        int disp_size = s_edit_art->w * (456 / s_edit_art->w);  // 实际显示尺寸
        int cell = disp_size / 3;
        int gx = rel_x / cell;
        int gy = rel_y / cell;
        if (gx < 0 || gx > 2 || gy < 0 || gy > 2) return;
        // 加载选中文件，复制到对应格子
        char path[128];
        snprintf(path, sizeof(path), "/sdcard/User/Beads/%s", s_file_list[s_selected_file]);
        BeadArt* piece = beads_load(path);
        if (!piece) return;
        int piece_size = piece->w;
        int cell_px = s_edit_art->w / 3;  // L2: 24, L3: 72
        for (int y = 0; y < piece_size && y < cell_px; y++) {
            for (int x = 0; x < piece_size && x < cell_px; x++) {
                draw_pixel(s_edit_art, gx * cell_px + x, gy * cell_px + y,
                           piece->pixels[y * piece->w + x]);
            }
        }
        beads_free(piece);
        refresh_canvas();
        s_selected_file = -1;
    }, LV_EVENT_CLICKED, NULL);

    // 右侧作品列表
    int sub_level = level - 1;
    int n = beads_list_files(sub_level, s_file_list, 128);
    s_file_count = n;
    lv_obj_t* list = lv_obj_create(s_overlay);
    lv_obj_set_size(list, 480, 130);
    lv_obj_set_pos(list, 0, 516);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x2A2A30), 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int i = 0; i < n; i++) {
        lv_obj_t* lb = lv_btn_create(list);
        lv_obj_set_size(lb, 72, 28);
        lv_obj_set_style_bg_color(lb, lv_color_hex(0x333338), 0);
        lv_obj_t* ll = lv_label_create(lb);
        lv_label_set_text(ll, s_file_list[i] + 3);  // 去掉 B1_ 前缀
        lv_obj_set_style_text_color(ll, lv_color_white(), 0);
        lv_obj_set_style_text_font(ll, s_font, 0);
        lv_obj_center(ll);
        int fi = i;
        lv_obj_add_event_cb(lb, [](lv_event_t* e) {
            s_selected_file = (int)(uintptr_t)lv_event_get_user_data(e);
            ESP_LOGI(TAG, "selected %s", s_file_list[s_selected_file]);
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)fi);
    }

    // 底部
    const char* act_names[] = {"保存", "加载", "清空", "返回"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* ab = lv_btn_create(s_overlay);
        lv_obj_set_size(ab, 110, 40);
        lv_obj_set_pos(ab, 20 + i * 115, 660);
        lv_obj_set_style_bg_color(ab, lv_color_hex(0x333338), 0);
        lv_obj_t* al = lv_label_create(ab);
        lv_label_set_text(al, act_names[i]);
        lv_obj_set_style_text_color(al, lv_color_white(), 0);
        lv_obj_set_style_text_font(al, s_font, 0);
        lv_obj_center(al);
        int act = i;
        lv_obj_add_event_cb(ab, [](lv_event_t* e) {
            int a = (int)(uintptr_t)lv_event_get_user_data(e);
            if (a == 0) save_current();
            else if (a == 1) picker_open(s_edit_level, NULL);  // 加载同级别成品文件
            else if (a == 2) {
                memset(s_edit_art->pixels, 0, (size_t)s_edit_art->w * s_edit_art->h);
                refresh_canvas();
            }
            else if (a == 3) beads_main_show();
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)act);
    }
}

// ── 主页面 ──
void beads_main_show(void) {
    // 清理旧页面
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    if (s_board_buf) { heap_caps_free(s_board_buf); s_board_buf = NULL; }
    s_canvas = NULL;
    if (s_edit_art) { beads_free(s_edit_art); s_edit_art = NULL; }
    s_selected_file = -1;
    memset(s_pal_btns, 0, sizeof(s_pal_btns));

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 480, 800);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x1E1E22), 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_overlay);
    lv_label_set_text(title, "一起拼豆");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    const char* modes[] = {"新建 24×24", "二级合成 3×3", "三级合成 3×3"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* mb = lv_btn_create(s_overlay);
        lv_obj_set_size(mb, 300, 70);
        lv_obj_set_pos(mb, 90, 100 + i * 90);
        lv_obj_set_style_bg_color(mb, lv_color_hex(0x33333A), 0);
        lv_obj_set_style_radius(mb, 10, 0);
        lv_obj_t* ml = lv_label_create(mb);
        lv_label_set_text(ml, modes[i]);
        lv_obj_set_style_text_color(ml, lv_color_white(), 0);
        lv_obj_set_style_text_font(ml, s_font, 0);
        lv_obj_center(ml);
        int mode = i;
        lv_obj_add_event_cb(mb, [](lv_event_t* e) {
            int m = (int)(uintptr_t)lv_event_get_user_data(e);
            if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
            if (m == 0) l1_editor_show();
            else compose_show(m + 1);  // m=1 → L2, m=2 → L3
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)mode);
    }

    // 返回按钮
    lv_obj_t* back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 120, 44);
    lv_obj_set_pos(back, 180, 420);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x444455), 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, "返回");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, s_font, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t* e) {
        beads_main_hide();
    }, LV_EVENT_CLICKED, NULL);
}

void beads_main_hide(void) {
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    if (s_board_buf) { heap_caps_free(s_board_buf); s_board_buf = NULL; }
    s_canvas = NULL;
    if (s_edit_art) { beads_free(s_edit_art); s_edit_art = NULL; }
    memset(s_pal_btns, 0, sizeof(s_pal_btns));
    extern void ppd_interaction_resume_after_app(void);
    ppd_interaction_resume_after_app();   // 从 PPD 交互经菜单打开时：回 PPD 待机
}

// 字体设置（从 ImageDisplay 传入）
void beads_set_font(const lv_font_t* font) {
    s_font = font;
}
