#pragma once
#include <stdint.h>
#include <stdbool.h>

// 初始化PPA和缓冲区
bool ppa_init(void);

// 加载静态背景图片 (JPEG, 480x800)
bool ppa_load_background(const char *path);

// 预加载帧到PSRAM（消除SD卡IO延迟）
void ppa_preload_frames(const char *paths[], int count);
void ppa_free_cache(void);

// 解码前景帧并PPA抠图合成，返回RGB565缓冲区指针（从缓存读取）
uint8_t* ppa_composite_frame(int frame_index);

// 释放所有资源
void ppa_deinit(void);
