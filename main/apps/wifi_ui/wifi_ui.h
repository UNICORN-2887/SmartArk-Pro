#pragma once
/* 屏幕 WiFi 列表页(2026-09-20):开机连接失败/无已存 WiFi 时显示。
 * 手机式交互:扫描列表(信号/加密/已存✓)→ 点选 → 密码键盘 → 连接 → 存 NVS。
 * wifi_ui_show 阻塞至用户完成:返回 true=已连接,false=用户跳过(调用方走 AP 兜底)。
 * 纯 C++ 调用方,不走 C 链接。 */
#include "lvgl.h"

void wifi_ui_init(const lv_font_t *font);
bool wifi_ui_show(void);
