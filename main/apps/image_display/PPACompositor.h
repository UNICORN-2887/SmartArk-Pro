#pragma once
#include <stdint.h>
#include <stdbool.h>

// 初始化PPA和缓冲区
bool ppa_init(void);

// 加载静态背景图片 (JPEG, 480x800)
bool ppa_load_background(const char *path);
// 卸载背景（释放 PSRAM，切换为直通模式）
void ppa_unload_background(void);
bool ppa_has_background(void);

// 预加载帧到PSRAM（逐文件模式，消除fopen延迟）
void ppa_preload_frames(const char *paths[], int count);

// 获取已预加载的帧数
int ppa_get_cache_count(void);

// 从MJPEG文件预加载（同步，阻塞显示~130ms）
int ppa_preload_mjpeg(const char *path);

// 异步预加载到后备缓冲区（后台任务，不阻塞显示）
void ppa_preload_mjpeg_async(const char *path);

// 交换活跃/后备缓冲区（<1ms，零SD访问）
// 返回新帧数，若后备未就绪返回0
int ppa_swap_emotion(void);

// 打开MJPEG文件（内存高效模式，fseek读取）
bool ppa_open_mjpeg(const char *path, int *out_frame_count);
// 关闭 MJPEG fseek 模式
void ppa_close_mjpeg(void);

// 解码前景帧并PPA抠图合成
uint8_t* ppa_composite_frame(int frame_index);

// 获取上次解码的图片高度（用于 >800px 图片底部对齐裁切）
int ppa_get_last_decoded_height(void);
// 释放所有资源
void ppa_deinit(void);
