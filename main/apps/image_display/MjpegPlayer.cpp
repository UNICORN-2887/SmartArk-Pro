/*
 * MJPEG Player — reads concatenated JPEG frames from a single file via fseek
 * Memory: offset table (~480B for 120 frames) + 512KB read-ahead buffer
 * No SDMMC conflict: uses standard VFS fopen/fseek/fread after mount
 *
 * 读放大（read amplification）：顺序播放时一次 fread 512KB 大块覆盖 ~7 帧，
 * 帧数据零拷贝指向预读缓冲内部——小 fread 的 SD 协议开销是逐帧读的主要耗时
 *（standee 横屏立牌 7fps 排查结论：65KB/帧的小请求把 SD 带宽利用率拉低到 1MB/s 级）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "MjpegPlayer.h"

#define TAG "MjpegPlayer"
#define MAX_FRAMES 256
#define RA_BUF_SIZE (512 * 1024)   // 预读大块：单帧上限也以此为界

static FILE *s_mjpeg_fp = NULL;
static uint32_t s_frame_count = 0;
static uint32_t s_frame_offsets[MAX_FRAMES];
static int s_last_index = -1;    // 上次读取的帧号（用于顺序读取优化）
static uint8_t *s_ra_buf = NULL;      // 预读缓冲（懒分配，close 不释放——跨 open 复用）
static size_t s_ra_pos = 0;           // 预读缓冲内当前帧起始偏移
static size_t s_ra_size = 0;          // 缓冲内有效字节数
static uint32_t s_ra_file_start = 0;  // 缓冲内容对应的文件偏移


bool mjpeg_open(const char *path)
{
    mjpeg_close();

    s_mjpeg_fp = fopen(path, "rb");
    if (!s_mjpeg_fp) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    // Read header: [frame_count][offsets...]
    if (fread(&s_frame_count, 4, 1, s_mjpeg_fp) != 1 || s_frame_count > MAX_FRAMES) {
        ESP_LOGE(TAG, "Bad header: count=%lu", s_frame_count);
        mjpeg_close();
        return false;
    }

    for (int i = 0; i < (int)s_frame_count; i++) {
        if (fread(&s_frame_offsets[i], 4, 1, s_mjpeg_fp) != 1) {
            ESP_LOGE(TAG, "Failed to read offset %d", i);
            mjpeg_close();
            return false;
        }
    }

    s_last_index = -1;
    s_ra_pos = s_ra_size = 0;
    ESP_LOGI(TAG, "Opened %s: %lu frames", path, s_frame_count);
    return true;
}

bool mjpeg_get_frame(int index, uint8_t **jpeg_data, size_t *jpeg_size)
{
    if (!s_mjpeg_fp || index < 0 || index >= (int)s_frame_count) return false;

    size_t jpg_start = s_frame_offsets[index];
    size_t jpg_end;
    if (index < (int)s_frame_count - 1) {
        jpg_end = s_frame_offsets[index + 1];
    } else {
        fseek(s_mjpeg_fp, 0, SEEK_END);
        jpg_end = ftell(s_mjpeg_fp);
    }
    size_t size = jpg_end - jpg_start;
    if (size == 0 || size > RA_BUF_SIZE) return false;

    // 懒分配预读缓冲（PSRAM），后续复用
    if (!s_ra_buf) {
        s_ra_buf = (uint8_t*)heap_caps_malloc(RA_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_ra_buf) return false;
    }

    // 命中预读缓冲：顺序播放 + 帧完全在缓冲内 → 零拷贝直接返回内部指针
    bool hit = (index == s_last_index + 1) &&
               (jpg_start >= s_ra_file_start) &&
               (jpg_start + size <= s_ra_file_start + s_ra_size);
    if (!hit) {
        // 未命中：fseek 到大块起点 + 一次大 fread 预读（覆盖后续 ~7 帧）
        if (fseek(s_mjpeg_fp, jpg_start, SEEK_SET) != 0) {
            s_last_index = -1;
            return false;
        }
        s_ra_size = fread(s_ra_buf, 1, RA_BUF_SIZE, s_mjpeg_fp);
        s_ra_file_start = jpg_start;
        s_ra_pos = 0;
        if (s_ra_size < size) {
            s_last_index = -1;
            return false;
        }
    }

    *jpeg_data = s_ra_buf + (jpg_start - s_ra_file_start);
    s_ra_pos = (size_t)(jpg_start - s_ra_file_start) + size;   // 下一帧起点
    s_last_index = index;
    *jpeg_size = size;
    return true;
}

int mjpeg_get_frame_count(void) { return (int)s_frame_count; }

void mjpeg_close(void)
{
    if (s_mjpeg_fp) { fclose(s_mjpeg_fp); s_mjpeg_fp = NULL; }
    s_frame_count = 0;
    s_last_index = -1;
    s_ra_pos = s_ra_size = 0;
}
