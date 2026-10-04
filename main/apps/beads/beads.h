/** 拼豆像素画编辑器 — 24×24 基础格，3×3 二级/三级合成。
 *  存储：/sdcard/User/Beads/B{level}_{name}.bead，只存展开像素数据。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct lv_font_t lv_font_t;  // 前向声明（与 LVGL 9 定义一致）

#define BEADS_L1_SIZE 24       // 一级 24×24
#define BEADS_L2_SIZE 72       // 二级 3×24
#define BEADS_L3_SIZE 216      // 三级 9×24
#define BEADS_MAX_SIZE BEADS_L3_SIZE
#define BEADS_COLORS 36

// ── 36 色水彩笔色板（RGB565）──
extern const uint16_t g_beads_palette[BEADS_COLORS];

// ── 画作数据 ──
typedef struct {
    int w, h;                       // 尺寸（24/72/216）
    uint8_t* pixels;                // w*h 字节，每字节=色号+1（0=空），值 1..36
} BeadArt;

// ── API ──
BeadArt* beads_create(int w, int h);            // 全空画作
void beads_free(BeadArt* art);
bool beads_save(BeadArt* art, const char* name, int level);  // 存 /sdcard/User/Beads/
BeadArt* beads_load(const char* filepath);      // 加载（自动识别级别）
bool beads_delete(const char* filename);       // 删除 /sdcard/User/Beads/<filename>
int beads_list_files(int level, char out[][64], int max_n);  // 列出某级别文件，返回数量

// ── UI 入口 ──
void beads_main_show(void);   // 拼豆主页面（选择模式）
void beads_main_hide(void);
void beads_set_font(const lv_font_t* font);
