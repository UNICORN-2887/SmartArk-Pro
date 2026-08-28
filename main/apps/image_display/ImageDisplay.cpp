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
#include "apps/loading/loading_ui.h"
#include "apps/live2d/lv2_render.h"  // lv2_set_progress_cb（C 文件，头文件带 extern "C"）
#include "apps/live2d/lv2_paperdoll.h"
#include "apps/settings/settings_ui.h"
#include "apps/menu/menu_ui.h"
#include <esp_task_wdt.h>
#include <freertos/task.h>

extern void lv2_update_animation(float time_sec);
extern void application_set_wake_word_detection(bool enable);

// Live2D framebuffer + test page forward decls
uint16_t* g_lv2_fb = NULL;
int g_lv2_fb_w = 0, g_lv2_fb_h = 0;
void lv2_test_show(void);  // 供 menu_ui 调用
static void lv2_test_hide(void);

#include "PPACompositor.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_decode.h"
#include "board.h"
#include "display/lcd_display.h"   // LcdDisplay::GetPanelFrameBuffer（PPA 直写面板 fb）
#include "driver/ppa.h"
#include "esp_cache.h"
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
static lv_obj_t *s_lv2_interact_btn = NULL;  // Live2D交互按钮（仅 expression 模式）
static lv_obj_t *s_lv2_interact_lbl = NULL;
static lv_obj_t *s_hide_btn = NULL;  // 右上角隐藏/显示按钮
static bool s_lv2_interaction = false;  // Live2D交互模式开关
static void lv2_interaction_start(void);
static void lv2_interaction_stop(void);
static lv_obj_t *s_ppd_interact_btn = NULL;  // PPD交互按钮（仅 expression 模式；纸偶引擎，Live2D 为后备）
static lv_obj_t *s_ppd_interact_lbl = NULL;
static bool s_pd_interaction = false;   // PPD交互模式开关
static bool s_pdq_mode = false;         // 横屏 Q 版互动：加载 PPD_Q（PC 预旋转横屏布局），退出回横屏立牌
static bool s_pdq_resume_after_chat = false;  // 横屏互动中唤醒对话 → 对话结束自动回横屏立牌
static bool s_anim_play = false;        // 动作测试播放中：decode 走普通分支（active 槽动画），
                                         // 否则 s_standee_mode=true 会走 standee 分支播立牌帧
static lv_obj_t *s_anim_btn = NULL;     // 互动画面"动作"按钮（横屏 Q 版互动 bg 上，与按钮组同排布 i=4）
static lv_obj_t *s_anim_panel = NULL;   // 动作列表面板（bg 上，点击"动作"展开/收起）
static lv_obj_t *s_anim_overlay = NULL; // 旧版动作测试页 overlay（备用入口）
static char s_anim_paths[16][340];
static int s_anim_count = 0;
static void anim_load_task(void *arg);  // 动作加载+播放任务（SD 读不在 LVGL 任务栈上）
static lv_obj_t* s_pd_canvas = NULL;    // PPD 互动 canvas（动作播放画到此 canvas；声明前移：decode 分支引用）
static lv_obj_t* s_chat_assistant_hdr = NULL;   // 回复框表头（横屏对话显示当前角色名）
static bool s_pd_suspend = false;       // 全屏应用抽屉（拼豆/蟑螂/Live2D测试）打开：直写暂停、交互保留
static void profile_progress_cb(const char* stage, int percent);   // profile 动图加载进度（定义在 profile 区）
static lv_obj_t* s_pd_interaction_bg = NULL;   // PPD 交互的全屏覆盖层（定义在 pd_test 区；动图播放时隐藏用）
static volatile bool s_pd_starting = false;  // 进入中守卫（防双点）
static void pd_interaction_start(void);
static void pd_interaction_stop(void);
static void pd_interaction_stop_internal(bool restart_mjpeg);
static void pdq_interaction_start(void);   // 横屏 Q 版互动入口（加载 PPD_Q，退出回横屏立牌）
static void pdq_anim_ui_create(lv_obj_t *bg);   // 互动内动作按钮+列表面板（横屏 Q 版）
// ── 横屏立牌（standee）：cover 模式下的横屏 MJPEG 立绘展示 ──
static bool s_standee_mode = false;        // 横屏立牌播放中
static volatile bool s_standee_starting = false;  // 进入中守卫（防双点）
static volatile bool s_standee_cancel = false;    // 加载中取消（唤醒对话等入口打断：task 完成后不进 standee）
static int s_standee_prev_count = 0;       // 进入前 cover 帧数（active 槽帧不动，退出秒恢复）
static int s_standee_prev_index = 0;
static void standee_task(void* arg);
static void standee_exit(bool restart_cover);
static void standee_suspend(void);   // 菜单全屏应用打开：暂停横屏播放（保留模式与 UI 旋转，返回后恢复）
static void standee_resume(void);
void ppd_interaction_resume_after_app(void);   // profile/lv2_test/beads 关闭时调用（回 PPD 待机）
static int pd_emoji_to_expression(const char* emoji);
static pd_model_t* s_pd_model = NULL;      // 纸偶模型（PDD 交互与测试页共用单例）
static SemaphoreHandle_t s_pd_mutex = NULL;  // 渲染 vs 切角色/触摸互斥
static lv_obj_t *s_profile_overlay = NULL; // Profile 全屏 overlay（点击返回）
static bool s_profile_was_cover = false;  // 进入 profile 前的模式
static int64_t s_profile_open_ms = 0;     // profile 打开时刻（防点击穿透误关）
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
static lv_obj_t *s_music_overlay = NULL;
static lv_obj_t *s_music_dd = NULL;
static TaskHandle_t s_music_task = NULL;
static volatile bool s_music_cancel = false;
#define MUSIC_DIR "/sdcard/main/music"

// ─── 九键键盘 ────────────────────────────────
static lv_obj_t *s_kb_btn = NULL;
static lv_obj_t *s_standee_btn = NULL;   // 横屏立牌按钮（仅通行证模式；占位接口）
static lv_obj_t *s_standee_lbl = NULL;
static lv_obj_t *s_settings_btn = NULL;   // 设置页按钮（cover+expression 都可见）
static lv_obj_t *s_menu_btn = NULL;       // 菜单页按钮（cover+expression 都可见）
static lv_obj_t *s_kb_overlay = NULL;
static lv_obj_t *s_kb_preview_lbl = NULL;
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
static void chat_overlay_set_landscape(bool land);   // 横屏对话：聊天框转 90° 侧边栏

// PPA抠图合成+显示（frame_index，自动MJPEG或缓存模式）
static bool decode_and_display_image(int frame_index)
{
    // 横屏立牌分支：480×800 竖帧（PC 端已转好 90°）直通解码 → canvas（与竖屏 cover 同构）；
    // 动作测试播放中走普通分支（active 槽动画帧），不能走 standee 分支（会播立牌帧）
    if (s_standee_mode && !s_anim_play) {
        uint8_t *out = ppa_composite_standee_frame(frame_index);
        if (!out) return false;
        if (!lvgl_port_lock(pdMS_TO_TICKS(100))) {
            // LVGL 忙丢帧统计（5fps 排查：旋转按钮增加 LVGL 重绘负担？）
            static int s_standee_lock_drop = 0;
            static int s_standee_lock_tick = 0;
            if (++s_standee_lock_tick >= 30) {
                ESP_LOGW(TAG, "standee: LVGL lock 超时丢帧 %d/30", s_standee_lock_drop);
                s_standee_lock_drop = 0;
                s_standee_lock_tick = 0;
            } else {
                s_standee_lock_drop++;
            }
            return false;
        }
        if (s_image_canvas) {
            lv_canvas_set_buffer(s_image_canvas, out, 480, 800, LV_COLOR_FORMAT_RGB565);
            lv_obj_invalidate(s_image_canvas);
        }
        lvgl_port_unlock();
        return true;
    }
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
    /* 动作播放：帧画到互动 bg 的 canvas（同页表达，不切 screen 层画面） */
    lv_obj_t *canvas = (s_anim_play && s_pd_canvas && lv_obj_is_valid(s_pd_canvas))
                           ? s_pd_canvas : s_image_canvas;
    if (canvas) {
        lv_canvas_set_buffer(canvas, comp_buf, 480, 800, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(canvas);
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

    // ── 加载动画进度接线：lv2 / PPA 加载回调 → loading 覆盖层 + profile 动图小指示 ──
    lv2_set_progress_cb([](const char* s, int p) { loading_set_stage(s, p); });
    extern void ppa_set_load_progress_cb(void (*)(const char*, int));
    ppa_set_load_progress_cb([](const char* s, int p) {
        loading_set_stage(s, p);
        profile_progress_cb(s, p);   // 动图加载小指示（s_profile_load_ind 非空才更新）
    });

    ESP_LOGI(TAG, "Image display initialized (empty canvas)");
    return true;
}

// ─── 展示模式（Cover）：PSRAM 预加载，30 FPS ──────────────

bool cover_display_start(const char *agent_sd_path);  // 前向声明（定义在下方）

// 索引页点角色 → cover 加载：后台 worker（latest-wins，PPA 访问串行化）。
// 卡片回调是 LVGL 事件上下文，直接同步 cover_display_start 会卡 UI（2-4MB SD 读）。
static volatile int s_cover_switch_gen = 0;   // 请求版本（每次点卡片 +1）
static bool s_cover_worker_busy = false;
static char s_cover_pending_path[300];

static void cover_switch_task(void*) {
    do {
        int want_gen = s_cover_switch_gen;
        cover_display_start(s_cover_pending_path);
        if (s_cover_switch_gen == want_gen) break;  // 期间无新请求 → 完成
    } while (true);
    loading_hide();
    s_cover_worker_busy = false;
    vTaskDelete(NULL);
}

static void cover_display_start_async(const char* agent_path) {
    strncpy(s_cover_pending_path, agent_path, sizeof(s_cover_pending_path) - 1);
    s_cover_switch_gen = s_cover_switch_gen + 1;
    loading_show("加载立绘");  // PPA 帧级回调自动报"封面 N%"
    if (!s_cover_worker_busy) {
        s_cover_worker_busy = true;
        xTaskCreate(cover_switch_task, "cover_sw", 6144, NULL, 2, NULL);
    }
}

bool cover_display_start(const char *agent_sd_path) {
    // agent 切换中（expression_display_start 持有锁）→ 跳过
    if (s_in_expression_start) return true;
    // 如果 cover 已经在跑了（mode_switch_task 先切了），跳过但确保按钮可见
    // （本函数现在可能跑在后台 cover_switch_task 里，此分支的 LVGL 调用必须加锁）
    if (s_cover_mode && strcmp(s_agent_path, agent_sd_path) == 0) {
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            if (s_rhodes_btn) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_kb_btn)    lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_standee_btn) lv_obj_remove_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
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
        if (s_standee_btn) lv_obj_remove_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_lv2_interact_btn) lv_obj_add_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_ppd_interact_btn) lv_obj_add_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);

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
    /* 横屏（立牌/Q 版互动）唤醒：不切竖屏表情模式——画面保持横屏（Q 版小人/立牌 +
       横屏按钮排布），对话直接叠加（聊天框 + 音频），表情联动走 PPD 引擎（互动中）。
       竖屏 MJPEG 切换/按钮复位/背景加载仅竖屏唤醒走 */
    if (s_standee_mode) {
        s_in_expression_start = true;
        strncpy(s_agent_path, agent_sd_path, sizeof(s_agent_path) - 1);
        s_agent_path[sizeof(s_agent_path) - 1] = 0;
        chat_overlay_set_landscape(true);   // 聊天框转 90° 视觉右侧侧边栏
        chat_overlay_show(true);
        s_in_expression_start = false;
        ESP_LOGI(TAG, "Expression skipped: landscape 对话叠加 (standee=%d pd=%d)",
                 (int)s_standee_mode, (int)s_pd_interaction);
        return true;
    }
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

    // 加载背景 → PPA blend 模式（横屏唤醒的对话用横屏适配背景）
    if (!ppa_has_background()) {
        const char *bg = s_pdq_resume_after_chat
                             ? "/sdcard/main/background/background_p.jpg"
                             : "/sdcard/main/background/background.jpg";
        ppa_load_background(bg);
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
        /* 横屏唤醒的对话：模式按钮 = 打断机制（点击退出对话回横屏立牌） */
        if (s_mode_label) lv_label_set_text(s_mode_label,
                                            s_pdq_resume_after_chat ? "退出对话" : "通行证模式");
        if (s_rhodes_btn) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_kb_btn)     lv_obj_remove_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);   // 立牌仅通行证(cover)模式显示
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
        if (s_lv2_interact_btn) { lv_obj_remove_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN); ESP_LOGI(TAG,"LV2 btn show (expr mode)"); }
        if (s_ppd_interact_btn) lv_obj_remove_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
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

// Restart MJPEG after Live2D interaction stops
void expression_restart_mjpeg(void) {
    if (s_cover_mode || s_agent_path[0] == '\0') return;
    char path[384];
    if (s_current_emotion[0]) {
        snprintf(path, sizeof(path), "%s/emoji/%s.mjpeg", s_agent_path, s_current_emotion);
    } else {
        snprintf(path, sizeof(path), "%s/emoji/neutral.mjpeg", s_agent_path);
    }
    int count = ppa_preload_mjpeg(path);
    if (count == 0) {
        snprintf(path, sizeof(path), "%s/emoji/neutral.mjpeg", s_agent_path);
        count = ppa_preload_mjpeg(path);
    }
    if (count > 0) video_playback_start(30);
}

// 切换表情（交互模式下，同智能体）
void expression_switch_emotion(const char *emotion) {
    if (s_agent_path[0] == '\0') return;

    // PPD interaction mode: emoji → 纸偶表达式（索引与 PC 仿真器 EXPRESSIONS 一致）。
    // 必须在 cover 检查之前：横屏 Q 版互动时 s_cover_mode=true 也要联动表情
    if (s_pd_interaction) {
        int expr = pd_emoji_to_expression(emotion);
        if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            pd_set_expression(s_pd_model, expr);
            xSemaphoreGive(s_pd_mutex);
        }
        ESP_LOGI(TAG, "PPD emoji: %s → expr %d", emotion, expr);
        return;
    }
    if (s_cover_mode) return;

    // Live2D interaction mode: map emoji to expression directly
    if (s_lv2_interaction) {
        extern int lv2_emoji_to_expression(const char*);
        extern void lv2_set_expression(int);
        int expr = lv2_emoji_to_expression(emotion);
        lv2_set_expression(expr);
        ESP_LOGI(TAG, "Live2D emoji: %s → expr %d", emotion, expr);
        return;
    }

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
static int s_decode_fail_streak = 0;  // 连续解码失败帧数（≥3 且非 neutral 时紧急回退）

// ── 模式切换辅助任务（大栈、低优先级，不阻塞音视频核心线程）──
static void mode_switch_task(void *arg) {
    bool to_expression = (bool)arg;

    // 横屏立牌兜底退出（唤醒词/其他入口进入模式切换时，立牌状态必须清干净）；
    // 横屏 Q 版互动（叠加在 standee 状态上）同样必须退出——stop 内 standee_resume
    // 因 s_standee_mode 已清空而 no-op，画面由下方 to_expression 分支接管。
    // 横屏状态（立牌或互动）下唤醒对话：记录标志 → 对话结束自动回横屏立牌 + 横屏背景
    if (s_standee_mode) s_pdq_resume_after_chat = true;
    if (s_standee_mode) standee_exit(false);
    if (s_pd_interaction) pd_interaction_stop();

    if (to_expression) {
        // 等异步预加载完成（防半成品帧）
        loading_set_stage("等待预加载…", -1);
        ppa_wait_pending_preload();
        ppa_wait_cover_preload();
        // 先把 active 中的 cover 移入 cover 槽（永久保留）
        if (s_cover_mode && ppa_get_cache_count() > 0) {
            if (ppa_has_cover()) {
                ppa_swap_to_cover();  // 槽有数据（旧表情）：交换恢复，active=表情可复用
            } else {
                // 槽被 profile 释放过：把 active(cover) 整体搬进槽，active 清空 → 强制加载表情
                // （否则 count>0 走"复用"分支，把 cover 帧当表情播放）
                ppa_save_active_to_cover(s_agent_path);
            }
        }
        loading_set_stage("停止播放…", -1);
        ppa_close_mjpeg();
        if (!ppa_has_background()) {
            /* 横屏互动中唤醒的对话（s_pdq_resume_after_chat 已在开头置位）用横屏适配背景
               （PC 预旋转 480×800 竖帧，用户横持看正立） */
            const char *bg = s_pdq_resume_after_chat
                                 ? "/sdcard/main/background/background_p.jpg"
                                 : "/sdcard/main/background/background.jpg";
            ppa_load_background(bg);  // 内部报"加载背景"
        }

        // save-cover swap 后 active 可能已有所需帧（从 slot 恢复的），直接复用
        int count = ppa_get_cache_count();
        if (count == 0) {
            if (strcmp(s_pending_emotion, "neutral") == 0)
                count = ppa_swap_emotion();
            if (count == 0) {
                char path[300];
                snprintf(path, sizeof(path), "%s/emoji/neutral.mjpeg", s_agent_path);
                loading_set_stage("表情", 0);  // 内部帧级回调报 N%
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
                if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
                if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
                if (s_lv2_interact_btn) { lv_obj_remove_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN); ESP_LOGI(TAG,"LV2 btn show (wake path)"); }
                if (s_ppd_interact_btn) lv_obj_remove_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
                /* 横屏唤醒的对话：模式按钮 = 打断机制（点击退出对话回横屏立牌） */
                if (s_mode_label) lv_label_set_text(s_mode_label,
                                                    s_pdq_resume_after_chat ? "退出对话" : "通行证模式");
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
        loading_set_stage("卸载背景…", -1);
        ppa_unload_background();
        s_cover_mode = true;  // 提前设标志，防 cover_display_start 竞态
        chat_overlay_show(false);
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            if (s_rhodes_btn) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_lv2_interact_btn) lv_obj_add_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);  // 与罗德岛共用槽位，必须隐藏防重合
            if (s_ppd_interact_btn) lv_obj_add_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_kb_btn)    lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_standee_btn) lv_obj_remove_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_mode_label) lv_label_set_text(s_mode_label, "对话模式");
            lvgl_port_unlock();
        }

        ESP_LOGI(TAG, "→ Return-to-cover: waiting preloads (cover=%d pending=%d)",
                 (int)ppa_has_cover(), 0);
        loading_set_stage("等待封面…", -1);
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
                loading_set_stage("封面", 0);  // 内部帧级回调报 N%
                count = ppa_preload_cover(path);
                if (count > 0) {
                    count = ppa_swap_to_cover();
                    ppa_free_cover_slot();
                    s_image_count = count; s_current_index = 0;
                    video_playback_start(30);
                }
            }
        }
        // 横屏互动中唤醒对话 → 对话结束自动重进横屏立牌（standee 槽仍在，秒回）
        if (s_pdq_resume_after_chat) {
            s_pdq_resume_after_chat = false;
            if (!s_standee_mode && !s_standee_starting) {
                s_standee_starting = true;
                loading_show("进入横屏立牌");
                xTaskCreate(standee_task, "standee", 10240, NULL, 2, NULL);
            }
        }
    }
    loading_hide();  // 统一收尾：覆盖成功/失败/无帧所有路径
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
        if (!image_display_next()) {
            // 解码失败：索引保持前进（重置回 0 会永远重试同一坏帧并误触发 auto-revert）。
            // 连续失败紧急切回 neutral 兜底——表情帧损坏场景实测会连败到 DMA2D
            // assert 崩溃（relaxed 坏帧），3 帧内止损。
            s_decode_fail_streak++;
            if (!s_cover_mode && s_decode_fail_streak >= 3 &&
                s_current_emotion[0] && strcmp(s_current_emotion, "neutral") != 0) {
                ESP_LOGE(TAG, "连续解码失败 %d 帧，紧急切回 neutral", s_decode_fail_streak);
                s_decode_fail_streak = 0;
                expression_switch_emotion("neutral");
            }
        } else {
            s_decode_fail_streak = 0;
        }
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
                    snprintf(old_emotion, sizeof(old_emotion), "%s", s_current_emotion);
                    snprintf(s_current_emotion, sizeof(s_current_emotion), "%s", s_pending_emotion);
                    snprintf(s_pending_emotion, sizeof(s_pending_emotion), "%s", old_emotion);
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
    if ((s_pd_interaction && !s_pd_suspend) || s_pd_starting) {
        /* PPD 交互期间吞掉表情动画请求（speaking 状态机/表情切换的 video_playback_start）：
           MJPEG 播放与纸偶渲染抢 CPU，且其 LVGL 全屏重绘会压垮 CPU 0（4fps 卡顿元凶之一）。
           退出交互后由 expression_restart_mjpeg 统一恢复。
           例外：s_pd_suspend（全屏应用抽屉打开，直写已暂停）期间放行——
           蟑螂派对等应用的动图需要正常播放 */
        return false;
    }
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
    /* 等播放任务彻底退出：防其最后一帧 PPA 合成与后续加载（profile/纸偶）的
       JPEG 解码/混合在 2D-DMA 通道竞争（症状：blend pending 满 + JPEG EOF 失败） */
    int guard = 0;
    while (s_video_task && guard++ < 200) vTaskDelay(pdMS_TO_TICKS(10));
}

int video_get_fps(void)
{
    return s_fps_display;
}

// ─── 横屏立牌（standee）：cover 模式下的横屏 MJPEG 立绘展示 ───
// 进入：停 cover 播放（active 槽帧保留）→ 扫描 <agent>/standee/*.mjpeg → 打开 → 双缓冲 → 按钮转 90° 排顶边
// 退出：停播放 → 关 mjpeg → 按钮复原 → 恢复 cover 帧（零重载秒切）
// 数据源：/sdcard/main/operator/<职业>/<星级>/<干员>/standee/*.mjpeg（800×480 横构图）

// 右上 5 按钮转 90° 排到竖屏顶边（= 用户横持设备的左侧）；
// 罗德岛横屏隐藏（换角色仅竖屏 cover 可做）；显示/隐藏按钮加入排布（横屏对话侧边栏开关）
// 旋转几何：pivot 左上角 (0,0) + rotation 900（顺时针）→ 视觉矩形 x∈[pos_x,pos_x+35], y∈[pos_y-110,pos_y]
// 目标视觉：x 从 190 开始、y∈[-100,10] → pos_x=190+i*40, pos_y=10（用户实测位置）
//（对照实验结论：旋转渲染只值 ~2fps，非主瓶颈；恢复 900）
static void standee_ui_rotate(bool enter) {
    lv_obj_t* btns[5] = {
        s_settings_btn,
        s_menu_btn,
        lv_obj_get_parent(s_mode_label),   // 模式切换按钮
        s_standee_btn,
        s_hide_btn,                        // 显示/隐藏聊天框（横屏对话侧边栏开关）
    };
    static const int orig_pos[5][2] = {
        {366, 5}, {366, 45}, {366, 85}, {366, 285}, {366, 125},
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t* b = btns[i];
        if (!b) continue;
        if (enter) {
            lv_obj_set_pos(b, 190 + i * 40, 10);
            lv_obj_set_style_transform_pivot_x(b, 0, 0);
            lv_obj_set_style_transform_pivot_y(b, 0, 0);
            lv_obj_set_style_transform_rotation(b, 900, 0);
        } else {
            lv_obj_set_pos(b, orig_pos[i][0], orig_pos[i][1]);
            lv_obj_set_style_transform_rotation(b, 0, 0);
            // pivot 恢复对象中心（110×35）
            lv_obj_set_style_transform_pivot_x(b, 55, 0);
            lv_obj_set_style_transform_pivot_y(b, 17, 0);
        }
    }
    // 横屏隐藏罗德岛（换角色仅竖屏 cover 可做），退出横屏恢复显示
    if (s_rhodes_btn) {
        if (enter) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void standee_task(void* arg) {
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    // 保存 cover 播放状态：active 槽帧数据不动，退出时只恢复计数即可秒切
    s_standee_prev_count = s_image_count;
    s_standee_prev_index = s_current_index;

    // 扫描 <agent>/standee/ 第一个 .mjpeg
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/standee", s_agent_path);
    char path[400] = {0};
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *ext = strrchr(e->d_name, '.');
            if (ext && strcasecmp(ext, ".mjpeg") == 0) {
                // 长度检查 + memcpy 拼接（snprintf %.*s 触发 -Wformat-truncation）
                size_t alen = strlen(s_agent_path), nlen = strlen(e->d_name);
                if (alen + 9 + nlen >= sizeof(path)) continue;   // "/standee/" = 9
                memcpy(path, s_agent_path, alen);
                memcpy(path + alen, "/standee/", 9);
                memcpy(path + alen + 9, e->d_name, nlen + 1);
                break;
            }
        }
        closedir(d);
    }
    if (!path[0]) {
        ESP_LOGW(TAG, "standee: %s 无 .mjpeg 文件", dir);
        loading_hide();
        s_standee_starting = false;
        if (!s_standee_cancel) video_playback_start(30);   // 取消中不回退播放（与对话冲突）
        s_standee_cancel = false;
        vTaskDelete(NULL);
        return;
    }
    // 全量预加载到独立 standee 槽（首次 ~13s 进度条自动驱动；同角色重进秒返回）
    int count = ppa_preload_standee(path);
    if (count <= 0) {
        ESP_LOGE(TAG, "standee: preload fail %s", path);
        loading_hide();
        s_standee_starting = false;
        if (!s_standee_cancel) video_playback_start(30);
        s_standee_cancel = false;
        vTaskDelete(NULL);
        return;
    }
    if (s_standee_cancel) {
        /* 加载期间被唤醒对话等入口打断：不进入 standee 模式（槽内容保留无害） */
        s_standee_starting = false;
        s_standee_cancel = false;
        ESP_LOGI(TAG, "Standee enter cancelled");
        vTaskDelete(NULL);
        return;
    }
    s_image_count = count;
    s_current_index = 0;
    s_standee_mode = true;

    // UI：按钮文字切换 + 右上按钮组转 90° 排顶边
    lvgl_port_lock(0);
    if (s_standee_lbl) lv_label_set_text(s_standee_lbl, "竖屏通行证模式");
    standee_ui_rotate(true);
    lvgl_port_unlock();

    loading_hide();
    video_playback_start(30);
    s_standee_starting = false;
    ESP_LOGI(TAG, "Standee mode ON: %s (%d frames)", path, count);
    vTaskDelete(NULL);
}

// cover 重载任务：互动退出后 active 槽已被释放（ppa_release_playback_caches），
// standee_exit(true) 恢复 cover 时无帧可播（曾卡死在立牌一帧）——后台重载
static void standee_reload_cover_task(void *arg) {
    loading_show("返回通行证");
    char cover_dir[300];
    snprintf(cover_dir, sizeof(cover_dir), "%s/cover", s_agent_path);
    char path[520] = {0};
    DIR *d = opendir(cover_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *ext = strrchr(e->d_name, '.');
            if (!ext || strcasecmp(ext, ".mjpeg") != 0) continue;
            size_t alen = strlen(cover_dir), nlen = strlen(e->d_name);
            if (alen + 1 + nlen < sizeof(path)) {
                memcpy(path, cover_dir, alen);
                path[alen] = '/';
                memcpy(path + alen + 1, e->d_name, nlen + 1);
            }
            break;
        }
        closedir(d);
    }
    int count = 0;
    if (path[0]) {
        count = ppa_preload_cover(path);
        if (count > 0) count = ppa_swap_to_cover();
    }
    loading_hide();
    if (count > 0) {
        s_image_count = count;
        s_current_index = 0;
        s_loop_count = 0;
        video_playback_start(30);
        ESP_LOGI(TAG, "Cover reloaded (%d frames) after interaction", count);
    }
    vTaskDelete(NULL);
}

static void standee_exit(bool restart_cover) {
    if (!s_standee_mode) {
        /* 加载中（standee_task 还在跑）：置取消标志，task 完成后不进 standee */
        if (s_standee_starting) s_standee_cancel = true;
        return;
    }
    video_playback_stop();
    s_standee_mode = false;   // 先清标志：decode 分支回主路径
    // standee 槽缓存保留：同角色重进秒开（PSRAM 32MB 充裕）
    // UI 复原（文字/位置/旋转）
    lvgl_port_lock(0);
    if (s_standee_lbl) lv_label_set_text(s_standee_lbl, "横屏立牌");
    standee_ui_rotate(false);
    lvgl_port_unlock();
    // 恢复 cover 播放：active 槽仍有效（未经过互动）→ 秒切；
    // 互动后 active 槽已被释放 → 后台重载 cover（否则卡死在立牌最后一帧）
    if (restart_cover && s_standee_prev_count > 0) {
        if (ppa_get_cache_count() > 0) {
            s_image_count = s_standee_prev_count;
            s_current_index = s_standee_prev_index;
            s_loop_count = 0;
            video_playback_start(30);
        } else {
            xTaskCreate(standee_reload_cover_task, "cover_reload", 10240, NULL, 2, NULL);
        }
    }
    ESP_LOGI(TAG, "Standee mode OFF (restart_cover=%d)", (int)restart_cover);
}

// 菜单全屏应用打开前暂停横屏播放：缓存槽保留（帧数据在 PSRAM，无 SD 资源可释放）
static void standee_suspend(void) {
    if (!s_standee_mode) return;
    video_playback_stop();
    ESP_LOGI(TAG, "Standee suspended (frame %d)", s_current_index);
}

// 应用关闭后恢复横屏播放（缓存仍在，直接续播）
static void standee_resume(void) {
    if (!s_standee_mode) return;
    if (s_pd_interaction) return;   // Q 版互动中不启动 MJPEG（PPA 直写与播放互斥）
    video_playback_start(30);
    ESP_LOGI(TAG, "Standee resumed (idx %d)", s_current_index);
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
    loading_set_font(s_chat_font);  // 加载动画覆盖层也用板级中文字体
    menu_ui_init(s_chat_font);      // 菜单页同样用板级中文字体
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
    // Restore buttons (same set as profile_hide)；横屏下罗德岛保持隐藏
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_rhodes_btn && !s_standee_mode) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn)  lv_obj_remove_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)      lv_obj_remove_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)    lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    video_playback_start(30);
    ESP_LOGI(TAG, "Voice UI hidden");
}

void voice_ui_show(void) {
    if (s_voice_overlay || s_profile_overlay || settings_ui_is_open() || menu_ui_is_open()) return;

    // Hide all buttons
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_rhodes_btn)    lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn)  lv_obj_add_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)      lv_obj_add_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)    lv_obj_add_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_add_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create overlay（横屏：400×480 虚拟页绕屏幕 (240,600) 顺时针转 90° → 竖屏底部 = 横持视觉右侧）
    lvgl_port_lock(0);
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    bool land = s_standee_mode;
    if (land) {
        lv_obj_set_size(overlay, 400, 480);
        lv_obj_set_pos(overlay, 40, 360);
        lv_obj_set_style_transform_pivot_x(overlay, 200, 0);
        lv_obj_set_style_transform_pivot_y(overlay, 240, 0);
        lv_obj_set_style_transform_rotation(overlay, 900, 0);
    } else {
        lv_obj_set_size(overlay, 480, 420);
        lv_obj_set_pos(overlay, 0, 380);
    }
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_voice_overlay = overlay;

    // ── Title bar ──
    lv_obj_t *bar = lv_obj_create(overlay);
    lv_obj_set_size(bar, land ? 400 : 480, 44);
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
    lv_obj_set_pos(cat_dd, land ? 100 : 140, land ? 140 : 220);
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
    lv_obj_set_pos(entry_dd, land ? 100 : 140, land ? 200 : 275);
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
        if (s_rhodes_btn && !s_standee_mode) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn) lv_obj_remove_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)     lv_obj_remove_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)   lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    video_playback_start(30);
    ESP_LOGI(TAG, "Music UI hidden");
}

void music_ui_show(void) {
    if (s_music_overlay || s_voice_overlay || s_profile_overlay || settings_ui_is_open() || menu_ui_is_open()) return;

    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_rhodes_btn)   lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn) lv_obj_add_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)     lv_obj_add_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
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
    bool land = s_standee_mode;
    if (land) {
        // 横屏面板：400×480 虚拟页绕屏幕 (240,600) 顺时针转 90° → 竖屏底部 = 横持视觉右侧
        lv_obj_set_size(overlay, 400, 480);
        lv_obj_set_pos(overlay, 40, 360);
        lv_obj_set_style_transform_pivot_x(overlay, 200, 0);
        lv_obj_set_style_transform_pivot_y(overlay, 240, 0);
        lv_obj_set_style_transform_rotation(overlay, 900, 0);
    } else {
        lv_obj_set_size(overlay, 480, 420);
        lv_obj_set_pos(overlay, 0, 380);
    }
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_music_overlay = overlay;

    lv_obj_t *bar = lv_obj_create(overlay);
    lv_obj_set_size(bar, land ? 400 : 480, 44);
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
    lv_obj_set_pos(dd, land ? 40 : 80, land ? 200 : 220);
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
        if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn) lv_obj_remove_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)     lv_obj_remove_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
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

void keyboard_ui_show(void) {
    if (s_kb_overlay || s_voice_overlay || s_music_overlay || s_profile_overlay || settings_ui_is_open() || menu_ui_is_open()) return;
    // Lazy-alloc input buffer from PSRAM (avoids BSS corruption)
    if (!s_kb_input) {
        s_kb_input = (char*)heap_caps_malloc(KB_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (s_kb_input) memset(s_kb_input, 0, KB_BUF_SIZE);
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_kb_btn)       lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn) lv_obj_add_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)     lv_obj_add_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
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
static lv_img_dsc_t *s_profile_dsc = NULL;  // profile 占位 JPG 的 dsc（hide 时释放）
static lv_obj_t* s_profile_load_ind = NULL; // 动图加载小指示（spin+百分比，user_data 挂百分比 label）
static lv_obj_t* s_profile_spin = NULL;     // 指示里的旋转环（删除指示前先删动画，防回调踩空）

static void profile_spin_rotate_cb(void* var, int32_t v) { lv_arc_set_rotation((lv_obj_t*)var, (uint16_t)v); }

// 动图加载进度回调（PPA 帧级进度 → 小指示的百分比 label；指示未建时 no-op）
static void profile_progress_cb(const char* stage, int percent) {
    if (!s_profile_load_ind || strcmp(stage, "资料") != 0 || percent < 0) return;
    lvgl_port_lock(0);
    if (s_profile_load_ind) {
        lv_obj_t* pct = (lv_obj_t*)lv_obj_get_user_data(s_profile_load_ind);
        if (pct) lv_label_set_text_fmt(pct, "%d%%", percent);
    }
    lvgl_port_unlock();
}

void profile_show(void) {
    ESP_LOGI(TAG, "Profile show: enter");
    if (s_profile_overlay || settings_ui_is_open() || menu_ui_is_open()) return;
    if (s_lv2_interaction) lv2_interaction_stop();  // close Live2D before profile
    /* suspend 场景（PPD 交互经菜单打开）不退出交互：返回后 resume 回 PPD 待机 */
    if (s_standee_mode) standee_suspend();    // 横屏立牌暂停（profile 返回后恢复横屏）
    if (s_pd_interaction && !s_pd_suspend) pd_interaction_stop();    // close PPD before profile
    s_profile_was_cover = s_cover_mode;
    s_profile_chat_was_visible = !s_cover_mode;  // 表达式模式对话框可见

    // 隐藏右上角按钮 + 对话框
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        chat_overlay_show(false);
        if (s_rhodes_btn)  lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_lv2_interact_btn) lv_obj_add_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_ppd_interact_btn) lv_obj_add_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_kb_btn)      lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn) lv_obj_add_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)     lv_obj_add_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)  lv_obj_add_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        // 隐藏按钮也在 btn_labels[0]
        if (s_btn_labels[0]) lv_obj_add_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }

    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    // 释放可牺牲缓存（pending/streaming/mask/cover槽，保留 active 帧缓存），
    // 给 Profile JPG 解码让路——退出 profile 时 cover 仍可秒切
    ppa_release_expendable_caches();

    // ① 立刻显示 JPG 占位（LVGL 顶层 canvas）
    const char *jpg_path = "/sdcard/User/Ur_Info/Profile.jpg";
    ESP_LOGI(TAG, "Profile show: opening JPG");
    FILE *fp = fopen(jpg_path, "rb");
    bool has_jpg = false;
    if (fp) {
        fclose(fp);
        char lv_path[300];
        snprintf(lv_path, sizeof(lv_path), "S:/User/Ur_Info/Profile.jpg");
        lv_img_dsc_t *dsc = load_jpg_thumbnail(lv_path, 0);
        if (dsc) {
            s_profile_dsc = dsc;  // 保存，profile_hide 时释放（防每次打开泄漏 ~770KB PSRAM）
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

    if (!has_jpg) { ESP_LOGW(TAG, "Profile: JPG load failed (PSRAM tight?), closing"); profile_hide(); return; }

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
            /* 小加载指示（动图按钮旁）：spin + 百分比——静图切动图要加载 ~5 秒，给用户反馈 */
            lvgl_port_lock(pdMS_TO_TICKS(200));
            if (s_profile_overlay) {
                s_profile_load_ind = lv_obj_create(s_profile_overlay);
                lv_obj_set_size(s_profile_load_ind, 92, 30);
                lv_obj_set_pos(s_profile_load_ind, 420, 700);   // 靠近"动图"按钮 (475,713)
                lv_obj_set_style_bg_color(s_profile_load_ind, lv_color_hex(0x222222), 0);
                lv_obj_set_style_bg_opa(s_profile_load_ind, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(s_profile_load_ind, 6, 0);
                lv_obj_set_style_border_width(s_profile_load_ind, 0, 0);
                lv_obj_set_style_pad_all(s_profile_load_ind, 0, 0);
                lv_obj_set_style_transform_rotation(s_profile_load_ind, 900, 0);   // 与动图按钮同向（横屏）
                lv_obj_t* sp = lv_arc_create(s_profile_load_ind);
                s_profile_spin = sp;
                lv_obj_set_size(sp, 22, 22);
                lv_obj_remove_style(sp, NULL, LV_PART_KNOB);
                lv_arc_set_bg_angles(sp, 0, 300);
                lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
                lv_obj_set_style_arc_color(sp, lv_color_hex(0x4A7BFF), LV_PART_INDICATOR);
                lv_obj_align(sp, LV_ALIGN_LEFT_MID, 6, 0);
                lv_anim_t a; lv_anim_init(&a);
                lv_anim_set_var(&a, sp);
                lv_anim_set_exec_cb(&a, profile_spin_rotate_cb);
                lv_anim_set_values(&a, 0, 360);
                lv_anim_set_time(&a, 700);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_set_path_cb(&a, lv_anim_path_linear);
                lv_anim_start(&a);
                lv_obj_t* pct = lv_label_create(s_profile_load_ind);
                lv_label_set_text(pct, "0%");
                lv_obj_set_style_text_color(pct, lv_color_white(), 0);
                lv_obj_set_style_text_font(pct, s_chat_font, 0);
                lv_obj_align(pct, LV_ALIGN_RIGHT_MID, -8, 0);
                lv_obj_set_user_data(s_profile_load_ind, pct);
            }
            lvgl_port_unlock();
            xTaskCreate([](void *arg) {
                int gen = (int)(intptr_t)arg;
                int n = ppa_preload_profile("/sdcard/User/Ur_Info/Profile.mjpeg");
                if (gen == s_profile_gen && n > 0 && s_profile_overlay) {
                    ppa_use_profile_cache(true);
                    s_image_count = n; s_cover_mode = false;
                    video_playback_start(25);
                    if (s_profile_overlay) {
                        lvgl_port_lock(pdMS_TO_TICKS(200));
                        /* 先删指示动画并清指针，再 clean——clean 会连指示对象一起删，
                           后清的代码会对已删对象操作（use-after-free 崩溃） */
                        if (s_profile_load_ind && s_profile_spin) lv_anim_delete(s_profile_spin, profile_spin_rotate_cb);
                        s_profile_load_ind = NULL; s_profile_spin = NULL;
                        lv_obj_clean(s_profile_overlay);
                        lv_obj_set_style_bg_opa(s_profile_overlay, LV_OPA_0, 0);
                        /* 动图在 screen 层播放，被 PPD 的 top 层 bg（冻结帧）盖住——
                           隐藏 bg 露出 MJPEG；resume 恢复直写时再显示回来 */
                        if (s_pd_interaction_bg && lv_obj_is_valid(s_pd_interaction_bg))
                            lv_obj_add_flag(s_pd_interaction_bg, LV_OBJ_FLAG_HIDDEN);
                        lvgl_port_unlock();
                    }
                    ESP_LOGI(TAG, "Profile: MJPEG %d frames", n);
                }
                s_profile_loading = false;
                lvgl_port_lock(pdMS_TO_TICKS(200));
                if (s_profile_load_ind) {   // 失败/被新加载覆盖路径：指示还在，安全删除
                    if (s_profile_spin) lv_anim_delete(s_profile_spin, profile_spin_rotate_cb);
                    lv_obj_del(s_profile_load_ind);
                    s_profile_load_ind = NULL; s_profile_spin = NULL;
                }
                lvgl_port_unlock();
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
        // 打开瞬间（800ms 内）忽略点击：菜单按钮的释放事件会穿透到新 overlay，
        // 否则刚打开就被误判"点击关闭"→ profile_hide 与 show 交错 → 卡死
        if (esp_timer_get_time() / 1000 - s_profile_open_ms < 800) return;
        profile_hide();
    }, LV_EVENT_CLICKED, NULL);
    s_profile_open_ms = esp_timer_get_time() / 1000;
}

static void profile_hide(void) {
    if (!s_profile_overlay) return;
    /* 加载中关闭：指示对象随 overlay 一起删，但动画必须显式删（防回调踩已删对象） */
    if (s_profile_load_ind && s_profile_spin) lv_anim_delete(s_profile_spin, profile_spin_rotate_cb);
    s_profile_load_ind = NULL; s_profile_spin = NULL;
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    bool overlay_deleted = false;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        lv_obj_del(s_profile_overlay);
        s_profile_overlay = NULL;
        lvgl_port_unlock();
        overlay_deleted = true;
    }

    // 释放占位 JPG 的 dsc + 像素数据（仅当 canvas 已随 overlay 删除，防悬空引用）
    if (overlay_deleted && s_profile_dsc) {
        if (s_profile_dsc->data) heap_caps_free((void*)s_profile_dsc->data);
        heap_caps_free(s_profile_dsc);
        s_profile_dsc = NULL;
    }

    // 恢复按钮
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        /* 罗德岛按钮只在 cover 立绘模式显示（cover_display_start 显式恢复）；
           profile 场景一律隐藏——与 Live2D 共用槽位 (366,165)，同显必重合 */
        if (s_rhodes_btn) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_settings_btn) lv_obj_remove_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)     lv_obj_remove_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)  lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        if (!s_profile_was_cover) {  // 回 expression 模式才恢复
            if (s_lv2_interact_btn) lv_obj_remove_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_ppd_interact_btn) lv_obj_remove_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_kb_btn) lv_obj_remove_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);   // profile 返回对话模式：立牌隐藏
        }
        lvgl_port_unlock();
    }

    // 切回 active 播放源 + 清 profile 槽 + 恢复状态
    ppa_use_profile_cache(false);
    ppa_free_profile_slot();
    s_cover_mode = s_profile_was_cover;
    s_current_index = 0; s_loop_count = 0;
    // active 帧 + mask 均保留（profile_show 只释放可牺牲缓存）→ 秒切恢复
    video_playback_start(30);
    if (s_profile_chat_was_visible) chat_overlay_show(true);
    ppd_interaction_resume_after_app();   // 从 PPD 交互经菜单打开时：停 MJPEG、回 PPD 待机
    ESP_LOGI(TAG, "Profile hidden");
}

// ─── 角色索引页面（罗德岛）──────────────────────────────

enum { CARD_W = 108, CARD_H = 228, COLS = 4, ROWS = 3, CARDS_PER_PAGE = 12 };
static lv_obj_t *s_index_page = NULL;
static lv_obj_t *s_index_grid = NULL;
static lv_obj_t *s_index_pg_label = NULL;
static lv_obj_t *s_index_hint = NULL;   // 空列表提示
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
static volatile bool s_index_building = false;  // 索引页构建中屏蔽手势（防点击/滑动重入）
static volatile int s_prefetch_gen = 0;         // 预加载任务版本号（latest-wins）
static int s_filter_prof = 0;        // 0=全部, 1..8
static int s_filter_rarity = 0;      // 0=全部, 1..6
static lv_obj_t *s_prof_dd = NULL;   // 下拉框句柄（用于联动重置）
static lv_obj_t *s_rarity_dd = NULL;

// ── 两段式切换立绘：点卡片仅选中（高亮边框），点底部"切换立绘"按钮才确认 ──
static int s_selected_ai = -1;         // 选中干员（主列表下标），-1=无
static lv_obj_t *s_confirm_btn = NULL; // 底部确认按钮

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
static void on_index_gesture(lv_event_t *e);  // 前向声明（indev 级触摸判定，定义在下方）

static void agent_index_hide(void) {
    s_index_building = false;
    // 移除 indev 级触摸监听（防索引页关闭后残留回调）
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        uint32_t remain = lv_indev_remove_event_cb_with_user_data(indev, on_index_gesture, NULL);
        ESP_LOGI(TAG, "Indev touch listener removed (%lu cb remain)", (unsigned long)remain);
    }
    if (s_index_page) {
        lv_obj_del(s_index_page);
        s_index_page = NULL;
        s_index_grid = NULL;
        s_index_pg_label = NULL;
        s_index_hint = NULL;
        s_confirm_btn = NULL;
        s_selected_ai = -1;
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
        // 重新加载 cover（索引页打开时播放缓存已释放）
        if (s_agent_path[0]) cover_display_start_async(s_agent_path);
    }
}

// ── JPEG 缩略图加载（双引擎：A=LVGL 现场加载，B=预加载任务；零锁零等待）──
static jpeg_decoder_handle_t s_thumb_handle = NULL;      // 引擎 A（LVGL 任务）
static jpeg_decoder_handle_t s_thumb_handle_pfx = NULL;  // 引擎 B（预加载任务）
static jpeg_decode_cfg_t s_thumb_jpg_cfg = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR };

static void thumb_fail_log(const char *why, int idx, const char *path) {
    ESP_LOGW(TAG, "[%d] thumb fail: %s (PSRAM free %u KB) %s", idx, why,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), path);
}

static lv_img_dsc_t* load_jpg_thumbnail_ex(const char *path, int idx, jpeg_decoder_handle_t handle) {
    char fs_path[300];
    snprintf(fs_path, sizeof(fs_path), "/sdcard%s", path + 2);
    FILE *fp = fopen(fs_path, "rb");
    if (!fp) { thumb_fail_log("fopen", idx, fs_path); return NULL; }
    fseek(fp, 0, SEEK_END);
    size_t jpg_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (jpg_size == 0 || jpg_size > 512 * 1024) { fclose(fp); thumb_fail_log("size", idx, fs_path); return NULL; }
    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(jpg_size, MALLOC_CAP_SPIRAM);
    if (!jpg_data) { fclose(fp); thumb_fail_log("jpg alloc", idx, fs_path); return NULL; }
    size_t rd = fread(jpg_data, 1, jpg_size, fp);
    fclose(fp);
    if (rd != jpg_size) { ESP_LOGW(TAG, "[%d] short read %u/%u", idx, (unsigned)rd, (unsigned)jpg_size); free(jpg_data); return NULL; }

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg_data, jpg_size, &info) != ESP_OK) { free(jpg_data); thumb_fail_log("info", idx, fs_path); return NULL; }
    uint32_t aw = (info.width + 15) & ~15, ah = (info.height + 15) & ~15;

    jpeg_decode_memory_alloc_cfg_t rx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t tx_sz, rx_sz;
    uint8_t *tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpg_size, &tx_cfg, &tx_sz);
    uint8_t *rx_buf = (uint8_t*)jpeg_alloc_decoder_mem(aw * ah * 2, &rx_cfg, &rx_sz);
    if (!tx_buf || !rx_buf) { free(jpg_data); free(tx_buf); free(rx_buf); thumb_fail_log("buf alloc", idx, fs_path); return NULL; }
    memcpy(tx_buf, jpg_data, jpg_size);
    free(jpg_data);

    uint32_t dec;
    esp_err_t e = jpeg_decoder_process(handle, &s_thumb_jpg_cfg, tx_buf, tx_sz, rx_buf, rx_sz, &dec);
    free(tx_buf);
    if (e != ESP_OK) { ESP_LOGW(TAG, "[%d] dec fail %d", idx, (int)e); free(rx_buf); return NULL; }

    lv_img_dsc_t *dsc = (lv_img_dsc_t*)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
    if (!dsc) { free(rx_buf); thumb_fail_log("dsc alloc", idx, fs_path); return NULL; }
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (lv_coord_t)aw; dsc->header.h = (lv_coord_t)ah;
    dsc->data_size = aw * ah * 2; dsc->data = rx_buf; dsc->header.stride = aw * 2;
    return dsc;
}

// 确保引擎已创建（失败返回 NULL）
static jpeg_decoder_handle_t thumb_engine_ensure(jpeg_decoder_handle_t *slot) {
    if (*slot) return *slot;
    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 1000 };  // 1s 兜底超时
    for (int retry = 0; retry < 3; retry++) {
        if (jpeg_new_decoder_engine(&eng_cfg, slot) == ESP_OK) return *slot;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // 诊断：现场/预取加载"0 卡片无日志"的盲区路径
    ESP_LOGE(TAG, "thumb engine create FAILED (PSRAM free %u KB, slot=%p)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (void*)slot);
    return NULL;
}

// 现场加载（LVGL 任务，引擎 A）
static lv_img_dsc_t* load_jpg_thumbnail(const char *path, int idx) {
    jpeg_decoder_handle_t h = thumb_engine_ensure(&s_thumb_handle);
    if (!h) return NULL;
    return load_jpg_thumbnail_ex(path, idx, h);
}

// ── 分页显示（窗口预加载：当前页同步加载，前后 X=1 页后台异步预加载，越窗释放）──
static void prefetch_pages_task(void *arg);

static void agent_index_show_page(int page) {
    if (!s_index_grid) return;
    // 翻页清选中（两段式确认：防止旧页选中状态残留）
    s_selected_ai = -1;
    for (int i = 0; i < CARDS_PER_PAGE; i++) {
        if (s_card_objs[i]) {
            lv_obj_set_style_border_color(s_card_objs[i], lv_color_hex(0x555555), 0);
            lv_obj_set_style_border_width(s_card_objs[i], 1, 0);
        }
    }
    if (s_confirm_btn) lv_obj_add_flag(s_confirm_btn, LV_OBJ_FLAG_HIDDEN);
    // 释放窗口外缩略图（窗口 = 当前页 ±1 页，共 3 页）。
    // 注意：窗口边界是"筛选下标"，而 s_agent_dsc 按"主列表下标"索引，
    // 近卫等职业的主列表下标可能远大于窗口值 → 必须通过 s_filtered 映射判断，
    // 否则翻页时预取的缩略图被全部误释放，现场重载阻塞 UI 数秒
    int keep_start = (page - 1) * CARDS_PER_PAGE;
    if (keep_start < 0) keep_start = 0;
    int keep_end = (page + 2) * CARDS_PER_PAGE;
    if (keep_end > s_filtered_count) keep_end = s_filtered_count;
    static bool s_keep_flags[MAX_AGENT_DSC];
    memset(s_keep_flags, 0, sizeof(s_keep_flags));
    for (int k = keep_start; k < keep_end; k++) s_keep_flags[s_filtered[k]] = true;
    for (int i = 0; i < s_agent_cap; i++) {
        if (!s_keep_flags[i] && s_agent_dsc[i]) {
            if (s_agent_dsc[i]->data) heap_caps_free((void*)s_agent_dsc[i]->data);
            heap_caps_free(s_agent_dsc[i]);
            s_agent_dsc[i] = NULL;
        }
    }
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
                if (!s_agent_dsc[ai])
                    ESP_LOGW(TAG, "Page %d: live load NULL ai=%d (PSRAM free %u KB) %s",
                             page, ai,
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                             s_agents[ai].path);
            }
            lv_obj_t *c = lv_obj_get_child(card, 0);
            if (s_agent_dsc[ai] && c) {
                lv_canvas_set_buffer(c, (uint8_t*)s_agent_dsc[ai]->data,
                                     s_agent_dsc[ai]->header.w, s_agent_dsc[ai]->header.h,
                                     LV_COLOR_FORMAT_RGB565);
                shown++;
            } else if (c) {
                // 加载失败：清空 canvas，防残留上一页旧图（视觉"重复干员"）
                // 注意：LVGL 9 的 lv_canvas_set_buffer 断言 buf 非 NULL，不能用 NULL 清空！
                static uint8_t s_blank_px[4] = {0};
                lv_canvas_set_buffer(c, s_blank_px, 1, 1, LV_COLOR_FORMAT_RGB565);
            }
        } else {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ESP_LOGI(TAG, "Page %d: %d cards shown", page, shown);
    if (s_index_pg_label) {  // 防御：构建期重入时 label 可能尚未创建
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", page + 1, s_index_page_total);
        lv_label_set_text(s_index_pg_label, buf);
    }
    s_index_page_cur = page;

    // 前后各 1 页后台异步预加载（latest-wins：快速翻页时旧任务自动过期退出）
    s_prefetch_gen = s_prefetch_gen + 1;
    if (xTaskCreate(prefetch_pages_task, "pfx", 8192, (void*)(intptr_t)s_prefetch_gen, 1, NULL) != pdPASS)
        ESP_LOGW(TAG, "prefetch task create failed (gen=%d, sram free %u)",
                 s_prefetch_gen, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// 窗口预加载 worker：加载当前页前后各一页的缩略图（越窗页已在 show_page 释放）
static void prefetch_pages_task(void *arg) {
    int gen = (int)(intptr_t)arg;
    int page = s_index_page_cur;
    // 独立引擎 B：与 LVGL 现场加载（引擎 A）零锁零等待
    jpeg_decoder_handle_t h = thumb_engine_ensure(&s_thumb_handle_pfx);
    if (!h) { vTaskDelete(NULL); return; }
    int loaded = 0;
    for (int delta = -1; delta <= 1; delta += 2) {
        int p = page + delta;
        if (p < 0 || p >= s_index_page_total) continue;
        for (int i = 0; i < CARDS_PER_PAGE; i++) {
            if (gen != s_prefetch_gen) { vTaskDelete(NULL); return; }  // 版本过期，退出
            int fi = p * CARDS_PER_PAGE + i;
            if (fi >= s_filtered_count) break;
            int ai = s_filtered[fi];
            if (!s_agent_dsc[ai]) {
                s_agent_dsc[ai] = load_jpg_thumbnail_ex(s_agents[ai].path, ai, h);
                if (s_agent_dsc[ai]) loaded++;
                vTaskDelay(pdMS_TO_TICKS(50));  // 让出 CPU 与 SD 带宽
            }
        }
    }
    ESP_LOGI(TAG, "Prefetch gen=%d done: %d loaded (page %d, PSRAM free %u KB)",
             gen, loaded, page,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    vTaskDelete(NULL);
}

static void agent_index_prev_page(void) {
    if (s_index_page_cur > 0) agent_index_show_page(s_index_page_cur - 1);
}
static void agent_index_next_page(void) {
    if (s_index_page_cur < s_index_page_total - 1) agent_index_show_page(s_index_page_cur + 1);
}

// 滑动/点击判定：挂载在 indev（输入设备）级而非 grid 对象级。
// 原因：从卡片上按下的事件不保证冒泡到 grid 对象（对象事件传播依赖对象树），
// 实测卡片上滑动不翻页、点击起点陈旧；indev 事件是全局的，任何位置按下都必然到达。
static lv_point_t s_press_point;
static bool s_gesture_handled = false;
static volatile bool s_gesture_moved = false;   // 滑动位移超阈值：抑制卡片 CLICKED（防误触）
static void on_index_gesture(lv_event_t *e) {
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) indev = lv_indev_active();
    if (!indev) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    // 诊断日志：实时显示触点位置与判定状态（PRESSING 高频不打）
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_GESTURE) {
        ESP_LOGI(TAG, "TOUCH %s (%d,%d) | page=%d building=%d | press=(%d,%d) moved=%d handled=%d",
                 code == LV_EVENT_PRESSED ? "PRESS" : code == LV_EVENT_RELEASED ? "RELEASE" : "GESTURE",
                 (int)pt.x, (int)pt.y, (int)(s_index_page != NULL), (int)s_index_building,
                 (int)s_press_point.x, (int)s_press_point.y,
                 (int)s_gesture_moved, (int)s_gesture_handled);
    }
    if (!s_index_page || s_index_building) return;  // 索引页关闭/构建期屏蔽
    if (code == LV_EVENT_PRESSED) {
        s_gesture_handled = false;
        s_gesture_moved = false;
        // indev 链回调先于对象链到达，但此时 act_point 尚未更新（(-1,-1)）：
        // 无效坐标不记点，等对象链（冒泡）的 PRESSED 用有效坐标覆盖
        if (pt.x >= 0 && pt.y >= 0) s_press_point = pt;
        return;
    }
    if (s_gesture_handled) return;
    // 按压期间位移 >8px → 置"已滑动"标志（供卡片 CLICKED 防线 1 拦截）。
    // 必须跳过无效坐标：indev 链事件先到且坐标未更新（(-1,-1)），
    // 否则 |-1 - press| 恒 >8 会把原地点击误判成滑动
    if (pt.x >= 0 && pt.y >= 0 &&
        (LV_ABS(pt.x - s_press_point.x) > 8 || LV_ABS(pt.y - s_press_point.y) > 8)) {
        s_gesture_moved = true;
    }
    // LVGL 内置手势（位移 ≥LV_INDEV_DEF_GESTURE_LIMIT=50px 时 indev 发 GESTURE 事件）
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_LEFT)  { s_gesture_handled = true; agent_index_next_page(); return; }
        if (dir == LV_DIR_RIGHT) { s_gesture_handled = true; agent_index_prev_page(); return; }
        return;
    }
    // 松手判定：释放点与按下点位移 >12px 即翻页；≤12px 交给卡片 CLICKED 判定为点击
    if (code == LV_EVENT_RELEASED) {
        if (pt.x < 0 || pt.y < 0) return;  // indev 链先到的无效坐标：等对象链 RELEASED
        lv_coord_t dx = pt.x - s_press_point.x;
        if (dx < -12) { s_gesture_handled = true; agent_index_next_page(); }
        else if (dx > 12) { s_gesture_handled = true; agent_index_prev_page(); }
    }
}

static void agent_index_show(void) {
    if (s_index_page) { agent_index_hide(); return; }

    s_index_building = true;  // 构建完成前屏蔽手势（防点击/滑动重入崩溃）

    // 触摸判定挂到 indev 级（grid 对象级也在构建时挂载，双保险）：
    // 从卡片上按下的事件可能不冒泡到 grid 对象，indev 事件全局可达
    // （构建期由 s_index_building 屏蔽，关闭页由 s_index_page 判空屏蔽）
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_add_event_cb(indev, on_index_gesture, LV_EVENT_ALL, NULL);
        ESP_LOGI(TAG, "Indev touch listener attached (indev=%p)", (void*)indev);
    } else {
        ESP_LOGW(TAG, "Indev touch listener attach FAILED (no active indev)");
    }

    // 重置卡片追踪
    for (int i = 0; i < CARDS_PER_PAGE; i++) s_card_agent_idx[i] = -1;
    if (s_settings_btn) lv_obj_add_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_menu_btn) lv_obj_add_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);

    // 保存 cover active→slot，等 cover_display_start 秒换回来
    if (s_cover_mode && ppa_has_cover()) ppa_swap_to_cover();
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    // 释放可牺牲缓存（pending 预加载 3.4MB / streaming / alpha 1.7MB 等）：
    // 缩略图加载需要 PSRAM（实测翻页时仅剩 380KB → buf alloc 失败）
    ppa_release_expendable_caches();
    s_cover_mode = false;
    ppa_close_mjpeg();
    ppa_release_jpeg_engine();
    extern void application_set_wake_word_detection(bool enable);
    application_set_wake_word_detection(false);
    s_total_agents = scan_sd_agents();
    // 初始筛选 = 全部：不输出列表（427 干员全量缩略图压力大），提示用户选择筛选
    s_filter_prof = 0;
    s_filter_rarity = 0;
    s_filtered_count = 0;
    if (s_filtered) { heap_caps_free(s_filtered); s_filtered = NULL; }
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
    // 双保险：grid 对象级 + indev 级都挂 on_index_gesture（s_gesture_handled 防重复翻页）
    lv_obj_add_event_cb(s_index_grid, on_index_gesture, LV_EVENT_ALL, NULL);

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
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);  // 禁卡片自身滚动：滑动事件上抛给 grid 翻页

        // 缩略图画布
        lv_obj_t *c = lv_canvas_create(card);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_pos(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);

        // 点击卡片 → 仅选中（高亮边框），底部"切换立绘"按钮确认后才真正切换（防滑动误触）
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        // LVGL 9 事件冒泡默认关闭（lv_obj 默认 flags 只有 CLICKABLE）：
        // 必须显式开启，卡片上的 PRESS/RELEASE 才能冒泡到 grid 的 on_index_gesture 做翻页判定
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(card, [](lv_event_t *e) {
            lv_indev_t *indev = lv_indev_active();
            lv_point_t pt;
            lv_indev_get_point(indev, &pt);
            if (s_gesture_moved) {
                ESP_LOGI(TAG, "CARD CLICKED blocked: moved flag (pt=%d,%d press=%d,%d)",
                         (int)pt.x, (int)pt.y, (int)s_press_point.x, (int)s_press_point.y);
                s_gesture_moved = false; return;
            }
            // 松手时判定（用户要求：按下时不判定）：
            // 释放点与按下点位移 >12px = 滑动，只有原地按下再移开（≤12px 自然抖动）才算点击。
            // 与 on_index_gesture 的翻页阈值一致，点击与翻页互斥无死区
            if (LV_ABS(pt.x - s_press_point.x) > 12 || LV_ABS(pt.y - s_press_point.y) > 12) {
                ESP_LOGI(TAG, "CARD CLICKED blocked: swipe dist (pt=%d,%d press=%d,%d)",
                         (int)pt.x, (int)pt.y, (int)s_press_point.x, (int)s_press_point.y);
                return;
            }
            ESP_LOGI(TAG, "CARD CLICKED accepted (pt=%d,%d press=%d,%d)",
                     (int)pt.x, (int)pt.y, (int)s_press_point.x, (int)s_press_point.y);
            int card_i = (int)(intptr_t)lv_event_get_user_data(e);
            int fi = s_index_page_cur * CARDS_PER_PAGE + card_i;
            if (fi >= s_filtered_count) return;
            int ai = s_filtered[fi];
            s_selected_ai = ai;
            // 刷新边框：选中的绿色高亮，其余恢复
            for (int k = 0; k < CARDS_PER_PAGE; k++) {
                lv_obj_t *ck = s_card_objs[k];
                if (!ck) continue;
                int fk = s_index_page_cur * CARDS_PER_PAGE + k;
                bool sel = (fk < s_filtered_count && s_filtered[fk] == ai);
                lv_obj_set_style_border_color(ck, lv_color_hex(sel ? 0x00CC66 : 0x555555), 0);
                lv_obj_set_style_border_width(ck, sel ? 3 : 1, 0);
            }
            if (s_confirm_btn) {
                char buf[96];
                snprintf(buf, sizeof(buf), "切换立绘：%s", s_agents[ai].name);
                lv_label_set_text(lv_obj_get_child(s_confirm_btn, 0), buf);
                lv_obj_remove_flag(s_confirm_btn, LV_OBJ_FLAG_HIDDEN);
            }
            ESP_LOGI(TAG, "Card selected: %s (ai=%d)", s_agents[ai].name, ai);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

        s_card_objs[i] = card;
    }

    // ── 页码指示器（右上角，顶部 bar 内；底部区域留给"切换立绘"确认按钮）──
    s_index_pg_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_index_pg_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_index_pg_label, s_chat_font, 0);
    lv_obj_align(s_index_pg_label, LV_ALIGN_RIGHT_MID, -8, 0);

    // ── 底部"切换立绘"确认按钮（两段式：点卡片选中后出现）──
    s_confirm_btn = lv_btn_create(page);
    lv_obj_set_size(s_confirm_btn, 190, 30);
    lv_obj_set_style_bg_color(s_confirm_btn, lv_color_hex(0x00AA55), 0);
    lv_obj_set_style_radius(s_confirm_btn, 6, 0);
    lv_obj_set_style_border_width(s_confirm_btn, 0, 0);
    lv_obj_align(s_confirm_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_t *cf_lbl = lv_label_create(s_confirm_btn);
    lv_label_set_text(cf_lbl, "切换立绘");
    lv_obj_set_style_text_color(cf_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cf_lbl, s_chat_font, 0);
    lv_obj_center(cf_lbl);
    lv_obj_add_flag(s_confirm_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_confirm_btn, [](lv_event_t *e) {
        int ai = s_selected_ai;
        if (ai < 0 || ai >= s_total_agents) return;
        char agent_path[300];
        snprintf(agent_path, sizeof(agent_path), "/sdcard/main/operator/%s/%s/%s",
                 PROF_EN[s_agents[ai].prof], RARITY_DIR[s_agents[ai].rarity], s_agents[ai].name);
        ESP_LOGI(TAG, "Confirm switch: %s → %s", s_agents[ai].name, agent_path);
        agent_index_hide();
        cover_display_start_async(agent_path);  // 后台加载，loading 动画覆盖等待期
    }, LV_EVENT_CLICKED, NULL);

    // ── 空列表提示（两个筛选均为"全部"时显示）──
    s_index_hint = lv_label_create(page);
    lv_label_set_text(s_index_hint, "请选择职业或星级\n查看干员");
    lv_obj_set_style_text_color(s_index_hint, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_index_hint, s_chat_font, 0);
    lv_obj_set_style_text_align(s_index_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_index_hint, LV_ALIGN_CENTER, 0, 30);
    if (s_filtered_count > 0) lv_obj_add_flag(s_index_hint, LV_OBJ_FLAG_HIDDEN);

    // 只预加载第一页（最多 12 张），翻页时延迟加载
    int preload = s_filtered_count < CARDS_PER_PAGE ? s_filtered_count : CARDS_PER_PAGE;
    for (int i = 0; i < preload; i++) {
        int ai = s_filtered[i];
        s_agent_dsc[ai] = load_jpg_thumbnail(s_agents[ai].path, ai);
        if (i < preload - 1) vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 注意：播放缓存已释放（缩略图需要 PSRAM），下层 cover 不恢复播放；
    // 退出索引页时（agent_index_hide）再重新加载 cover
    if (s_settings_btn) lv_obj_remove_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_menu_btn) lv_obj_remove_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);

    // 显示第一页
    s_index_building = false;  // 构建完成，开放手势
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
    // 2. 分配筛选数组（PSRAM）；两个筛选都为"全部"时不输出列表（减轻缩略图压力）
    if (s_filtered) { heap_caps_free(s_filtered); s_filtered = NULL; }
    s_filtered_count = 0;
    if (s_total_agents > 0 && (s_filter_prof > 0 || s_filter_rarity > 0)) {
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
    // 空列表提示切换
    if (s_index_hint) {
        if (s_filtered_count > 0) lv_obj_add_flag(s_index_hint, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_index_hint, LV_OBJ_FLAG_HIDDEN);
    }
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

    // 表头（横屏对话时动态改为当前角色名，竖屏保持默认）
    hdr = lv_label_create(s_chat_assistant_box);
    lv_label_set_text(hdr, "Kal'tsit：");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(hdr, s_chat_font, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    s_chat_assistant_hdr = hdr;

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
    s_hide_btn = btn;  // save for Live2D overlay
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 125);
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

    // ── ② 返回罗德岛（与 Live2D交互 共用槽位：cover 显示罗德岛 / expression 显示交互）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 165);
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
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 85);
    // (位置见上：设置 5 / 菜单 45 / 模式 85 / 隐藏 125 / 交互 165 / 键盘 205)
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    s_mode_label = lv_label_create(btn);
    lv_label_set_text(s_mode_label, "对话模式");   // 初始 cover（立绘）模式：按钮显示目标模式名
    lv_obj_set_style_text_color(s_mode_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_mode_label, s_chat_font, 0);
    lv_obj_center(s_mode_label);
    s_btn_labels[2] = s_mode_label;
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        if (s_cover_mode) {
            if (s_standee_mode) {
                // 横屏立牌下点"对话模式"：不切竖屏对话模式，进入 Q 版互动；
                // 互动中再点 → 打断对话（若有）+ 退出互动回横屏立牌
                if (s_pd_interaction) {
                    extern void application_end_conversation(void);
                    application_end_conversation();   // 对话中退出：先打断（关音频通道）
                    pd_interaction_stop();
                    chat_overlay_set_landscape(false);   // 聊天框恢复竖屏布局
                    chat_overlay_show(false);            // 对话框不残留在立牌上
                } else {
                    pdq_interaction_start();   // 立牌模式点"对话模式"：进入 Q 版互动
                }
            } else {
                s_req_expression = true;
                loading_show("进入对话模式");  // mode_switch_task 完成时统一隐藏
            }
        } else {
            // Expression → cover: close Live2D/PPD interaction first
            if (s_lv2_interaction) lv2_interaction_stop();
            if (s_pd_interaction) pd_interaction_stop();
            if (s_standee_mode) standee_exit(false);
            s_req_cover = true;
            loading_show("返回展示模式");
        }
    }, LV_EVENT_CLICKED, NULL);

    // ── ④ Live2D 交互（expression 模式可见；与罗德岛共用槽位 (366,165)）──
    s_lv2_interact_btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(s_lv2_interact_btn, 110, 35);
    lv_obj_set_pos(s_lv2_interact_btn, 366, 165);
    lv_obj_set_style_bg_color(s_lv2_interact_btn, lv_color_hex(0x448866), 0);
    lv_obj_set_style_bg_opa(s_lv2_interact_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_lv2_interact_btn, 6, 0);
    lv_obj_set_style_border_width(s_lv2_interact_btn, 0, 0);
    s_lv2_interact_lbl = lv_label_create(s_lv2_interact_btn);
    lv_label_set_text(s_lv2_interact_lbl, "Live2D交互");
    lv_obj_set_style_text_color(s_lv2_interact_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lv2_interact_lbl, s_chat_font, 0);
    lv_obj_center(s_lv2_interact_lbl);
    lv_obj_add_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，仅 expression 模式显示
    ESP_LOGI(TAG, "Live2D交互按钮已创建 (ptr=%p)", (void*)s_lv2_interact_btn);
    lv_obj_add_event_cb(s_lv2_interact_btn, [](lv_event_t *e) {
        if (!s_lv2_interaction) {
            if (s_pd_interaction) pd_interaction_stop();   // 互斥：先关 PPD 再开 Live2D（后备引擎）
            lv2_interaction_start();
            lv_label_set_text(s_lv2_interact_lbl, "关闭Live2D");
            lv_obj_set_style_bg_color(s_lv2_interact_btn, lv_color_hex(0x884444), 0);
        } else {
            lv2_interaction_stop();
            lv_label_set_text(s_lv2_interact_lbl, "Live2D交互");
            lv_obj_set_style_bg_color(s_lv2_interact_btn, lv_color_hex(0x448866), 0);
        }
    }, LV_EVENT_CLICKED, NULL);

    // ── ④b PPD 交互（expression 模式可见；槽位 (366,205)，弹出键盘下移至 (366,245)）──
    s_ppd_interact_btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(s_ppd_interact_btn, 110, 35);
    lv_obj_set_pos(s_ppd_interact_btn, 366, 205);
    lv_obj_set_style_bg_color(s_ppd_interact_btn, lv_color_hex(0x886644), 0);
    lv_obj_set_style_bg_opa(s_ppd_interact_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_ppd_interact_btn, 6, 0);
    lv_obj_set_style_border_width(s_ppd_interact_btn, 0, 0);
    s_ppd_interact_lbl = lv_label_create(s_ppd_interact_btn);
    lv_label_set_text(s_ppd_interact_lbl, "PPD交互");
    lv_obj_set_style_text_color(s_ppd_interact_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ppd_interact_lbl, s_chat_font, 0);
    lv_obj_center(s_ppd_interact_lbl);
    lv_obj_add_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，仅 expression 模式显示
    ESP_LOGI(TAG, "PPD交互按钮已创建 (ptr=%p)", (void*)s_ppd_interact_btn);
    lv_obj_add_event_cb(s_ppd_interact_btn, [](lv_event_t *e) {
        if (!s_pd_interaction) {
            s_pdq_mode = false;   // 竖屏入口：确保非 Q 版模式（stop 已复位，防御残留）
            if (s_lv2_interaction) lv2_interaction_stop();   // 互斥：先关 Live2D 再开 PPD
            pd_interaction_start();
            lv_label_set_text(s_ppd_interact_lbl, "关闭PPD");
            lv_obj_set_style_bg_color(s_ppd_interact_btn, lv_color_hex(0x884444), 0);
            if (s_lv2_interact_btn) lv_obj_add_state(s_lv2_interact_btn, LV_STATE_DISABLED);  // 开 PPD 期间 Live2D 置灰
        } else {
            pd_interaction_stop();
            lv_label_set_text(s_ppd_interact_lbl, "PPD交互");
            lv_obj_set_style_bg_color(s_ppd_interact_btn, lv_color_hex(0x886644), 0);
            if (s_lv2_interact_btn) lv_obj_remove_state(s_lv2_interact_btn, LV_STATE_DISABLED);
        }
    }, LV_EVENT_CLICKED, NULL);

    // ── ⑤ 设置（cover+expression 都可见）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 5);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "设置");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    s_settings_btn = btn;
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        settings_ui_show();
    }, LV_EVENT_CLICKED, NULL);

    // ── ⑥ 菜单（cover+expression 都可见；拼豆/派对/语音/音乐/测试/键盘迁入菜单页）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 45);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x5588AA), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "菜单");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    s_menu_btn = btn;
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        menu_ui_show(s_cover_mode, s_standee_mode);
    }, LV_EVENT_CLICKED, NULL);

    // ── ⑧ 弹出键盘（仅 expression 模式可见；PPD 交互按钮占 (366,205)，本按钮下移至 245）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 245);
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

    // ── ⑨ 横屏立牌（仅通行证模式可见；占位接口：点击只切自身文字，后续扩展）──
    s_standee_btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(s_standee_btn, 110, 35);
    lv_obj_set_pos(s_standee_btn, 366, 285);
    lv_obj_set_style_bg_color(s_standee_btn, lv_color_hex(0x885522), 0);
    lv_obj_set_style_bg_opa(s_standee_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_standee_btn, 6, 0);
    lv_obj_set_style_border_width(s_standee_btn, 0, 0);
    s_standee_lbl = lv_label_create(s_standee_btn);
    lv_label_set_text(s_standee_lbl, "横屏立牌");
    lv_obj_set_style_text_color(s_standee_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_standee_lbl, s_chat_font, 0);
    lv_obj_center(s_standee_lbl);
    lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，仅通行证模式显示
    lv_obj_add_event_cb(s_standee_btn, [](lv_event_t *e) {
        /* 横屏立牌开关：进入/退出横屏 MJPEG 展示（SD IO 在 standee_task 大栈里做）。
           Q 版互动中点此按钮 → 先退互动（回横屏立牌），再退出横屏回竖屏 cover */
        if (s_pd_interaction) {
            pd_interaction_stop();
            chat_overlay_set_landscape(false);   // 聊天框恢复竖屏布局
            chat_overlay_show(false);            // 对话框不残留
        }
        if (!s_standee_mode && !s_standee_starting) {
            s_standee_starting = true;
            s_standee_cancel = false;
            loading_show("进入横屏立牌");
            xTaskCreate(standee_task, "standee", 10240, NULL, 2, NULL);
        } else if (s_standee_mode) {
            standee_exit(true);
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
    /* 诊断"对话框闪现消失"：记录每次显隐切换的调用者（地址对照 build/xxx.map 定位） */
    ESP_LOGI(TAG, "chat_ovl %s ← %p", show ? "SHOW" : "HIDE", __builtin_return_address(0));
    if (show) {
        lv_obj_remove_flag(s_chat_user_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_chat_assistant_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_chat_user_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chat_assistant_box, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

// 横屏对话：聊天框转 90° 成视觉右侧侧边栏（竖帧 y 620-800 深度，用户/LLM **上下**排列）。
// 旋转几何（顺时针 90°，pivot 左上角）：本地 (x,y) → 屏幕 (pos_x-y, pos_y+x)；
// 用户横持视觉 (u,v) = (y, 479-x)：u = 屏幕 y（视觉右 = 竖帧底），v 小 = 屏幕 x 大（视觉上）。
// user（视觉上）：本地 180×90 @ (480,620) → 屏幕 x∈[390,480]、y∈[620,800]
// assistant（视觉下）：本地 180×90 @ (390,620) → 屏幕 x∈[300,390]、y∈[620,800]
static bool s_chat_landscape = false;
static void chat_overlay_set_landscape(bool land) {
    if (!s_chat_user_box || !s_chat_assistant_box) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (land) {
        lv_obj_set_size(s_chat_user_box, 180, 90);
        lv_obj_set_pos(s_chat_user_box, 480, 610);
        lv_obj_set_style_transform_pivot_x(s_chat_user_box, 0, 0);
        lv_obj_set_style_transform_pivot_y(s_chat_user_box, 0, 0);
        lv_obj_set_style_transform_rotation(s_chat_user_box, 900, 0);
        lv_obj_set_width(s_chat_user_label, 168);   // 窄框换行
        lv_obj_set_style_bg_opa(s_chat_user_box, LV_OPA_60, 0);   // 半透明（用户要求；60% 平衡可读性与透底）
        lv_obj_set_scrollbar_mode(s_chat_user_box, LV_SCROLLBAR_MODE_OFF);   // 不显示滚动条（"页面滑动"观感根因之一）
        lv_obj_remove_flag(s_chat_user_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   // 长文本裁剪不溢出
        lv_obj_set_size(s_chat_assistant_box, 180, 90);
        lv_obj_set_pos(s_chat_assistant_box, 365, 610);   // 与输入框拉开 25px 间距（用户反馈重合）
        lv_obj_set_style_transform_pivot_x(s_chat_assistant_box, 0, 0);
        lv_obj_set_style_transform_pivot_y(s_chat_assistant_box, 0, 0);
        lv_obj_set_style_transform_rotation(s_chat_assistant_box, 900, 0);
        lv_obj_set_width(s_chat_assistant_label, 168);
        lv_obj_set_style_bg_opa(s_chat_assistant_box, LV_OPA_60, 0);
        lv_obj_set_scrollbar_mode(s_chat_assistant_box, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(s_chat_assistant_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        // 回复框表头 = 当前角色名（s_agent_path 最后一段，如 "Amiya"）
        if (s_chat_assistant_hdr && s_agent_path[0]) {
            const char *slash = strrchr(s_agent_path, '/');
            const char *nm = slash ? slash + 1 : s_agent_path;
            size_t nl = strlen(nm);
            if (nl > 78) nl = 78;   // 80 缓冲：名字 + '：' + NUL（长度检查避免 -Wformat-truncation）
            char hdrbuf[80];
            memcpy(hdrbuf, nm, nl);
            hdrbuf[nl] = ':';
            hdrbuf[nl + 1] = 0;
            lv_label_set_text(s_chat_assistant_hdr, hdrbuf);
        }
    } else {
        lv_obj_set_size(s_chat_user_box, 440, 100);
        lv_obj_set_pos(s_chat_user_box, 20, 520);
        lv_obj_set_style_transform_rotation(s_chat_user_box, 0, 0);
        lv_obj_set_width(s_chat_user_label, 425);
        lv_obj_set_size(s_chat_assistant_box, 440, 140);
        lv_obj_set_pos(s_chat_assistant_box, 20, 630);
        lv_obj_set_style_transform_rotation(s_chat_assistant_box, 0, 0);
        lv_obj_set_width(s_chat_assistant_label, 425);
    }
    s_chat_landscape = land;
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
    /* 文本更新触发聊天框重绘，与 PPA 直写竞争（canvas 底图 DMA 更新 vs LVGL 读底图叠加）
       → 每句回复小范围闪烁：先停直写 → 同步重绘落盘 → 恢复直写 */
    bool was_suspend = s_pd_suspend;
    if (s_pd_interaction && !was_suspend) {
        s_pd_suspend = true;
        vTaskDelay(pdMS_TO_TICKS(25));   // 等 pd_anim 检测并停直写
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) { s_pd_suspend = was_suspend; return; }
    const char *old = lv_label_get_text(s_chat_assistant_label);
    char buf[1024];
    if (old && old[0]) {
        snprintf(buf, sizeof(buf), "%s%s", old, text);
    } else {
        snprintf(buf, sizeof(buf), "%s", text);
    }
    lv_label_set_text(s_chat_assistant_label, buf);
    lv_obj_scroll_to_y(s_chat_assistant_box, LV_COORD_MAX, LV_ANIM_OFF);
    lv_refr_now(NULL);   // 同步重绘落盘（与直写严格串行）
    lvgl_port_unlock();
    if (s_pd_suspend != was_suspend) {
        vTaskDelay(pdMS_TO_TICKS(10));
        s_pd_suspend = was_suspend;
    }
}

// ══════════════════════════════════════════════════
// Live2D 测试页（眼球追踪 + 动画）
// ══════════════════════════════════════════════════

static TaskHandle_t s_lv2_task = NULL;
static bool s_lv2_running = false;
static int s_fps_frame = 0;
static int64_t s_fps_last = 0;
static lv_obj_t* s_lv2_test_overlay = NULL;
static lv_obj_t* s_lv2_canvas = NULL;
static lv_obj_t* s_lv2_active_overlay = NULL;  // overlay to invalidate (test page or interaction)

static void lv2_anim_task(void*) {
    int64_t start_us = esp_timer_get_time();
    while (s_lv2_running) {
        float t = (esp_timer_get_time() - start_us) / 1000000.0f;
        int64_t t0 = esp_timer_get_time();
        lv2_update_animation(t);
        int64_t t1 = esp_timer_get_time();
        s_fps_frame++;
        if (s_fps_last == 0) s_fps_last = t1;
        if (t1 - s_fps_last > 5000000) {
            float fps = s_fps_frame * 1000000.0f / (t1 - s_fps_last);
            ESP_LOGI("LV2", "FPS: %.1f (render: %d ms)", fps, (int)((t1-t0)/1000));
            s_fps_frame = 0; s_fps_last = t1;
        }
        lvgl_port_lock(0);
        extern uint16_t* g_lv2_fb;
        // s_lv2_running 判空：do_switch 进行中退出交互时，canvas 已被 stop 删除
        if (s_lv2_running && s_lv2_canvas && g_lv2_fb) lv_canvas_set_buffer(s_lv2_canvas, (uint8_t*)g_lv2_fb, 480, 800, LV_COLOR_FORMAT_RGB565);
        if (s_lv2_active_overlay) lv_obj_invalidate(s_lv2_active_overlay);
        lvgl_port_unlock();
        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

// ── Live2D interaction mode (replaces PPA MJPEG in expression mode) ──
static lv_obj_t* s_lv2_interaction_canvas = NULL;
static volatile bool s_lv2_starting = false;  // 交互进入中守卫（防双点）

static void lv2_interaction_stop(void);  // 前向声明（失败回滚复用）

// 后台任务：排队切换 → 建交互层 → 等切换完成（进度由 lv2 钩子驱动）→ 收 loading
static void interaction_start_task(void* arg) {
    int chr = (int)(intptr_t)arg;
    extern void lv2_switch_character(int);
    lv2_switch_character(chr);  // 排队切换（anim task 执行 do_switch）
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    lvgl_port_lock(0);
    // Canvas on top layer — only way to cover PPA hardware
    lv_obj_t* bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, 480, 800); lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_move_background(bg);  // bg behind chat bubbles on same layer
    // Move ALL sidebar buttons to overlay so they stay visible
    lv_obj_t* side_btns[] = {s_lv2_interact_btn, s_rhodes_btn, s_kb_btn, s_hide_btn,
                             s_settings_btn, s_menu_btn};
    for (int i=0;i<6;i++) if (side_btns[i]) lv_obj_set_parent(side_btns[i], bg);
    if (s_mode_label && lv_obj_is_valid(lv_obj_get_parent(s_mode_label)))
        lv_obj_set_parent(lv_obj_get_parent(s_mode_label), bg);
    extern uint16_t* g_lv2_fb;
    lv_obj_t* c = lv_canvas_create(bg);
    lv_obj_set_size(c, 480, 800);
    lv_canvas_set_buffer(c, (uint8_t*)g_lv2_fb, 480, 800, LV_COLOR_FORMAT_RGB565);
    // Bring ALL sidebar buttons above the canvas
    // 注意：side_btns 只有 6 个元素，循环必须 i<6（曾写 i<7 越界读任务栈 0xa5a5a5a5 → Load access fault）
    for (int i=0;i<6;i++) if (side_btns[i] && lv_obj_is_valid(side_btns[i])) lv_obj_move_foreground(side_btns[i]);
    if (s_mode_label && lv_obj_is_valid(lv_obj_get_parent(s_mode_label)))
        lv_obj_move_foreground(lv_obj_get_parent(s_mode_label));
    s_lv2_interaction_canvas = bg;

    // Touch → eye tracking + head shy
    lv_obj_add_event_cb(bg, [](lv_event_t* e){
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t pt; lv_indev_get_point(indev, &pt);
        extern float g_eye_target_x, g_eye_target_y;
        extern bool g_touching_head;
        float rx = (float)pt.x / 480.0f;
        float ry = (float)pt.y / 800.0f;
        g_eye_target_x = 1.0f - 2.0f * rx;
        g_eye_target_y = 2.0f * ry - 1.0f;
        if (rx > 0.25f && rx < 0.75f && ry > 0.08f && ry < 0.20f) {
            g_touching_head = true;
        }
    }, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(bg, [](lv_event_t*){
        extern bool g_touching_head;
        g_touching_head = false;
    }, LV_EVENT_RELEASED, NULL);

    lvgl_port_unlock();
    loading_raise();  // bg 后建会盖住 loading 覆盖层，抬回最前

    s_lv2_canvas = c;
    s_lv2_active_overlay = bg;
    s_lv2_interaction = true;
    if (!s_lv2_task) { s_lv2_running = true; xTaskCreate(lv2_anim_task, "lv2_anim", 4096, NULL, 3, &s_lv2_task); }

    // 等 do_switch 完成（替代旧代码的固定 vTaskDelay(500) 干等；进度由 lv2 钩子实时刷新）
    extern void lv2_wait_switch_done(int);
    lv2_wait_switch_done(15000);
    extern bool lv2_is_model_ready(void);
    if (!lv2_is_model_ready()) {
        ESP_LOGW("LV2","interaction: model load failed, rollback");
        lv2_interaction_stop();  // 撤交互层 + 恢复 MJPEG（内部含 loading_hide）
        s_lv2_starting = false;
        vTaskDelete(NULL);
        return;
    }
    loading_hide();
    s_lv2_starting = false;
    vTaskDelete(NULL);
}

static void lv2_interaction_start(void) {
    if (s_lv2_interaction || s_lv2_starting) return;
    // Auto-switch to correct character based on agent path
    extern char s_agent_path[256];
    int chr = -1;
    if (strstr(s_agent_path, "Theresia") || strstr(s_agent_path, "theresia"))
        chr = 1;
    else if (strstr(s_agent_path, "Amiya") || strstr(s_agent_path, "amiya"))
        chr = 0;
    else {
        ESP_LOGW("LV2","interaction: agent not supported (%s)", s_agent_path);
        return;
    }
    ESP_LOGI("LV2","interaction start for %s", s_agent_path);
    s_lv2_starting = true;
    loading_show("进入 Live2D 交互");
    xTaskCreate(interaction_start_task, "lv2_start", 8192, (void*)(intptr_t)chr, 2, NULL);
}

static void lv2_interaction_stop(void) {
    if (!s_lv2_interaction) return;
    ESP_LOGI("LV2","interaction stop");
    loading_hide();  // 加载动画残留清理（幂等）
    s_lv2_running = false; vTaskDelay(pdMS_TO_TICKS(100)); s_lv2_task = NULL;
    // Move ALL buttons OUT of bg BEFORE deleting bg (they are children of bg!)
    lvgl_port_lock(0);
    lv_obj_t* side_btns[] = {s_lv2_interact_btn, s_rhodes_btn, s_kb_btn, s_hide_btn,
                             s_settings_btn, s_menu_btn};
    for (int i=0;i<6;i++) if (side_btns[i] && lv_obj_is_valid(side_btns[i])) lv_obj_set_parent(side_btns[i], lv_screen_active());
    if (s_mode_label && lv_obj_is_valid(lv_obj_get_parent(s_mode_label)))
        lv_obj_set_parent(lv_obj_get_parent(s_mode_label), lv_screen_active());
    lvgl_port_unlock();
    s_lv2_canvas = NULL;           // 先清指针再删对象，防 anim task 中途重绑定踩空
    s_lv2_active_overlay = NULL;
    if (s_lv2_interaction_canvas) {
        lvgl_port_lock(0);
        lv_obj_del(s_lv2_interaction_canvas);
        lvgl_port_unlock();
        s_lv2_interaction_canvas = NULL;
    }
    s_lv2_interaction = false;
    // Reset button label and color (under lock)
    lvgl_port_lock(0);
    if (s_lv2_interact_lbl) lv_label_set_text(s_lv2_interact_lbl, "Live2D交互");
    if (s_lv2_interact_btn) lv_obj_set_style_bg_color(s_lv2_interact_btn, lv_color_hex(0x448866), 0);
    lvgl_port_unlock();
    // Restart MJPEG with current emotion
    extern void expression_restart_mjpeg(void);
    expression_restart_mjpeg();
}

static void lv2_test_hide(void) {
    if (s_lv2_test_overlay) {
        s_lv2_running = false; vTaskDelay(pdMS_TO_TICKS(100)); s_lv2_task = NULL;
        lv_obj_del(s_lv2_test_overlay); s_lv2_test_overlay = NULL;
        s_lv2_canvas = NULL;
        s_lv2_active_overlay = NULL;
        loading_hide();  // 切角色加载中退出测试页的残留清理
        application_set_wake_word_detection(true);
        video_playback_start(30);
        ppd_interaction_resume_after_app();   // 从 PPD 交互经菜单打开时：回 PPD 待机
    }
}

void lv2_test_show(void) {
    if (s_lv2_test_overlay || !g_lv2_fb) return;
    video_playback_stop();
    application_set_wake_word_detection(false);
    lvgl_port_lock(0);
    s_lv2_test_overlay = lv_obj_create(lv_layer_top());
    s_lv2_active_overlay = s_lv2_test_overlay;  // set active overlay for anim task
    lv_obj_set_size(s_lv2_test_overlay, 480, 800);
    lv_obj_set_pos(s_lv2_test_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_lv2_test_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lv2_test_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_lv2_test_overlay, 0, 0);
    lv_obj_clear_flag(s_lv2_test_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* c = lv_canvas_create(s_lv2_test_overlay);
    s_lv2_canvas = c;
    lv_obj_set_size(c, 480, 800);
    lv_canvas_set_buffer(c, (uint8_t*)g_lv2_fb, g_lv2_fb_w, g_lv2_fb_h, LV_COLOR_FORMAT_RGB565);
    // Touch → eye tracking
    lv_obj_add_event_cb(s_lv2_test_overlay, [](lv_event_t* e){
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t pt; lv_indev_get_point(indev, &pt);
        extern float g_eye_target_x, g_eye_target_y, g_shy_trigger;
        extern bool g_touching_head;
        float rx = (float)pt.x / 480.0f;
        float ry = (float)pt.y / 800.0f;
        g_eye_target_x = 1.0f - 2.0f * rx;
        g_eye_target_y = 2.0f * ry - 1.0f;
        // Head zone: exclude top ~8% (finger entry area), ry 0.08~0.20
        if (rx > 0.25f && rx < 0.75f && ry > 0.08f && ry < 0.20f) {
            g_shy_trigger = 1.0f;
            g_touching_head = true;
            ESP_LOGI("SHY_TOUCH", "HEAD rx=%.2f ry=%.2f", rx, ry);
        }
    }, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_lv2_test_overlay, [](lv_event_t*){
        extern bool g_touching_head;
        g_touching_head = false;
        ESP_LOGI("SHY_TOUCH", "RELEASED");
    }, LV_EVENT_RELEASED, NULL);
    // 返回按钮
    lv_obj_t* back = lv_btn_create(s_lv2_test_overlay);
    lv_obj_set_size(back, 60, 36);
    lv_obj_set_pos(back, 410, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_t* lbl = lv_label_create(back);
    lv_label_set_text(lbl, "返回");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(back, [](lv_event_t*){ lv2_test_hide(); }, LV_EVENT_CLICKED, NULL);

    // ── Character switch button ──
    extern void lv2_switch_character(int);
    lv_obj_t* ch_btn = lv_btn_create(s_lv2_test_overlay);
    lv_obj_set_size(ch_btn, 60, 26);
    lv_obj_set_pos(ch_btn, 340, 10);
    lv_obj_set_style_bg_color(ch_btn, lv_color_hex(0x885522), 0);
    lv_obj_set_style_bg_opa(ch_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(ch_btn, 5, 0);
    lv_obj_set_style_border_width(ch_btn, 0, 0);
    lv_obj_t* ch_lbl = lv_label_create(ch_btn);
    lv_label_set_text(ch_lbl, "特蕾西娅");
    lv_obj_set_style_text_color(ch_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ch_lbl, s_chat_font, 0);
    lv_obj_center(ch_lbl);
    lv_obj_add_event_cb(ch_btn, [](lv_event_t* e){
        static int chr=0; chr=(chr+1)%4;
        loading_show("切换角色");  // do_switch 完成时统一隐藏（sd_test.cc）
        lv2_switch_character(chr);
        static const char* names[]={"阿米娅","特蕾西娅","芙宁娜","足力零"};
        lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e),0), names[(chr+1)%4]);
    }, LV_EVENT_CLICKED, NULL);

    // ── Expression buttons (right column) ──
    extern void lv2_set_expression(int);
    static const char* enames[]={"自动","开心","悲伤","惊讶","自信","困惑","思考","大笑","困倦","傻气","生气","害羞"};
    static const int evals[]={0,1,2,3,4,5,6,7,8,9,10,11};
    for(int i=0;i<12;i++){
        lv_obj_t* eb=lv_btn_create(s_lv2_test_overlay);
        lv_obj_set_size(eb,50,26); lv_obj_set_pos(eb,425,60+i*29);
        lv_obj_set_style_bg_color(eb,lv_color_hex(0x555555),0);
        lv_obj_set_style_bg_opa(eb,LV_OPA_80,0);
        lv_obj_set_style_radius(eb,5,0); lv_obj_set_style_border_width(eb,0,0);
        lv_obj_t* el=lv_label_create(eb);
        lv_label_set_text(el,enames[i]);
        lv_obj_set_style_text_color(el,lv_color_white(),0);
        lv_obj_set_style_text_font(el,s_chat_font,0);
        lv_obj_center(el);
        int v=evals[i];
        lv_obj_add_event_cb(eb,[](lv_event_t* e){lv2_set_expression((int)(uintptr_t)lv_event_get_user_data(e));},LV_EVENT_CLICKED,(void*)(uintptr_t)v);
    }

    lvgl_port_unlock();
    if (!s_lv2_task) { s_lv2_running = true; xTaskCreate(lv2_anim_task, "lv2_anim", 4096, NULL, 3, &s_lv2_task); }
}

// ══════════════════════════════════════════════════
// 纸偶测试页（sprite + 仿射渲染器 lv2_paperdoll；待机/摸头/视线追踪，暂无表情映射）
// ══════════════════════════════════════════════════

static lv_obj_t* s_pd_overlay = NULL;
static lv_obj_t* s_pd_name_lbl = NULL;
static TaskHandle_t s_pd_task = NULL;
static bool s_pd_running = false;
static uint16_t* s_pd_fb[3] = {NULL, NULL, NULL};  // 480×800 RGB565：fb[0/1]=渲染双缓冲（防撕裂）
                                                    // fb[2]=干净底图（PPA 每帧拷入，canvas 指向它——
                                                    // LVGL 重绘 UI 矩形时读干净帧再叠半透明，不会自叠加）
static int s_pd_fb_idx = 0;
static char s_pd_dirs[8][64];             // /sdcard/main/operator/<职业>/<星级>/<干员> 相对路径（ASCII，如 "CASTER/5STAR/Amiya"）
static int s_pd_count = 0, s_pd_index = 0;
// s_pd_interaction_bg 声明上移至文件头部声明区（profile 动图播放需前向引用）
static bool s_pd_throttle = false;            // true=交互模式 5fps 节流（默认）；false=测试页快节奏（无 AFE 压力）

// ═══════ PPA 直写上屏管线（PPD 交互专用）═══════
// 绕过 LVGL 全屏重绘（partial 模式 ~100ms/帧是 4fps 的元凶）：渲染帧经 PPA SRM（1:1 硬件拷贝）
// DMA 直写面板 fb，按钮列/画质按钮矩形跳过（保留 fb 上 LVGL 画的按钮像素，按钮不闪不遮）。
// LVGL 只做按钮状态重绘：canvas 缓冲直接指向面板 fb，按钮矩形重绘时底图自洽（同址自拷）。
// CPU 0 每帧零参与（PPA 是硬件 DMA），帧率 = 渲染 58ms + 2×msync ~4ms ≈ 16fps。
static bool s_pd_direct = false;              // PPA 直写模式激活（panel fb 可用 + PPA 注册成功）
static ppa_client_handle_t s_pd_ppa_srm = NULL;
static SemaphoreHandle_t s_pd_ppa_sem = NULL;
static int s_pd_ppa_pending = 0;              // 已提交未完成的 DMA 段数（临界区保护跨 ISR 共享）
static portMUX_TYPE s_pd_ppa_crit = portMUX_INITIALIZER_UNLOCKED;   // 保护 pending 计数（任务/ISR 双端更新）
static uint16_t* s_pd_panel_fb = NULL;        // 面板 framebuffer（480×800 RGB565，PSRAM）
static LcdDisplay* s_pd_lcd = NULL;           // 直写期间的 LcdDisplay 句柄（隐藏/恢复状态栏用）

// 掩码保护区（不拷贝，保留 fb 上 LVGL 画的 UI 像素）：
// 侧边按钮 110×35（设置5/菜单45/模式85/隐藏125/Live2D165/PPD205/键盘245，x 366-476）
// + 画质按钮 80×26 (10,10) + 用户对话框 440×100 (20,520) + 助手对话框 440×140 (20,630)。
// pd_ppa_present 按此矩形列表做行带扫描切段（动态生成，按钮搬动只需改这里）。

static bool pd_ppa_trans_done_cb(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data) {
    /* ISR 上下文：spinlock 临界区保护计数（提交侧 ++ 在任务上下文，丢更新会导致死等）。
       宏用 IDF 扩展名 portENTER_CRITICAL_ISR（带 mux 参数），非内核无参 task 版 */
    portENTER_CRITICAL_ISR(&s_pd_ppa_crit);
    s_pd_ppa_pending--;
    portEXIT_CRITICAL_ISR(&s_pd_ppa_crit);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_pd_ppa_sem, &hp);
    return hp == pdTRUE;
}

static bool pd_ppa_init(void) {
    if (s_pd_ppa_srm) return true;
    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 64,   // 行带扫描每帧 ~31 段（保护矩形底图 10 + 掩码段 ~21）+ 首帧 1 段
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&cfg, &s_pd_ppa_srm) != ESP_OK) {
        s_pd_ppa_srm = NULL;
        return false;
    }
    ppa_event_callbacks_t cbs = { .on_trans_done = pd_ppa_trans_done_cb };
    ppa_client_register_event_callbacks(s_pd_ppa_srm, &cbs);
    s_pd_ppa_sem = xSemaphoreCreateCounting(64, 0);   // 与 max_pending 一致（每帧 ~31 段）
    return s_pd_ppa_sem != NULL;
}

static void pd_ppa_wait_idle(void) {
    while (s_pd_ppa_pending > 0) xSemaphoreTake(s_pd_ppa_sem, portMAX_DELAY);
}

// 一段矩形 SRM 拷贝（1:1 无旋转缩放，RGB565；异步提交，完成回调计数）
static void pd_ppa_copy_rect(uint16_t* src, uint16_t* dst, int x0, int y0, int x1, int y1) {
    if (!s_pd_ppa_srm || !src || !dst || x0 >= x1 || y0 >= y1) return;
    ppa_srm_oper_config_t cfg = {};
    cfg.in.buffer = src;
    cfg.in.pic_w = 480; cfg.in.pic_h = 800;
    cfg.in.block_w = (uint32_t)(x1 - x0); cfg.in.block_h = (uint32_t)(y1 - y0);
    cfg.in.block_offset_x = (uint32_t)x0; cfg.in.block_offset_y = (uint32_t)y0;
    cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    cfg.out.buffer = dst;
    cfg.out.buffer_size = 480 * 800 * 2;
    cfg.out.pic_w = 480; cfg.out.pic_h = 800;
    cfg.out.block_offset_x = (uint32_t)x0; cfg.out.block_offset_y = (uint32_t)y0;
    cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x = 1.0f; cfg.scale_y = 1.0f;
    cfg.mirror_x = false; cfg.mirror_y = false;
    cfg.rgb_swap = false; cfg.byte_swap = false;
    cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    cfg.mode = PPA_TRANS_MODE_NON_BLOCKING;
    if (ppa_do_scale_rotate_mirror(s_pd_ppa_srm, &cfg) == ESP_OK) {
        portENTER_CRITICAL(&s_pd_ppa_crit);
        s_pd_ppa_pending++;
        portEXIT_CRITICAL(&s_pd_ppa_crit);
    } else {
        ESP_LOGW("PD", "PPA SRM submit fail @(%d,%d)", x0, y0);
    }
}

// 分块全屏拷贝：行带扫描，跳过保护矩形（UI 元素像素自保，其余区域每帧覆盖）
static void pd_ppa_present(uint16_t* src, uint16_t* dst) {
    struct protect_rect_t { int x0, y0, x1, y1; };
    static const protect_rect_t PROTECT_PORTRAIT[] = {
        {366, 5,   476, 40},    // 设置
        {366, 45,  476, 80},    // 菜单
        {366, 85,  476, 120},   // 模式
        {366, 125, 476, 160},   // 隐藏
        {366, 165, 476, 200},   // Live2D 交互
        {366, 205, 476, 240},   // PPD 交互
        {366, 245, 476, 280},   // 弹出键盘
        {20,  520, 460, 620},   // 用户对话框
        {20,  630, 460, 800},   // 助手对话框
    };
    // 横屏 Q 版互动：4 按钮旋转排布（pos 190+i*40,10 + pivot(0,0) + 顺时针 90°）
    // → 屏幕 x∈[pos-35,pos], y∈[10,120]；动作按钮（i=4：x∈[315,350]）与动作列表面板
    // （旋转后顶部横条 x∈[0,480] y∈[10,160]）同样保护。
    // 横屏聊天框侧边栏（旋转后 x∈[350,480] y∈[360,780]）在下方动态追加（仅可见时保护，
    // 隐藏时不保护——否则直写跳过的区域纸偶画面冻结）
    static const protect_rect_t PROTECT_LANDSCAPE[] = {
        {155, 10, 190, 120},   // 设置
        {195, 10, 230, 120},   // 菜单
        {235, 10, 270, 120},   // 模式
        {275, 10, 310, 120},   // 横屏立牌
        {315, 10, 350, 120},   // 显示/隐藏聊天框
        {355, 10, 390, 120},   // 动作按钮
        {0,   10, 480, 160},   // 动作列表面板（展开时顶部横条）
    };
    protect_rect_t rects[10];
    int nr = 0;
    if (s_pdq_mode) {
        memcpy(rects, PROTECT_LANDSCAPE, sizeof(PROTECT_LANDSCAPE));
        nr = (int)(sizeof(PROTECT_LANDSCAPE) / sizeof(PROTECT_LANDSCAPE[0]));
        // 横屏侧边栏聊天框：可见时保护（竖帧 y 610-800 整块区域）
        if (s_chat_landscape && s_chat_user_box &&
            !lv_obj_has_flag(s_chat_user_box, LV_OBJ_FLAG_HIDDEN)) {
            rects[nr++] = {275, 610, 480, 800};
        }
    } else {
        memcpy(rects, PROTECT_PORTRAIT, sizeof(PROTECT_PORTRAIT));
        nr = (int)(sizeof(PROTECT_PORTRAIT) / sizeof(PROTECT_PORTRAIT[0]));
    }
    const protect_rect_t *PROTECT = rects;
    const int N = nr;
    /* 干净底图：只把保护矩形区域拷入 fb[2]（canvas 底图源）——LVGL 重绘 UI 矩形时
       从它自拷再叠半透明背景，避免读 fb 上已混合的画面造成逐次叠加变灰。
       只拷保护区（~90KB）而非全屏 768KB：DMA 总量大降，配合 max_pending=64
       解决 SRM 段积压（"exceed maximum pending"——全屏段 15ms 与 BLEND 引擎
       抢 2D-DMA 通道，21 段/帧消费不完 56ms 帧周期） */
    if (s_pd_fb[2]) {
        for (int i = 0; i < N; i++)
            pd_ppa_copy_rect(src, s_pd_fb[2], PROTECT[i].x0, PROTECT[i].y0, PROTECT[i].x1, PROTECT[i].y1);
    }
    // 收集 y 边界并排序去重 → 行带
    int ys[24]; int ny = 0;
    ys[ny++] = 0; ys[ny++] = 800;
    for (int i = 0; i < N; i++) { ys[ny++] = PROTECT[i].y0; ys[ny++] = PROTECT[i].y1; }
    for (int i = 0; i < ny; i++)
        for (int j = i + 1; j < ny; j++)
            if (ys[j] < ys[i]) { int t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
    int uniq[24]; int nu = 0;
    for (int i = 0; i < ny; i++)
        if (nu == 0 || ys[i] != uniq[nu - 1]) uniq[nu++] = ys[i];
    // 每带内：x 区间减去带内保护矩形 → 拷贝段
    for (int k = 0; k + 1 < nu; k++) {
        int y0 = uniq[k], y1 = uniq[k + 1];
        if (y1 <= y0) continue;
        int x0s[16], x1s[16]; int nx = 0;
        for (int i = 0; i < N; i++) {
            if (PROTECT[i].y0 <= y0 && PROTECT[i].y1 >= y1) { x0s[nx] = PROTECT[i].x0; x1s[nx] = PROTECT[i].x1; nx++; }
        }
        int x = 0;
        while (x < 480) {
            int xEnd = 480;
            bool skip = false;
            for (int i = 0; i < nx; i++) {
                if (x >= x0s[i] && x < x1s[i]) { x = x1s[i]; skip = true; break; }
                if (x0s[i] > x && x0s[i] < xEnd) xEnd = x0s[i];
            }
            if (skip) continue;
            if (xEnd > x) { pd_ppa_copy_rect(src, dst, x, y0, xEnd, y1); x = xEnd; }
        }
    }
}

/* 文件规范（2026-08）：PPD 角色数据位于
   /sdcard/main/operator/<职业>/<星级>/<干员>/PPD/{scene.json,*.raw}
   职业白名单 7 个（INDEX/PRTS/REINSTALL 是系统目录，不扫）；每职业 1STAR..6STAR */
static const char* s_pd_professions[] = {"CASTER","GUARD","MEDIC","SNIPER","SPECIALIST","SUPPORTER","VANGUARD"};
static const char* s_pd_rarities[] = {"1STAR","2STAR","3STAR","4STAR","5STAR","6STAR"};

// 扫描 <职业>/<星级>/<干员>/<sub>/scene.json 存在的角色（sub = "PPD" 竖屏 / "PPD_Q" 横屏 Q 版）
static void pd_scan_chars_sub(const char* sub) {
    s_pd_count = 0;
    for (int p = 0; p < 7 && s_pd_count < 8; p++) {
        for (int r = 0; r < 6 && s_pd_count < 8; r++) {
            char prof_path[64];
            snprintf(prof_path, sizeof(prof_path), "/sdcard/main/operator/%s/%s",
                     s_pd_professions[p], s_pd_rarities[r]);
            DIR* d = opendir(prof_path);
            if (!d) continue;
            struct dirent* e;
            while ((e = readdir(d)) && s_pd_count < 8) {
                if (e->d_name[0] == '.') continue;
                // 长度显式检查后 memcpy 拼接（snprintf 会触发 -Wformat-truncation：d_name 最坏 255B）
                size_t pl = strlen(s_pd_professions[p]), rl = strlen(s_pd_rarities[r]), nl = strlen(e->d_name);
                if (pl + 1 + rl + 1 + nl >= sizeof(s_pd_dirs[0])) continue;   // 干员名过长，跳过
                char rel[64];
                memcpy(rel, s_pd_professions[p], pl);
                rel[pl] = '/';
                memcpy(rel + pl + 1, s_pd_rarities[r], rl);
                rel[pl + 1 + rl] = '/';
                memcpy(rel + pl + rl + 2, e->d_name, nl + 1);
                char path[128];   // 最坏 22 + 63(rel) + 17("/PPD_Q/scene.json") = 102 < 128
                snprintf(path, sizeof(path), "/sdcard/main/operator/%s/%s/scene.json", rel, sub);
                struct stat st;
                if (stat(path, &st) != 0) continue;
                strncpy(s_pd_dirs[s_pd_count], rel, sizeof(s_pd_dirs[0]) - 1);
                s_pd_dirs[s_pd_count][sizeof(s_pd_dirs[0]) - 1] = 0;
                s_pd_count++;
            }
            closedir(d);
        }
    }
    ESP_LOGI("PD", "scan: %d chars under /sdcard/main/operator/*/*/*/%s", s_pd_count, sub);
}

static void pd_scan_chars(void) { pd_scan_chars_sub("PPD"); }

static void pd_load_current(void) {
    ESP_LOGI("PD", "switch: waiting mutex...");
    xSemaphoreTake(s_pd_mutex, portMAX_DELAY);   // 渲染/触摸暂停（切角色加载期间画面冻结在上一帧）
    ESP_LOGI("PD", "switch: mutex taken, loading %s", s_pd_dirs[s_pd_index]);
    loading_show("切换角色");
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    char path[96];
    snprintf(path, sizeof(path), "/sdcard/main/operator/%s/PPD", s_pd_dirs[s_pd_index]);
    s_pd_model = pd_load(path);   // 同步加载（分块读 + 让步）
    loading_hide();
    ESP_LOGI("PD", "switch: load done %p", (void*)s_pd_model);
    xSemaphoreGive(s_pd_mutex);
    if (s_pd_name_lbl) {
        lvgl_port_lock(0);
        if (s_pd_name_lbl && lv_obj_is_valid(s_pd_name_lbl))
            lv_label_set_text(s_pd_name_lbl, s_pd_model ? s_pd_model->name : "载入失败");
        lvgl_port_unlock();
    }
    ESP_LOGI("PD", "load %s -> %s", path, s_pd_model ? s_pd_model->name : "FAILED");
}

static void pd_anim_task(void*) {
    int64_t start_us = esp_timer_get_time();
    int fps_frame = 0; int64_t fps_last = 0;
    while (s_pd_running) {
        uint32_t t_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        uint16_t* cur = s_pd_fb[s_pd_fb_idx];
        int64_t t0 = esp_timer_get_time();
        if (s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_pd_model && cur) pd_render(s_pd_model, cur, 480, 800, t_ms);
            xSemaphoreGive(s_pd_mutex);
        }
        int64_t t1 = esp_timer_get_time();
        fps_frame++;
        if (fps_last == 0) fps_last = t1;
        if (t1 - fps_last > 5000000) {
            ESP_LOGI("PD", "FPS: %.1f (render: %d ms)", fps_frame * 1000000.0f / (t1 - fps_last), (int)((t1 - t0) / 1000));
            fps_frame = 0; fps_last = t1;
        }
        if (s_pd_direct) {
            /* 面板打开（设置/菜单/键盘/语音/音乐）或全屏应用抽屉（拼豆/蟑螂/Live2D测试，
               s_pd_suspend）时暂停直写：大面积 UI 的 LVGL 重绘/滚动与 PPA 每帧拷贝并发
               会互相覆盖（闪烁看不清）。期间画面冻结在最后一帧（渲染继续推进动画参数），
               关闭后恢复 */
            bool ui_open = settings_ui_is_open() || menu_ui_is_open() || s_kb_overlay || s_voice_overlay || s_music_overlay || s_pd_suspend;
            static bool ui_was = false;
            if (ui_open != ui_was) {
                if (!ui_open) {
                    /* 恢复直写前（在 pd_anim 上下文，不阻塞 LVGL 任务）：
                       停应用期间启动的 MJPEG（其全屏 flush 与直写互斥）→
                       等 overlay 删除的全屏重绘 flush 落盘 → 恢复 PPD bg 显示
                       （动图播放成功时 bg 被隐藏，露出 screen 层 MJPEG） */
                    video_playback_stop();
                    vTaskDelay(pdMS_TO_TICKS(150));
                    if (s_pd_interaction_bg) {
                        lvgl_port_lock(0);
                        if (s_pd_interaction_bg && lv_obj_is_valid(s_pd_interaction_bg))
                            lv_obj_remove_flag(s_pd_interaction_bg, LV_OBJ_FLAG_HIDDEN);
                        lvgl_port_unlock();
                    }
                }
                ui_was = ui_open;
            }
            if (!ui_open) {
                /* PPA 直写上屏：等上一帧 DMA 完成 → msync（CPU 写 s_pd_fb 对 PPA 可见 /
                   PPA 写 fb 对扫描 DMA 可见）→ 行带扫描硬件拷贝（跳过 UI 保护矩形）。
                   CPU 0 零参与；上帧 DMA 与下帧渲染重叠（双缓冲异区），帧率 ~16fps。
                   撕裂防护：渲染写 idx 与 DMA 读 idx^1 异区；写回同区要等 2 帧周期 */
                pd_ppa_wait_idle();
                esp_cache_msync((void*)s_pd_panel_fb, 480 * 800 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                esp_cache_msync((void*)cur, 480 * 800 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                pd_ppa_present(cur, s_pd_panel_fb);
            }
        } else {
            /* canvas 老路（测试页 / 直写不可用回退）：LVGL 软件全屏重绘 ~100ms，
               交互回退时由下方 5fps 节流兜底（Live2D 同级节奏，稳定基线） */
            lvgl_port_lock(0);
            if (s_pd_running && s_pd_canvas && cur)
                lv_canvas_set_buffer(s_pd_canvas, (uint8_t*)cur, 480, 800, LV_COLOR_FORMAT_RGB565);
            if (s_pd_overlay) lv_obj_invalidate(s_pd_overlay);
            lvgl_port_unlock();
        }
        s_pd_fb_idx ^= 1;   // 下一帧写另一个缓冲，本帧源不被改写
        /* 帧率控制：直写模式不限帧（渲染 ~56ms 自定节奏 ~17fps）；
           老路：交互节流 5fps / 测试页快节奏。
           直写模式下渲染占满 CPU 1，vTaskDelay(5) 给 IDLE1 留 5ms/帧 喂 watchdog
           （1ms 窗口被 audio_detection 等就绪任务抢走 → IDLE1 5s 无运行 → task_wdt 崩溃） */
        int64_t t2 = esp_timer_get_time();
        int64_t frame_period_us = s_pd_direct ? 0 : (s_pd_throttle ? 200000 : 56000);
        if (s_pd_direct) {
            vTaskDelay(5);
        } else if (t2 - t1 < frame_period_us) {
            vTaskDelay((frame_period_us - (t2 - t1)) / 1000);
        }
    }
    vTaskDelete(NULL);
}

// ══════════════════════════════════════════════════
// PPD 交互模式（照抄 Live2D 交互：停 MJPEG → 全屏纸偶 → 侧边按钮搬家 → 退出恢复 MJPEG）
// Live2D 交互保留为后备，两者互斥切换（按钮回调里互相 stop）
// ══════════════════════════════════════════════════

// Emoji 名 → 纸偶表达式索引（与 PC 仿真器 EXPRESSIONS 表一致：
// 0=无,1=说话,2=害羞,3=惊讶,4=开心,5=生气,6=俏皮,7=悲伤,8=疑惑,9=困倦,10=微笑,11=待机,12=自信）
static int pd_emoji_to_expression(const char* emoji) {
    if (!emoji || !emoji[0]) return 0;
    if (strstr(emoji,"speak") || strstr(emoji,"talk") || strstr(emoji,"说话")) return 1;
    if (strstr(emoji,"shy") || strstr(emoji,"害羞")) return 2;
    if (strstr(emoji,"surpris") || strstr(emoji,"惊讶") || strstr(emoji,"shock")) return 3;
    if (strstr(emoji,"happy") || strstr(emoji,"开心") || strstr(emoji,"laugh") || strstr(emoji,"大笑")) return 4;
    if (strstr(emoji,"angry") || strstr(emoji,"生气")) return 5;
    if (strstr(emoji,"silly") || strstr(emoji,"俏皮") || strstr(emoji,"goofy")) return 6;
    if (strstr(emoji,"sad") || strstr(emoji,"悲伤") || strstr(emoji,"crying")) return 7;
    if (strstr(emoji,"confus") || strstr(emoji,"困惑") || strstr(emoji,"think") || strstr(emoji,"思考")) return 8;
    if (strstr(emoji,"sleep") || strstr(emoji,"困倦") || strstr(emoji,"tired")) return 9;
    if (strstr(emoji,"smile") || strstr(emoji,"微笑")) return 10;
    if (strstr(emoji,"idle") || strstr(emoji,"待机") || strstr(emoji,"neutral")) return 11;
    if (strstr(emoji,"confident") || strstr(emoji,"自信") || strstr(emoji,"wink") || strstr(emoji,"cool")) return 12;
    return 0;   // 默认：自动（恢复待机演示）
}

// ASCII 大小写不敏感子串匹配（agent 路径 vs 纸偶角色目录名）
static bool pd_ci_strstr(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (!n) return true;
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 0x20) == (needle[i] | 0x20)) i++;   // ASCII 小写化
        if (i == n) return true;
    }
    return false;
}

static void pd_interaction_task(void* arg) {
    // 角色扫描与选择：必须在本任务里做——SD 卡 opendir/readdir/stat 调用链很深，
    // 在 LVGL 任务（按钮回调）里执行会撑爆其栈（Stack dump → LVGL 状态损坏 → 疯狂重绘 → watchdog）
    const char* sub = s_pdq_mode ? "PPD_Q" : "PPD";
    pd_scan_chars_sub(sub);
    if (s_pd_count == 0) {
        ESP_LOGW("PD", "interaction: 无角色（/sdcard/main/operator/<职业>/<星级>/<干员>/%s/scene.json）", sub);
        loading_hide();
        s_pd_starting = false;
        s_pdq_mode = false;   // 失败复位：防残留导致下次竖屏入口误进 Q 版
        vTaskDelete(NULL);
        return;
    }
    int idx = 0;
    for (int i = 0; i < s_pd_count; i++) {
        if (pd_ci_strstr(s_agent_path, s_pd_dirs[i])) { idx = i; break; }
    }
    char dir[64];
    strncpy(dir, s_pd_dirs[idx], sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    ESP_LOGI("PD", "interaction start: agent=%s → dir=%s", s_agent_path, dir);

    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    ppa_release_playback_caches();   // MJPEG 播放缓存让路（退出时自动重载）
    ppa_close_mjpeg();
    ppa_release_jpeg_engine();
    /* 注意：不禁唤醒词检测——PPD 交互是待机展示，用户说唤醒词后才进入对话（与 Live2D 交互一致）。
       AFE 内部任务固定 CPU 1，渲染任务不 pin 让 FreeRTOS 自由调度避开冲突。 */

    // 双缓冲（与纸偶测试页共用；fb 尾部 4KB 填充防 LVGL SIMD 过读）
    if (!s_pd_fb[0] || !s_pd_fb[1] || !s_pd_fb[2]) {
        for (int i = 0; i < 3; i++) {
            if (!s_pd_fb[i])   // 只补缺失块（部分失败后重进不覆盖已有指针，防泄漏）
                s_pd_fb[i] = (uint16_t*)heap_caps_aligned_alloc(64, 480 * 800 * 2 + 4096, MALLOC_CAP_SPIRAM);
            if (s_pd_fb[i]) memset(s_pd_fb[i], 0, 480 * 800 * 2);
        }
        if (!s_pd_fb[0] || !s_pd_fb[1] || !s_pd_fb[2]) {
            ESP_LOGE("PD", "interaction: fb alloc fail");
            s_pd_starting = false;   // 先清标志：video_playback_start 有 s_pd_starting 守卫
            s_pdq_mode = false;
            video_playback_start(30);
            vTaskDelete(NULL);
            return;
        }
    }
    if (!s_pd_mutex) s_pd_mutex = xSemaphoreCreateMutex();

    // PPA 直写上屏管线：注册 SRM 客户端 + 取面板 fb（失败回退 canvas 老路 5fps 兜底）
    s_pd_direct = false;
    s_pd_panel_fb = NULL;
    s_pd_lcd = NULL;
    if (pd_ppa_init()) {
        Display* disp = Board::GetInstance().GetDisplay();
        /* 本板固定 MipiLcdDisplay（继承 LcdDisplay）；若未来换板走 OledDisplay，
           static_cast 会踩空——GetDisplay 返回非 LcdDisplay 时判空回退 */
        if (disp) {
            LcdDisplay* lcd = (LcdDisplay*)disp;
            s_pd_panel_fb = lcd->GetPanelFrameBuffer();
            if (s_pd_panel_fb) { s_pd_direct = true; s_pd_lcd = lcd; }
        }
    }
    if (s_pd_direct && s_pd_lcd) {
        /* 直写绕过 LVGL 后，screen 层的时钟重绘 flush 会覆盖直写帧（无每帧重绘兜底）——
           隐藏状态栏（视觉上本来就被全屏 canvas 遮住，与 Live2D 交互观感一致） */
        s_pd_lcd->SetStatusBarVisible(false);
    }
    ESP_LOGI("PD", "interaction present path: %s", s_pd_direct ? "PPA DIRECT (~16fps)" : "LVGL canvas fallback (5fps)");

    // 加载角色（持锁；anim task 尚未启动，此锁只防 pd_test 侧并发——互斥页面实际不会发生）
    if (s_pd_mutex) xSemaphoreTake(s_pd_mutex, portMAX_DELAY);
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    char path[96];
    snprintf(path, sizeof(path), "/sdcard/main/operator/%s/%s", dir, sub);
    s_pd_model = pd_load(path);
    if (s_pd_mutex) xSemaphoreGive(s_pd_mutex);
    if (!s_pd_model) {
        ESP_LOGE("PD", "interaction: pd_load fail %s", path);
        loading_hide();
        s_pd_starting = false;   // 先清标志：video_playback_start 有 s_pd_starting 守卫
        s_pdq_mode = false;
        video_playback_start(30);
        vTaskDelete(NULL);
        return;
    }
    /* 高画质默认：PPA 硬件混合已开（legwear/back hair/footwear 等大层走硬件，省 ~15-20ms/帧），
       全分辨率下 CPU 预算足够；若个别角色仍触发 watchdog，可用左上角"画质"按钮临时切快模式。 */
    s_pd_model->half_res = false;

    /* 直写模式：先渲染首帧到 fb[2]（干净底图缓冲，canvas 将指向它）——
       canvas 创建时 set_buffer 的 invalidate 重绘即可自拷到首帧，不会黑屏 */
    if (s_pd_direct) {
        if (s_pd_mutex) xSemaphoreTake(s_pd_mutex, portMAX_DELAY);
        pd_render(s_pd_model, s_pd_fb[2], 480, 800, 0);
        if (s_pd_mutex) xSemaphoreGive(s_pd_mutex);
        esp_cache_msync((void*)s_pd_fb[2], 480 * 800 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    lvgl_port_lock(0);
    // Canvas on top layer — only way to cover PPA hardware（照抄 Live2D 交互）
    lv_obj_t* bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, 480, 800); lv_obj_set_pos(bg, 0, 0);
    /* bg 透明：canvas 全屏 800 全覆盖（纸偶 fb 每帧整屏填充），
       省掉每帧一次的 480×800 全屏底色填充（~8ms CPU 0） */
    lv_obj_set_style_bg_opa(bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    /* CLICKABLE 必须加：LVGL 的 PRESSING 事件只发给 CLICKABLE 对象（或其祖先），
       不加则触摸回调收不到 → 摸头/拖动/视线跟随全部失效（已踩坑） */
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    /* SCROLLABLE 必须清：lv_obj 默认可滚动——滑动触摸会滚动整个 bg（canvas/按钮组
       都是其子对象跟着移走："按键组 x 突变 0 到底部"、右侧滑动条全是它的滚动） */
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    /* 顶层容器同样清滚动：bg 不可滚后滑动会冒泡到 lv_layer_top（可滚容器）——
       top 层整体滚动 → 聊天框/按钮组/面板全下移、露出 screen 层立牌（条形码） */
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(bg);  // bg 在聊天框之下
    // 侧边按钮全部搬到覆盖层（含 PPD 自身与横屏立牌按钮），否则被 canvas 盖住
    lv_obj_t* side_btns[] = {s_lv2_interact_btn, s_ppd_interact_btn, s_rhodes_btn, s_kb_btn,
                             s_hide_btn, s_settings_btn, s_menu_btn, s_standee_btn};
    /* 罗德岛只在 cover 立绘模式显示；PPD 交互画面里与 Live2D 按钮同槽 (366,165) 会重合 */
    if (s_rhodes_btn) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < (int)(sizeof(side_btns) / sizeof(side_btns[0])); i++)
        if (side_btns[i]) lv_obj_set_parent(side_btns[i], bg);
    if (s_mode_label && lv_obj_is_valid(lv_obj_get_parent(s_mode_label)))
        lv_obj_set_parent(lv_obj_get_parent(s_mode_label), bg);
    lv_obj_t* c = lv_canvas_create(bg);
    lv_obj_set_size(c, 480, 800);
    /* 直写模式：canvas 缓冲 = s_pd_fb[2]（PPA 每帧拷入的干净底图）——LVGL 重绘 UI 矩形
       （按钮/对话框）时从干净帧自拷再叠半透明背景，写回 fb 不会自叠加变灰。
       老路：canvas = s_pd_fb[0]，pd_anim 每帧 set_buffer+invalidate */
    if (s_pd_direct)
        lv_canvas_set_buffer(c, (uint8_t*)s_pd_fb[2], 480, 800, LV_COLOR_FORMAT_RGB565);
    else
        lv_canvas_set_buffer(c, (uint8_t*)s_pd_fb[0], 480, 800, LV_COLOR_FORMAT_RGB565);
    for (int i = 0; i < (int)(sizeof(side_btns) / sizeof(side_btns[0])); i++)
        if (side_btns[i] && lv_obj_is_valid(side_btns[i])) lv_obj_move_foreground(side_btns[i]);
    if (s_mode_label && lv_obj_is_valid(lv_obj_get_parent(s_mode_label)))
        lv_obj_move_foreground(lv_obj_get_parent(s_mode_label));
    s_pd_interaction_bg = bg;
    s_pd_canvas = c;
    s_pd_overlay = bg;   // pd_anim_task 每帧 invalidate 的宿主

    // 横屏 Q 版互动：动作按钮 + 列表面板（视觉左侧；选动作播放 MJPEG）
    if (s_pdq_mode) pdq_anim_ui_create(bg);

    // 触摸：拖动转头/点头 / 按住头部摸头 / 触点视线追踪（与测试页一致）
    // （画质按钮已删：PPA 直写后 CPU 0 零参与，全分辨率 18fps 稳，无需快模式）

    lv_obj_add_event_cb(bg, [](lv_event_t* e){
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t pt; lv_indev_get_point(indev, &pt);
        if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            pd_touch(s_pd_model, pt.x, pt.y, true);
            xSemaphoreGive(s_pd_mutex);
        }
    }, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(bg, [](lv_event_t*){
        if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            pd_touch(s_pd_model, 0, 0, false);
            xSemaphoreGive(s_pd_mutex);
        }
    }, LV_EVENT_RELEASED, NULL);
    lvgl_port_unlock();

    /* 直写模式进入序列——所有 LVGL 全屏重绘与 PPA 直拷严格串行（并发会导致撕裂/灰黑残块）：
       ① loading 遮罩删除 + bg/canvas 创建的全屏 invalidate 合并重绘 → 等 150ms 落盘
          （canvas 已指向 fb[2]=首帧，重绘自拷 = 首帧，遮罩圈消失）
       ② fb[2] 首帧整屏直拷（无掩码，覆盖面板 fb 上的旧聊天画面）
       ③ invalidate(bg) → 重绘（canvas 自拷 = 首帧 + 按钮补画）→ 等 150ms 落盘
       ④ 起 pd_anim：此后 LVGL 只有 UI 矩形重绘（与掩码区不重叠），无并发 */
    if (s_pd_direct) {
        loading_hide();                    // ①
        vTaskDelay(pdMS_TO_TICKS(150));
        pd_ppa_copy_rect(s_pd_fb[2], s_pd_panel_fb, 0, 0, 480, 800);   // ②
        pd_ppa_wait_idle();
        esp_cache_msync((void*)s_pd_panel_fb, 480 * 800 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        lvgl_port_lock(0);
        lv_obj_invalidate(bg);             // ③
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    loading_raise();   // 老路：抬 loading 到最前；直写模式 loading 已 hide，no-op

    s_pd_interaction = true;
    /* 上屏路径：s_pd_direct=PPA 硬件直写（~16fps，渲染全速）；回退时老路 canvas + 5fps 节流兜底 */
    s_pd_throttle = !s_pd_direct;
    /* 横屏 Q 版互动：模式按钮文本切换（点击 = 退出互动回立牌） */
    if (s_pdq_mode && s_mode_label && lv_obj_is_valid(s_mode_label)) {
        lvgl_port_lock(0);
        if (s_mode_label && lv_obj_is_valid(s_mode_label))
            lv_label_set_text(s_mode_label, "退出互动");
        lvgl_port_unlock();
    }
    /* 渲染专用核 CPU 1 */
    if (!s_pd_task) { s_pd_running = true; xTaskCreatePinnedToCore(pd_anim_task, "pd_anim", 8192, NULL, 3, &s_pd_task, 1); }
    loading_hide();
    s_pd_starting = false;
    vTaskDelete(NULL);
}

static void pd_interaction_start(void) {
    if (s_pd_interaction || s_pd_starting) return;
    /* 注意：本函数运行在 LVGL 任务（按钮回调）栈上——只做轻量启动，
       任何 SD 卡 IO（扫描/选角色/加载）都在 pd_interaction_task（10240 栈）里执行 */
    s_pd_starting = true;
    loading_show(s_pdq_mode ? "进入 Q 版互动" : "进入 PPD 交互");
    xTaskCreate(pd_interaction_task, "pd_start", 10240, NULL, 2, NULL);
}

// 横屏 Q 版互动入口（横屏立牌下点"对话模式"）：
// 复用 PPD 交互框架加载 PPD_Q（PC 预旋转横屏布局，渲染零旋转），退出回横屏立牌
static void pdq_interaction_start(void) {
    if (s_pd_interaction || s_pd_starting) return;
    s_pdq_mode = true;
    pd_interaction_start();
}

static void pd_interaction_stop_internal(bool restart_mjpeg) {
    if (!s_pd_interaction && !s_pd_starting) return;
    // 等进入任务结束（最长 15s；task 成功/失败路径都必置 s_pd_starting=false）
    int guard = 0;
    while (s_pd_starting && guard++ < 300) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_pd_interaction) return;
    // 动作动画播放中退出互动：先停播放（否则与 standee_resume 的播放冲突）
    if (s_anim_play) {
        video_playback_stop();
        s_anim_play = false;
    }
    s_anim_btn = NULL;   // bg 删除时子对象一并删除，指针置空
    s_anim_panel = NULL;
    ESP_LOGI("PD", "interaction stop");
    loading_hide();   // 加载动画残留清理（幂等）
    s_pd_running = false; vTaskDelay(pdMS_TO_TICKS(100)); s_pd_task = NULL;
    if (s_pd_direct) pd_ppa_wait_idle();   // 等 pending DMA 完成，防下方 free fb 时 DMA 还在读
    s_pd_direct = false;
    if (s_pd_lcd) { s_pd_lcd->SetStatusBarVisible(true); s_pd_lcd = NULL; }   // 恢复状态栏
    // 按钮必须先移出 bg 再删 bg（教训：它们是 bg 的子对象）
    lvgl_port_lock(0);
    lv_obj_t* side_btns[] = {s_lv2_interact_btn, s_ppd_interact_btn, s_rhodes_btn, s_kb_btn,
                             s_hide_btn, s_settings_btn, s_menu_btn, s_standee_btn};
    // 数组 8 元素，用 sizeof 计算长度（曾写死 8 越界读栈垃圾 → Load access fault @0x4ffc0000）
    for (int i = 0; i < (int)(sizeof(side_btns) / sizeof(side_btns[0])); i++)
        if (side_btns[i] && lv_obj_is_valid(side_btns[i])) lv_obj_set_parent(side_btns[i], lv_screen_active());
    if (s_mode_label && lv_obj_is_valid(lv_obj_get_parent(s_mode_label)))
        lv_obj_set_parent(lv_obj_get_parent(s_mode_label), lv_screen_active());
    lvgl_port_unlock();
    s_pd_throttle = false;   // 恢复测试页节奏
    s_pd_canvas = NULL;   // 先清指针再删对象，防 anim task 中途重绑定踩空
    s_pd_overlay = NULL;
    if (s_pd_interaction_bg) {
        lvgl_port_lock(0);
        lv_obj_del(s_pd_interaction_bg);
        lvgl_port_unlock();
        s_pd_interaction_bg = NULL;
    }
    if (s_pd_mutex) { vSemaphoreDelete(s_pd_mutex); s_pd_mutex = NULL; }
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    for (int i = 0; i < 3; i++) {
        if (s_pd_fb[i]) { heap_caps_free(s_pd_fb[i]); s_pd_fb[i] = NULL; }
    }
    s_pd_interaction = false;
    // 复位按钮状态（PPD 文本/颜色 + 解除 Live2D 置灰）；横屏 Q 版互动无这些竖屏按钮交互，跳过
    if (!s_pdq_mode) {
        lvgl_port_lock(0);
        if (s_ppd_interact_lbl) lv_label_set_text(s_ppd_interact_lbl, "PPD交互");
        if (s_ppd_interact_btn) lv_obj_set_style_bg_color(s_ppd_interact_btn, lv_color_hex(0x886644), 0);
        if (s_lv2_interact_btn) lv_obj_remove_state(s_lv2_interact_btn, LV_STATE_DISABLED);
        lvgl_port_unlock();
    } else {
        // 横屏 Q 版互动退出：模式按钮文本恢复"对话模式"（cover 模式目标名）
        lvgl_port_lock(0);
        if (s_mode_label && lv_obj_is_valid(s_mode_label))
            lv_label_set_text(s_mode_label, "对话模式");
        lvgl_port_unlock();
    }
    // 恢复画面：竖屏互动 → MJPEG（当前表情）；横屏 Q 版互动 → 回横屏立牌（standee 槽仍在，秒恢复）
    if (restart_mjpeg) {
        if (s_pdq_mode) {
            standee_resume();
        } else {
            extern void expression_restart_mjpeg(void);
            expression_restart_mjpeg();
        }
    }
    s_pdq_mode = false;
}

static void pd_interaction_stop(void) { pd_interaction_stop_internal(true); }

// ── 菜单全屏应用与 PPD 交互的协作 ──
// 拼豆/蟑螂派对/Live2D测试：与 PPD 无资源冲突 → 暂停直写（交互保留），关闭后回到 PPD 待机。
// 纸偶测试：与 PPD 共用 s_pd_fb/s_pd_canvas → 必须退出交互，且跳过 MJPEG 恢复
//（应用内部自己停视频+释放 caches，stop 的 restart_mjpeg 会白加载 124 帧卡 ~5 秒）。

void ppd_interaction_suspend_for_app(void) {
    if (s_standee_mode) standee_suspend();    // 暂停横屏立牌（应用返回后 standee_resume 恢复横屏）
    if (s_pd_interaction) s_pd_suspend = true;
}

void ppd_interaction_resume_after_app(void) {
    /* 只清标志：停 MJPEG/等重绘落盘由 pd_anim 恢复段执行（本函数常跑在 LVGL 任务
       ——触摸回调链——在这里等会阻塞 LVGL 2 秒+，overlay 删除的重绘迟迟不执行，
       静图残留屏幕上"3 秒转换"） */
    s_pd_suspend = false;
    if (s_standee_mode) standee_resume();   // 菜单应用返回：恢复横屏立牌播放
}

// ══════════════════════════════════════════════════
// Q 版动作测试页（横屏菜单进入）：Spine 动画 MJPEG 序列播放（PPD_Q/anim/*.mjpeg）
// 竖屏列表页：右侧窄面板列动作名，左侧透明看动作画面（screen 层 canvas 播放，
// 互动 bg 临时隐藏露出画面）；退出恢复互动直写
// ══════════════════════════════════════════════════
// 扫描 <agent>/PPD_Q/anim/*.mjpeg → s_anim_paths（按文件名排序，目录扫描顺序不稳定）
static void scan_anim_files(void) {
    s_anim_count = 0;
    char adir[320];
    snprintf(adir, sizeof(adir), "%s/PPD_Q/anim", s_agent_path);
    DIR *d = opendir(adir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && s_anim_count < 16) {
            const char *ext = strrchr(e->d_name, '.');
            if (!ext || strcasecmp(ext, ".mjpeg") != 0) continue;
            size_t alen = strlen(adir), nlen = strlen(e->d_name);
            if (alen + 1 + nlen >= sizeof(s_anim_paths[0])) continue;
            memcpy(s_anim_paths[s_anim_count], adir, alen);
            s_anim_paths[s_anim_count][alen] = '/';
            memcpy(s_anim_paths[s_anim_count] + alen + 1, e->d_name, nlen + 1);
            s_anim_count++;
        }
        closedir(d);
    }
    for (int i = 0; i < s_anim_count; i++)
        for (int j = i + 1; j < s_anim_count; j++)
            if (strcmp(s_anim_paths[i], s_anim_paths[j]) > 0) {
                char t[340]; memcpy(t, s_anim_paths[i], sizeof(t));
                memcpy(s_anim_paths[i], s_anim_paths[j], sizeof(t));
                memcpy(s_anim_paths[j], t, sizeof(t));
            }
}

// 播放原生动作动画：角色图层自然切换动作（anims.json 增量时间轴，纸偶引擎插值渲染），
// 非 MJPEG 录像——画面保持互动页内、背景/待机/触摸全程有效
static void pdq_anim_play(const char *name) {
    if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
        pd_set_anim(s_pd_model, name);
        xSemaphoreGive(s_pd_mutex);
    }
}

// 停止动作播放：回待机（纸偶渲染继续）
static void pdq_anim_stop(void) {
    if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
        pd_set_anim(s_pd_model, NULL);
        xSemaphoreGive(s_pd_mutex);
    }
}

// 横屏 Q 版互动内的动作 UI：左侧"动作"按钮（与按钮组同排布 i=5）+
// 动作列表面板（点击展开/收起；选动作 = pd_set_anim 原生播放，选"恢复互动"回待机）
static void pdq_anim_ui_create(lv_obj_t *bg) {
    if (!s_pd_model || s_pd_model->n_anims <= 0) {
        ESP_LOGW(TAG, "pdq: 无原生动画（anims.json）");
        return;
    }

    s_anim_btn = lv_btn_create(bg);
    lv_obj_set_size(s_anim_btn, 110, 35);
    lv_obj_set_pos(s_anim_btn, 390, 10);   // 排布 i=5（hide 按钮占 i=4）
    lv_obj_set_style_transform_pivot_x(s_anim_btn, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_anim_btn, 0, 0);
    lv_obj_set_style_transform_rotation(s_anim_btn, 900, 0);
    lv_obj_set_style_bg_color(s_anim_btn, lv_color_hex(0xAA6644), 0);
    lv_obj_set_style_bg_opa(s_anim_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_anim_btn, 6, 0);
    lv_obj_set_style_border_width(s_anim_btn, 0, 0);
    lv_obj_t *albl = lv_label_create(s_anim_btn);
    lv_label_set_text(albl, "动作");
    lv_obj_set_style_text_color(albl, lv_color_white(), 0);
    lv_obj_set_style_text_font(albl, s_chat_font, 0);
    lv_obj_center(albl);
    lv_obj_add_event_cb(s_anim_btn, [](lv_event_t *e) {
        if (!s_anim_panel || !lv_obj_is_valid(s_anim_panel)) return;
        if (lv_obj_has_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, NULL);

    // 动作列表面板（与按钮组同向旋转：本地 150×480 @ (480,10) + pivot(0,0) + 顺时针 90°
    // → 屏幕顶部横条 x∈[0,480] y∈[10,160] = 横持视觉上方，文字正立；默认隐藏）
    s_anim_panel = lv_obj_create(bg);
    lv_obj_set_size(s_anim_panel, 150, 480);
    lv_obj_set_pos(s_anim_panel, 480, 10);
    lv_obj_set_style_transform_pivot_x(s_anim_panel, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_anim_panel, 0, 0);
    lv_obj_set_style_transform_rotation(s_anim_panel, 900, 0);
    lv_obj_set_style_bg_color(s_anim_panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_anim_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_anim_panel, 0, 0);
    lv_obj_set_style_pad_all(s_anim_panel, 0, 0);
    lv_obj_clear_flag(s_anim_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ptitle = lv_label_create(s_anim_panel);
    lv_label_set_text(ptitle, "动作");
    lv_obj_set_style_text_color(ptitle, lv_color_white(), 0);
    lv_obj_set_style_text_font(ptitle, s_chat_font, 0);
    lv_obj_set_pos(ptitle, 5, 6);

    // 恢复互动（停止动作播放）
    lv_obj_t *stop_btn = lv_btn_create(s_anim_panel);
    lv_obj_set_size(stop_btn, 140, 26);
    lv_obj_set_pos(stop_btn, 5, 30);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0x558855), 0);
    lv_obj_set_style_radius(stop_btn, 5, 0);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, "恢复互动");
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stop_lbl, s_chat_font, 0);
    lv_obj_center(stop_lbl);
    lv_obj_add_event_cb(stop_btn, [](lv_event_t *e) {
        pdq_anim_stop();
        if (s_anim_panel && lv_obj_is_valid(s_anim_panel))
            lv_obj_add_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN);   // 收起面板（反馈明确）
    }, LV_EVENT_CLICKED, NULL);

    // 动画列表（来自模型 anims.json 的原生动画名）
    for (int i = 0; i < s_pd_model->n_anims && i < 14; i++) {
        char name[32];
        snprintf(name, sizeof(name), "%s", s_pd_model->anims[i].name);

        lv_obj_t *b = lv_btn_create(s_anim_panel);
        lv_obj_set_size(b, 140, 26);
        lv_obj_set_pos(b, 5, 62 + i * 30);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_radius(b, 5, 0);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, name);
        lv_obj_set_style_text_color(bl, lv_color_white(), 0);
        lv_obj_set_style_text_font(bl, s_chat_font, 0);
        lv_obj_center(bl);
        static char s_anim_names[16][32];
        snprintf(s_anim_names[i], sizeof(s_anim_names[i]), "%s", name);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            pdq_anim_play((const char*)lv_event_get_user_data(e));
            if (s_anim_panel && lv_obj_is_valid(s_anim_panel))
                lv_obj_add_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN);   // 播放时收起面板，完整看清动作
        }, LV_EVENT_CLICKED, (void*)s_anim_names[i]);
    }
    lv_obj_move_foreground(s_anim_btn);
    lv_obj_move_foreground(s_anim_panel);
}

// 动作加载+播放（SD 读在独立任务做，不在 LVGL 任务栈上——同 pd_interaction 教训）
static void anim_load_task(void *arg) {
    const char *path = (const char*)arg;
    int cnt = ppa_preload_mjpeg(path);   // active 槽加载动画帧
    loading_hide();
    if (cnt <= 0) { ESP_LOGE(TAG, "anim: load fail %s", path); vTaskDelete(NULL); return; }
    s_image_count = cnt;
    s_current_index = 0;
    s_anim_play = true;   // decode 走普通分支（active 槽动画帧），不走 standee 分支
    video_playback_start(30);
    ESP_LOGI(TAG, "anim play: %s (%d 帧)", path, cnt);
    vTaskDelete(NULL);
}

static void anim_test_hide(void) {
    if (!s_anim_overlay) return;
    video_playback_stop();
    s_anim_play = false;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        lv_obj_del(s_anim_overlay);
        s_anim_overlay = NULL;
        // 恢复互动画面显示（pd_anim 恢复段也会 remove，双保险）
        if (s_pd_interaction_bg && lv_obj_is_valid(s_pd_interaction_bg))
            lv_obj_remove_flag(s_pd_interaction_bg, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    ppd_interaction_resume_after_app();   // 恢复互动直写 / 横屏立牌
    ESP_LOGI(TAG, "Anim test hidden");
}

void anim_test_show(void) {
    if (s_anim_overlay) return;
    ppd_interaction_suspend_for_app();   // 暂停互动直写 + 横屏立牌（返回后恢复）

    // 扫描 <agent>/PPD_Q/anim/*.mjpeg（s_agent_path 即 operator 根目录）
    scan_anim_files();
    if (s_anim_count == 0) {
        ESP_LOGW(TAG, "anim_test: %s/PPD_Q/anim 无动画", s_agent_path);
        ppd_interaction_resume_after_app();
        return;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) { ppd_interaction_resume_after_app(); return; }
    // 隐藏互动 bg：露出 screen 层 MJPEG 播放画面
    if (s_pd_interaction_bg && lv_obj_is_valid(s_pd_interaction_bg))
        lv_obj_add_flag(s_pd_interaction_bg, LV_OBJ_FLAG_HIDDEN);

    s_anim_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_anim_overlay, 480, 800);
    lv_obj_set_pos(s_anim_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_anim_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_anim_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_anim_overlay, 0, 0);
    lv_obj_clear_flag(s_anim_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // 右侧面板（按钮列表区）
    lv_obj_t *panel = lv_obj_create(s_anim_overlay);
    lv_obj_set_size(panel, 150, 800);
    lv_obj_set_pos(panel, 330, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, "动作测试");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_set_pos(lbl, 5, 8);

    lv_obj_t *back = lv_btn_create(panel);
    lv_obj_set_size(back, 140, 30);
    lv_obj_set_pos(back, 5, 40);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x885544), 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_font(back_lbl, s_chat_font, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back, [](lv_event_t *e) { anim_test_hide(); }, LV_EVENT_CLICKED, NULL);

    // 动作按钮列表（名字取文件名去扩展名）
    for (int i = 0; i < s_anim_count; i++) {
        const char *fn = strrchr(s_anim_paths[i], '/');
        fn = fn ? fn + 1 : s_anim_paths[i];
        char name[64];
        size_t nl = strlen(fn);
        if (nl > 8 && strcasecmp(fn + nl - 8, ".mjpeg") == 0) nl -= 8;
        if (nl >= sizeof(name)) nl = sizeof(name) - 1;
        memcpy(name, fn, nl); name[nl] = '\0';

        lv_obj_t *b = lv_btn_create(panel);
        lv_obj_set_size(b, 140, 30);
        lv_obj_set_pos(b, 5, 80 + i * 36);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, name);
        lv_obj_set_style_text_color(bl, lv_color_white(), 0);
        lv_obj_set_style_text_font(bl, s_chat_font, 0);
        lv_obj_center(bl);
        char *p = s_anim_paths[i];
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            const char *path = (const char*)lv_event_get_user_data(e);
            loading_show("加载动作");
            xTaskCreate(anim_load_task, "anim_load", 8192, (void*)path, 2, NULL);
        }, LV_EVENT_CLICKED, (void*)p);
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Anim test shown: %d 动画", s_anim_count);
}

void ppd_interaction_stop_for_app(void) {
    if (s_standee_mode) standee_suspend();    // 纸偶测试打开前暂停（返回后恢复横屏）
    pd_interaction_stop_internal(false);
}

// 对话空闲钩子（application.cc OnAudioChannelClosed 调用）：
// 横屏互动中唤醒对话 → 对话结束（通道关闭）自动切回 cover → mode_switch_task(false)
// 末尾消费 s_pdq_resume_after_chat 重进横屏立牌（s_req_cover 由 video_playback_task 消费）
void pdq_chat_idle_check(void) {
    if (!s_pdq_resume_after_chat) return;
    if (s_standee_mode) return;   // 已在横屏（不可能，防御）
    ESP_LOGI(TAG, "pdq: 对话结束 → 自动返回横屏立牌");
    s_req_expression = false;
    s_req_cover = true;   // video_playback_task 循环检测到后生成 mode_switch_task(false)
}

// 应用层查询：PPD/Live2D 交互是否激活（OnAudioChannelClosed 在交互中跳过 cover 切换，
// 画面留在交互待机——否则长时间不对话后 UI 被 cover 按钮组破坏）
bool ppd_interaction_active(void) { return s_pd_interaction || s_pd_starting; }
bool lv2_interaction_active(void) { return s_lv2_interaction; }

void pd_test_hide(void) {
    if (!s_pd_overlay) return;
    s_pd_running = false; vTaskDelay(pdMS_TO_TICKS(100)); s_pd_task = NULL;
    if (s_pd_mutex) { vSemaphoreDelete(s_pd_mutex); s_pd_mutex = NULL; }
    s_pd_canvas = NULL;
    lv_obj_del(s_pd_overlay); s_pd_overlay = NULL;
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    for (int i = 0; i < 2; i++) {
        if (s_pd_fb[i]) { heap_caps_free(s_pd_fb[i]); s_pd_fb[i] = NULL; }
    }
    application_set_wake_word_detection(true);
    video_playback_start(30);
    ppd_interaction_resume_after_app();   // 横屏立牌暂停恢复（若从横屏进纸偶测试，返回回横屏）
}

void pd_test_show(void) {
    if (s_pd_overlay) return;
    ESP_LOGI("PD", "show: PSRAM free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    pd_scan_chars();
    if (s_pd_count == 0) {
        ESP_LOGW("PD", "无角色：请在 SD 卡 /sdcard/main/operator/<职业>/<星级>/<干员>/PPD/ 放入 scene.json + *.raw");
        return;
    }
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    ppa_release_playback_caches();   // cover/emoji/mask 等播放缓存让路（退出时自动重载）
    ppa_close_mjpeg();
    ppa_release_jpeg_engine();       // 下次 composite 时 lazy 重建（与索引页同款序列）
    ESP_LOGI("PD", "after release: PSRAM free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    application_set_wake_word_detection(false);
    /* fb 尾部加 4KB 填充：LVGL 软件混合按 12 像素块对齐会过读尾块，
       缓冲若落在 PSRAM 映射顶端会踩未映射页直接 Load access fault */
    for (int i = 0; i < 2; i++) {
        /* 64B 对齐：PPA 的 out 缓冲强制要求（esp_cache_get_alignment） */
        s_pd_fb[i] = (uint16_t*)heap_caps_aligned_alloc(64, 480 * 800 * 2 + 4096, MALLOC_CAP_SPIRAM);
        if (s_pd_fb[i]) memset(s_pd_fb[i], 0, 480 * 800 * 2);
    }
    if (!s_pd_fb[0] || !s_pd_fb[1]) {
        ESP_LOGE("PD", "fb alloc fail");
        for (int i = 0; i < 2; i++) {
            if (s_pd_fb[i]) { heap_caps_free(s_pd_fb[i]); s_pd_fb[i] = NULL; }
        }
        application_set_wake_word_detection(true);
        video_playback_start(30);
        return;
    }

    lvgl_port_lock(0);
    s_pd_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_pd_overlay, 480, 800);
    lv_obj_set_pos(s_pd_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_pd_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_pd_overlay, lv_color_hex(0x0b0d12), 0);  // canvas 796 高底部露底：同色不露白边
    lv_obj_set_style_border_width(s_pd_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_pd_overlay, 0, 0);
    lv_obj_clear_flag(s_pd_overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_pd_canvas = lv_canvas_create(s_pd_overlay);
    /* 高度 796：全不透明全屏图像走 lv_memcpy 快路径，SIMD 尾块会过读 ~24B；
       若目标缓冲顶在 PSRAM 池末端，最后一行过读踩未映射页 → Load access fault。
       留 4 行余量让过读落在缓冲内（底部 4px 是深色背景，无视觉差异） */
    lv_obj_set_size(s_pd_canvas, 480, 796);
    lv_canvas_set_buffer(s_pd_canvas, (uint8_t*)s_pd_fb[0], 480, 796, LV_COLOR_FORMAT_RGB565);

    // 触摸：拖动转头 / 按住头部摸头 / 触点视线追踪
    lv_obj_add_event_cb(s_pd_overlay, [](lv_event_t* e){
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t pt; lv_indev_get_point(indev, &pt);
        if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            pd_touch(s_pd_model, pt.x, pt.y, true);
            xSemaphoreGive(s_pd_mutex);
        }
    }, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_pd_overlay, [](lv_event_t*){
        if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            pd_touch(s_pd_model, 0, 0, false);
            xSemaphoreGive(s_pd_mutex);
        }
    }, LV_EVENT_RELEASED, NULL);

    // 角色名（左上）
    s_pd_name_lbl = lv_label_create(s_pd_overlay);
    lv_obj_set_pos(s_pd_name_lbl, 10, 14);
    lv_obj_set_style_text_color(s_pd_name_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pd_name_lbl, s_chat_font, 0);

    // 返回按钮
    lv_obj_t* back = lv_btn_create(s_pd_overlay);
    lv_obj_set_size(back, 60, 36);
    lv_obj_set_pos(back, 410, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_lbl, s_chat_font, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back, [](lv_event_t*){ pd_test_hide(); }, LV_EVENT_CLICKED, NULL);

    // 角色切换按钮
    lv_obj_t* ch_btn = lv_btn_create(s_pd_overlay);
    lv_obj_set_size(ch_btn, 80, 26);
    lv_obj_set_pos(ch_btn, 320, 15);
    lv_obj_set_style_bg_color(ch_btn, lv_color_hex(0x885522), 0);
    lv_obj_set_style_bg_opa(ch_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(ch_btn, 5, 0);
    lv_obj_set_style_border_width(ch_btn, 0, 0);
    lv_obj_t* ch_lbl = lv_label_create(ch_btn);
    lv_label_set_text(ch_lbl, "切换角色");
    lv_obj_set_style_text_color(ch_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ch_lbl, s_chat_font, 0);
    lv_obj_center(ch_lbl);
    lv_obj_add_event_cb(ch_btn, [](lv_event_t*){
        s_pd_index = (s_pd_index + 1) % s_pd_count;
        pd_load_current();
    }, LV_EVENT_CLICKED, ch_lbl);

    // 取景切换按钮（全身→特写，两档循环）
    lv_obj_t* fr_btn = lv_btn_create(s_pd_overlay);
    lv_obj_set_size(fr_btn, 80, 26);
    lv_obj_set_pos(fr_btn, 230, 15);
    lv_obj_set_style_bg_color(fr_btn, lv_color_hex(0x555588), 0);
    lv_obj_set_style_bg_opa(fr_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(fr_btn, 5, 0);
    lv_obj_set_style_border_width(fr_btn, 0, 0);
    lv_obj_t* fr_lbl = lv_label_create(fr_btn);
    lv_label_set_text(fr_lbl, "取景:全身");
    lv_obj_set_style_text_color(fr_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(fr_lbl, s_chat_font, 0);
    lv_obj_center(fr_lbl);
    lv_obj_add_event_cb(fr_btn, [](lv_event_t* e){
        static int fi = 0;  // 0=全身(1.3×) 1=特写(2.8×)
        static const float fr_z[] = {1.3f, 2.8f};
        static const float fr_p[] = {0, 230};
        static const char* fr_n[] = {"取景:全身", "取景:特写"};
        fi = (fi + 1) % 2;
        if (s_pd_model) { s_pd_model->zoom = fr_z[fi]; s_pd_model->pan_y = fr_p[fi]; }
        lv_label_set_text((lv_obj_t*)lv_event_get_user_data(e), fr_n[fi]);
    }, LV_EVENT_CLICKED, fr_lbl);

    // 画质切换按钮（高=全分辨率，快=半分辨率+2×放大）
    lv_obj_t* q_btn = lv_btn_create(s_pd_overlay);
    lv_obj_set_size(q_btn, 70, 26);
    lv_obj_set_pos(q_btn, 152, 15);
    lv_obj_set_style_bg_color(q_btn, lv_color_hex(0x665533), 0);
    lv_obj_set_style_bg_opa(q_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(q_btn, 5, 0);
    lv_obj_set_style_border_width(q_btn, 0, 0);
    lv_obj_t* q_lbl = lv_label_create(q_btn);
    lv_label_set_text(q_lbl, "画质:高");
    lv_obj_set_style_text_color(q_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(q_lbl, s_chat_font, 0);
    lv_obj_center(q_lbl);
    lv_obj_add_event_cb(q_btn, [](lv_event_t* e){
        if (!s_pd_model) return;
        s_pd_model->half_res = !s_pd_model->half_res;
        lv_label_set_text((lv_obj_t*)lv_event_get_user_data(e),
                          s_pd_model->half_res ? "画质:快" : "画质:高");
    }, LV_EVENT_CLICKED, q_lbl);

    // ── 表情按钮（右列，与 PC 仿真器 EXPRESSIONS 表 1:1 对应）──
    static const char* enames[] = {"自动", "说话", "害羞", "惊讶", "开心", "生气",
                                   "俏皮", "悲伤", "疑惑", "困倦", "微笑", "待机", "自信"};
    for (int i = 0; i < 13; i++) {
        lv_obj_t* eb = lv_btn_create(s_pd_overlay);
        lv_obj_set_size(eb, 50, 26); lv_obj_set_pos(eb, 425, 50 + i * 28);
        lv_obj_set_style_bg_color(eb, lv_color_hex(0x555555), 0);
        lv_obj_set_style_bg_opa(eb, LV_OPA_80, 0);
        lv_obj_set_style_radius(eb, 5, 0); lv_obj_set_style_border_width(eb, 0, 0);
        lv_obj_t* el = lv_label_create(eb);
        lv_label_set_text(el, enames[i]);
        lv_obj_set_style_text_color(el, lv_color_white(), 0);
        lv_obj_set_style_text_font(el, s_chat_font, 0);
        lv_obj_center(el);
        lv_obj_add_event_cb(eb, [](lv_event_t* e){
            if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
                pd_set_expression(s_pd_model, (int)(uintptr_t)lv_event_get_user_data(e));
                xSemaphoreGive(s_pd_mutex);
            }
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }
    lvgl_port_unlock();

    s_pd_index = 0;
    s_pd_mutex = xSemaphoreCreateMutex();
    pd_load_current();
    if (!s_pd_model) { pd_test_hide(); return; }
    s_pd_running = true;
    xTaskCreatePinnedToCore(pd_anim_task, "pd_anim", 8192, NULL, 3, &s_pd_task, 1);  /* 渲染专用核 CPU 1；CPU 0 留给 LVGL+AFE（AFE 已改 core 0） */
}
