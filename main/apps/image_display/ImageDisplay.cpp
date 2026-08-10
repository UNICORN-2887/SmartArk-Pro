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

extern void lv2_update_animation(float time_sec);
extern void application_set_wake_word_detection(bool enable);
#include <esp_task_wdt.h>
#include "PPACompositor.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_decode.h"
#include "board.h"
#include "audio_codec.h"
#include "pinyin_table.h"
#include "audio/tts_engine.h"

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
static bool s_profile_chat_was_visible = false; // 进入前对话框可见?
static bool s_profile_loading = false;     // 后台任务互斥
static int s_profile_gen = 0;             // 后台任务版本号
static const lv_font_t *s_chat_font = NULL;

// ─── 语音记录 ────────────────────────────────
struct VoiceEntry { const char *file; const char *label; };

static const VoiceEntry VOICE_DAILY[] = {
    {"annivers", "周年庆典"}, {"Arknights", "Arknights"}, {"assignfaculty", "进驻设施"},
    {"assign_assit", "任命助理"}, {"birthday", "生日"}, {"conver1", "交谈1"},
    {"conver2", "交谈2"}, {"conver3", "交谈3"}, {"enroll", "干员报到"},
    {"exp_watching", "观看作战记录"}, {"greeting", "问候"}, {"idle", "闲置"},
    {"newyear", "新年祝福"}, {"poke", "戳戳"}, {"touch", "摸摸"},
};
static const int VOICE_DAILY_N = sizeof(VOICE_DAILY) / sizeof(VOICE_DAILY[0]);

static const VoiceEntry VOICE_FIGHT[] = {
    {"alloc1", "部署1"}, {"alloc2", "部署2"}, {"assigncap", "任命队长"},
    {"combating1", "作战中1"}, {"combating2", "作战中2"}, {"combating3", "作战中3"},
    {"combating4", "作战中4"}, {"diff", "完成高难行动"}, {"include", "编入队伍"},
    {"inperfect", "非3星结束行动"}, {"misfail", "行动失败"}, {"misgo", "行动出发"},
    {"misstart", "行动开始"}, {"perfect", "3星结束行动"}, {"sel1", "选中干员1"}, {"sel2", "选中干员2"},
};
static const int VOICE_FIGHT_N = sizeof(VOICE_FIGHT) / sizeof(VOICE_FIGHT[0]);

static const VoiceEntry VOICE_PROMOTION[] = {
    {"elitepm1", "精英化晋升1"}, {"elitepm2", "精英化晋升2"}, {"pmconver1", "晋升后交谈1"},
    {"pmconver2", "晋升后交谈2"}, {"trustpmconver1", "信赖提升后交谈1"},
    {"trustpmconver2", "信赖提升后交谈2"}, {"trustpmconver3", "信赖提升后交谈3"},
};
static const int VOICE_PROMOTION_N = sizeof(VOICE_PROMOTION) / sizeof(VOICE_PROMOTION[0]);

static const char* const VOICE_CATEGORIES[] = {"daily", "fight", "promotion"};

static lv_obj_t *s_voice_btn = NULL;
static lv_obj_t *s_voice_overlay = NULL;
static lv_obj_t *s_voice_cat_dd = NULL;
static lv_obj_t *s_voice_entry_dd = NULL;
static int s_voice_cat_sel = 0;
static TaskHandle_t s_voice_task = NULL;
static volatile bool s_voice_cancel = false;
static lv_obj_t *s_voice_text_obj = NULL;   // 语音文本显示框（cover 模式）
static lv_obj_t *s_voice_text_label = NULL;
static char s_voice_text_buf[1024] = {0};

// ─── 背景音乐 ────────────────────────────────
static lv_obj_t *s_music_btn = NULL;
static lv_obj_t *s_music_overlay = NULL;
static lv_obj_t *s_music_dd = NULL;
static TaskHandle_t s_music_task = NULL;
static volatile bool s_music_cancel = false;
#define MUSIC_DIR "/sdcard/main/music"

// ─── 九键键盘 ────────────────────────────────
static lv_obj_t *s_kb_btn = NULL;
static lv_obj_t *s_kb_overlay = NULL;
static lv_obj_t *s_kb_preview_lbl = NULL;

// Live2D render framebuffer (from sd_test boot test)
uint16_t* g_lv2_fb = NULL;
int g_lv2_fb_w = 0, g_lv2_fb_h = 0;
static lv_obj_t *s_kb_cand_btns[6] = {NULL};
static lv_obj_t *s_kb_cand_labels[6] = {NULL};
static int s_kb_cand_page = 0;
static int s_kb_cand_total = 0;
static bool s_kb_mode_9key = false; // false=26key, true=9key
static char *s_kb_input = NULL;  // PSRAM alloc (avoids BSS corruption)
#define KB_BUF_SIZE 320
static int  s_kb_pos = 0;
static int  s_kb_last_key = -1;
static int  s_kb_tap_count = 0;
static int64_t s_kb_last_tap_us = 0;
// Multi-tap key maps for 2-9
static const char* const KB_KEYS[] = {"", "1", "2abc", "3def", "4ghi", "5jkl", "6mno", "7pqrs", "8tuv", "9wxyz"};

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
        if (s_kb_btn)    lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_voice_btn) lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_music_btn) lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn) lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
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
        if (s_kb_btn)    lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_voice_btn) lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_music_btn) lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn) lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);

        // Voice text box (create once, reuse)
        if (!s_voice_text_obj) {
            s_voice_text_obj = lv_obj_create(lv_screen_active());
            lv_obj_set_size(s_voice_text_obj, 385, 120);
            lv_obj_set_pos(s_voice_text_obj, 80, 530);
            lv_obj_set_style_bg_color(s_voice_text_obj, lv_color_hex(0x222222), 0);
            lv_obj_set_style_bg_opa(s_voice_text_obj, LV_OPA_80, 0);
            lv_obj_set_style_border_width(s_voice_text_obj, 0, 0);
            lv_obj_set_style_radius(s_voice_text_obj, 6, 0);
            lv_obj_set_style_pad_all(s_voice_text_obj, 8, 0);
            lv_obj_set_scrollbar_mode(s_voice_text_obj, LV_SCROLLBAR_MODE_OFF);
            s_voice_text_label = lv_label_create(s_voice_text_obj);
            lv_label_set_text(s_voice_text_label, "");
            lv_obj_set_style_text_color(s_voice_text_label, lv_color_white(), 0);
            lv_obj_set_style_text_font(s_voice_text_label, s_chat_font, 0);
            lv_obj_set_width(s_voice_text_label, 369);
            lv_label_set_long_mode(s_voice_text_label, LV_LABEL_LONG_WRAP);
        }
        // Initially hidden; shown by voice_text_update or hide button toggle
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
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
        if (s_kb_btn)     lv_obj_remove_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
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
                if (s_kb_btn)     lv_obj_remove_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
                if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
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
        if (s_kb_btn)    lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_voice_btn) lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_music_btn) lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_music_btn) lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
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
    if (s_voice_text_label) lv_obj_set_style_text_font(s_voice_text_label, font, 0);
    lvgl_port_unlock();
}

// ─── 语音记录（voice record）────────────────────────────────

struct WavHeader {
    char     riff[4]; uint32_t file_size; char     wave[4];
};

// Update voice text box from text.yaml for the given key (English filename)
static void voice_text_update(const char *key) {
    if (!s_agent_path[0]) return;
    char path[300];
    snprintf(path, sizeof(path), "%s/voice/text.yaml", s_agent_path);
    FILE *fp = fopen(path, "r");
    if (!fp) { ESP_LOGW(TAG, "No text.yaml at %s", path); return; }

    char line[1024];
    int klen = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        // Format: "key : value\n"
        if (strncmp(line, key, klen) == 0 && line[klen] == ' ' && line[klen+1] == ':' && line[klen+2] == ' ') {
            char *val = line + klen + 3;
            int vlen = strlen(val);
            while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r')) val[--vlen] = '\0';
            if (vlen > 0) {
                strncpy(s_voice_text_buf, val, sizeof(s_voice_text_buf) - 1);
                s_voice_text_buf[sizeof(s_voice_text_buf) - 1] = '\0';
                lvgl_port_lock(pdMS_TO_TICKS(200));
                if (s_voice_text_label) {
                    lv_label_set_text(s_voice_text_label, s_voice_text_buf);
                    // Only show text box in cover mode (not dialog/pending)
                    if (s_voice_text_obj && s_cover_mode)
                        lv_obj_remove_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
                }
                lvgl_port_unlock();
            }
            fclose(fp);
            return;
        }
    }
    fclose(fp);
    ESP_LOGW(TAG, "Key '%s' not found in text.yaml", key);
}

static void voice_play_task(void *path_arg) {
    char *wav_path = (char*)path_arg;
    ESP_LOGI(TAG, "Voice play: %s", wav_path);
    s_voice_cancel = false;

    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    FILE *fp = NULL;
    AudioCodec *codec = NULL;
    int16_t *chunk = NULL;
    bool was_output_on = false;  // save original state

    do {
        fp = fopen(wav_path, "rb");
        if (!fp) { ESP_LOGE(TAG, "Cannot open %s", wav_path); break; }

        // Parse WAV: skip to "fmt " chunk
        WavHeader riff;
        if (fread(&riff, sizeof(riff), 1, fp) != 1) { ESP_LOGE(TAG, "Bad RIFF header"); break; }
        if (memcmp(riff.riff, "RIFF", 4) || memcmp(riff.wave, "WAVE", 4)) {
            ESP_LOGE(TAG, "Not a WAV file"); break;
        }

        // Scan chunks until we find "fmt " and "data"
        uint16_t audio_fmt = 0, bits = 0, channels = 0;
        uint32_t sample_rate = 0, data_size = 0;
        int chunks_found = 0;

        while (chunks_found < 2) {
            char id[4]; uint32_t size;
            if (fread(id, 1, 4, fp) != 4) break;
            if (fread(&size, 4, 1, fp) != 1) break;

            if (memcmp(id, "fmt ", 4) == 0) {
                fread(&audio_fmt, 2, 1, fp);
                fread(&channels, 2, 1, fp);
                fread(&sample_rate, 4, 1, fp);
                fseek(fp, 6, SEEK_CUR);  // skip byte_rate + block_align
                fread(&bits, 2, 1, fp);
                // skip rest of fmt chunk if > 16
                if (size > 16) fseek(fp, size - 16, SEEK_CUR);
                chunks_found++;
                ESP_LOGI(TAG, "WAV fmt: %lu Hz, %d ch, %d bit", sample_rate, channels, bits);
            } else if (memcmp(id, "data", 4) == 0) {
                data_size = size;
                chunks_found++;
                ESP_LOGI(TAG, "WAV data: %lu bytes", data_size);
            } else {
                // Skip unknown chunk
                fseek(fp, size, SEEK_CUR);
            }
        }

        if (audio_fmt != 1) { ESP_LOGE(TAG, "Not PCM"); break; }
        if (bits != 16) { ESP_LOGE(TAG, "Not 16-bit"); break; }
        if (sample_rate != 16000) {
            ESP_LOGE(TAG, "WAV is %lu Hz — must be 16000 Hz", sample_rate); break;
        }
        if (data_size == 0) { ESP_LOGE(TAG, "No data chunk found"); break; }

        // Save original output state, then enable
        codec = Board::GetInstance().GetAudioCodec();
        was_output_on = codec->output_enabled();
        if (!was_output_on) codec->EnableOutput(true);

        #define V_CHUNK 768   // 48ms @ 16kHz — fits within DMA buffer (~90ms)
        chunk = (int16_t*)malloc(V_CHUNK * sizeof(int16_t));
        if (!chunk) break;

        int ch = (channels == 2) ? 2 : 1;
        uint32_t remain = data_size / 2;  // samples
        int loops = 0;

        while (remain > 0 && !s_voice_cancel) {
            int64_t t0 = esp_timer_get_time();

            int to_read = (int)(remain < (uint32_t)(V_CHUNK * ch) ? (remain / ch) * ch : V_CHUNK * ch);
            size_t n = fread(chunk, sizeof(int16_t), to_read, fp);
            if (n == 0) break;
            int frames = (int)(n / ch);
            loops++;

            int16_t *out = chunk;
            if (ch == 2) {
                for (int i = 0; i < frames; i++) {
                    out[i] = (int16_t)((chunk[i*2] + chunk[i*2+1]) / 2);
                }
            }

            std::vector<int16_t> pcm(out, out + frames);
            extern void application_audio_notify_output(void);
            application_audio_notify_output();
            if (!codec->output_enabled()) codec->EnableOutput(true);
            codec->OutputData(pcm);

            // Pace: each chunk = frames/sample_rate seconds, minus processing time
            int chunk_us = (int)(frames * 1000000LL / sample_rate);
            int spent_us = (int)(esp_timer_get_time() - t0);
            int delay_us = chunk_us - spent_us;
            if (delay_us > 1000) vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));

            remain -= (uint32_t)n;
        }
        ESP_LOGI(TAG, "WAV played: %d chunks, %lu ms", loops,
                 (unsigned long)((loops * V_CHUNK * 1000LL) / sample_rate));
        #undef V_CHUNK

    } while (false);

    if (chunk) free(chunk);
    if (fp) fclose(fp);
    // Only disable output if we were the ones who enabled it
    if (!was_output_on && codec) {
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableOutput(false);
    }
    free(wav_path);
    s_voice_task = NULL;

    // Only restart video if we played to completion (not cancelled)
    if (!s_voice_cancel) video_playback_start(30);

    ESP_LOGI(TAG, "Voice playback done%s", s_voice_cancel ? " (cancelled)" : "");
    vTaskDelete(NULL);
}

static void voice_ui_hide(void) {
    if (!s_voice_overlay) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        lv_obj_del(s_voice_overlay);
        s_voice_overlay = NULL;
        s_voice_cat_dd = NULL;
        s_voice_entry_dd = NULL;
        lvgl_port_unlock();
    }
    // Restore buttons (same set as profile_hide)
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_voice_btn)     lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn)    lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn)   lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)    lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)    lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    video_playback_start(30);
    ESP_LOGI(TAG, "Voice UI hidden");
}

static void voice_ui_show(void) {
    if (s_voice_overlay || s_profile_overlay) return;

    // Hide all buttons
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_voice_btn)     lv_obj_add_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn)    lv_obj_add_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn)   lv_obj_add_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)    lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)    lv_obj_add_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_add_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create overlay
    lvgl_port_lock(0);
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, 480, 800);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_voice_overlay = overlay;

    // ── Title bar ──
    lv_obj_t *bar = lv_obj_create(overlay);
    lv_obj_set_size(bar, 480, 44);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "语音记录");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_center(title);

    // ── Category dropdown ──
    lv_obj_t *cat_dd = lv_dropdown_create(overlay);
    lv_obj_set_pos(cat_dd, 140, 220);
    lv_obj_set_size(cat_dd, 200, 38);
    lv_dropdown_set_options(cat_dd, "日常\n作战中\n晋升");
    lv_dropdown_set_symbol(cat_dd, ">");
    lv_obj_set_style_bg_color(cat_dd, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(cat_dd, 4, 0);
    lv_obj_set_style_border_width(cat_dd, 0, 0);
    lv_obj_set_style_text_color(cat_dd, lv_color_white(), 0);
    lv_obj_set_style_text_font(cat_dd, s_chat_font, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(cat_dd), s_chat_font, 0);
    lv_dropdown_set_selected(cat_dd, s_voice_cat_sel);
    s_voice_cat_dd = cat_dd;

    // ── Entry dropdown (populated after category selection) ──
    lv_obj_t *entry_dd = lv_dropdown_create(overlay);
    lv_obj_set_pos(entry_dd, 140, 275);
    lv_obj_set_size(entry_dd, 200, 38);
    lv_dropdown_set_symbol(entry_dd, ">");
    lv_obj_set_style_bg_color(entry_dd, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(entry_dd, 4, 0);
    lv_obj_set_style_border_width(entry_dd, 0, 0);
    lv_obj_set_style_text_color(entry_dd, lv_color_white(), 0);
    lv_obj_set_style_text_font(entry_dd, s_chat_font, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(entry_dd), s_chat_font, 0);
    s_voice_entry_dd = entry_dd;

    // Helper: populate entry dropdown for current category
    auto populate_entries = [&]() {
        const VoiceEntry *tbl = nullptr; int n = 0;
        switch (s_voice_cat_sel) {
            case 0: tbl = VOICE_DAILY; n = VOICE_DAILY_N; break;
            case 1: tbl = VOICE_FIGHT; n = VOICE_FIGHT_N; break;
            case 2: tbl = VOICE_PROMOTION; n = VOICE_PROMOTION_N; break;
        }
        char buf[2048]; int pos = 0;
        for (int i = 0; i < n; i++) {
            if (i > 0) buf[pos++] = '\n';
            int len = strlen(tbl[i].label);
            memcpy(buf + pos, tbl[i].label, len); pos += len;
        }
        buf[pos] = '\0';
        lv_dropdown_set_options(entry_dd, buf);
        lv_dropdown_set_selected(entry_dd, 0);
    };
    populate_entries();

    // Category change → repopulate entries
    lv_obj_add_event_cb(cat_dd, [](lv_event_t *e) {
        int sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
        if (sel == s_voice_cat_sel) return;
        s_voice_cat_sel = sel;
        auto populate = [&]() {
            const VoiceEntry *tbl = nullptr; int n = 0;
            switch (s_voice_cat_sel) {
                case 0: tbl = VOICE_DAILY; n = VOICE_DAILY_N; break;
                case 1: tbl = VOICE_FIGHT; n = VOICE_FIGHT_N; break;
                case 2: tbl = VOICE_PROMOTION; n = VOICE_PROMOTION_N; break;
            }
            char buf[2048]; int pos = 0;
            for (int i = 0; i < n; i++) {
                if (i > 0) buf[pos++] = '\n';
                int len = strlen(tbl[i].label);
                memcpy(buf + pos, tbl[i].label, len); pos += len;
            }
            buf[pos] = '\0';
            lv_dropdown_set_options(s_voice_entry_dd, buf);
            lv_dropdown_set_selected(s_voice_entry_dd, 0);
        };
        populate();
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Entry selected → play
    lv_obj_add_event_cb(entry_dd, [](lv_event_t *e) {
        int sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
        const VoiceEntry *tbl = nullptr;
        switch (s_voice_cat_sel) {
            case 0: tbl = VOICE_DAILY; break;
            case 1: tbl = VOICE_FIGHT; break;
            case 2: tbl = VOICE_PROMOTION; break;
        }
        if (!tbl) return;
        if (!s_agent_path[0]) return;

        // Build path
        int len = snprintf(nullptr, 0, "%s/voice/%s/%s.wav",
                           s_agent_path, VOICE_CATEGORIES[s_voice_cat_sel], tbl[sel].file);
        char *wav_path = (char*)malloc(len + 1);
        if (!wav_path) return;
        snprintf(wav_path, len + 1, "%s/voice/%s/%s.wav",
                 s_agent_path, VOICE_CATEGORIES[s_voice_cat_sel], tbl[sel].file);

        ESP_LOGI(TAG, "Voice select: %s", wav_path);
        voice_text_update(tbl[sel].file);

        // Stop any previous playback (voice or music)
        if (s_music_task) { s_music_cancel = true; while (s_music_task) vTaskDelay(pdMS_TO_TICKS(10)); }
        if (s_voice_task) { s_voice_cancel = true; while (s_voice_task) vTaskDelay(pdMS_TO_TICKS(10)); }

        voice_ui_hide();

        xTaskCreate(voice_play_task, "voice", 8192, wav_path, 3, &s_voice_task);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Tap overlay background → cancel
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, [](lv_event_t *e) {
        if (lv_event_get_target(e) != s_voice_overlay) return;  // ignore children
        voice_ui_hide();
    }, LV_EVENT_CLICKED, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Voice UI shown");
}

// ─── 背景音乐 ────────────────────────────────

static void music_play_task(void *path_arg) {
    char *wav_path = (char*)path_arg;
    ESP_LOGI(TAG, "Music play: %s", wav_path);
    s_music_cancel = false;

    // Don't stop video for music — it plays in background
    FILE *fp = NULL;
    AudioCodec *codec = NULL;
    int16_t *chunk = NULL;
    bool was_output_on = false;

    do {
        fp = fopen(wav_path, "rb");
        if (!fp) { ESP_LOGE(TAG, "Cannot open %s", wav_path); break; }

        WavHeader riff;
        if (fread(&riff, sizeof(riff), 1, fp) != 1) { ESP_LOGE(TAG, "Bad RIFF header"); break; }
        if (memcmp(riff.riff, "RIFF", 4) || memcmp(riff.wave, "WAVE", 4)) {
            ESP_LOGE(TAG, "Not a WAV file"); break; }

        uint16_t audio_fmt = 0, bits = 0, channels = 0;
        uint32_t sample_rate = 0, data_size = 0;
        int chunks_found = 0;
        while (chunks_found < 2) {
            char id[4]; uint32_t size;
            if (fread(id, 1, 4, fp) != 4) break;
            if (fread(&size, 4, 1, fp) != 1) break;
            if (memcmp(id, "fmt ", 4) == 0) {
                fread(&audio_fmt, 2, 1, fp); fread(&channels, 2, 1, fp);
                fread(&sample_rate, 4, 1, fp); fseek(fp, 6, SEEK_CUR); fread(&bits, 2, 1, fp);
                if (size > 16) fseek(fp, size - 16, SEEK_CUR);
                chunks_found++;
            } else if (memcmp(id, "data", 4) == 0) {
                data_size = size; chunks_found++;
            } else { fseek(fp, size, SEEK_CUR); }
        }
        if (audio_fmt != 1 || bits != 16 || sample_rate != 16000 || data_size == 0) {
            ESP_LOGE(TAG, "Bad WAV: fmt=%d bits=%d rate=%lu data=%lu", audio_fmt, bits, sample_rate, data_size);
            break;
        }
        ESP_LOGI(TAG, "Music WAV: %lu Hz, %d ch, %lu bytes", sample_rate, channels, data_size);

        codec = Board::GetInstance().GetAudioCodec();
        was_output_on = codec->output_enabled();
        if (!was_output_on) codec->EnableOutput(true);

        #define M_CHUNK 768
        chunk = (int16_t*)malloc(M_CHUNK * sizeof(int16_t));
        if (!chunk) break;

        int ch = (channels == 2) ? 2 : 1;
        uint32_t remain = data_size / 2;
        int loops = 0;

        while (remain > 0 && !s_music_cancel) {
            int64_t t0 = esp_timer_get_time();
            int to_read = (int)(remain < (uint32_t)(M_CHUNK * ch) ? (remain / ch) * ch : M_CHUNK * ch);
            size_t n = fread(chunk, sizeof(int16_t), to_read, fp);
            if (n == 0) break;
            int frames = (int)(n / ch); loops++;

            int16_t *out = chunk;
            if (ch == 2) {
                for (int i = 0; i < frames; i++) out[i] = (int16_t)((chunk[i*2] + chunk[i*2+1]) / 2);
            }
            std::vector<int16_t> pcm(out, out + frames);
            extern void application_audio_notify_output(void);
            application_audio_notify_output();
            if (!codec->output_enabled()) codec->EnableOutput(true);
            codec->OutputData(pcm);

            int chunk_us = (int)(frames * 1000000LL / sample_rate);
            int spent_us = (int)(esp_timer_get_time() - t0);
            int delay_us = chunk_us - spent_us;
            if (delay_us > 1000) vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
            remain -= (uint32_t)n;
        }
        #undef M_CHUNK
        ESP_LOGI(TAG, "Music played: %d chunks", loops);
    } while (false);

    if (chunk) free(chunk);
    if (fp) fclose(fp);
    if (!was_output_on && codec) { vTaskDelay(pdMS_TO_TICKS(100)); codec->EnableOutput(false); }
    free(wav_path);
    s_music_task = NULL;
    ESP_LOGI(TAG, "Music playback done%s", s_music_cancel ? " (cancelled)" : "");
    vTaskDelete(NULL);
}

static void music_ui_hide(void) {
    if (!s_music_overlay) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        lv_obj_del(s_music_overlay);
        s_music_overlay = NULL; s_music_dd = NULL;
        lvgl_port_unlock();
    }
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_music_btn)    lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_voice_btn)    lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn)  lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)   lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)   lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    video_playback_start(30);
    ESP_LOGI(TAG, "Music UI hidden");
}

static void music_ui_show(void) {
    if (s_music_overlay || s_voice_overlay || s_profile_overlay) return;

    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_music_btn)    lv_obj_add_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_voice_btn)    lv_obj_add_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn)  lv_obj_add_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)   lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)   lv_obj_add_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_add_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Read backgroundmusic.yaml → build options
    char options[4096] = {0};
    int opt_pos = 0;
    FILE *yf = fopen(MUSIC_DIR "/backgroundmusic.yaml", "r");
    if (yf) {
        char line[256];
        while (fgets(line, sizeof(line), yf) && opt_pos < (int)sizeof(options) - 64) {
            // Format: "display_name : filename.wav"
            char *sep = strstr(line, " : ");
            if (!sep) continue;
            if (opt_pos > 0) options[opt_pos++] = '\n';
            int dlen = (int)(sep - line);
            if (dlen > 60) dlen = 60;
            memcpy(options + opt_pos, line, dlen);
            opt_pos += dlen;
        }
        fclose(yf);
    }
    if (opt_pos == 0) {
        // Fallback: list .wav files directly
        DIR *d = opendir(MUSIC_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && opt_pos < (int)sizeof(options) - 64) {
                const char *n = e->d_name;
                int nlen = strlen(n);
                if (nlen < 5 || strcasecmp(n + nlen - 4, ".wav") != 0) continue;
                if (opt_pos > 0) options[opt_pos++] = '\n';
                int cp = (nlen - 4 < 60) ? nlen - 4 : 60;
                memcpy(options + opt_pos, n, cp);
                opt_pos += cp;
            }
            closedir(d);
        }
    }

    lvgl_port_lock(0);
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, 480, 800);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_music_overlay = overlay;

    lv_obj_t *bar = lv_obj_create(overlay);
    lv_obj_set_size(bar, 480, 44);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "背景音乐");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_center(title);

    lv_obj_t *dd = lv_dropdown_create(overlay);
    lv_obj_set_pos(dd, 80, 220);
    lv_obj_set_size(dd, 320, 38);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_symbol(dd, ">");
    lv_obj_set_style_bg_color(dd, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(dd, 4, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_text_color(dd, lv_color_white(), 0);
    lv_obj_set_style_text_font(dd, s_chat_font, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), s_chat_font, 0);
    s_music_dd = dd;

    lv_obj_add_event_cb(dd, [](lv_event_t *e) {
        int sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
        // Get selected display name from the dropdown options string
        char sel_name[64] = {0};
        lv_dropdown_get_selected_str((lv_obj_t*)lv_event_get_target(e), sel_name, sizeof(sel_name));
        ESP_LOGI(TAG, "Music select: %s (idx=%d)", sel_name, sel);

        // Look up filename from yaml
        char filename[128] = {0};
        FILE *yf = fopen(MUSIC_DIR "/backgroundmusic.yaml", "r");
        if (yf) {
            char line[256];
            while (fgets(line, sizeof(line), yf)) {
                char *sep = strstr(line, " : ");
                if (!sep) continue;
                int dlen = (int)(sep - line);
                if (dlen > 63) dlen = 63;
                if (strncmp(line, sel_name, dlen) == 0 && (int)strlen(sel_name) == dlen) {
                    char *fn = sep + 3;
                    int flen = strlen(fn);
                    while (flen > 0 && (fn[flen-1] == '\n' || fn[flen-1] == '\r')) fn[--flen] = '\0';
                    if (flen < (int)sizeof(filename)) { memcpy(filename, fn, flen); filename[flen] = '\0'; }
                    break;
                }
            }
            fclose(yf);
        }
        // Fallback: use display name as filename
        if (filename[0] == '\0') snprintf(filename, sizeof(filename), "%s.wav", sel_name);
        // Ensure .wav extension
        int fnlen = strlen(filename);
        if (fnlen < 4 || strcasecmp(filename + fnlen - 4, ".wav") != 0) {
            if (fnlen < (int)sizeof(filename) - 4) strcat(filename, ".wav");
        }

        // Cancel any playing audio
        if (s_voice_task) { s_voice_cancel = true; while (s_voice_task) vTaskDelay(pdMS_TO_TICKS(10)); }
        if (s_music_task) { s_music_cancel = true; while (s_music_task) vTaskDelay(pdMS_TO_TICKS(10)); }

        int len = snprintf(nullptr, 0, MUSIC_DIR "/%s", filename);
        char *wav_path = (char*)malloc(len + 1);
        if (!wav_path) return;
        snprintf(wav_path, len + 1, MUSIC_DIR "/%s", filename);
        ESP_LOGI(TAG, "Music play: %s", wav_path);

        music_ui_hide();
        xTaskCreate(music_play_task, "music", 8192, wav_path, 3, &s_music_task);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, [](lv_event_t *e) {
        if (lv_event_get_target(e) != s_music_overlay) return;
        music_ui_hide();
    }, LV_EVENT_CLICKED, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Music UI shown");
}

// ─── 九键键盘 ────────────────────────────────

static int kb_update_candidates(void);
static void kb_select_candidate(int idx);

static void keyboard_ui_hide(void) {
    if (!s_kb_overlay) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        lv_obj_del(s_kb_overlay);
        s_kb_overlay = NULL;
        s_kb_preview_lbl = NULL;
        for (int i = 0; i < 6; i++) { s_kb_cand_btns[i] = NULL; s_kb_cand_labels[i] = NULL; }
        lvgl_port_unlock();
    }
    // Restore expression-mode buttons (no rhodes_btn — cover-only)
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_kb_btn)       lv_obj_remove_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_voice_btn)    lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn)    lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn)  lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)   lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    if (s_kb_btn && lvgl_port_lock(pdMS_TO_TICKS(200))) {
        lv_obj_t *lbl = lv_obj_get_child(s_kb_btn, 0);
        if (lbl) lv_label_set_text(lbl, "弹出键盘");
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "Keyboard UI hidden");
}

static void keyboard_sync_to_input(void) {
    if (lvgl_port_lock(pdMS_TO_TICKS(2000))) {
        if (s_chat_user_label) {
            lv_label_set_text(s_chat_user_label, s_kb_input);
            lv_obj_invalidate(s_chat_user_label);
        }
        if (s_kb_preview_lbl) {
            lv_label_set_text(s_kb_preview_lbl, s_kb_input);
        }
        int nc = kb_update_candidates();
        ESP_LOGI(TAG, "KB sync: '%s' → %d candidates", s_kb_input, nc);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "KB sync: LOCK FAILED");
    }
}

static lv_obj_t* make_kb_btn(lv_obj_t *parent, const char *text,
                               int x, int y, int w, int h,
                               lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, s_chat_font, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

// Direct character insertion (for 26-key mode)
static void kb_insert_char(char c) {
    s_kb_last_key = -1; // reset multi-tap state
    if (s_kb_pos < (int)KB_BUF_SIZE - 1) {
        s_kb_input[s_kb_pos++] = c;
        s_kb_input[s_kb_pos] = '\0';
    }
    keyboard_sync_to_input();
}

static void kb_tap_key(int key) {
    int64_t now = esp_timer_get_time();
    // If same key within 1500ms, cycle to next letter
    if (key == s_kb_last_key && (now - s_kb_last_tap_us) < 700000) {
        s_kb_tap_count++;
    } else {
        s_kb_tap_count = 0;
    }

    const char *letters = KB_KEYS[key];
    int n = strlen(letters);

    // Replace last char if same key, else append new char
    if (s_kb_tap_count > 0 && s_kb_pos > 0 && s_kb_last_key == key) {
        // Replace last character with next in cycle
        int idx = s_kb_tap_count % n;
        s_kb_input[s_kb_pos - 1] = letters[idx];
    } else {
        if (s_kb_pos < (int)KB_BUF_SIZE - 1) {
            s_kb_input[s_kb_pos++] = letters[s_kb_tap_count % n];
            s_kb_input[s_kb_pos] = '\0';
        }
    }
    s_kb_last_key = key;
    s_kb_last_tap_us = now;
    ESP_LOGI(TAG, "KB input: '%s'", s_kb_input);
    keyboard_sync_to_input();
}

static void kb_backspace(void) {
    if (s_kb_pos <= 0) return;
    int del = 1;
    if (s_kb_pos >= 2) {
        unsigned char c = (unsigned char)s_kb_input[s_kb_pos - 1];
        if ((c & 0xC0) == 0x80) {
            del = 2;
            if (s_kb_pos >= 3 && ((unsigned char)s_kb_input[s_kb_pos - 3] & 0xF0) == 0xE0) del = 3;
            if (s_kb_pos >= 4 && ((unsigned char)s_kb_input[s_kb_pos - 4] & 0xF8) == 0xF0) del = 4;
        }
    }
    s_kb_pos -= del;
    s_kb_input[s_kb_pos] = '\0';
    s_kb_last_key = -1;
    keyboard_sync_to_input();
}

// Extract last word: stops at spaces or Chinese characters (non-ASCII)
static const char* kb_last_word(void) {
    if (s_kb_pos == 0) return s_kb_input;
    // Walk backward — stop at space or any byte >= 0x80 (Chinese UTF-8)
    int start = s_kb_pos;
    for (int i = s_kb_pos - 1; i >= 0; i--) {
        unsigned char c = (unsigned char)s_kb_input[i];
        if (c == ' ' || c >= 0x80) { start = i + 1; break; }
        if (i == 0) start = 0;
    }
    return s_kb_input + start;
}

static int kb_update_candidates(void) {
    s_kb_cand_page = 0;
    const char *last = kb_last_word();
    int plen = strlen(last);
    if (plen < 1 || plen > 7) { for (int i = 0; i < 6; i++) if (s_kb_cand_labels[i]) lv_label_set_text(s_kb_cand_labels[i], ""); s_kb_cand_total = 0; return 0; }
    const uint8_t *data = py_lookup(last);
    if (!data) { for (int i = 0; i < 6; i++) if (s_kb_cand_labels[i]) lv_label_set_text(s_kb_cand_labels[i], ""); s_kb_cand_total = 0; return 0; }
    int count = data[0]; data++; s_kb_cand_total = count;
    s_kb_cand_page = 0;
    int show = (count > 6) ? 5 : count;
    for (int i = 0; i < show; i++) {
        if (s_kb_cand_labels[i]) {
            char u[8]={0}; int cl=strlen((const char*)data); if(cl>7)cl=7;
            memcpy(u,data,cl); lv_label_set_text(s_kb_cand_labels[i],u); data+=cl+1;
        }
    }
    for (int i = show; i < 5; i++) if(s_kb_cand_labels[i]) lv_label_set_text(s_kb_cand_labels[i],"");
    if (s_kb_cand_labels[5]) {
        if (count > 6) lv_label_set_text(s_kb_cand_labels[5], ">");
        else if (count == 6) { char u[8]={0}; int cl=strlen((const char*)data); if(cl>7)cl=7; memcpy(u,data,cl); lv_label_set_text(s_kb_cand_labels[5],u); }
        else lv_label_set_text(s_kb_cand_labels[5], "");
    }
    return count;
}

static void kb_show_cand_page(int page) {
    const char *last = kb_last_word();
    const uint8_t *data = py_lookup(last);
    if (!data) return;
    int count = data[0]; data++;
    int start = page * 5;
    if (start >= count) return;
    for (int i = 0; i < start; i++) data += strlen((const char*)data) + 1;
    int n = count - start; if (n > 5) n = 5;
    for (int i = 0; i < 5; i++) {
        if (i < n && s_kb_cand_labels[i]) { char u[8]={0}; int cl=strlen((const char*)data); if(cl>7)cl=7; memcpy(u,data,cl); lv_label_set_text(s_kb_cand_labels[i],u); data+=cl+1; }
        else if (s_kb_cand_labels[i]) lv_label_set_text(s_kb_cand_labels[i],"");
    }
    if (s_kb_cand_labels[5]) {
        if ((page+1)*5 < count) lv_label_set_text(s_kb_cand_labels[5], ">");
        else lv_label_set_text(s_kb_cand_labels[5], page>0 ? "<" : "");
    }
    s_kb_cand_page = page;
}


// Replace last pinyin word with selected Chinese character
static void kb_select_candidate(int idx) {
    const char *last = kb_last_word();
    int last_start = last - s_kb_input;
    int plen = strlen(last);
    const uint8_t *data = py_lookup(last);
    if (!data) return;
    int count = data[0]; data++;
    if (idx >= count) return;
    // Skip to idx-th character
    for (int i = 0; i < idx; i++) data += strlen((const char*)data) + 1;
    const char *utf8 = (const char*)data;
    int clen = strlen(utf8);
    ESP_LOGI(TAG, "KB select: last='%s' start=%d clen=%d plen=%d", last, last_start, clen, plen);

    // Replace pinyin with selected Chinese character
    int new_pos = last_start + clen;
    memmove(s_kb_input + last_start + clen, s_kb_input + last_start + plen, s_kb_pos - last_start - plen + 1);
    memcpy(s_kb_input + last_start, utf8, clen);
    s_kb_pos = last_start + clen + (s_kb_pos - last_start - plen);
    s_kb_last_key = -1;
    keyboard_sync_to_input();
    kb_update_candidates();
}

static void kb_space_or_zero(void) {
    // Cycle: 0 → space → 0 ...
    int64_t now = esp_timer_get_time();
    if (s_kb_last_key == 0 && (now - s_kb_last_tap_us) < 700000) {
        // Replace last char with space
        if (s_kb_pos > 0 && s_kb_input[s_kb_pos - 1] == '0') {
            s_kb_input[s_kb_pos - 1] = ' ';
        } else if (s_kb_pos > 0 && s_kb_input[s_kb_pos - 1] == ' ') {
            s_kb_input[s_kb_pos - 1] = '0';
        }
    } else if (s_kb_pos < (int)KB_BUF_SIZE - 1) {
        s_kb_input[s_kb_pos++] = '0';
        s_kb_input[s_kb_pos] = '\0';
    }
    s_kb_last_key = 0;
    s_kb_last_tap_us = now;
    keyboard_sync_to_input();
}

static void keyboard_ui_show(void) {
    if (s_kb_overlay || s_voice_overlay || s_music_overlay || s_profile_overlay) return;
    // Lazy-alloc input buffer from PSRAM (avoids BSS corruption)
    if (!s_kb_input) {
        s_kb_input = (char*)heap_caps_malloc(KB_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (s_kb_input) memset(s_kb_input, 0, KB_BUF_SIZE);
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_kb_btn)       lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_voice_btn)    lv_obj_add_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn)    lv_obj_add_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn)  lv_obj_add_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)   lv_obj_add_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_add_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }

    lvgl_port_lock(0);
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, 480, 420);
    lv_obj_set_pos(overlay, 0, 380);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_kb_overlay = overlay;

    lv_obj_t *preview = lv_obj_create(overlay);
    lv_obj_set_size(preview, 460, 36);
    lv_obj_set_pos(preview, 10, 4);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_radius(preview, 4, 0);
    lv_obj_set_style_pad_all(preview, 4, 0);
    s_kb_preview_lbl = lv_label_create(preview);
    lv_label_set_text(s_kb_preview_lbl, s_kb_input);
    lv_obj_set_style_text_color(s_kb_preview_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_kb_preview_lbl, s_chat_font, 0);
    lv_obj_center(s_kb_preview_lbl);

    int cand_w = 73, cand_h = 38, cand_gap = 5, cand_y = 44;
    for (int i = 0; i < 6; i++) {
        int cx = 10 + i * (cand_w + cand_gap);
        lv_obj_t *cb = lv_btn_create(overlay);
        lv_obj_set_pos(cb, cx, cand_y);
        lv_obj_set_size(cb, cand_w, cand_h);
        lv_obj_set_style_bg_color(cb, lv_color_hex(0x335566), 0);
        lv_obj_set_style_radius(cb, 3, 0);
        lv_obj_set_style_border_width(cb, 0, 0);
        lv_obj_t *cl = lv_label_create(cb);
        lv_label_set_text(cl, "");
        lv_obj_set_style_text_color(cl, lv_color_white(), 0);
        lv_obj_set_style_text_font(cl, s_chat_font, 0);
        lv_obj_center(cl);
        s_kb_cand_btns[i] = cb;
        s_kb_cand_labels[i] = cl;
        lv_obj_add_event_cb(cb, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (idx == 5 && s_kb_cand_total > 6) {
                int np = s_kb_cand_page + 1;
                if (np * 5 >= s_kb_cand_total) np = 0; // wrap
                kb_show_cand_page(np);
            } else {
                kb_select_candidate(s_kb_cand_page * 5 + idx);
            }
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    if (s_kb_mode_9key) {
        #define K9W 112
        #define K9H 52
        #define K9G 3
        #define K9X(c)  (10+(c)*((K9W)+(K9G)))
        #define K9Y(r)  (88+(r)*((K9H)+(K9G)))
        make_kb_btn(overlay,"1",       K9X(0),K9Y(0),K9W,K9H, [](lv_event_t*){ kb_tap_key(1); });
        make_kb_btn(overlay,"2 abc",  K9X(1),K9Y(0),K9W,K9H, [](lv_event_t*){ kb_tap_key(2); });
        make_kb_btn(overlay,"3 def",  K9X(2),K9Y(0),K9W,K9H, [](lv_event_t*){ kb_tap_key(3); });
        make_kb_btn(overlay,"\xe5\x88\xa0\xe9\x99\xa4", K9X(3),K9Y(0),K9W,K9H, [](lv_event_t*){ kb_backspace(); });
        make_kb_btn(overlay,"4 ghi",  K9X(0),K9Y(1),K9W,K9H, [](lv_event_t*){ kb_tap_key(4); });
        make_kb_btn(overlay,"5 jkl",  K9X(1),K9Y(1),K9W,K9H, [](lv_event_t*){ kb_tap_key(5); });
        make_kb_btn(overlay,"6 mno",  K9X(2),K9Y(1),K9W,K9H, [](lv_event_t*){ kb_tap_key(6); });
        make_kb_btn(overlay,"\xe6\xb8\x85\xe7\xa9\xba", K9X(3),K9Y(1),K9W,K9H, [](lv_event_t*){ s_kb_input[0]=0;s_kb_pos=0;s_kb_last_key=-1;keyboard_sync_to_input(); });
        make_kb_btn(overlay,"7 pqrs", K9X(0),K9Y(2),K9W,K9H, [](lv_event_t*){ kb_tap_key(7); });
        make_kb_btn(overlay,"8 tuv",  K9X(1),K9Y(2),K9W,K9H, [](lv_event_t*){ kb_tap_key(8); });
        make_kb_btn(overlay,"9 wxyz", K9X(2),K9Y(2),K9W,K9H, [](lv_event_t*){ kb_tap_key(9); });
        make_kb_btn(overlay,"\xe5\x8f\x91\xe9\x80\x81", K9X(3),K9Y(2),K9W,K9H, [](lv_event_t*){
            ESP_LOGI(TAG,"KB send:%s",s_kb_input);
            if (s_kb_input[0]) {
                char *text = strdup(s_kb_input);
                keyboard_ui_hide();
                xTaskCreate([](void*a){tts_speak((const char*)a);free(a);vTaskDelete(NULL);},"tts",40960,text,3,NULL);
            } else { keyboard_ui_hide(); }
        });
        make_kb_btn(overlay,"\xe9\x9a\x90\xe8\x97\x8f", K9X(2),K9Y(3),K9W*2+K9G,K9H, [](lv_event_t*){ keyboard_ui_hide(); });
        #undef K9W
        #undef K9H
        #undef K9G
        #undef K9X
        #undef K9Y
    } else {
        #define KX(c)  (8+(c)*45)
        #define KY(r)  (88+(r)*43)
        #define KW 43
        #define KH 39
        make_kb_btn(overlay,"q",KX(0),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('q');});
        make_kb_btn(overlay,"w",KX(1),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('w');});
        make_kb_btn(overlay,"e",KX(2),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('e');});
        make_kb_btn(overlay,"r",KX(3),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('r');});
        make_kb_btn(overlay,"t",KX(4),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('t');});
        make_kb_btn(overlay,"y",KX(5),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('y');});
        make_kb_btn(overlay,"u",KX(6),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('u');});
        make_kb_btn(overlay,"i",KX(7),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('i');});
        make_kb_btn(overlay,"o",KX(8),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('o');});
        make_kb_btn(overlay,"p",KX(9),KY(0),KW,KH,[](lv_event_t*){kb_insert_char('p');});
// Row 2: a..l + Send (aligned with Row 1)
        make_kb_btn(overlay,"a",KX(0),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('a');});
        make_kb_btn(overlay,"s",KX(1),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('s');});
        make_kb_btn(overlay,"d",KX(2),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('d');});
        make_kb_btn(overlay,"f",KX(3),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('f');});
        make_kb_btn(overlay,"g",KX(4),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('g');});
        make_kb_btn(overlay,"h",KX(5),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('h');});
        make_kb_btn(overlay,"j",KX(6),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('j');});
        make_kb_btn(overlay,"k",KX(7),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('k');});
        make_kb_btn(overlay,"l",KX(8),KY(1),KW,KH,[](lv_event_t*){kb_insert_char('l');});
        int send_x = KX(9)+2, send_w = 480-8-KX(9)-2;
        make_kb_btn(overlay,"Send",send_x,KY(1),send_w,KH,[](lv_event_t*){
            ESP_LOGI(TAG,"KB send:%s",s_kb_input);
            if (s_kb_input[0]) {
                char *text = strdup(s_kb_input);
                keyboard_ui_hide();
                xTaskCreate([](void*a){tts_speak((const char*)a);free(a);vTaskDelete(NULL);},"tts",40960,text,3,NULL);
            } else { keyboard_ui_hide(); }
        });
        // Row 3: z..m + Del (staggered left)
        make_kb_btn(overlay,"z",KX(1),KY(2),KW,KH,[](lv_event_t*){kb_insert_char('z');});
        make_kb_btn(overlay,"x",KX(2),KY(2),KW,KH,[](lv_event_t*){kb_insert_char('x');});
        make_kb_btn(overlay,"c",KX(3),KY(2),KW,KH,[](lv_event_t*){kb_insert_char('c');});
        make_kb_btn(overlay,"v",KX(4),KY(2),KW,KH,[](lv_event_t*){kb_insert_char('v');});
        make_kb_btn(overlay,"b",KX(5),KY(2),KW,KH,[](lv_event_t*){kb_insert_char('b');});
        make_kb_btn(overlay,"n",KX(6),KY(2),KW,KH,[](lv_event_t*){kb_insert_char('n');});
        make_kb_btn(overlay,"m",KX(7),KY(2),KW,KH,[](lv_event_t*){kb_insert_char('m');});
        int del_x = KX(8)+2, del_w = 480-8-KX(8)-2;
        make_kb_btn(overlay,"Del",del_x,KY(2),del_w,KH,[](lv_event_t*){kb_backspace();});
        // Row 4: 9Key Hide Space Clear
        make_kb_btn(overlay,"9Key",8,  KY(3),90,KH,[](lv_event_t*){s_kb_mode_9key=true;keyboard_ui_hide();keyboard_ui_show();});
        make_kb_btn(overlay,"Hide",101,KY(3),90,KH,[](lv_event_t*){keyboard_ui_hide();});
        make_kb_btn(overlay,"Space",194,KY(3),185,KH,[](lv_event_t*){kb_insert_char(' ');});
        make_kb_btn(overlay,"Clear",382,KY(3),90,KH,[](lv_event_t*){s_kb_input[0]=0;s_kb_pos=0;s_kb_last_key=-1;keyboard_sync_to_input();});
        #undef KX
        #undef KY
        #undef KW
        #undef KH
    }

    kb_update_candidates();

    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, [](lv_event_t *e) {
        if (lv_event_get_target(e) != s_kb_overlay) return;
        keyboard_ui_hide();
    }, LV_EVENT_CLICKED, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Keyboard UI shown");
}

// ─── 个性主页（蟑螂派对）────────────────────────────────

static lv_img_dsc_t* load_jpg_thumbnail(const char *path, int idx);
static void profile_hide(void);

static esp_timer_handle_t s_lv2_timer = NULL;
static bool s_rendering = false;
static int s_fps_frame = 0;
static int64_t s_fps_last = 0;
static void lv2_timer_cb(void*) {
    if (s_rendering) return;
    s_rendering = true;
    static float t = 0; t += 0.25f;
    int64_t t0 = esp_timer_get_time();
    esp_task_wdt_delete(NULL);
    lv2_update_animation(t);
    esp_task_wdt_add(NULL);
    int64_t t1 = esp_timer_get_time();
    s_fps_frame++;
    if (s_fps_last == 0) s_fps_last = t1;
    if (t1 - s_fps_last > 5000000) {
        float fps = s_fps_frame * 1000000.0f / (t1 - s_fps_last);
        ESP_LOGI("LV2", "FPS: %.1f (render: %d ms)", fps, (int)((t1-t0)/1000));
        s_fps_frame = 0; s_fps_last = t1;
    }
    lvgl_port_lock(0);
    if (s_profile_overlay) lv_obj_invalidate(s_profile_overlay);
    lvgl_port_unlock();
    s_rendering = false;
}

static void profile_show(void) {
    if (g_lv2_fb && g_lv2_fb_w > 0 && g_lv2_fb_h > 0) {
        if (s_profile_overlay) return;
        video_playback_stop();
        application_set_wake_word_detection(false); // Free PSRAM for Live2D
        lvgl_port_lock(0);
        s_profile_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_profile_overlay, g_lv2_fb_w, g_lv2_fb_h);
        lv_obj_set_pos(s_profile_overlay, 0, 0);
        lv_obj_set_style_bg_opa(s_profile_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_profile_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_profile_overlay, 0, 0);
        lv_obj_clear_flag(s_profile_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* c = lv_canvas_create(s_profile_overlay);
        lv_obj_set_size(c, g_lv2_fb_w, g_lv2_fb_h);
        lv_canvas_set_buffer(c, (uint8_t*)g_lv2_fb, g_lv2_fb_w, g_lv2_fb_h, LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(s_profile_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_profile_overlay, [](lv_event_t*){
            if (s_lv2_timer) { esp_timer_stop(s_lv2_timer); esp_timer_delete(s_lv2_timer); s_lv2_timer = NULL; }
            lv_obj_del(s_profile_overlay); s_profile_overlay = NULL;
            application_set_wake_word_detection(true); // Re-enable AFE
            video_playback_start(30);
        }, LV_EVENT_CLICKED, NULL);
        lvgl_port_unlock();
        if (!s_lv2_timer) {
            esp_timer_create_args_t a = {}; a.callback = lv2_timer_cb;
            a.dispatch_method = ESP_TIMER_TASK; a.name = "lv2";
            esp_timer_create(&a, &s_lv2_timer);
            esp_timer_start_periodic(s_lv2_timer, 250000); // 4 fps
        }
        ESP_LOGI(TAG, "Live2D preview shown (animated, AFE disabled)");
        return;
    }

    if (s_profile_overlay) return;
    s_profile_was_cover = s_cover_mode;
    s_profile_chat_was_visible = !s_cover_mode;  // 表达式模式对话框可见

    // 隐藏右上角按钮 + 对话框
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        chat_overlay_show(false);
        if (s_voice_btn)   lv_obj_add_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn)  lv_obj_add_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn) lv_obj_add_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)  lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)  lv_obj_add_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        // 隐藏按钮也在 btn_labels[0]
        if (s_btn_labels[0]) lv_obj_add_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
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
            lv_obj_clear_flag(s_profile_overlay, LV_OBJ_FLAG_SCROLLABLE);
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
        lv_obj_set_size(btn, 80, 45);
        lv_obj_set_pos(btn, 475, 713);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "动图");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, s_chat_font, 0);
        lv_obj_set_style_transform_rotation(btn, 900, 0);
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
        if (s_voice_btn)   lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_music_btn)  lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_profile_btn) lv_obj_remove_flag(s_profile_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_rhodes_btn)  lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)  lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }

    // 切回 active 播放源 + 清 profile 槽 + 恢复状态
    ppa_use_profile_cache(false);
    ppa_free_profile_slot();
    s_cover_mode = s_profile_was_cover;
    s_current_index = 0; s_loop_count = 0;
    video_playback_start(30);
    if (s_profile_chat_was_visible) chat_overlay_show(true);
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
    if (s_voice_btn) lv_obj_add_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_music_btn) lv_obj_add_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);

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
    if (s_voice_btn) lv_obj_remove_flag(s_voice_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_music_btn) lv_obj_remove_flag(s_music_btn, LV_OBJ_FLAG_HIDDEN);

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

    // 点击切换: cover→文本框, expression→对话框
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        static bool hidden = false;
        hidden = !hidden;
        if (s_cover_mode) {
            if (s_voice_text_obj) {
                if (hidden) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
                else        lv_obj_remove_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            chat_overlay_show(!hidden);
        }
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

    // ── ⑤ 语音记录（cover + expression 都可见）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 100, 35);
    lv_obj_set_pos(btn, 375, 150);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "语音记录");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    s_btn_labels[3] = lbl;
    s_voice_btn = btn;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        voice_ui_show();
    }, LV_EVENT_CLICKED, NULL);

    // ── ⑥ 背景音乐（cover + expression 都可见）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 100, 35);
    lv_obj_set_pos(btn, 375, 190);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "背景音乐");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    s_music_btn = btn;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        music_ui_show();
    }, LV_EVENT_CLICKED, NULL);

    // ── ⑦ 弹出键盘（仅 expression 模式可见）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 100, 35);
    lv_obj_set_pos(btn, 375, 230);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "弹出键盘");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    s_kb_btn = btn;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，expression 模式才显示
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        if (s_kb_overlay) {
            keyboard_ui_hide();
        } else {
            keyboard_ui_show();
        }
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
