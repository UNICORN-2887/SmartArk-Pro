#ifndef SETTINGS_UI_H
#define SETTINGS_UI_H

#include <lvgl.h>

// 设置页 overlay：音频转发开关/本地静音/音量/亮度/连接状态
void settings_ui_init(const lv_font_t* font);
void settings_ui_show(void);
void settings_ui_hide(void);
bool settings_ui_is_open(void);

#endif
