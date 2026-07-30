#pragma once
#include <stdint.h>
#include <stdbool.h>

// 初始化PPA和缓冲区
bool ppa_init(void);

// 加载静态背景图片 (JPEG, 480x800)
bool ppa_load_background(const char *path);

// 预加载帧到PSRAM（逐文件模式，消除fopen延迟）
void ppa_preload_frames(const char *paths[], int count);

// 获取已预加载的帧数
int ppa_get_cache_count(void);

// 从MJPEG文件预加载（1次fopen + N次顺序fread，比逐文件快3倍）
int ppa_preload_mjpeg(const char *path);

// 打开MJPEG文件（内存高效模式，fseek读取，~1.5MB RAM）
bool ppa_open_mjpeg(const char *path, int *out_frame_count);

// 解码前景帧并PPA抠图合成（自动选择MJPEG或缓存模式）
uint8_t* ppa_composite_frame(int frame_index);

// 释放所有资源
void ppa_deinit(void);
