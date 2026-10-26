/* 交友雷达页 UI(2026-10-22) */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 显示雷达页(全屏覆盖层);已显示返回 0,失败 -1。 */
int radar_ui_show(void);

/* 关闭雷达页。 */
void radar_ui_hide(void);

#ifdef __cplusplus
}
#endif
