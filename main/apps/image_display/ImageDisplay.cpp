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
#include "PPACompositor.h"

#define TAG "AppImageDisplay"

#define SD_MOUNT_POINT            "/sdcard"

static lv_obj_t *s_image_canvas = NULL;
static uint8_t *s_output_buf = NULL;
static size_t s_output_buf_size = 0;
static int s_current_index = 0;
static int s_image_count = 0;
static char s_image_paths[256][300];

// 展示模式 vs 交互模式
static bool s_cover_mode = false;
static bool s_first_cover = true;
static char s_agent_path[256] = {0};
static volatile bool s_force_swap = false;  // LLM 抢占式切换标志
static int s_loop_count = 0;               // 非 neutral 表情已循环次数
static char s_current_emotion[32] = {0};   // 当前表情名
static char s_pending_emotion[32] = {0};   // 后备表情名

// 前向声明（定义在后面）
void video_playback_stop(void);
bool video_playback_start(int fps);
void chat_overlay_show(bool show);

// PPA抠图合成+显示（frame_index，自动MJPEG或缓存模式）
static bool decode_and_display_image(int frame_index)
{
    uint8_t *comp_buf = ppa_composite_frame(frame_index);
    if (!comp_buf) return false;

    // Cover 直通模式：图片可能高于屏幕（如 840px），底部对齐裁切顶部黑边
    if (!ppa_has_background()) {
        int img_h = ppa_get_last_decoded_height();
        if (img_h > 800) {
            int skip = (img_h - 800) * 480 * 2;  // RGB565
            comp_buf += skip;
        }
    }

    lvgl_port_lock(0);
    if (s_image_canvas) {
        lv_canvas_set_buffer(s_image_canvas, comp_buf, 480, 800, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s_image_canvas);
    }
    lvgl_port_unlock();
    return true;
}

bool display_image_by_index(int index) {
    if (index < 0 || index >= s_image_count) return false;
    s_current_index = index;
    return decode_and_display_image(index);
}

// 查找SD卡中的图片文件（排除 background.jpg）
static int search_image_files(void)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        ESP_LOGE(TAG, "Failed to open directory: %s", SD_MOUNT_POINT);
        return 0;
    }

    s_image_count = 0;
    struct dirent *dir;

    while ((dir = readdir(d)) != NULL && s_image_count < 256) {
        if (dir->d_type != DT_DIR) {
            // 排除背景图
            if (strcasecmp(dir->d_name, "background.jpg") == 0) continue;

            const char *ext = strrchr(dir->d_name, '.');
            if (ext && (strcasecmp(ext, ".png") == 0 ||
                       strcasecmp(ext, ".jpg") == 0 ||
                       strcasecmp(ext, ".jpeg") == 0)) {
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

// 初始化图片显示（最小化：仅 PPA + 画布，不加载内容）
bool image_display_init(void)
{
    ESP_LOGI(TAG, "Initializing image display...");

    if (!ppa_init()) {
        ESP_LOGE(TAG, "PPA init failed");
        return false;
    }

    lvgl_port_lock(0);
    s_image_canvas = lv_canvas_create(lv_scr_act());
    lv_obj_set_pos(s_image_canvas, 0, 0);
    static lv_style_t canvas_style;
    lv_style_init(&canvas_style);
    lv_style_set_bg_color(&canvas_style, lv_color_black());
    lv_obj_add_style(s_image_canvas, &canvas_style, 0);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Image display initialized (empty canvas)");
    return true;
}

// ─── 展示模式（Cover）：PSRAM 预加载，30 FPS ──────────────

bool cover_display_start(const char *agent_sd_path) {
    video_playback_stop();
    chat_overlay_show(false);  // 回到展示模式，隐藏聊天
    ppa_unload_background();

    // 扫描 cover 目录找 .mjpeg 文件
    char cover_dir[300], path[300] = {0};
    snprintf(cover_dir, sizeof(cover_dir), "%s/cover", agent_sd_path);
    DIR *d = opendir(cover_dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, ".mjpeg") == 0) {
                snprintf(path, sizeof(path), "%s/cover/%s", agent_sd_path, entry->d_name);
                break;
            }
        }
        closedir(d);
    }
    if (path[0] == '\0') { ESP_LOGE(TAG, "No .mjpeg in cover dir"); return false; }

    // 首次 cover 直接复用 sd_mount_task 在 WiFi 前预加载的帧
    int frame_count;
    if (s_first_cover) {
        frame_count = ppa_get_cache_count();
        s_first_cover = false;
    } else {
        frame_count = 0;
    }
    if (frame_count == 0) {
        frame_count = ppa_preload_mjpeg(path);
    }
    if (frame_count == 0) { ESP_LOGE(TAG, "Failed to preload cover"); return false; }

    strncpy(s_agent_path, agent_sd_path, sizeof(s_agent_path) - 1);
    s_image_count = frame_count;
    s_current_index = 0;
    s_cover_mode = true;

    ESP_LOGI(TAG, "Cover mode: %s (%d frames, PSRAM)", path, frame_count);
    video_playback_start(30);
    return true;
}

// ─── 交互模式（Expression）：预加载 + PPA 色键合成 ────────────

bool expression_display_start(const char *agent_sd_path, const char *emotion) {
    video_playback_stop();

    // 关闭 cover fseek 模式 → 切到缓存模式
    ppa_close_mjpeg();

    // 加载背景 → PPA blend 模式
    if (!ppa_has_background()) {
        ppa_load_background("/sdcard/main/background/background.jpg");
    }

    strncpy(s_agent_path, agent_sd_path, sizeof(s_agent_path) - 1);

    char path[300];
    snprintf(path, sizeof(path), "%s/emoji/%s.mjpeg", agent_sd_path, emotion);

    int count = ppa_preload_mjpeg(path);
    if (count == 0) {
        // 回退到 neutral
        snprintf(path, sizeof(path), "%s/emoji/neutral.mjpeg", agent_sd_path);
        count = ppa_preload_mjpeg(path);
    }
    if (count == 0) {
        ESP_LOGE(TAG, "Failed to load expression: %s", path);
        return false;
    }

    s_image_count = count;
    s_current_index = 0;
    s_cover_mode = false;
    s_loop_count = 0;
    chat_overlay_show(true);
    strncpy(s_current_emotion, emotion, sizeof(s_current_emotion) - 1);

    // 预加载 thinking_test 作为后备（临时测试用，测完改回 thinking）
    snprintf(path, sizeof(path), "%s/emoji/thinking_test.mjpeg", agent_sd_path);
    strncpy(s_pending_emotion, "thinking_test", sizeof(s_pending_emotion) - 1);
    ppa_preload_mjpeg_async(path);

    ESP_LOGI(TAG, "Expression mode: %s/%s.mjpeg (%d frames)", agent_sd_path, emotion, count);
    video_playback_start(30);
    return true;
}

// 切换表情（交互模式下，同智能体）
void expression_switch_emotion(const char *emotion) {
    if (s_cover_mode || s_agent_path[0] == '\0') return;

    // 如果目标表情和当前相同，跳过
    if (strcmp(s_current_emotion, emotion) == 0) return;

    // 如果后备已经是目标表情，直接标记可交换（数据有效，无需重载）
    if (strcmp(s_pending_emotion, emotion) == 0) {
        s_force_swap = true;
        return;
    }

    // 临时映射：LLM "thinking" → 文件 "thinking_test"（测试用，测完删除）
    const char *filename = emotion;
    if (strcmp(emotion, "thinking") == 0) filename = "thinking_test";

    char path[300];
    snprintf(path, sizeof(path), "%s/emoji/%s.mjpeg", s_agent_path, filename);
    ppa_preload_mjpeg_async(path);
    strncpy(s_pending_emotion, emotion, sizeof(s_pending_emotion) - 1);
    s_force_swap = true;
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

    // LLM 情绪抢占式驱动（无定时轮播）
    while (s_video_running) {
        int64_t frame_start = esp_timer_get_time();

        // 推进一帧，检测是否循环回 0（完整轮播一圈）
        int prev_index = s_current_index;
        if (!image_display_next()) s_current_index = 0;
        if (s_current_index == 0 && prev_index > 0) {
            // 非 neutral 表情播完一轮 → 自动回 neutral
            if (!s_cover_mode && s_current_emotion[0] &&
                strcmp(s_current_emotion, "neutral") != 0) {
                s_loop_count++;
                if (s_loop_count == 1) {  // 仅首次触发，避免重复
                    ESP_LOGI(TAG, "🔄 Auto-revert %s → neutral", s_current_emotion);
                    expression_switch_emotion("neutral");
                }
            }
        }
        s_frame_count++;

        if (!s_cover_mode) {
            // ── 抢占式表情切换：LLM 下发的表情加载完立即换 ──
            if (s_force_swap) {
                int count = ppa_swap_emotion();
                if (count > 0) {
                    s_image_count = count;
                    s_current_index = 0;
                    s_force_swap = false;
                    s_loop_count = 0;
                    // 旧活跃 → 落入后备缓存，记录表情名
                    char old_emotion[32];
                    strncpy(old_emotion, s_current_emotion, sizeof(old_emotion) - 1);
                    strncpy(s_current_emotion, s_pending_emotion, sizeof(s_current_emotion) - 1);
                    strncpy(s_pending_emotion, old_emotion, sizeof(s_pending_emotion) - 1);
                    ESP_LOGI(TAG, "🎭 Preemptive swap: %s (%d frames)", s_current_emotion, count);
                }
            }

        }

        // 每秒统计FPS
        int64_t now = esp_timer_get_time();
        if (now - s_last_fps_time >= 1000000) {
            s_fps_display = s_frame_count;
            s_frame_count = 0;
            s_last_fps_time = now;
            ESP_LOGI(TAG, "FPS:%d [%s]", s_fps_display,
                     s_cover_mode ? "cover" : (s_current_emotion[0] ? s_current_emotion : "?"));
        }

        // 帧率控制
        int64_t frame_time = esp_timer_get_time() - frame_start;
        int32_t wait_ms = (1000 / s_video_fps) - (frame_time / 1000);
        if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
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
    ppa_deinit();

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

// ─── 聊天覆盖层（半透明，置顶，叠在 PPA Canvas 上方）───

static lv_obj_t *s_chat_user_box = NULL;
static lv_obj_t *s_chat_user_label = NULL;
static lv_obj_t *s_chat_assistant_box = NULL;
static lv_obj_t *s_chat_assistant_label = NULL;
static const lv_font_t *s_chat_font = NULL;

void chat_overlay_set_font(const lv_font_t *font) {
    s_chat_font = font ? font : LV_FONT_DEFAULT;
    lvgl_port_lock(0);
    if (s_chat_user_label)    lv_obj_set_style_text_font(s_chat_user_label, font, 0);
    if (s_chat_assistant_label) lv_obj_set_style_text_font(s_chat_assistant_label, font, 0);
    // Also update header labels
    if (s_chat_user_box && lv_obj_get_child_cnt(s_chat_user_box) > 0)
        lv_obj_set_style_text_font(lv_obj_get_child(s_chat_user_box, 0), font, 0);
    if (s_chat_assistant_box && lv_obj_get_child_cnt(s_chat_assistant_box) > 0)
        lv_obj_set_style_text_font(lv_obj_get_child(s_chat_assistant_box, 0), font, 0);
    lvgl_port_unlock();
}

void chat_overlay_init(const lv_font_t *font) {
    chat_overlay_set_font(font);

    lvgl_port_lock(0);
    lv_obj_t *top = lv_layer_top();

    // ── 用户输入框（底部偏上，较小）──
    s_chat_user_box = lv_obj_create(top);
    lv_obj_set_size(s_chat_user_box, 440, 100);
    lv_obj_set_pos(s_chat_user_box, 20, 520);
    lv_obj_set_style_bg_color(s_chat_user_box, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(s_chat_user_box, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_chat_user_box, 0, 0);
    lv_obj_set_style_radius(s_chat_user_box, 8, 0);
    lv_obj_set_style_pad_all(s_chat_user_box, 6, 0);
    lv_obj_set_scrollbar_mode(s_chat_user_box, LV_SCROLLBAR_MODE_OFF);

    // 表头 "Dr.星马梦缘："
    lv_obj_t *hdr = lv_label_create(s_chat_user_box);
    lv_label_set_text(hdr, "Dr.XM：");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(hdr, s_chat_font, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    // 消息文字（可滚动）
    s_chat_user_label = lv_label_create(s_chat_user_box);
    lv_label_set_text(s_chat_user_label, "");
    lv_obj_set_style_text_color(s_chat_user_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_chat_user_label, s_chat_font, 0);
    lv_obj_set_width(s_chat_user_label, 425);
    lv_label_set_long_mode(s_chat_user_label, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(s_chat_user_label, hdr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
    lv_obj_set_scrollbar_mode(s_chat_user_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_chat_user_box, LV_DIR_VER);

    // ── 助理回复框（紧贴底部）──
    s_chat_assistant_box = lv_obj_create(top);
    lv_obj_set_size(s_chat_assistant_box, 440, 140);
    lv_obj_set_pos(s_chat_assistant_box, 20, 630);
    lv_obj_set_style_bg_color(s_chat_assistant_box, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(s_chat_assistant_box, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_chat_assistant_box, 0, 0);
    lv_obj_set_style_radius(s_chat_assistant_box, 8, 0);
    lv_obj_set_style_pad_all(s_chat_assistant_box, 6, 0);

    // 表头 "凯尔希："
    hdr = lv_label_create(s_chat_assistant_box);
    lv_label_set_text(hdr, "Kal'tsit：");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(hdr, s_chat_font, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    // 消息文字（可滚动）
    s_chat_assistant_label = lv_label_create(s_chat_assistant_box);
    lv_label_set_text(s_chat_assistant_label, "");
    lv_obj_set_style_text_color(s_chat_assistant_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_chat_assistant_label, s_chat_font, 0);
    lv_obj_set_width(s_chat_assistant_label, 425);
    lv_label_set_long_mode(s_chat_assistant_label, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(s_chat_assistant_label, hdr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
    lv_obj_set_scrollbar_mode(s_chat_assistant_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_chat_assistant_box, LV_DIR_VER);

    // 初始隐藏，唤醒后显示
    lv_obj_add_flag(s_chat_user_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_chat_assistant_box, LV_OBJ_FLAG_HIDDEN);

    // ── 右上角隐藏/显示按钮（放在主屏幕，确保触控）──
    lv_obj_t *btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 60, 30);
    lv_obj_set_pos(btn, 415, 5);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "隐藏");
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_label, s_chat_font, 0);
    lv_obj_center(btn_label);

    // 点击切换
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        static bool hidden = false;
        hidden = !hidden;
        chat_overlay_show(!hidden);
        lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0),
                          hidden ? "显示" : "隐藏");
    }, LV_EVENT_CLICKED, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Chat overlay initialized (hidden)");
}

void chat_overlay_toggle(void) {
    if (!s_chat_user_box) return;
    bool shown = !lv_obj_has_flag(s_chat_user_box, LV_OBJ_FLAG_HIDDEN);
    chat_overlay_show(!shown);
}

void chat_overlay_show(bool show) {
    if (!s_chat_user_box || !s_chat_assistant_box) return;
    lvgl_port_lock(0);
    if (show) {
        lv_obj_remove_flag(s_chat_user_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_chat_assistant_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_chat_user_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chat_assistant_box, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void chat_overlay_set_user(const char *text) {
    if (!s_chat_user_label || !text) return;
    lvgl_port_lock(0);
    lv_label_set_text(s_chat_user_label, text);
    lv_obj_scroll_to_y(s_chat_user_box, 0, LV_ANIM_OFF);
    // 用户发言时清空上一轮 LLM 回复
    lv_label_set_text(s_chat_assistant_label, "");
    lvgl_port_unlock();
}

void chat_overlay_append_assistant(const char *text) {
    if (!s_chat_assistant_label || !text) return;
    lvgl_port_lock(0);
    const char *old = lv_label_get_text(s_chat_assistant_label);
    char buf[1024];
    if (old && old[0]) {
        snprintf(buf, sizeof(buf), "%s%s", old, text);
    } else {
        snprintf(buf, sizeof(buf), "%s", text);
    }
    lv_label_set_text(s_chat_assistant_label, buf);
    lv_obj_scroll_to_y(s_chat_assistant_box, LV_COORD_MAX, LV_ANIM_OFF);
    lvgl_port_unlock();
}
