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
#include "driver/jpeg_decode.h"
#include "driver/jpeg_decode.h"

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
static volatile bool s_in_expression_start = false;  // 防 OnAudioChannelClosed 竞态
static char s_agent_path[256] = {0};
static volatile bool s_force_swap = false;  // LLM 抢占式切换标志
static volatile bool s_req_expression = false;  // 按钮：切到表情模式
static volatile bool s_req_cover = false;       // 按钮：切到展示模式
static int s_loop_count = 0;               // 非 neutral 表情已循环次数
static char s_current_emotion[32] = {0};   // 当前表情名
static char s_pending_emotion[32] = {0};   // 后备表情名
static lv_obj_t *s_mode_label = NULL;    // 模式切换按钮 label
static lv_obj_t *s_rhodes_btn = NULL;    // 罗德岛按钮（仅 cover 模式显示）
static lv_obj_t *s_profile_btn = NULL;   // 蟑螂派对！按钮（cover+expression 都显示）
static lv_obj_t *s_profile_overlay = NULL; // Profile 全屏 overlay（点击返回）
static bool s_profile_was_cover = false;  // 进入 profile 前的模式
static bool s_profile_loading = false;     // 后台任务互斥
static int s_profile_gen = 0;             // 后台任务版本号

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

    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) {
        // LVGL 任务繁忙，丢弃本帧（避免竞态崩溃）
        return false;
    }
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
    // agent 切换中（expression_display_start 持有锁）→ 跳过
    if (s_in_expression_start) return true;
    // 如果 cover 已经在跑了（mode_switch_task 先切了），跳过但确保按钮可见
    if (s_cover_mode && strcmp(s_agent_path, agent_sd_path) == 0) {
        if (s_rhodes_btn) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));  // 等旧 video task 退出+PPA 事务完成
    chat_overlay_show(false);  // 回到展示模式，隐藏聊天
    ppa_unload_background();

    // 等异步 cover 加载完成（防竞态）
    ppa_wait_cover_preload();
    // 优先从 cover 专用槽恢复（三槽缓存，秒切）
    int frame_count = 0;
    if (ppa_has_cover() && strcmp(ppa_get_cover_agent(), agent_sd_path) == 0) {
        frame_count = ppa_swap_to_cover();  // 同角色：cover→active
        if (frame_count > 0)
            ESP_LOGI(TAG, "Cover restored from cache (%d frames, instant)", frame_count);
    }

    // 缓存未命中 → 从 SD 加载到 cover 槽（旧 active 保留，避免鬼图）
    char path[520] = {0};
    if (frame_count == 0) {
        // 异角色旧 cover：先不清，等新 cover 就位再 swap+free
        char cover_dir[300];
        snprintf(cover_dir, sizeof(cover_dir), "%s/cover", agent_sd_path);
        DIR *d = opendir(cover_dir);
        if (d) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && strcasecmp(ext, ".mjpeg") == 0) {
                    snprintf(path, sizeof(path), "%s/cover/%.*s", agent_sd_path, 200, entry->d_name);
                    break;
                }
            }
            closedir(d);
        }
        if (path[0] == '\0') { ESP_LOGE(TAG, "No .mjpeg in cover dir"); return false; }
        frame_count = ppa_preload_cover(path);  // 新 cover→slot（旧 cover 仍在 active 显示）
        if (frame_count > 0) {
            frame_count = ppa_swap_to_cover();  // 新 cover⇄旧 active，旧→slot
            ppa_free_cover_slot();  // 释放 swap 弹进 slot 的旧帧（新 cover 已在 active）
        }
    }
    if (frame_count == 0) { ESP_LOGE(TAG, "Failed to preload cover"); return false; }

    strncpy(s_agent_path, agent_sd_path, sizeof(s_agent_path) - 1);
    s_image_count = frame_count;
    s_current_index = 0;
    s_cover_mode = true;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_mode_label) lv_label_set_text(s_mode_label, "对话模式");
        if (s_rhodes_btn) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }

    // 预加载 neutral 表情到后备缓存（唤醒/切换时秒切）
    char neutral_path[300];
    snprintf(neutral_path, sizeof(neutral_path), "%s/emoji/neutral.mjpeg", agent_sd_path);
    ppa_preload_mjpeg_async(neutral_path);
    strncpy(s_pending_emotion, "neutral", sizeof(s_pending_emotion) - 1);
    ESP_LOGI(TAG, "Cover mode: %s (%d frames), neutral preloading", path, frame_count);

    video_playback_start(30);
    return true;
}

// ─── 交互模式（Expression）：预加载 + PPA 色键合成 ────────────

bool expression_display_start(const char *agent_sd_path, const char *emotion) {
    s_in_expression_start = true;
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));  // 等旧 video task 退出+PPA 事务完成
    ppa_wait_pending_preload();

    // 换角色探测（在改 s_agent_path 之前）
    bool same_agent = (strcmp(s_agent_path, agent_sd_path) == 0);

    // 保存 cover 到专用槽（仅同角色，异角色直接清掉换新）
    if (s_cover_mode && ppa_get_cache_count() > 0 && ppa_has_cover()) {
        if (same_agent) {
            ppa_swap_to_cover();  // active(cover)→slot
        } else {
            ppa_unload_cover();  // 异角色：直接丢弃旧 cover
        }
    }
    ppa_close_mjpeg();

    // 加载背景 → PPA blend 模式
    if (!ppa_has_background()) {
        ppa_load_background("/sdcard/main/background/background.jpg");
    }

    // 换角色 → 清旧 cover 槽
    // 异角色：清旧 cover 槽（同角色的 save-cover 上面已处理）
    if (s_agent_path[0] && !same_agent) {
        ESP_LOGI(TAG, "Agent changed: %s → %s", s_agent_path, agent_sd_path);
        ppa_unload_cover();  // slot 可能还有旧数据，确保清掉
    }
    strncpy(s_agent_path, agent_sd_path, sizeof(s_agent_path) - 1);

    // 同角色 save-cover swap 后 active 已有帧，直接复用
    int count = 0;
    if (same_agent) {
        count = ppa_get_cache_count();
        if (count > 0) ESP_LOGI(TAG, "Reusing %d frames from slot", count);
    }
    if (count == 0) {
        char path[300];
        snprintf(path, sizeof(path), "%s/emoji/%s.mjpeg", agent_sd_path, emotion);
        count = ppa_preload_mjpeg(path);
        if (count == 0) {
            snprintf(path, sizeof(path), "%s/emoji/neutral.mjpeg", agent_sd_path);
            count = ppa_preload_mjpeg(path);
        }
    }
    if (count == 0) {
        ESP_LOGE(TAG, "Failed to load expression: %s/%s", agent_sd_path, emotion);
        s_in_expression_start = false;
        return false;
    }

    s_image_count = count;
    s_current_index = 0;
    s_cover_mode = false;
    s_loop_count = 0;
    chat_overlay_show(true);
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_mode_label) lv_label_set_text(s_mode_label, "通行证模式");
        if (s_rhodes_btn) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    strncpy(s_current_emotion, emotion, sizeof(s_current_emotion) - 1);
    s_pending_emotion[0] = '\0';  // pending 保留 cover 帧，等 LLM 真正用时才加载
    s_force_swap = false;  // 清掉旧 agent 残留的 swap 标志

    ESP_LOGI(TAG, "Expression mode: %s/%s.mjpeg (%d frames)", agent_sd_path, emotion, count);
    video_playback_start(30);
    s_in_expression_start = false;

    // 换角色 → 后台异步加载新 cover 到槽，对话结束时秒切
    if (!same_agent) {
        char cover_dir[300];
        snprintf(cover_dir, sizeof(cover_dir), "%s/cover", agent_sd_path);
        DIR *d = opendir(cover_dir);
        if (d) {
            struct dirent *entry;
            while ((entry = readdir(d))) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && strcasecmp(ext, ".mjpeg") == 0) {
                    char cover_path[520];
                    snprintf(cover_path, sizeof(cover_path), "%s/cover/%s", agent_sd_path, entry->d_name);
                    ppa_preload_cover_async(cover_path);
                    ESP_LOGI(TAG, "Preloading new cover async: %s", cover_path);
                    break;
                }
            }
            closedir(d);
        }
    }
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

// ── 模式切换辅助任务（大栈、低优先级，不阻塞音视频核心线程）──
static void mode_switch_task(void *arg) {
    bool to_expression = (bool)arg;

    if (to_expression) {
        // 等异步预加载完成（防半成品帧）
        ppa_wait_pending_preload();
        ppa_wait_cover_preload();
        // 先把 active 中的 cover 移入 cover 槽（永久保留）
        if (s_cover_mode && ppa_get_cache_count() > 0 && ppa_has_cover()) {
            ppa_swap_to_cover();  // 保存 active(cover)→slot
        }
        ppa_close_mjpeg();
        if (!ppa_has_background())
            ppa_load_background("/sdcard/main/background/background.jpg");

        // save-cover swap 后 active 可能已有所需帧（从 slot 恢复的），直接复用
        int count = ppa_get_cache_count();
        if (count == 0) {
            if (strcmp(s_pending_emotion, "neutral") == 0)
                count = ppa_swap_emotion();
            if (count == 0) {
                char path[300];
                snprintf(path, sizeof(path), "%s/emoji/neutral.mjpeg", s_agent_path);
                count = ppa_preload_mjpeg(path);
            }
        } else {
            ESP_LOGI(TAG, "Reusing %d frames from slot", count);
        }
        if (count > 0) {
            s_image_count = count; s_current_index = 0;
            s_cover_mode = false; s_loop_count = 0;
            chat_overlay_show(true);
            if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                if (s_rhodes_btn) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
                lvgl_port_unlock();
            }
            strncpy(s_current_emotion, "neutral", sizeof(s_current_emotion) - 1);
            s_pending_emotion[0] = '\0';
            s_force_swap = false;  // 清掉旧 agent 残留
            video_playback_start(30);
        }
    } else {
        ESP_LOGI(TAG, "→ Return-to-cover: start");
        extern void application_end_conversation(void);
        application_end_conversation();  // 关音频通道
        ESP_LOGI(TAG, "→ Return-to-cover: audio closed");
        ppa_unload_background();
        s_cover_mode = true;  // 提前设标志，防 cover_display_start 竞态
        chat_overlay_show(false);
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            if (s_rhodes_btn) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }

        ESP_LOGI(TAG, "→ Return-to-cover: waiting preloads (cover=%d pending=%d)",
                 (int)ppa_has_cover(), 0);
        ppa_wait_cover_preload();
        ESP_LOGI(TAG, "→ Return-to-cover: cover_preload done, has_cover=%d", (int)ppa_has_cover());
        ppa_wait_pending_preload();
        int count = 0;
        if (ppa_has_cover()) {
            ESP_LOGI(TAG, "→ Return-to-cover: swapping cover from slot…");
            count = ppa_swap_to_cover();
            ESP_LOGI(TAG, "→ Return-to-cover: swap returned %d", count);
            if (count > 0) {
                s_image_count = count; s_current_index = 0;
                s_loop_count = 0;
                ESP_LOGI(TAG, "Cover restored from cache (%d frames, instant)", count);
                video_playback_start(30);
            }
        }
        if (count == 0) {
            ESP_LOGI(TAG, "→ Return-to-cover: cache miss, loading from SD (agent=%s)", s_agent_path);
            char path[520] = {0};
            char cover_dir[300];
            snprintf(cover_dir, sizeof(cover_dir), "%s/cover", s_agent_path);
            DIR *d = opendir(cover_dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    const char *ext = strrchr(e->d_name, '.');
                    if (ext && strcasecmp(ext, ".mjpeg") == 0) {
                        snprintf(path, sizeof(path), "%s/cover/%.*s", s_agent_path, 200, e->d_name);
                        break;
                    }
                }
                closedir(d);
            }
            if (path[0]) {
                count = ppa_preload_cover(path);
                if (count > 0) {
                    count = ppa_swap_to_cover();
                    ppa_free_cover_slot();
                    s_image_count = count; s_current_index = 0;
                    video_playback_start(30);
                }
            }
        }
    }
    vTaskDelete(NULL);
}

static void video_playback_task(void *arg)
{
    s_video_running = true;
    s_frame_count = 0;
    s_last_fps_time = esp_timer_get_time();

    while (s_video_running) {
        // 按钮请求模式切换？
        if (s_req_expression || s_req_cover) {
            break;  // 退出循环，末尾生成切换任务
        }
        int64_t frame_start = esp_timer_get_time();

        int prev_index = s_current_index;
        if (!image_display_next()) s_current_index = 0;
        if (s_current_index == 0 && prev_index > 0) {
            if (!s_cover_mode && s_current_emotion[0] &&
                strcmp(s_current_emotion, "neutral") != 0) {
                s_loop_count++;
                if (s_loop_count == 1) {
                    ESP_LOGI(TAG, "🔄 Auto-revert %s → neutral", s_current_emotion);
                    expression_switch_emotion("neutral");
                }
            }
        }
        s_frame_count++;

        if (!s_cover_mode) {
            if (s_force_swap) {
                int count = ppa_swap_emotion();
                if (count > 0) {
                    s_image_count = count;
                    s_current_index = 0;
                    s_force_swap = false;
                    s_loop_count = 0;
                    char old_emotion[32];
                    strncpy(old_emotion, s_current_emotion, sizeof(old_emotion) - 1);
                    strncpy(s_current_emotion, s_pending_emotion, sizeof(s_current_emotion) - 1);
                    strncpy(s_pending_emotion, old_emotion, sizeof(s_pending_emotion) - 1);
                    ESP_LOGI(TAG, "🎭 Preemptive swap: %s (%d frames)", s_current_emotion, count);
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - s_last_fps_time >= 1000000) {
            s_fps_display = s_frame_count;
            s_frame_count = 0;
            s_last_fps_time = now;
            ESP_LOGI(TAG, "FPS:%d [%s]", s_fps_display,
                     s_cover_mode ? "cover" : (s_current_emotion[0] ? s_current_emotion : "?"));
        }

        int64_t frame_time = esp_timer_get_time() - frame_start;
        int32_t wait_ms = (1000 / s_video_fps) - (frame_time / 1000);
        if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    // ── 创建独立的大栈低优先级任务做 SD I/O，本任务立即退出 ──
    s_video_running = false;
    if (s_req_expression) {
        s_req_expression = false;
        xTaskCreate(mode_switch_task, "mode_sw_expr", 10240, (void*)true, 3, NULL);
    } else if (s_req_cover) {
        s_req_cover = false;
        xTaskCreate(mode_switch_task, "mode_sw_cover", 10240, (void*)false, 3, NULL);
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

    xTaskCreatePinnedToCore(video_playback_task, "video_play", 4096, NULL, 2, &s_video_task, 0);
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
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            lv_obj_del(s_image_canvas);
            s_image_canvas = NULL;
            lvgl_port_unlock();
        }
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
static lv_obj_t *s_btn_labels[4] = {NULL};  // 隐藏/罗德岛/对话模式 按钮 label
static const lv_font_t *s_chat_font = NULL;

void chat_overlay_set_font(const lv_font_t *font) {
    s_chat_font = font ? font : LV_FONT_DEFAULT;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_chat_user_label)    lv_obj_set_style_text_font(s_chat_user_label, font, 0);
    if (s_chat_assistant_label) lv_obj_set_style_text_font(s_chat_assistant_label, font, 0);
    if (s_chat_user_box && lv_obj_get_child_cnt(s_chat_user_box) > 0)
        lv_obj_set_style_text_font(lv_obj_get_child(s_chat_user_box, 0), font, 0);
    if (s_chat_assistant_box && lv_obj_get_child_cnt(s_chat_assistant_box) > 0)
        lv_obj_set_style_text_font(lv_obj_get_child(s_chat_assistant_box, 0), font, 0);
    for (int i = 0; i < 4; i++)
        if (s_btn_labels[i]) lv_obj_set_style_text_font(s_btn_labels[i], font, 0);
    lvgl_port_unlock();
}

// ─── 个性主页（蟑螂派对）────────────────────────────────

static lv_img_dsc_t* load_jpg_thumbnail(const char *path, int idx);
static void profile_hide(void);

static void profile_show(void) {
    if (s_profile_overlay) return;
    s_profile_was_cover = s_cover_mode;

    // 隐藏右上角按钮
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_profile_btn) lv_obj_add_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)  lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)  lv_obj_add_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }

    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    // ① 立刻显示 JPG 占位（LVGL 顶层 canvas）
    const char *jpg_path = "/sdcard/User/Ur_Info/Profile.jpg";
    FILE *fp = fopen(jpg_path, "rb");
    bool has_jpg = false;
    if (fp) {
        fclose(fp);
        char lv_path[300];
        snprintf(lv_path, sizeof(lv_path), "S:/User/Ur_Info/Profile.jpg");
        lv_img_dsc_t *dsc = load_jpg_thumbnail(lv_path, 0);
        if (dsc) {
            s_profile_overlay = lv_obj_create(lv_layer_top());
            lv_obj_set_size(s_profile_overlay, 480, 800);
            lv_obj_set_pos(s_profile_overlay, 0, 0);
            lv_obj_set_style_bg_opa(s_profile_overlay, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_profile_overlay, 0, 0);
            lv_obj_set_style_pad_all(s_profile_overlay, 0, 0);
            lv_obj_t *c = lv_canvas_create(s_profile_overlay);
            lv_obj_set_size(c, 480, 800);
            lv_canvas_set_buffer(c, (uint8_t*)dsc->data, dsc->header.w, dsc->header.h, LV_COLOR_FORMAT_RGB565);
            has_jpg = true;
            ESP_LOGI(TAG, "Profile: JPG placeholder %dx%d", dsc->header.w, dsc->header.h);
        }
    }

    if (!has_jpg) { profile_hide(); return; }

    // ② "动图"按钮（右下角, 用户手动触发加载）
    FILE *mjpeg = fopen("/sdcard/User/Ur_Info/Profile.mjpeg", "rb");
    if (mjpeg) { fclose(mjpeg);
        lv_obj_t *btn = lv_btn_create(s_profile_overlay);
        lv_obj_set_size(btn, 50, 28);
        lv_obj_set_pos(btn, 420, 760);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x5588AA), 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "动图");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, LV_FONT_DEFAULT, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            if (s_profile_loading) return;
            s_profile_loading = true;
            int gen = ++s_profile_gen;
            xTaskCreate([](void *arg) {
                int gen = (int)(intptr_t)arg;
                int n = ppa_preload_profile("/sdcard/User/Ur_Info/Profile.mjpeg");
                if (gen == s_profile_gen && n > 0 && s_profile_overlay) {
                    ppa_use_profile_cache(true);
                    s_image_count = n; s_cover_mode = false;
                    video_playback_start(25);
                    if (s_profile_overlay) {
                        lvgl_port_lock(pdMS_TO_TICKS(200));
                        lv_obj_clean(s_profile_overlay);
                        lv_obj_set_style_bg_opa(s_profile_overlay, LV_OPA_0, 0);
                        lvgl_port_unlock();
                    }
                    ESP_LOGI(TAG, "Profile: MJPEG %d frames", n);
                }
                s_profile_loading = false;
                vTaskDelete(NULL);
            }, "pfl", 8192, (void*)(intptr_t)gen, 2, NULL);
        }, LV_EVENT_CLICKED, NULL);
    }

    // 触摸 overlay（JPG 已创建则复用并加触摸，否则新建透明层）
    if (!s_profile_overlay) {
        s_profile_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_profile_overlay, 480, 800);
        lv_obj_set_pos(s_profile_overlay, 0, 0);
        lv_obj_set_style_bg_opa(s_profile_overlay, LV_OPA_0, 0);
        lv_obj_set_style_border_width(s_profile_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_profile_overlay, 0, 0);
    }
    lv_obj_add_flag(s_profile_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_profile_overlay, [](lv_event_t *e) {
        profile_hide();
    }, LV_EVENT_CLICKED, NULL);
}

static void profile_hide(void) {
    if (!s_profile_overlay) return;
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        lv_obj_del(s_profile_overlay);
        s_profile_overlay = NULL;
        lvgl_port_unlock();
    }

    // 恢复按钮
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)  lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)  lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }

    // 切回 active 播放源 + 清 profile 槽 + 恢复状态
    ppa_use_profile_cache(false);
    ppa_free_profile_slot();
    s_cover_mode = s_profile_was_cover;
    s_current_index = 0;
    s_loop_count = 0;
    video_playback_start(30);
    ESP_LOGI(TAG, "Profile hidden");
}

// ─── 角色索引页面（罗德岛）──────────────────────────────

enum { CARD_W = 108, CARD_H = 228, COLS = 4, ROWS = 3, CARDS_PER_PAGE = 12 };
static lv_obj_t *s_index_page = NULL;
static lv_obj_t *s_index_grid = NULL;
static lv_obj_t *s_index_pg_label = NULL;
#define MAX_AGENT_DSC 768  // 缩略图缓存指针数组（DRAM，3KB）
static lv_obj_t *s_card_objs[CARDS_PER_PAGE] = {NULL};
static int s_card_agent_idx[CARDS_PER_PAGE] = {-1};
static lv_img_dsc_t *s_agent_dsc[MAX_AGENT_DSC] = {NULL};  // 按需加载缓存（下标=主列表下标）
static int s_index_page_cur = 0;
static int s_index_page_total = 1;

// ── 主列表（PSRAM 动态分配，支持任意数量）──
struct AgentInfo {
    char path[300];      // "S:/main/operator/INDEX/CASTER_108x228/5STAR/Amiya.jpg"
    char name[64];       // "Amiya"
    uint8_t prof;        // 1..8 → PROF_EN 下标
    uint8_t rarity;      // 1..6
};
static AgentInfo *s_agents = NULL;   // PSRAM 分配
static int s_total_agents = 0;
static int s_agent_cap = 0;

// ── 筛选状态 ──
static int *s_filtered = NULL;       // PSRAM 分配（s_agents 下标）
static int s_filtered_count = 0;
static int s_filter_prof = 0;        // 0=全部, 1..8
static int s_filter_rarity = 0;      // 0=全部, 1..6
static lv_obj_t *s_prof_dd = NULL;   // 下拉框句柄（用于联动重置）
static lv_obj_t *s_rarity_dd = NULL;

static const char* const PROFESSIONS[] = {
    "全部", "先锋", "近卫", "重装", "狙击", "术师", "医疗", "辅助", "特种", NULL
};
static const char* const PROF_EN[] = {
    "", "VANGUARD", "GUARD", "REINSTALL", "SNIPER", "CASTER", "MEDIC", "SUPPORTER", "SPECIALIST", NULL
};
static const char* const RARITIES[] = {
    "全部", "6星", "5星", "4星", "3星", "2星", "1星", NULL
};
static const char* const RARITY_DIR[] = {
    "", "6STAR", "5STAR", "4STAR", "3STAR", "2STAR", "1STAR", NULL
};

static bool is_jpg(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && strcasecmp(ext, ".jpg") == 0;
}

static int scan_sd_agents(void) {
    const char *base = "/sdcard/main/operator/INDEX";
    // ── 第一遍：计数 ──
    int total = 0;
    for (int p = 1; p <= 8; p++) {
        for (int r = 1; r <= 6; r++) {
            char dir[160];
            snprintf(dir, sizeof(dir), "%s/%s_108x228/%s", base, PROF_EN[p], RARITY_DIR[r]);
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (is_jpg(entry->d_name)) total++;
            }
            closedir(d);
        }
    }
    if (total == 0) { ESP_LOGW(TAG, "No agents found in %s", base); return 0; }

    // ── 分配 PSRAM ──
    if (s_agents) { heap_caps_free(s_agents); s_agents = NULL; }
    s_agent_cap = total;
    s_agents = (AgentInfo*)heap_caps_malloc(total * sizeof(AgentInfo), MALLOC_CAP_SPIRAM);
    if (!s_agents) { ESP_LOGE(TAG, "Failed to alloc %d AgentInfo in PSRAM", total); return 0; }

    // ── 第二遍：填充 ──
    int count = 0;
    for (int p = 1; p <= 8 && count < total; p++) {
        for (int r = 1; r <= 6 && count < total; r++) {
            char dir[160];
            snprintf(dir, sizeof(dir), "%s/%s_108x228/%s", base, PROF_EN[p], RARITY_DIR[r]);
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL && count < total) {
                if (!is_jpg(entry->d_name)) continue;
                AgentInfo *a = &s_agents[count];
                snprintf(a->path, sizeof(a->path), "S:/main/operator/INDEX/%s_108x228/%s/%s",
                         PROF_EN[p], RARITY_DIR[r], entry->d_name);
                size_t nl = strlen(entry->d_name);
                const char *ext = strrchr(entry->d_name, '.');
                if (ext) nl = ext - entry->d_name;
                if (nl > sizeof(a->name) - 1) nl = sizeof(a->name) - 1;
                memcpy(a->name, entry->d_name, nl);
                a->name[nl] = '\0';
                a->prof = (uint8_t)p;
                a->rarity = (uint8_t)r;
                count++;
            }
            closedir(d);
        }
    }
    ESP_LOGI(TAG, "Index: %d agents scanned (%d PSRAM bytes)", count, (int)(total * sizeof(AgentInfo)));
    return count;
}

static void agent_index_refresh(void);

static void agent_index_hide(void) {
    if (s_index_page) {
        lv_obj_del(s_index_page);
        s_index_page = NULL;
        s_index_grid = NULL;
        s_index_pg_label = NULL;
        for (int i = 0; i < CARDS_PER_PAGE; i++) s_card_objs[i] = NULL;
        // 释放缩略图缓存
        if (s_agents) {
            for (int i = 0; i < s_total_agents; i++) {
                if (s_agent_dsc[i]) {
                    if (s_agent_dsc[i]->data) heap_caps_free((void*)s_agent_dsc[i]->data);
                    heap_caps_free(s_agent_dsc[i]);
                    s_agent_dsc[i] = NULL;
                }
            }
            heap_caps_free(s_agents); s_agents = NULL;
            s_total_agents = 0; s_agent_cap = 0;
        }
        if (s_filtered) { heap_caps_free(s_filtered); s_filtered = NULL; }
        s_filtered_count = 0;
        // 恢复 AFE
        extern void application_set_wake_word_detection(bool enable);
        application_set_wake_word_detection(true);
    }
}

// ── JPEG 缩略图加载（借 PPA JPEG 引擎，需先停视频）──
static lv_img_dsc_t* load_jpg_thumbnail(const char *path, int idx) {
    char fs_path[300];
    snprintf(fs_path, sizeof(fs_path), "/sdcard%s", path + 2);
    FILE *fp = fopen(fs_path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    size_t jpg_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (jpg_size == 0 || jpg_size > 512 * 1024) { fclose(fp); return NULL; }
    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(jpg_size, MALLOC_CAP_SPIRAM);
    if (!jpg_data) { fclose(fp); return NULL; }
    size_t rd = fread(jpg_data, 1, jpg_size, fp);
    fclose(fp);
    if (rd != jpg_size) { ESP_LOGW(TAG, "[%d] short read %u/%u", idx, (unsigned)rd, (unsigned)jpg_size); free(jpg_data); return NULL; }

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg_data, jpg_size, &info) != ESP_OK) { free(jpg_data); return NULL; }
    uint32_t aw = (info.width + 15) & ~15, ah = (info.height + 15) & ~15;

    jpeg_decode_memory_alloc_cfg_t rx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t tx_sz, rx_sz;
    uint8_t *tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpg_size, &tx_cfg, &tx_sz);
    uint8_t *rx_buf = (uint8_t*)jpeg_alloc_decoder_mem(aw * ah * 2, &rx_cfg, &rx_sz);
    if (!tx_buf || !rx_buf) { free(jpg_data); free(tx_buf); free(rx_buf); return NULL; }
    memcpy(tx_buf, jpg_data, jpg_size);
    free(jpg_data);

    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 5000 };
    jpeg_decode_cfg_t jpg_cfg = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR };
    jpeg_decoder_handle_t h = NULL;
    esp_err_t e;
    for (int retry = 0; retry < 3; retry++) {
        e = jpeg_new_decoder_engine(&eng_cfg, &h);
        if (e == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (e != ESP_OK) { ESP_LOGW(TAG, "[%d] eng fail %d", idx, (int)e); free(tx_buf); free(rx_buf); return NULL; }
    uint32_t dec;
    e = jpeg_decoder_process(h, &jpg_cfg, tx_buf, tx_sz, rx_buf, rx_sz, &dec);
    jpeg_del_decoder_engine(h);
    free(tx_buf);
    if (e != ESP_OK) { ESP_LOGW(TAG, "[%d] dec fail %d", idx, (int)e); free(rx_buf); return NULL; }

    lv_img_dsc_t *dsc = (lv_img_dsc_t*)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
    if (!dsc) { free(rx_buf); return NULL; }
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (lv_coord_t)aw; dsc->header.h = (lv_coord_t)ah;
    dsc->data_size = aw * ah * 2; dsc->data = rx_buf; dsc->header.stride = aw * 2;
    return dsc;
}

// ── 分页显示（使用筛选结果 + 延迟加载）──
static void agent_index_show_page(int page) {
    if (!s_index_grid) return;
    int start = page * CARDS_PER_PAGE;
    int shown = 0;
    for (int i = 0; i < CARDS_PER_PAGE; i++) {
        lv_obj_t *card = s_card_objs[i];
        if (!card) continue;
        int fi = start + i;  // 筛选结果下标
        if (fi < s_filtered_count) {
            int ai = s_filtered[fi];  // 主列表下标
            lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
            // 延迟加载：该 agent 缩略图未解码
            if (!s_agent_dsc[ai]) {
                s_agent_dsc[ai] = load_jpg_thumbnail(s_agents[ai].path, ai);
            }
            lv_obj_t *c = lv_obj_get_child(card, 0);
            if (s_agent_dsc[ai] && c) {
                lv_canvas_set_buffer(c, (uint8_t*)s_agent_dsc[ai]->data,
                                     s_agent_dsc[ai]->header.w, s_agent_dsc[ai]->header.h,
                                     LV_COLOR_FORMAT_RGB565);
                shown++;
            }
        } else {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ESP_LOGI(TAG, "Page %d: %d cards shown", page, shown);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", page + 1, s_index_page_total);
    lv_label_set_text(s_index_pg_label, buf);
    s_index_page_cur = page;
}

static void agent_index_prev_page(void) {
    if (s_index_page_cur > 0) agent_index_show_page(s_index_page_cur - 1);
}
static void agent_index_next_page(void) {
    if (s_index_page_cur < s_index_page_total - 1) agent_index_show_page(s_index_page_cur + 1);
}

// 滑动事件处理：监听 ALL 事件，检测手势或滑动
static lv_point_t s_press_point;
static bool s_gesture_handled = false;
static void on_index_gesture(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    if (code == LV_EVENT_PRESSED) {
        s_press_point = pt;
        s_gesture_handled = false;
        return;
    }
    if (s_gesture_handled) return;
    // 先尝试 LVGL 内置手势
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT)  { s_gesture_handled = true; agent_index_next_page(); return; }
    if (dir == LV_DIR_RIGHT) { s_gesture_handled = true; agent_index_prev_page(); return; }
    // fallback: 手动计算 PRESS→RELEASE 位移
    if (code == LV_EVENT_RELEASED) {
        lv_coord_t dx = pt.x - s_press_point.x;
        if (dx < -40) { s_gesture_handled = true; agent_index_next_page(); }
        else if (dx > 40) { s_gesture_handled = true; agent_index_prev_page(); }
    }
}

static void agent_index_show(void) {
    if (s_index_page) { agent_index_hide(); return; }

    // 重置卡片追踪
    for (int i = 0; i < CARDS_PER_PAGE; i++) s_card_agent_idx[i] = -1;
    if (s_profile_btn) lv_obj_add_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);

    // 保存 cover active→slot，等 cover_display_start 秒换回来
    if (s_cover_mode && ppa_has_cover()) ppa_swap_to_cover();
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    s_cover_mode = false;
    ppa_close_mjpeg();
    ppa_release_jpeg_engine();
    extern void application_set_wake_word_detection(bool enable);
    application_set_wake_word_detection(false);
    s_total_agents = scan_sd_agents();
    // 初始筛选 = 全部（PSRAM 分配）
    s_filter_prof = 0;
    s_filter_rarity = 0;
    s_filtered_count = 0;
    if (s_filtered) { heap_caps_free(s_filtered); s_filtered = NULL; }
    if (s_total_agents > 0) {
        s_filtered = (int*)heap_caps_malloc(s_total_agents * sizeof(int), MALLOC_CAP_SPIRAM);
        if (s_filtered) {
            for (int i = 0; i < s_total_agents; i++) {
                s_filtered[s_filtered_count++] = i;
            }
        }
    }
    s_index_page_total = (s_filtered_count + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
    if (s_index_page_total < 1) s_index_page_total = 1;
    s_index_page_cur = 0;
    ESP_LOGI(TAG, "Index: %d agents, %d pages", s_total_agents, s_index_page_total);

    lvgl_port_lock(0);
    lv_obj_t *page = lv_obj_create(lv_layer_top());
    lv_obj_set_size(page, 480, 800);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_90, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    s_index_page = page;

    // ── 顶部 Bar ──
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_set_size(bar, 480, 44);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 返回按钮
    lv_obj_t *back_btn = lv_btn_create(bar);
    lv_obj_set_size(back_btn, 50, 30);
    lv_obj_set_pos(back_btn, 4, 7);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(back_btn, 4, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_lbl, s_chat_font, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, [](lv_event_t *e) {
        agent_index_hide();
    }, LV_EVENT_CLICKED, NULL);

    // 职业下拉框
    lv_obj_t *dd_prof = lv_dropdown_create(bar);
    lv_obj_set_pos(dd_prof, 60, 7);
    lv_obj_set_size(dd_prof, 110, 30);
    lv_dropdown_set_options(dd_prof, "全部\n先锋\n近卫\n重装\n狙击\n术师\n医疗\n辅助\n特种");
    lv_dropdown_set_symbol(dd_prof, ">");
    lv_obj_set_style_text_font(dd_prof, s_chat_font, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd_prof), s_chat_font, 0);
    lv_obj_add_event_cb(dd_prof, [](lv_event_t *e) {
        int sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
        if (sel == s_filter_prof) return;  // 防重复触发
        ESP_LOGI(TAG, "Prof filter: %d", sel);
        s_filter_prof = sel;
        if (s_filter_rarity != 0) {
            s_filter_rarity = 0;
            if (s_rarity_dd) lv_dropdown_set_selected(s_rarity_dd, 0);
        }
        s_index_page_cur = 0;
        agent_index_refresh();
    }, LV_EVENT_VALUE_CHANGED, NULL);
    s_prof_dd = dd_prof;

    // 稀有度下拉框
    lv_obj_t *dd_rarity = lv_dropdown_create(bar);
    lv_obj_set_pos(dd_rarity, 176, 7);
    lv_obj_set_size(dd_rarity, 100, 30);
    lv_dropdown_set_options(dd_rarity, "全部\n6星\n5星\n4星\n3星\n2星\n1星");
    lv_dropdown_set_symbol(dd_rarity, ">");
    lv_obj_set_style_text_font(dd_rarity, s_chat_font, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd_rarity), s_chat_font, 0);
    lv_obj_add_event_cb(dd_rarity, [](lv_event_t *e) {
        int sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
        if (sel == s_filter_rarity) return;  // 防重复触发（含职业切换时的联动重置）
        ESP_LOGI(TAG, "Rarity filter: %d", sel);
        s_filter_rarity = sel;
        s_index_page_cur = 0;
        agent_index_refresh();
    }, LV_EVENT_VALUE_CHANGED, NULL);
    s_rarity_dd = dd_rarity;

    // ── 卡片网格容器（支持滑动）──
    s_index_grid = lv_obj_create(page);
    lv_obj_set_size(s_index_grid, 480, 720);
    lv_obj_set_pos(s_index_grid, 0, 48);
    lv_obj_set_style_bg_opa(s_index_grid, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_index_grid, 0, 0);
    lv_obj_set_style_pad_all(s_index_grid, 0, 0);
    lv_obj_set_scrollbar_mode(s_index_grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_index_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_index_grid, on_index_gesture, LV_EVENT_ALL, NULL);
    ESP_LOGI(TAG, "Grid ALL-events listener added");

    int gap_x = (480 - COLS * CARD_W) / (COLS + 1);
    int gap_y = (720 - ROWS * CARD_H) / (ROWS + 1);
    if (gap_y < 8) gap_y = 8;

    for (int i = 0; i < CARDS_PER_PAGE; i++) {
        int row = i / COLS;
        int col = i % COLS;
        lv_obj_t *card = lv_obj_create(s_index_grid);
        int x = gap_x + col * (CARD_W + gap_x);
        int y = gap_y + row * (CARD_H + gap_y);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x555555), 0);
        lv_obj_set_style_radius(card, 4, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);  // 关滚动条

        // 缩略图画布
        lv_obj_t *c = lv_canvas_create(card);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_pos(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);

        // 点击卡片 → 切换角色 cover
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t *e) {
            int card_i = (int)(intptr_t)lv_event_get_user_data(e);
            int fi = s_index_page_cur * CARDS_PER_PAGE + card_i;
            if (fi >= s_filtered_count) return;
            int ai = s_filtered[fi];
            char agent_path[300];
            snprintf(agent_path, sizeof(agent_path), "/sdcard/main/operator/%s/%s/%s",
                     PROF_EN[s_agents[ai].prof], RARITY_DIR[s_agents[ai].rarity], s_agents[ai].name);
            ESP_LOGI(TAG, "Card tap: %s → %s", s_agents[ai].name, agent_path);
            agent_index_hide();
            cover_display_start(agent_path);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

        s_card_objs[i] = card;
    }

    // ── 页码指示器 ──
    s_index_pg_label = lv_label_create(page);
    lv_obj_set_style_text_color(s_index_pg_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_index_pg_label, s_chat_font, 0);
    lv_obj_align(s_index_pg_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    // 只预加载第一页（最多 12 张），翻页时延迟加载
    int preload = s_filtered_count < CARDS_PER_PAGE ? s_filtered_count : CARDS_PER_PAGE;
    for (int i = 0; i < preload; i++) {
        int ai = s_filtered[i];
        s_agent_dsc[ai] = load_jpg_thumbnail(s_agents[ai].path, ai);
        if (i < preload - 1) vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 恢复 cover 动图
    cover_display_start(s_agent_path);
    if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);

    // 显示第一页
    agent_index_show_page(0);

    lvgl_port_unlock();
}

static void agent_index_refresh(void) {
    // 1. 释放旧缩略图
    for (int i = 0; i < s_total_agents; i++) {
        if (s_agent_dsc[i]) {
            if (s_agent_dsc[i]->data) heap_caps_free((void*)s_agent_dsc[i]->data);
            heap_caps_free(s_agent_dsc[i]);
            s_agent_dsc[i] = NULL;
        }
    }
    // 2. 分配筛选数组（PSRAM）
    if (s_filtered) { heap_caps_free(s_filtered); s_filtered = NULL; }
    s_filtered_count = 0;
    if (s_total_agents > 0) {
        s_filtered = (int*)heap_caps_malloc(s_total_agents * sizeof(int), MALLOC_CAP_SPIRAM);
        if (!s_filtered) { ESP_LOGE(TAG, "Failed to alloc filtered array"); return; }
        for (int i = 0; i < s_total_agents; i++) {
            if (s_filter_prof > 0 && s_agents[i].prof != s_filter_prof) continue;
            if (s_filter_rarity > 0 && s_agents[i].rarity != s_filter_rarity) continue;
            s_filtered[s_filtered_count++] = i;
        }
    }
    // 3. 页码
    s_index_page_total = (s_filtered_count + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
    if (s_index_page_total < 1) s_index_page_total = 1;
    s_index_page_cur = 0;
    ESP_LOGI(TAG, "Filter: prof=%d rarity=%d → %d agents, %d pages",
             s_filter_prof, s_filter_rarity, s_filtered_count, s_index_page_total);
    // 4. 预加载第 0 页
    int preload = s_filtered_count < CARDS_PER_PAGE ? s_filtered_count : CARDS_PER_PAGE;
    for (int i = 0; i < preload; i++) {
        int ai = s_filtered[i];
        s_agent_dsc[ai] = load_jpg_thumbnail(s_agents[ai].path, ai);
        if (i < preload - 1) vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 5. 显示
    agent_index_show_page(0);
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
    s_btn_labels[0] = btn_label;

    // 点击切换
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        static bool hidden = false;
        hidden = !hidden;
        chat_overlay_show(!hidden);
        lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0),
                          hidden ? "显示" : "隐藏");
    }, LV_EVENT_CLICKED, NULL);

    // ── ② 返回罗德岛 ──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 80, 30);
    lv_obj_set_pos(btn, 395, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "罗德岛");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    s_btn_labels[1] = lbl;
    s_rhodes_btn = btn;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，cover 模式才显示
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        agent_index_show();
    }, LV_EVENT_CLICKED, NULL);

    // ── ③ 对话模式/通行证模式 ──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 80, 30);
    lv_obj_set_pos(btn, 395, 75);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    s_mode_label = lv_label_create(btn);
    lv_label_set_text(s_mode_label, "对话模式");
    lv_obj_set_style_text_color(s_mode_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_mode_label, s_chat_font, 0);
    lv_obj_center(s_mode_label);
    s_btn_labels[2] = s_mode_label;
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        if (s_cover_mode) s_req_expression = true;
        else             s_req_cover = true;
    }, LV_EVENT_CLICKED, NULL);

    // ── ④ 蟑螂派对！（cover+expression 都可见）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 100, 35);
    lv_obj_set_pos(btn, 375, 110);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x5588AA), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "蟑螂派对！");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    s_profile_btn = btn;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        profile_show();
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
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;  // 非 LVGL 任务调用，必须等锁
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
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    lv_label_set_text(s_chat_user_label, text);
    lv_obj_scroll_to_y(s_chat_user_box, 0, LV_ANIM_OFF);
    // 用户发言时清空上一轮 LLM 回复
    lv_label_set_text(s_chat_assistant_label, "");
    lvgl_port_unlock();
}

void chat_overlay_append_assistant(const char *text) {
    if (!s_chat_assistant_label || !text) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
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
