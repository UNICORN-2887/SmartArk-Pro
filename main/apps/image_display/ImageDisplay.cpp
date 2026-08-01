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
static volatile bool s_in_expression_start = false;  // 防 OnAudioChannelClosed 竞态
static char s_agent_path[256] = {0};
static volatile bool s_force_swap = false;  // LLM 抢占式切换标志
static volatile bool s_req_expression = false;  // 按钮：切到表情模式
static volatile bool s_req_cover = false;       // 按钮：切到展示模式
static int s_loop_count = 0;               // 非 neutral 表情已循环次数
static char s_current_emotion[32] = {0};   // 当前表情名
static char s_pending_emotion[32] = {0};   // 后备表情名
static lv_obj_t *s_mode_label = NULL;    // 模式切换按钮 label

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
    // agent 切换中（expression_display_start 持有锁）→ 跳过
    if (s_in_expression_start) return true;
    // 如果 cover 已经在跑了（mode_switch_task 先切了），跳过
    if (s_cover_mode && strcmp(s_agent_path, agent_sd_path) == 0) return true;
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
    lvgl_port_lock(0);
    if (s_mode_label) lv_label_set_text(s_mode_label, "对话模式");
    lvgl_port_unlock();

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
    lvgl_port_lock(0);
    if (s_mode_label) lv_label_set_text(s_mode_label, "通行证模式");
    lvgl_port_unlock();
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
            strncpy(s_current_emotion, "neutral", sizeof(s_current_emotion) - 1);
            s_pending_emotion[0] = '\0';
            s_force_swap = false;  // 清掉旧 agent 残留
            video_playback_start(30);
        }
    } else {
        extern void application_end_conversation(void);
        application_end_conversation();  // 关音频通道
        ppa_unload_background();
        s_cover_mode = true;  // 提前设标志，防 cover_display_start 竞态
        chat_overlay_show(false);

        ppa_wait_cover_preload();
        ppa_wait_pending_preload();
        int count = 0;
        if (ppa_has_cover()) {
            count = ppa_swap_to_cover();
            if (count > 0) {
                s_image_count = count; s_current_index = 0;
                s_loop_count = 0;
                ESP_LOGI(TAG, "Cover restored from cache (%d frames, instant)", count);
                video_playback_start(30);
            }
        }
        if (count == 0) {
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
static lv_obj_t *s_btn_labels[4] = {NULL};  // 隐藏/罗德岛/对话模式 按钮 label
static const lv_font_t *s_chat_font = NULL;

void chat_overlay_set_font(const lv_font_t *font) {
    s_chat_font = font ? font : LV_FONT_DEFAULT;
    lvgl_port_lock(0);
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
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        extern lv_obj_t *g_agent_panel;
        if (g_agent_panel) {
            bool vis = lv_obj_has_flag(g_agent_panel, LV_OBJ_FLAG_HIDDEN);
            if (vis) lv_obj_remove_flag(g_agent_panel, LV_OBJ_FLAG_HIDDEN);
            else     lv_obj_add_flag(g_agent_panel, LV_OBJ_FLAG_HIDDEN);
        }
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

    // ── 角色选择面板 ──
    static lv_obj_t *panel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(panel, 160, 120);
    lv_obj_set_pos(panel, 300, 110);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    extern lv_obj_t *g_agent_panel;
    g_agent_panel = panel;

    struct { const char *name, *id, *path; } agents[] = {
        {"Kal'tsit", "0a20483553fe4ff784a016d0fafabfff", "/sdcard/main/operator/MEDIC/6STAR/Kaltsit"},
        {"Amiya",   "5838c85f30ab4b33a4341bf8b0736e26", "/sdcard/main/operator/CASTER/5STAR/Amiya"},
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *ab = lv_btn_create(panel);
        lv_obj_set_size(ab, 150, 50);
        lv_obj_set_style_bg_color(ab, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(ab, 4, 0);
        lv_obj_set_style_border_width(ab, 0, 0);
        lv_obj_t *al = lv_label_create(ab);
        lv_label_set_text(al, agents[i].name);
        lv_obj_set_style_text_color(al, lv_color_white(), 0);
        lv_obj_set_style_text_font(al, s_chat_font, 0);
        lv_obj_center(al);
        lv_obj_add_event_cb(ab, [](lv_event_t *e) {
            int idx = (int)(uintptr_t)lv_event_get_user_data(e);
            struct { const char *name, *id, *path; } ag[] = {
                {"Kal'tsit", "0a20483553fe4ff784a016d0fafabfff", "/sdcard/main/operator/MEDIC/6STAR/Kaltsit"},
                {"Amiya",   "5838c85f30ab4b33a4341bf8b0736e26", "/sdcard/main/operator/CASTER/5STAR/Amiya"},
            };
            extern void application_switch_agent(const char *id);
            application_switch_agent(ag[idx].id);
            cover_display_start(ag[idx].path);
            if (g_agent_panel) lv_obj_add_flag(g_agent_panel, LV_OBJ_FLAG_HIDDEN);
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Chat overlay initialized (hidden)");
}

lv_obj_t *g_agent_panel = NULL;

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
