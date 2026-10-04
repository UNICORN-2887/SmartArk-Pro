#ifndef MENU_UI_H
#define MENU_UI_H

#include <lvgl.h>

// 全屏菜单页：2 列网格大按钮，收纳次要功能入口
// （一起拼豆 / 蟑螂派对 / 语音记录 / 背景音乐 / Live2D测试 / 弹出键盘）
// landscape=true：横屏菜单（800×480 虚拟页整体转 90°，仅 4 按钮，无测试按钮）
void menu_ui_init(const lv_font_t* font);
void menu_ui_show(bool cover_mode, bool landscape);
void menu_ui_hide(void);
bool menu_ui_is_open(void);

#endif
