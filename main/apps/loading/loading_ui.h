/** 全屏加载动画覆盖层（进度条 + 旋转环 + 阶段文字）。
 *  可从任意任务调用，内部 lvgl_port_lock 保证线程安全；单实例，内置 15s 超时兜底。
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 创建/复用覆盖层（lv_layer_top，全屏，吃触摸）。已显示时更新标题并重置超时。返回 false=锁失败
bool loading_show(const char* title);
// 更新阶段文字与进度：percent 0-100 → 进度条+百分比；-1 → 旋转环（不确定阶段）
void loading_set_stage(const char* text, int percent);
// 隐藏并销毁（幂等，多处调用安全）
void loading_hide(void);
// 把覆盖层移到 lv_layer_top 最前（Live2D canvas 等晚创建的同层对象会盖住它）
void loading_raise(void);
bool loading_is_active(void);
// 中文字体（默认 LVGL 内置字体；由 application.cc 传入板级中文字体）
void loading_set_font(const void* font);

#ifdef __cplusplus
}
#endif
