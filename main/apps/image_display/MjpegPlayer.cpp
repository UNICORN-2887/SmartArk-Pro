/*
 * MJPEG Player — reads concatenated JPEG frames from a single file via fseek
 * Memory: offset table (~480B for 120 frames) + 2 decode buffers (~1.5MB)
 * No SDMMC conflict: uses standard VFS fopen/fseek/fread after mount
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "MjpegPlayer.h"

#define TAG "MjpegPlayer"
#define MAX_FRAMES 256

static FILE *s_mjpeg_fp = NULL;
static uint32_t s_frame_count = 0;
static uint32_t s_frame_offsets[MAX_FRAMES];

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

    ESP_LOGI(TAG, "Opened %s: %lu frames", path, s_frame_count);
    return true;
}

bool mjpeg_get_frame(int index, uint8_t **jpeg_data, size_t *jpeg_size)
{
    if (!s_mjpeg_fp || index < 0 || index >= (int)s_frame_count) return false;

    // Calculate JPEG size from next frame offset (or EOF for last frame)
    size_t jpg_start = s_frame_offsets[index];
    size_t jpg_end;
    if (index < (int)s_frame_count - 1) {
        jpg_end = s_frame_offsets[index + 1];
    } else {
        fseek(s_mjpeg_fp, 0, SEEK_END);
        jpg_end = ftell(s_mjpeg_fp);
    }
    size_t size = jpg_end - jpg_start;
    if (size == 0 || size > 512 * 1024) return false; // sanity check

    *jpeg_data = (uint8_t*)malloc(size);
    if (!*jpeg_data) return false;

    fseek(s_mjpeg_fp, jpg_start, SEEK_SET);
    if (fread(*jpeg_data, 1, size, s_mjpeg_fp) != size) {
        free(*jpeg_data);
        return false;
    }

    *jpeg_size = size;
    return true;
}

int mjpeg_get_frame_count(void) { return (int)s_frame_count; }

void mjpeg_close(void)
{
    if (s_mjpeg_fp) { fclose(s_mjpeg_fp); s_mjpeg_fp = NULL; }
    s_frame_count = 0;
}
