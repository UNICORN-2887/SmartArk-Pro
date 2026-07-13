/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <algorithm>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "driver/jpeg_decode.h"

#define TAG "AppImageDisplay"

#define APP_SUPPORT_IMAGE_FILE_EXT ".jpg"
#define SD_MOUNT_POINT            "/sdcard"
#define APP_IMAGE_FRAME_BUF_SIZE  (480 * 800 * 2)  // 适配当前屏幕 480x800
#define APP_CACHE_BUF_SIZE        (64 * 1024)

static jpeg_decoder_handle_t jpgd_handle = NULL;
static jpeg_decode_engine_cfg_t decode_eng_cfg = {
    .timeout_ms = 40,
};
static jpeg_decode_cfg_t decode_cfg_rgb = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};
static jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
    .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
};
static jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
    .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
};

static lv_obj_t *s_image_canvas = NULL;
static uint8_t *s_output_buf = NULL;
static size_t s_output_buf_size = 0;
static int s_current_index = 0;
static int s_image_count = 0;
static char s_image_paths[64][300];

// 解码并显示图片
static bool decode_and_display_image(const char *image_path)
{
    if (!image_path) {
        return false;
    }

    ESP_LOGI(TAG, "Displaying image: %s", image_path);

    // 打开文件
    FILE *image_fp = fopen(image_path, "rb");
    if (image_fp == NULL) {
        ESP_LOGE(TAG, "Failed to open image file: %s", image_path);
        return false;
    }

    // 获取文件大小
    fseek(image_fp, 0, SEEK_END);
    int image_size = ftell(image_fp);
    fseek(image_fp, 0, SEEK_SET);

    if (image_size <= 0) {
        ESP_LOGE(TAG, "Invalid image file size");
        fclose(image_fp);
        return false;
    }

    // 分配输入缓冲区
    size_t input_buffer_size = 0;
    uint8_t *input_buf = (uint8_t *)jpeg_alloc_decoder_mem(image_size, &tx_mem_cfg, &input_buffer_size);
    if (input_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate input buffer");
        fclose(image_fp);
        return false;
    }

    // 读取文件
    fread(input_buf, 1, input_buffer_size, image_fp);
    fclose(image_fp);

    // 获取图片信息
    jpeg_decode_picture_info_t image_info;
    esp_err_t ret = jpeg_decoder_get_info(input_buf, input_buffer_size, &image_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get JPEG info");
        free(input_buf);
        return false;
    }

    ESP_LOGI(TAG, "Image: %dx%d", image_info.width, image_info.height);

    // 分配输出缓冲区（如果需要）
    size_t required_size = image_info.width * image_info.height * 2;
    if (s_output_buf == NULL || s_output_buf_size < required_size) {
        if (s_output_buf) {
            free(s_output_buf);
        }
        s_output_buf = (uint8_t *)jpeg_alloc_decoder_mem(required_size, &rx_mem_cfg, &s_output_buf_size);
        if (s_output_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate output buffer");
            free(input_buf);
            return false;
        }
    }

    // 解码图片
    uint32_t out_size = 0;
    
    // 删除旧的解码器
    if (jpgd_handle) {
        jpeg_del_decoder_engine(jpgd_handle);
    }
    
    // 创建新的解码器
    ret = jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder");
        free(input_buf);
        return false;
    }

    // 解码
    ret = jpeg_decoder_process(jpgd_handle, &decode_cfg_rgb, 
                               input_buf, input_buffer_size, 
                               s_output_buf, s_output_buf_size, 
                               &out_size);
    free(input_buf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed");
        jpeg_del_decoder_engine(jpgd_handle);
        jpgd_handle = NULL;
        return false;
    }

    // 显示图片
    lvgl_port_lock(0);
    if (s_image_canvas) {
        lv_canvas_set_buffer(s_image_canvas, s_output_buf, 
                            image_info.width, image_info.height, 
                            LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s_image_canvas);
    }
    lvgl_port_unlock();

    return true;
}

// 显示指定索引的图片
bool display_image_by_index(int index)
{
    if (index < 0 || index >= s_image_count) {
        ESP_LOGE(TAG, "Invalid image index: %d (total: %d)", index, s_image_count);
        return false;
    }

    s_current_index = index;
    return decode_and_display_image(s_image_paths[index]);
}

// 查找SD卡中的图片文件
static int search_image_files(void)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        ESP_LOGE(TAG, "Failed to open directory: %s", SD_MOUNT_POINT);
        return 0;
    }

    s_image_count = 0;
    struct dirent *dir;
    
    while ((dir = readdir(d)) != NULL && s_image_count < APP_CACHE_BUF_SIZE) {
        if (dir->d_type != DT_DIR) {
            const char *ext = strrchr(dir->d_name, '.');
            if (ext && (strcasecmp(ext, ".jpg") == 0 || 
                       strcasecmp(ext, ".jpeg") == 0 ||
                       strcasecmp(ext, ".bmp") == 0 ||
                       strcasecmp(ext, ".png") == 0)) {
                snprintf(s_image_paths[s_image_count], sizeof(s_image_paths[0]), 
                        "%s/%s", SD_MOUNT_POINT, dir->d_name);
                ESP_LOGI(TAG, "Found image: %s", dir->d_name);
                s_image_count++;
            }
        }
    }
    
    closedir(d);
    ESP_LOGI(TAG, "Total images found: %d", s_image_count);
    return s_image_count;
}

// 初始化图片显示
bool image_display_init(void)
{
    ESP_LOGI(TAG, "Initializing image display...");

    // 查找图片文件
    if (search_image_files() == 0) {
        ESP_LOGW(TAG, "No images found on SD card");
        return false;
    }

    // 创建显示画布
    lvgl_port_lock(0);
    s_image_canvas = lv_canvas_create(lv_scr_act());
    lv_obj_set_pos(s_image_canvas, 0, 0);
    // 设置黑色背景
    static lv_style_t canvas_style;
    lv_style_init(&canvas_style);
    lv_style_set_bg_color(&canvas_style, lv_color_black());
    lv_obj_add_style(s_image_canvas, &canvas_style, 0);
    lvgl_port_unlock();

    // 显示第一张图片
    s_current_index = 0;
    return decode_and_display_image(s_image_paths[0]);
}

// 显示下一张图片
bool image_display_next(void)
{
    if (s_image_count == 0) {
        return false;
    }
    s_current_index = (s_current_index + 1) % s_image_count;
    return display_image_by_index(s_current_index);
}

// 显示上一张图片
bool image_display_prev(void)
{
    if (s_image_count == 0) {
        return false;
    }
    s_current_index = (s_current_index - 1 + s_image_count) % s_image_count;
    return display_image_by_index(s_current_index);
}

// 显示指定名称的图片
bool image_display_by_name(const char *filename)
{
    if (!filename || s_image_count == 0) {
        return false;
    }

    char target_path[256];
    snprintf(target_path, sizeof(target_path), "%s/%s", SD_MOUNT_POINT, filename);

    for (int i = 0; i < s_image_count; i++) {
        if (strcmp(s_image_paths[i], target_path) == 0) {
            return display_image_by_index(i);
        }
    }
    return false;
}

// ==================== 视频播放 ====================
static TaskHandle_t s_video_task = NULL;
static int s_video_fps = 30;
static bool s_video_running = false;
static int s_frame_count = 0;
static int s_fps_display = 0;
static int64_t s_last_fps_time = 0;

static void video_playback_task(void *arg)
{
    s_video_running = true;
    s_frame_count = 0;
    s_last_fps_time = esp_timer_get_time();

    while (s_video_running) {
        int64_t frame_start = esp_timer_get_time();

        // 显示下一帧
        if (!image_display_next()) {
            // 循环回到第一帧
            s_current_index = 0;
        }

        s_frame_count++;

        // 每秒统计一次FPS
        int64_t now = esp_timer_get_time();
        if (now - s_last_fps_time >= 1000000) {
            s_fps_display = s_frame_count;
            s_frame_count = 0;
            s_last_fps_time = now;
            ESP_LOGI(TAG, "Video FPS: %d (target: %d)", s_fps_display, s_video_fps);
        }

        // 帧率控制
        int64_t frame_time = esp_timer_get_time() - frame_start;
        int32_t wait_ms = (1000 / s_video_fps) - (frame_time / 1000);
        if (wait_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    s_video_task = NULL;
    vTaskDelete(NULL);
}

bool video_playback_start(int fps)
{
    if (s_image_count == 0) {
        ESP_LOGW(TAG, "No images to play");
        return false;
    }
    if (s_video_running) {
        ESP_LOGW(TAG, "Video already playing");
        return false;
    }

    s_video_fps = (fps > 0 && fps <= 120) ? fps : 30;
    ESP_LOGI(TAG, "Starting video playback at %d FPS (total: %d images)", s_video_fps, s_image_count);

    xTaskCreate(video_playback_task, "video_play", 4096, NULL, 5, &s_video_task);
    return true;
}

void video_playback_stop(void)
{
    s_video_running = false;
}

int video_get_fps(void)
{
    return s_fps_display;
}

// 清理图片显示
void image_display_cleanup(void)
{
    if (jpgd_handle) {
        jpeg_del_decoder_engine(jpgd_handle);
        jpgd_handle = NULL;
    }
    
    if (s_output_buf) {
        free(s_output_buf);
        s_output_buf = NULL;
        s_output_buf_size = 0;
    }

    if (s_image_canvas) {
        lvgl_port_lock(0);
        lv_obj_del(s_image_canvas);
        s_image_canvas = NULL;
        lvgl_port_unlock();
    }

    s_image_count = 0;
    s_current_index = 0;
}

// 获取当前图片索引
int image_display_get_current_index(void)
{
    return s_current_index;
}

// 获取图片总数
int image_display_get_count(void)
{
    return s_image_count;
}
