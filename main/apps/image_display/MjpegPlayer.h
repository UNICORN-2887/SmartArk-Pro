#pragma once
#include <stdint.h>
#include <stdbool.h>

// 打开MJPEG文件，读取偏移表
// 返回true表示成功，之后可用mjpeg_get_frame()逐帧读取
bool mjpeg_open(const char *path);

// 获取第index帧的JPEG数据和大小（从MJPEG文件中fseek+fread）
// jpeg_data: 输出JPEG数据指针（调用者负责释放）
// jpeg_size: 输出数据大小
bool mjpeg_get_frame(int index, uint8_t **jpeg_data, size_t *jpeg_size);

// 获取总帧数
int mjpeg_get_frame_count(void);

// 关闭文件，释放资源
void mjpeg_close(void);
