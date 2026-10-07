/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
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
#include "apps/role_download/role_downloader.h"
#include "wifi_station.h"   // 2026-10-07 开机预下载前等待网络就绪(IsConnected)
#include <esp_task_wdt.h>
#include "esp_system.h"   // esp_random(2026-09-29 开机背景随机起点)
#include <freertos/task.h>
#include "cJSON.h"

extern void lv2_update_animation(float time_sec);
extern void application_set_wake_word_detection(bool enable);

/* 2026-09-17:PDQ 动画名接口(C 链接,全局声明——extern "C" 不能放函数体内) */
extern "C" {
int pdq_anim_count(void *m);
const char *pdq_anim_name(void *m, int i);
}

// Live2D framebuffer + test page forward decls
uint16_t* g_lv2_fb = NULL;
int g_lv2_fb_w = 0, g_lv2_fb_h = 0;
void lv2_test_show(void);  // 供 menu_ui 调用
static void lv2_test_hide(void);

#include "PPACompositor.h"
#include "bind_qr.h"   /* 2026-10-05 六位码绑定页二维码(200x200 RGB565) */
#include "driver/jpeg_decode.h"
#include "driver/jpeg_decode.h"
#include "board.h"
#include "application.h"
#include "display/lcd_display.h"   // LcdDisplay::GetPanelFrameBuffer（PPA 直写面板 fb）
#include "driver/ppa.h"
#include "esp_cache.h"
#include "audio_codec.h"
#include "pinyin_table.h"
#include "audio/tts_engine.h"

#define TAG "AppImageDisplay"

#define SD_MOUNT_POINT            "/sdcard"
#define THUMBS_DONE_FLAG "/sdcard/Arknights/main/operator/INDEX/.thumbs_sync_done"   // 缩略图同步完成标志

/* 428 缩略图同步状态(定义前移:image_display_init 开机检查引用) */
static volatile bool s_thumbs_syncing = false;
static volatile bool s_thumbs_blocking = false;   // 仅首次阻塞同步期间保持索引页手势屏蔽(2026-09-10)
static volatile int s_thumbs_done = 0;     // 同步进度(提示显示用)
static volatile int s_thumbs_total = 0;
static bool thumbs_sync_done(void);
static void thumbs_sync_task(void *arg);
static void index_hint_update(void);   // 索引页空列表提示(含同步进度)
void bg_music_stop(void);   // 背景音乐停止(导出:application.cc 唤醒词回调调用,2026-09-11)

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
static bool s_show_talk_hint = false;           // 手动点"对话模式"按钮:进入后显示引导提示条
static lv_obj_t *s_talk_hint = NULL;            // 顶部半透明引导条("请说你好XXX开始对话")
static int s_loop_count = 0;               // 非 neutral 表情已循环次数
static char s_current_emotion[32] = {0};   // 当前表情名
static char s_pending_emotion[32] = {0};   // 后备表情名
static lv_obj_t *s_mode_label = NULL;    // 模式切换按钮 label
static lv_obj_t *s_rhodes_btn = NULL;    // 罗德岛按钮（仅 cover 模式显示）
static lv_obj_t *s_fashion_btn = NULL;   // 时装按钮（竖屏 cover + cover 目录含 fashion_*.mjpeg 时显示）
static lv_obj_t *s_fashion_lbl = NULL;
static char s_fashion_names[8][64];      // 时装名（fashion_ 之后到 .mjpeg 的 stem）
static char s_fashion_default[80];       // 默认精二文件名（cover 内第一个非 fashion_ 的 .mjpeg）
static int s_fashion_count = 0;
static int s_fashion_cur = -1;           // -1=默认精二（0 也代表默认;>0 时装下标+1）
static lv_obj_t *s_fashion_panel = NULL; // 时装选择全屏面板
// ── 背景系统状态(定义前移:dl_lock_buttons 等前部函数引用)──
static bool s_bg_unlocked = false;     // 解锁背景:触摸=滑动;未解锁:触摸=视线/摸头
static volatile bool s_bg_checking = false;  // 检查中防重入(解锁按钮)
static volatile bool s_bg_task_running = false;  // bg 检查/下载/切换任务在跑(stop 等待+竞态复查)
static int s_bg_cur = -1;              // 当前背景下标(-1=未挂载)
static int s_bg_off_x = 0;             // 视口左缘偏移(可见宽坐标系,0..w-480)
static int s_bg_last_x = -1;           // 滑动增量基准(-1=未按下)
static int s_bg_slot_active = -1;      // 活跃解码槽(0/1)
static int s_bg_pending_idx = -1;      // 切换任务目标下标
static char s_bg_names[40][80];        // 背景 stem(场景_XX),已排序
static int s_bg_count = 0;
static lv_obj_t *s_bg_unlock_btn = NULL;   // 解锁背景/锁定背景(PPD 互动 bg 上,左上竖排)
static lv_obj_t *s_bg_unlock_lbl = NULL;
static lv_obj_t *s_bg_switch_btn = NULL;   // 切换背景(解锁后显示)
static lv_obj_t *s_bg_switch_lbl = NULL;
static lv_obj_t *s_bg_play_btn = NULL;     // 播放/停止背景音乐(2026-09-11,左上竖排)
static lv_obj_t *s_bg_play_lbl = NULL;
static bool s_bg_music_playing = false;    // 背景音乐播放中
static int s_bg_missing_music = 0;         // 检查发现的缺失音乐数(弹窗/下载用)
static lv_obj_t *s_bg_panel = NULL;        // 切换列表面板(lv_layer_top,独立于 bg,stop 需显式删)
static lv_obj_t *s_pd_form_btn = NULL;     // 形态切换按钮(声明前移:bg_invalidate_ui 引用)
static lv_obj_t *s_bg_upd_popup = NULL;    // 新背景三键弹窗(回调经全局取,LVGL 只收普通函数指针)
static lv_obj_t *s_bg_check_popup = NULL;  // 检查中弹窗("正在检查背景…",角色检查同款)
static lv_obj_t *s_lv2_interact_btn = NULL;  // Live2D交互按钮（仅 expression 模式）
static lv_obj_t *s_lv2_interact_lbl = NULL;
static lv_obj_t *s_hide_btn = NULL;  // 右上角隐藏/显示按钮
static bool s_lv2_interaction = false;  // Live2D交互模式开关
static void lv2_interaction_start(void);
static void lv2_interaction_stop(void);
static lv_obj_t *s_ppd_interact_btn = NULL;  // PPD交互按钮（仅 expression 模式；纸偶引擎，Live2D 为后备）
static lv_obj_t *s_ppd_interact_lbl = NULL;
static bool s_pd_interaction = false;   // PPD交互模式开关
static bool s_no_emoji_fallback = false;  // 当前角色无 emoji 表情（退出 PPD/Live2D 交互后回 cover 而非表情 MJPEG）
static void cover_restore(void);   // 回通行证模式（cover 立绘）完整流程（定义在 mode_switch_task 前）
static bool lv2_agent_supported(const char* agent);   // Live2D 交互角色白名单（定义在 lv2 区）
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
static lv_obj_t* s_chat_user_hdr_label = NULL;  // 输入框表头（2026-09-28 显示绑定用户账号名）
static char s_chat_user_hdr[80] = {0};          // 输入框表头文本缓冲（账号名 + ':'，UTF-8）
static char s_chat_agent_hdr[80] = {0};         // 回复框表头文本缓冲（干员中文名 + ':'，UTF-8）
static bool s_pd_suspend = false;       // 全屏应用抽屉（拼豆/蟑螂/Live2D测试）打开：直写暂停、交互保留
static lv_obj_t *s_pd_skin_panel = NULL;   // 时装选择面板(声明前移:pd_anim_task 的 bg_ui 冻结判断引用,2026-09-27)
static void profile_progress_cb(const char* stage, int percent);   // profile 动图加载进度（定义在 profile 区）
static lv_obj_t* s_pd_interaction_bg = NULL;   // PPD 交互的全屏覆盖层（定义在 pd_test 区；动图播放时隐藏用）
static volatile bool s_pd_starting = false;  // 进入中守卫（防双点）
static void pd_interaction_start(void);
static void pd_interaction_stop(void);
static void pd_interaction_stop_internal(bool restart_mjpeg);
static void pdq_interaction_start(void);   // 横屏 Q 版互动入口（加载 PPD_Q，退出回横屏立牌）
static void pdq_anim_ui_create(lv_obj_t *bg);   // 互动内动作按钮+列表面板（横屏 Q 版）
static void pdq_anim_panel_set(bool show);      // 面板展开/收起(联动形态按钮显隐)
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
static lv_obj_t *s_profile_wait_ov = NULL;   // 2026-10-06 首次下载等待遮罩
static bool s_profile_waiting = false;       // 等待公共 Ur_Info 下载中
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
#define MUSIC_DIR "/sdcard/Arknights/main/music"

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
// ─── 电量显示(2026-10-02):右上角常驻,所有页面可见 ──────────────
static lv_obj_t *s_batt_icon = NULL;   // 充电闪电符号
static lv_obj_t *s_batt_pct = NULL;    // 百分比数字
static lv_obj_t *s_batt_box = NULL;    // 2026-10-03 胶囊容器:渲染循环逐帧失效(防滑动闪烁)
static bool s_batt_land = false;       // 2026-10-06 胶囊当前横屏态(与 s_pdq_mode 同步)
static void batt_ui_tick(lv_timer_t *timer);   // 前向声明(横屏切换函数重建后立即刷新)

/* 2026-10-06 横屏跟随:Q 版互动模式下胶囊与顶部横条按钮同旋转规则——
   竖 100×32 row(右上角)/ 横 32×100 column + pivot(0,0) rot 900
   (视觉仍为逻辑右上角 100×32 水平胶囊)。重建子对象:尺寸/流向变化后
   旧 flex 布局溢出;仅模式切换瞬间调用一次,内含一次电量采样可接受 */
static void batt_apply_orientation(void) {
    if (!s_batt_box || !lv_obj_is_valid(s_batt_box)) return;
    bool land = s_pdq_mode || s_standee_mode;   /* 2026-10-06 横屏立牌也要旋转 */
    if (land == s_batt_land) return;
    s_batt_land = land;
    ESP_LOGI(TAG, "batt orient → %s (pos %d,%d)", land ? "横屏" : "竖屏",
             land ? 480 : 374, land ? 700 : 2);   // 2026-10-06 诊断:横屏位置核对
    lv_obj_clean(s_batt_box);
    if (land) {
        /* 2026-10-06 layer_sys 层的 transform 旋转不渲染(横屏看不到胶囊的根因):
           横屏时挂到 screen 层(与旋转按钮同层同规律),竖屏回 sys(全页面置顶) */
        lv_obj_set_parent(s_batt_box, lv_screen_active());
        /* 2026-10-07 方向修正(用户对照"动图"按钮 80x45+rot900 物理横):
           横屏画面旋转 90° 显示,逻辑横条不旋转 → 物理竖条。因此横屏
           保持 100x32 横尺寸 + rot 900(与按钮同规律)→ 物理 100x32 横。
           旧方案 32x100+rot900 在物理上恰为竖条(用户多次报告"竖着") */
        lv_obj_set_size(s_batt_box, 100, 32);
        /* 位置:照抄"动图"按钮(475,713)附近;rot 900 绕中心,视觉与按钮同区 */
        lv_obj_align(s_batt_box, LV_ALIGN_TOP_LEFT, 475, 700);
        lv_obj_set_style_transform_rotation(s_batt_box, 900, 0);
        /* 2026-10-07 空框修复:rot+flex 组合在 LVGL 9 布局异常(图标/数字不渲染),
           横屏禁用 flex(LV_LAYOUT_NONE),子对象手动定位 */
        lv_obj_set_layout(s_batt_box, LV_LAYOUT_NONE);
        /* 2026-10-07 诊断:打印旋转样式值(用户报告胶囊仍竖着,确认 rotation 是否落上) */
        ESP_LOGI(TAG, "batt land set: pos=%d,%d size=%dx%d rot=%d parent=%p screen=%p",
                 lv_obj_get_x(s_batt_box), lv_obj_get_y(s_batt_box),
                 lv_obj_get_width(s_batt_box), lv_obj_get_height(s_batt_box),
                 (int)lv_obj_get_style_transform_rotation(s_batt_box, 0),
                 (void*)lv_obj_get_parent(s_batt_box), (void*)lv_screen_active());
    } else {
        lv_obj_set_parent(s_batt_box, lv_layer_sys());
        lv_obj_set_size(s_batt_box, 100, 32);
        lv_obj_set_pos(s_batt_box, 0, 0);
        lv_obj_set_style_transform_rotation(s_batt_box, 0, 0);
        lv_obj_set_layout(s_batt_box, LV_LAYOUT_FLEX);   // 2026-10-07 横屏曾置 NONE,回竖屏恢复
        lv_obj_set_flex_flow(s_batt_box, LV_FLEX_FLOW_ROW);
        lv_obj_align(s_batt_box, LV_ALIGN_TOP_RIGHT, -6, 2);
    }
    s_batt_icon = lv_label_create(s_batt_box);
    lv_label_set_text(s_batt_icon, "");
    s_batt_pct = lv_label_create(s_batt_box);
    lv_label_set_text(s_batt_pct, "");
    if (land) {
        /* 2026-10-07 横屏手动定位(rot+flex 组合曾不渲染内容):图标左、数字右 */
        lv_obj_set_pos(s_batt_icon, 6, 4);
        lv_obj_set_width(s_batt_icon, 24);
        lv_obj_set_pos(s_batt_pct, 32, 4);
        lv_obj_set_size(s_batt_pct, 62, 24);
    } else {
        lv_obj_set_width(s_batt_pct, 64);   /* 2026-10-07 横竖屏同为 100x32 横条,统一 64 宽 */
    }
    lv_obj_set_style_text_align(s_batt_pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_batt_pct,
        Board::GetInstance().GetDisplay()->GetTextFont(), 0);
    batt_ui_tick(NULL);
}

static void batt_ui_tick(lv_timer_t *timer) {
    /* 2026-10-07 兜底:横屏时若胶囊被任何布局重算拉离锚点/改回尺寸
       (实测曾出现 pos=374,2 size=100x32),每 tick 强制复位到 (475,700) 100×32 */
    if (s_batt_land && s_batt_box && lv_obj_is_valid(s_batt_box)) {
        if (lv_obj_get_x(s_batt_box) != 475 || lv_obj_get_y(s_batt_box) != 700 ||
            lv_obj_get_width(s_batt_box) != 100 || lv_obj_get_height(s_batt_box) != 32) {
            static int warn_cnt = 0;
            if (warn_cnt++ < 5)
                ESP_LOGW(TAG, "batt 兜底复位: 实际 pos=%d,%d size=%dx%d → (475,700) 100x32",
                         lv_obj_get_x(s_batt_box), lv_obj_get_y(s_batt_box),
                         lv_obj_get_width(s_batt_box), lv_obj_get_height(s_batt_box));
            lv_obj_set_size(s_batt_box, 100, 32);
            lv_obj_align(s_batt_box, LV_ALIGN_TOP_LEFT, 475, 700);
        }
    }
    int level = 0;
    bool charging = false, discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        lv_label_set_text(s_batt_icon, "");
        lv_label_set_text(s_batt_pct, "");
        return;
    }
    // 2026-10-02 多级电池图标(20% 一档)+ 充电闪电
    if (charging) {
        lv_label_set_text(s_batt_icon, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_color(s_batt_icon, lv_color_hex(0x6FDCFF), 0);
    } else {
        static const char* icons[] = {LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1,
                                      LV_SYMBOL_BATTERY_2, LV_SYMBOL_BATTERY_3,
                                      LV_SYMBOL_BATTERY_FULL, LV_SYMBOL_BATTERY_FULL};
        lv_label_set_text(s_batt_icon, icons[level / 20]);
        lv_obj_set_style_text_color(s_batt_icon,
            level <= 10 ? lv_color_hex(0xFF4444) : lv_color_hex(0xFFFFFF), 0);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", level);
    lv_label_set_text(s_batt_pct, buf);
    lv_obj_set_style_text_color(s_batt_pct,
        level <= 10 ? lv_color_hex(0xFF4444) : lv_color_hex(0xFFFFFF), 0);
}

/* 2026-10-03 电量读取独立任务:64 次 ADC 采样不再占用 LVGL 任务(选角色崩溃
   HP WDT 复位的头号嫌疑是恢复后的 LVGL 定时器 + ADC 组合),任何 ADC/测量
   异常只冻结电量显示,不拖垮 UI 与渲染。 */
static void battery_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
            /* 2026-10-06 兜底:pd_anim 循环不跑时(索引页等)方向切换在此生效 */
            if ((s_pdq_mode || s_standee_mode) != s_batt_land) batt_apply_orientation();
            batt_ui_tick(NULL);
            lvgl_port_unlock();
        }
    }
}

/* 2026-10-07 前向声明(定义在文件后部;开机预下载任务使用) */
static void ur_info_public_fetch_task(void *arg);
static void ur_info_fetch_task(void *arg);
static int scan_bound_user_uid(void);

bool image_display_init(void)
{
    /* 2026-10-06 版本戳:用户核对烧录版本(电量横屏位置/立牌旋转/按钮恢复/Ur_Info 下载) */
    ESP_LOGI(TAG, "FWV=20261007-0015 (batt-manual-pos + cp-ota-public + dl-free-psram)");
    ESP_LOGI(TAG, "Initializing image display...");

    if (!ppa_init()) {
        ESP_LOGE(TAG, "PPA init failed");
        return false;
    }

    lvgl_port_lock(0);
    s_image_canvas = lv_canvas_create(lv_scr_act());
    lv_obj_set_pos(s_image_canvas, 0, 0);
    /* 2026-09-26:canvas 此前未设置尺寸(默认极小)→ 开机索引页构建的几秒里
       LVGL 白底+状态栏裸露(用户报"纯白背景上跳出离线/待机");设全屏黑底盖住 */
    lv_obj_set_size(s_image_canvas, 480, 800);
    static lv_style_t canvas_style;
    lv_style_init(&canvas_style);
    lv_style_set_bg_color(&canvas_style, lv_color_black());
    lv_obj_add_style(s_image_canvas, &canvas_style, 0);

    // 2026-10-02 电量显示:右上角常驻,半透明黑底胶囊(电池图标 + 百分比)。
    // lv_layer_sys:在所有页面/弹窗之上(WIFI 配网页、角色选择页也可见)
    {
        lv_obj_t* batt_box = lv_obj_create(lv_layer_sys());
        /* 2026-10-05 固定尺寸(原 CONTENT 自适应):对话模式聊天框每帧更新触发
           全局重绘/布局重算,CONTENT 对象的包围盒微抖 ±3px(恰等于 pad),
           胶囊右/下边缘 3px 每帧闪烁。固定 100×32 后 bbox 恒定,不再抖
           (84 宽放不下 display 字体的 "100%"——曾被折成两行) */
        lv_obj_set_size(batt_box, 100, 32);
        lv_obj_set_style_bg_color(batt_box, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(batt_box, LV_OPA_50, 0);
        lv_obj_set_style_border_width(batt_box, 0, 0);
        lv_obj_set_style_radius(batt_box, 6, 0);
        lv_obj_set_style_pad_all(batt_box, 3, 0);
        lv_obj_set_flex_flow(batt_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(batt_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_align(batt_box, LV_ALIGN_TOP_RIGHT, -6, 2);   /* 2026-10-05 上移 2px:与设置按钮错开 */
        s_batt_box = batt_box;   // 2026-10-03 逐帧失效用
        s_batt_icon = lv_label_create(batt_box);
        lv_label_set_text(s_batt_icon, "");
        s_batt_pct = lv_label_create(batt_box);
        lv_label_set_text(s_batt_pct, "");
        lv_obj_set_width(s_batt_pct, 64);   /* 2026-10-05 百分比固定宽居中:100%→99% 不再改胶囊宽度 */
        lv_obj_set_style_text_align(s_batt_pct, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(s_batt_pct,
            Board::GetInstance().GetDisplay()->GetTextFont(), 0);
    }
    /* 2026-10-03 电量恢复:独立任务读 ADC(不再用 LVGL 定时器——64 次采样
       占 LVGL 任务,选角色时 HP WDT 复位的头号嫌疑);首次立即刷新一次 */
    batt_ui_tick(NULL);
    xTaskCreate(battery_task, "batt", 4096, NULL, 1, NULL);
    lvgl_port_unlock();

    // ── 加载动画进度接线：lv2 / PPA 加载回调 → loading 覆盖层 + profile 动图小指示 ──
    lv2_set_progress_cb([](const char* s, int p) { loading_set_stage(s, p); });
    extern void ppa_set_load_progress_cb(void (*)(const char*, int));
    ppa_set_load_progress_cb([](const char* s, int p) {
        loading_set_stage(s, p);
        profile_progress_cb(s, p);   // 动图加载小指示（s_profile_load_ind 非空才更新）
    });

    /* 开机增量检查(2026-09-10):同步标志已存在 → 后台静默补缺(新角色缩略图);
       标志不存在(首次)→ 不启动,等用户进罗德岛走阻塞式首同步 */
    if (thumbs_sync_done() && !s_thumbs_syncing) {
        s_thumbs_syncing = true;
        xTaskCreate(thumbs_sync_task, "thumbs_bg", 16384, (void*)0, 2, NULL);
    }

    /* 2026-10-07 蟑螂派对主页资源后台预下载(用户拍板:动图大,WiFi 后提前拉,
       不等到打开 profile 才下):
       公共 Ur_Info(默认照片/动图)+ 用户仓库 Ur_Info(用户上传的) */
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(8000));   // 等网络起来 + 不与缩略图同步抢带宽
        ur_info_public_fetch_task(NULL);
    }, "urinfo_boot", 8192, NULL, 2, NULL);
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(8000));
        int uid = scan_bound_user_uid();
        if (uid > 0) ur_info_fetch_task((void*)(intptr_t)uid);
        else vTaskDelete(NULL);
    }, "urinfo_uboot", 8192, NULL, 2, NULL);

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

/* preload 期间周期性刷新 loading(2026-09-13):加载 124 帧约 6-7 秒,
   遮罩被 15s 超时兜底/其他 hide 误杀或创建失败时会白屏——每 2 秒重刷新一次 */
static void cover_loading_keepalive_task(void *arg) {
    int guard = 0;
    while (s_cover_worker_busy && guard++ < 60) {   // 最长 2 分钟
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_cover_worker_busy) break;
        loading_show("加载立绘");
    }
    vTaskDelete(NULL);
}

static void cover_display_start_async(const char* agent_path) {
    strncpy(s_cover_pending_path, agent_path, sizeof(s_cover_pending_path) - 1);
    s_cover_switch_gen = s_cover_switch_gen + 1;
    loading_show("加载立绘");  // PPA 帧级回调自动报"封面 N%"
    if (!s_cover_worker_busy) {
        s_cover_worker_busy = true;
        xTaskCreate(cover_switch_task, "cover_sw", 6144, NULL, 2, NULL);
        xTaskCreate(cover_loading_keepalive_task, "cov_keep", 2048, NULL, 2, NULL);
    }
}

// ── 时装系统（竖屏 cover 多立绘切换;不记忆,切角色/重启回默认精二）──
// cover 目录文件约定:默认精二 = 任意非 fashion_ 前缀 .mjpeg;时装 = fashion_<名>.mjpeg
/* 2026-10-05 选默认精二立绘:优先第一个非 fashion_ 的 .mjpeg;
   全部是时装时回退第一个 .mjpeg(罕见)。修复夕等角色时装文件排前被误当默认 */
static bool cover_pick_default(const char *agent_sd_path, char *out, size_t out_sz) {
    if (!agent_sd_path || !agent_sd_path[0] || !out) return false;
    out[0] = '\0';
    char cover_dir[300];
    snprintf(cover_dir, sizeof(cover_dir), "%s/cover", agent_sd_path);
    DIR *d = opendir(cover_dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || strcasecmp(ext, ".mjpeg") != 0) continue;
        if (strncasecmp(e->d_name, "fashion_", 8) != 0) {
            snprintf(out, out_sz, "%s/cover/%.*s", agent_sd_path, 200, e->d_name);
            closedir(d);
            return true;
        }
    }
    rewinddir(d);
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (ext && strcasecmp(ext, ".mjpeg") == 0) {
            snprintf(out, out_sz, "%s/cover/%.*s", agent_sd_path, 200, e->d_name);
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

static void fashion_btn_sync(void);
static void fashion_panel_hide(void);
void agent_index_show(void);   /* 2026-09-26 开机直进索引页:去 static 供 application.cc 调用 */   // 前向声明:cover 加载失败回退索引页(2026-09-10)

static void scan_fashion(const char *agent_sd_path) {
    s_fashion_count = 0;
    s_fashion_cur = -1;
    s_fashion_default[0] = '\0';
    char cover_dir[300];
    snprintf(cover_dir, sizeof(cover_dir), "%s/cover", agent_sd_path);
    DIR *d = opendir(cover_dir);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcasecmp(ext, ".mjpeg") != 0) continue;
        if (strncasecmp(entry->d_name, "fashion_", 8) == 0) {
            int stem = (int)(ext - entry->d_name) - 8;
            if (stem > 0 && stem < (int)sizeof(s_fashion_names[0]) && s_fashion_count < 8) {
                memcpy(s_fashion_names[s_fashion_count], entry->d_name + 8, stem);
                s_fashion_names[s_fashion_count][stem] = '\0';
                s_fashion_count++;
            }
        } else if (s_fashion_default[0] == '\0') {
            /* 文件名最长 NAME_MAX(255)>缓冲 80:-Wformat-truncation 判为可能截断报错;
               精度限制后编译器可证不越界(d_name 必 NUL 结尾) */
            snprintf(s_fashion_default, sizeof(s_fashion_default), "%.*s",
                     (int)sizeof(s_fashion_default) - 1, entry->d_name);
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "fashion scan: %d 套时装 (默认 %s)", s_fashion_count,
             s_fashion_default[0] ? s_fashion_default : "无");
}

// 时装按钮与罗德岛按钮同显隐（竖屏 cover 恒显示;无时装时面板仅"默认（精二）"一项,2026-09-26 用户拍板）
static void fashion_btn_sync(void) {
    if (!s_fashion_btn) return;
    /* 2026-09-26 用户拍板:竖屏时装按钮恒显示(无时装时面板仅"默认精二") */
    bool show = !s_standee_mode &&
                s_rhodes_btn && !lv_obj_has_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
    if (show) lv_obj_remove_flag(s_fashion_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_fashion_btn, LV_OBJ_FLAG_HIDDEN);
}

// 切换时装:idx=0 默认精二;1..n = fashion_<名>.mjpeg
static void fashion_select(int idx) {
    if (idx < 0 || idx > s_fashion_count) idx = 0;
    if (idx == 0 && s_fashion_cur == 0) { fashion_panel_hide(); return; }
    if (idx > 0 && s_fashion_cur == idx) { fashion_panel_hide(); return; }
    char path[520];
    if (idx == 0) {
        if (s_fashion_default[0] == '\0') { fashion_panel_hide(); return; }
        snprintf(path, sizeof(path), "%s/cover/%s", s_agent_path, s_fashion_default);
    } else {
        snprintf(path, sizeof(path), "%s/cover/fashion_%s.mjpeg",
                 s_agent_path, s_fashion_names[idx - 1]);
    }
    int frames = ppa_preload_cover(path);
    if (frames > 0) {
        s_image_count = ppa_swap_to_cover();
        ppa_free_cover_slot();
        s_current_index = 0;
        s_fashion_cur = idx;
        ESP_LOGI(TAG, "时装切换: %s (%d 帧)", path, frames);
    } else {
        ESP_LOGE(TAG, "时装加载失败: %s", path);
    }
    fashion_panel_hide();
}

// 时装选择面板（全屏半透明底 + 列表;点空白/选中后关闭）
// 注:lv_obj_add_event_cb 只接受普通函数指针(捕获 lambda 不可转),idx 经 user_data 传
static void fashion_item_click(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    fashion_select(idx);
}

static void fashion_panel_show(void) {
    if (s_fashion_panel) return;
    lvgl_port_lock(0);
    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, 480, 800);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    s_fashion_panel = bg;
    lv_obj_add_event_cb(bg, [](lv_event_t *e) {
        fashion_panel_hide();   // 点空白关闭(列表项点击冒泡到 bg 时已 hide,幂等)
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *title = lv_label_create(bg);
    lv_label_set_text(title, "时装");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 140);
    int total = s_fashion_count + 1;
    for (int i = 0; i < total; i++) {
        const char *nm = (i == 0) ? "默认（精二）" : s_fashion_names[i - 1];
        lv_obj_t *b = lv_btn_create(bg);
        lv_obj_set_size(b, 320, 46);
        lv_obj_set_pos(b, 80, 200 + i * 56);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2a2f3a), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, nm);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, s_chat_font, 0);
        lv_obj_center(l);
        bool cur_sel = (i == 0) ? (s_fashion_cur <= 0) : (s_fashion_cur == i);
        if (cur_sel) lv_obj_set_style_bg_color(b, lv_color_hex(0x886644), 0);
        lv_obj_add_event_cb(b, fashion_item_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    lvgl_port_unlock();
}

static void fashion_panel_hide(void) {
    if (!s_fashion_panel) return;
    lvgl_port_lock(0);
    lv_obj_del(s_fashion_panel);
    s_fashion_panel = NULL;
    lvgl_port_unlock();
}

bool cover_display_start(const char *agent_sd_path) {
    // agent 切换中（expression_display_start 持有锁）→ 跳过
    if (s_in_expression_start) return true;
    // 如果 cover 已经在跑了（mode_switch_task 先切了），跳过但确保按钮可见
    // （本函数现在可能跑在后台 cover_switch_task 里，此分支的 LVGL 调用必须加锁）
    if (s_cover_mode && strcmp(s_agent_path, agent_sd_path) == 0) {
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            if (s_rhodes_btn) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
            fashion_btn_sync();
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
    ESP_LOGI(TAG, "cover: preload wait done (has=%d)", (int)ppa_has_cover());   /* 2026-10-03 诊断:定位选角色 HP WDT 挂点 */
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
        // 默认 = 第一个非 fashion_ 的 .mjpeg（时装文件排前时不能错选时装为精二）
        cover_pick_default(agent_sd_path, path, sizeof(path));
        if (path[0] == '\0') {
            /* 统一文件系统(2026-09-10):本地无立绘(未下载/被清空)→ 回退索引页
               (428 列表 + 克隆/下载入口),不再空屏 */
            ESP_LOGE(TAG, "No .mjpeg in cover dir → 回退索引页");
            agent_index_show();
            return false;
        }
        frame_count = ppa_preload_cover(path);  // 新 cover→slot（旧 cover 仍在 active 显示）
        ESP_LOGI(TAG, "cover: preload done %d frames (%s)", frame_count, path + strlen(agent_sd_path) + 7);   /* 2026-10-03 诊断 */
        if (frame_count > 0) {
            frame_count = ppa_swap_to_cover();  // 新 cover⇄旧 active，旧→slot
            ppa_free_cover_slot();  // 释放 swap 弹进 slot 的旧帧（新 cover 已在 active）
        }
    }
    if (frame_count == 0) {
        ESP_LOGE(TAG, "Failed to preload cover → 回退索引页");
        agent_index_show();
        return false;
    }

    strncpy(s_agent_path, agent_sd_path, sizeof(s_agent_path) - 1);
    s_image_count = frame_count;
    scan_fashion(agent_sd_path);   // 时装列表重扫;切换角色即重置选择(不记忆)
    s_current_index = 0;
    s_cover_mode = true;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_mode_label) lv_label_set_text(s_mode_label, "对话模式");
        if (s_rhodes_btn) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        fashion_btn_sync();
        if (s_kb_btn)    lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_standee_btn) lv_obj_remove_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_lv2_interact_btn) lv_obj_add_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_ppd_interact_btn) lv_obj_add_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
        /* 2026-09-28 对话模式就绪:立绘显示完成后才开唤醒词检测(用户拍板:
           进入对话模式前不检测,防未克隆角色/切换流程窗口的误唤醒) */
        application_set_wake_word_detection(true);

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
                             ? "/sdcard/Arknights/main/background/background_p.jpg"
                             : "/sdcard/Arknights/main/background/background.jpg";
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
        // 回退：恢复 cover 立绘显示（唤醒路径防黑屏；同角色 cover 帧已 swap 进槽，
        // 纯内存操作）。PPD 交互中唤醒时 cover 槽为空（进入时已释放），此分支 no-op，
        // 纸偶直写不受影响——但对话叠加聊天框仍需显示（与有 emoji 角色的 PPD 对话一致）。
        int cc = ppa_has_cover() ? ppa_swap_to_cover() : 0;
        if (cc > 0) {
            s_image_count = cc;
            s_current_index = 0;
            video_playback_start(30);
        }
        if (s_pd_interaction) chat_overlay_show(true);
        s_in_expression_start = false;
        return false;
    }

    s_image_count = count;
    s_current_index = 0;
    s_cover_mode = false;
    s_loop_count = 0;
    s_no_emoji_fallback = false;   // 表情加载成功 = 该角色有 emoji，清无 emoji 标志（换角色残留）
    chat_overlay_show(true);
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        /* 横屏唤醒的对话：模式按钮 = 打断机制（点击退出对话回横屏立牌） */
        if (s_mode_label) lv_label_set_text(s_mode_label,
                                            s_pdq_resume_after_chat ? "退出对话" : "通行证模式");
        if (s_rhodes_btn) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        fashion_btn_sync();
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
        char cover_path[520];
        if (cover_pick_default(agent_sd_path, cover_path, sizeof(cover_path))) {
            ppa_preload_cover_async(cover_path);
            ESP_LOGI(TAG, "Preloading new cover async: %s", cover_path);
        }
    }
    return true;
}

// Restart MJPEG after Live2D interaction stops
static void cover_restore_task(void*) {
    cover_restore();
    vTaskDelete(NULL);
}

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
    if (count == 0) {
        // 无 emoji 表情资源：退出交互不回表情 MJPEG，回通行证模式。
        // 本函数常跑在 LVGL 任务（按钮回调）栈上——cover 恢复含 SD 重载（PPD 进入时
        // release_playback_caches 已清 cover 槽），异步执行防卡 UI。
        ESP_LOGW(TAG, "无 emoji → 退出交互回通行证模式");
        xTaskCreate(cover_restore_task, "cover_rst", 10240, NULL, 3, NULL);
        return;
    }
    video_playback_start(30);
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

// ── 回通行证模式（cover 立绘）：关音频、卸载背景、恢复 cover 显示（槽命中秒切，否则 SD 重载）──
// 原 mode_switch_task 的 else 分支逻辑抽出复用；无 emoji 角色退出 PPD/Live2D 交互后
// 也走这里（PPD 进入时 release_playback_caches 已清 cover 槽 → 需从 SD 重载）。
static void cover_restore(void) {
    loading_show("返回通行证模式");
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
        fashion_btn_sync();
        if (s_lv2_interact_btn) lv_obj_add_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);  // 与罗德岛共用槽位，必须隐藏防重合
        if (s_ppd_interact_btn) lv_obj_add_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_kb_btn)    lv_obj_add_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_standee_btn) lv_obj_remove_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label) lv_label_set_text(s_mode_label, "对话模式");
        application_set_wake_word_detection(true);   /* 2026-09-28 返回对话模式:立绘恢复后开检测 */
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
        cover_pick_default(s_agent_path, path, sizeof(path));   /* 2026-10-05 跳过时装文件 */
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
    s_no_emoji_fallback = false;
    // 引导提示条只属于对话模式:切回 cover 时清掉(防残留)
    if (s_talk_hint && lvgl_port_lock(pdMS_TO_TICKS(200))) {
        lv_obj_del(s_talk_hint);
        s_talk_hint = NULL;
        lvgl_port_unlock();
    }
    loading_hide();  // 统一收尾：覆盖成功/失败/无帧所有路径
}

// ── 模式切换辅助任务（大栈、低优先级，不阻塞音视频核心线程）──
/* 顶部引导提示条:进入对话模式后显示「请说"你好XXX"开始对话」+[确认],点击消失。
 * 只对手动点"对话模式"按钮生效(s_show_talk_hint),唤醒进入不打扰。 */
static void talk_hint_show(void) {
    if (!s_agent_path[0]) return;
    const char *name = strrchr(s_agent_path, '/');
    name = name ? name + 1 : s_agent_path;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_talk_hint) { lv_obj_del(s_talk_hint); s_talk_hint = NULL; }  // 防重复
    s_talk_hint = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_talk_hint, 480, 96);
    lv_obj_set_pos(s_talk_hint, 0, 0);
    lv_obj_set_style_bg_color(s_talk_hint, lv_color_hex(0x111122), 0);
    lv_obj_set_style_bg_opa(s_talk_hint, LV_OPA_80, 0);   // 半透明
    lv_obj_set_style_border_width(s_talk_hint, 0, 0);
    lv_obj_set_style_pad_all(s_talk_hint, 0, 0);
    lv_obj_clear_flag(s_talk_hint, LV_OBJ_FLAG_SCROLLABLE);

    char msg[128];
    snprintf(msg, sizeof(msg), "请说\"你好%.63s\"开始对话", name);   // %.63s 防超长角色名截断告警
    lv_obj_t *lbl = lv_label_create(s_talk_hint);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, s_chat_font, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *ok = lv_btn_create(s_talk_hint);
    lv_obj_set_size(ok, 120, 36);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t *ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, "确认");
    lv_obj_set_style_text_font(ok_lbl, s_chat_font, 0);
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(ok, [](lv_event_t *e) {
        if (s_talk_hint) { lv_obj_del(s_talk_hint); s_talk_hint = NULL; }
    }, LV_EVENT_CLICKED, nullptr);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Talk hint shown (%s)", name);
}

static void mode_switch_task(void *arg) {
    bool to_expression = (bool)arg;
    bool delegated = false;   // 无 emoji 降级委托（PPD/Live2D 交互）：loading 由其进入任务统一隐藏

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
                                 ? "/sdcard/Arknights/main/background/background_p.jpg"
                                 : "/sdcard/Arknights/main/background/background.jpg";
            ppa_load_background(bg);  // 内部报"加载背景"
        }
        ESP_LOGI(TAG, "mode_switch: bg done, free %u maxblk %u",   /* 2026-10-03 诊断:定位卡加载页 */
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        // ── 对话模式资源优先级：PPD 场景 → live2d 白名单 → MJPEG emoji 表情 →
        //    都没有则回通行证模式（防黑屏卡死）──
        struct stat st;
        char pd_scene[300];
        snprintf(pd_scene, sizeof(pd_scene), "%s/PPD/scene.json", s_agent_path);
        bool has_pd = (stat(pd_scene, &st) == 0);
        if (has_pd || lv2_agent_supported(s_agent_path)) {
            ESP_LOGI(TAG, "对话模式 → 优先 %s 交互", has_pd ? "PPD" : "Live2D");
            // 记录角色 emoji 缺失（退出交互后 expression_restart_mjpeg 失败会自动回 cover；
            // 模式按钮"返回通行证"据此跳过 s_req_cover，避免多余一轮切换）
            char neutral_path[300];
            snprintf(neutral_path, sizeof(neutral_path), "%s/emoji/neutral.mjpeg", s_agent_path);
            s_no_emoji_fallback = (stat(neutral_path, &st) != 0);
            s_cover_mode = false;   // 交互视为对话模式：模式按钮点击=退出回通行证
            if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                if (s_mode_label) lv_label_set_text(s_mode_label, "通行证模式");
                if (s_rhodes_btn) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
                fashion_btn_sync();
                if (s_kb_btn) lv_obj_remove_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
                if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
                if (s_voice_text_obj) lv_obj_add_flag(s_voice_text_obj, LV_OBJ_FLAG_HIDDEN);
                /* 2026-10-03 "关闭PPD"/"关闭Live2D"与模式按钮"通行证模式"功能重复
                   (都是退出交互回封面),交互期间隐藏——退出统一走模式按钮;
                   退出后封面路径恢复可见性(7753 恢复标签)。 */
                if (s_ppd_interact_btn) lv_obj_add_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
                if (s_lv2_interact_btn) lv_obj_add_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);
                lvgl_port_unlock();
            }
            delegated = true;   // loading 由交互进入任务结束统一隐藏
            ESP_LOGI(TAG, "mode_switch: enter interaction (%s), free %u",   /* 2026-10-03 诊断 */
                     has_pd ? "PPD" : "Live2D",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            if (has_pd) pd_interaction_start();
            else lv2_interaction_start();
        } else {
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
                    fashion_btn_sync();
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
            } else {
                ESP_LOGW(TAG, "无 PPD/live2d/emoji → 返回通行证模式");
                cover_restore();
            }
        }
    } else {
        cover_restore();
    }
    if (!delegated) loading_hide();  // 降级 PPD/Live2D 委托：loading 由其进入任务统一隐藏
    if (!delegated && s_show_talk_hint && s_agent_path[0] && !s_cover_mode) {
        s_show_talk_hint = false;
        talk_hint_show();   // 手动进入对话模式:顶部引导提示条(点确认消失)
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
        /* 2026-10-05 电量胶囊每帧重绘:cover/对话模式直写帧覆盖面板 fb,
           胶囊此前只在 10s tick 重绘 → 闪烁。与 pd_render 路径同款修复
           (84×32 小区域,持 LVGL 锁防跨核失效链表破坏)。 */
        if (s_batt_box && lvgl_port_lock(pdMS_TO_TICKS(4))) {
            if (lv_obj_is_valid(s_batt_box)) lv_obj_invalidate(s_batt_box);
            lvgl_port_unlock();
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
// 数据源：/sdcard/Arknights/main/operator/<职业>/<星级>/<干员>/standee/*.mjpeg（800×480 横构图）

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
    /* 2026-10-06 更新为当前真实竖屏位置(2026-10-02 按钮列整体下移后旧值过时:
       设置回 (366,5) 与电量胶囊重合、立牌回 (366,285) 与时装按钮重合) */
    static const int orig_pos[5][2] = {
        {366, 35}, {366, 75}, {366, 115}, {366, 315}, {366, 155},
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
    // 横屏隐藏罗德岛（换角色仅竖屏 cover 可做），退出横屏恢复显示;时装同显隐
    if (s_rhodes_btn) {
        if (enter) lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
    }
    fashion_btn_sync();
    batt_apply_orientation();   /* 2026-10-06 电量胶囊跟随横屏立牌(曾等 10s 兜底,进入瞬间方向不对) */
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
    char path[520] = {0};
    cover_pick_default(s_agent_path, path, sizeof(path));   /* 2026-10-05 跳过时装文件 */
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
    role_downloader_set_font(s_chat_font);   // 角色下载页同样用板级中文字体(防豆腐块)
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

        // 播放期间把状态机切到 speaking：唤醒回调只在 idle 处理，语音不会误触发唤醒
        extern void application_set_voice_playback(bool);
        application_set_voice_playback(true);

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

    // 播放结束：状态机切回 idle，恢复唤醒词检测
    extern void application_set_voice_playback(bool);
    application_set_voice_playback(false);

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

// 轻提示(实现见背景音乐区 ui_toast;语音拦截/背景音乐共用)
static void ui_toast(const char *msg);

void voice_ui_show(void) {
    if (s_voice_overlay || s_profile_overlay || settings_ui_is_open() || menu_ui_is_open()) return;
    // 对话模式允许播语音(2026-09-13 用户拍板):判定同音乐——只要没开始对话就可播。
    // 播放期间状态机切 speaking,唤醒回调在 idle 才处理,播放不会被对话打断;
    // 对话中(idle 之外)禁止打开,避免与对话音频冲突。
    extern bool application_device_idle(void);
    if (!application_device_idle()) {
        ui_toast("对话中暂不支持语音播放");
        return;
    }
    fashion_panel_hide();   // 覆盖层打开前关时装面板(避免残留遮挡)

    // Hide all buttons
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_rhodes_btn)    lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        fashion_btn_sync();
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

/* 2026-10-06 开机音乐:boot 封面显示后播放 Rhodes_Island(与背景音乐同一链路,
   唤醒/对话时由 bg_music_stop 停掉)。文件缺失静默跳过。 */
void boot_music_play(void) {
    /* 2026-10-06 用户拍板:开机曲从生命流改为 Rhodes_Island(16kHz mono WAV,音乐库英文命名) */
    const char *dir = "/sdcard/Arknights/main/music";
    char path[220] = {0};
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strstr(e->d_name, "Rhodes_Island") && strstr(e->d_name, ".wav")) {
                snprintf(path, sizeof(path), "%s/%.*s", dir, 160, e->d_name);   /* 精度限制满足 -Werror=format-truncation */
                break;
            }
        }
        closedir(d);
    }
    if (!path[0]) {
        ESP_LOGI(TAG, "boot music: Rhodes_Island wav 不存在,跳过");
        return;
    }
    char *p = strdup(path);
    if (!p) return;
    if (xTaskCreate(music_play_task, "boot_music", 8192, p, 3, &s_music_task) != pdPASS) {
        free(p);
        s_music_task = NULL;
    }
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
    fashion_panel_hide();

    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (s_rhodes_btn)   lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        fashion_btn_sync();
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
        bg_music_stop();   // 音乐页播放时同步停止背景乐(两路音频不共存,2026-09-11)

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
                /* 2026-10-02 文本直发 agent(替代本地 TTS 朗读):
                   复用唤醒词协议 {"type":"listen","state":"detect","text":...},
                   服务器走 startToChat 文本链路(不经声纹,无需音频) */
                xTaskCreate([](void*a){
                    /* 2026-10-02 经 Application::SendUserText:自动建通道
                       (idle 状态也可发起对话),服务器文本链路不经声纹 */
                    Application::GetInstance().SendUserText((const char*)a);
                    free(a); vTaskDelete(NULL);
                },"kb_send",8192,text,3,NULL);
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
                /* 2026-10-02 文本直发 agent(替代本地 TTS 朗读),不经声纹 */
                xTaskCreate([](void*a){
                    /* 2026-10-02 经 Application::SendUserText:自动建通道
                       (idle 状态也可发起对话),服务器文本链路不经声纹 */
                    Application::GetInstance().SendUserText((const char*)a);
                    free(a); vTaskDelete(NULL);
                },"kb_send",8192,text,3,NULL);
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

/* 2026-10-06 用户仓库 Ur_Info 后台补下载(my.html 上传的横屏主页照片/视频);
   打开 profile 时本地缺失则拉取,下次打开生效(静默,不阻塞 UI) */
static int scan_bound_user_uid(void);   // 前向声明(定义在文件后部)
void profile_show(void);   // 2026-10-07 前向声明(ur_info_fetch_task 下载后重开主页)

/* 2026-10-07 开机预下载修复:实测 WiFi 40s+ 才连上,8s 延迟时网络未就绪,
   fetch 一次失败即静默放弃 → 用户进入后无预下载。先等网络就绪(最多 90s) */
static bool ur_info_wait_network(void) {
    for (int i = 0; i < 30; i++) {
        if (WifiStation::GetInstance().IsConnected()) return true;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    ESP_LOGW(TAG, "ur_info 预下载: 等网络 90s 未就绪,放弃");
    return false;
}

static void ur_info_fetch_task(void *arg) {
    int uid = (int)(intptr_t)arg;
    if (!ur_info_wait_network()) { vTaskDelete(NULL); return; }
    int64_t total = 0;
    /* 2026-10-07 判断修复:check 返回 0=部分缺失 / 1=齐全 / -1=网络失败 / -2=云端无包,
       原条件 missing>0(齐全)才下载 → 缺失时从不下载(蟑螂派对一直不更新上传内容的根因) */
    int missing = role_download_check_user(uid, "Ur_Info", &total);
    if (missing == 0) role_download_fetch_user(uid, "Ur_Info", NULL, NULL);
    /* 2026-10-07 下载完成且主页打开中 → 关闭重开刷新(曾:新图下载完成
       但页面仍显示旧图,用户以为没更新;profile_show 有 overlay 重入保护,
       故 hide 后再 show) */
    lvgl_port_lock(pdMS_TO_TICKS(2000));
    bool open = s_profile_overlay && lv_obj_is_valid(s_profile_overlay);
    lvgl_port_unlock();
    if (open && missing == 0) {
        profile_hide();
        profile_show();
    }
    vTaskDelete(NULL);
}

/* 2026-10-06 公共 Ur_Info 后台下载(设备 SD 从未预置过默认照片/动图,
   蟑螂派对 JPG load failed 的根因):Profile.jpg + Profile.mjpeg 缺失则拉取 */
void profile_show(void);   // 前向声明(下载完成重入显示)
static void ur_info_public_fetch_task(void *arg) {
    if (!ur_info_wait_network()) { vTaskDelete(NULL); return; }
    mkdir("/sdcard/User", 0777);            // 已存在时 EEXIST 忽略
    mkdir("/sdcard/User/Ur_Info", 0777);
    const char *files[2] = {"Profile.jpg", "Profile.mjpeg"};
    for (int i = 0; i < 2; i++) {
        char dst[96];
        snprintf(dst, sizeof(dst), "/sdcard/User/Ur_Info/%s", files[i]);
        struct stat st;
        if (stat(dst, &st) == 0 && st.st_size > 0) continue;
        /* 2026-10-07 失败重试 3 次(网络抖动时曾一次失败就放弃) */
        for (int attempt = 0; attempt < 3; attempt++) {
            if (role_download_fetch_public_profile(files[i], dst) == 0) break;
            ESP_LOGW(TAG, "ur_info 公共下载失败(第%d次): %s", attempt + 1, files[i]);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    /* 2026-10-06 下载完成:仍在等待遮罩 → 删遮罩并重入 profile_show 自动显示 */
    lvgl_port_lock(pdMS_TO_TICKS(2000));
    bool reenter = s_profile_waiting;
    if (s_profile_wait_ov && lv_obj_is_valid(s_profile_wait_ov)) {
        lv_obj_del(s_profile_wait_ov);
        s_profile_wait_ov = NULL;
    }
    s_profile_waiting = false;
    lvgl_port_unlock();
    if (reenter) profile_show();
    vTaskDelete(NULL);
}

void profile_show(void) {
    fashion_panel_hide();   // 覆盖层打开前关时装面板
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
        fashion_btn_sync();
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
    mjpeg_free_buffer();   // 2026-10-07 主页打开前帧缓冲让路(曾致 PSRAM 仅 132KB 解码失败)

    // ① 立刻显示 JPG 占位（LVGL 顶层 canvas）
    /* 2026-10-06 用户仓库 Ur_Info 优先(my.html 上传的横屏主页照片);
       本地无则回退公共默认,并后台补下载(下次打开生效) */
    int uid = scan_bound_user_uid();
    char jpg_path[96];
    snprintf(jpg_path, sizeof(jpg_path), "/sdcard/User/Ur_Info/Profile.jpg");
    if (uid > 0) {
        char cand[96];
        snprintf(cand, sizeof(cand), "/sdcard/_users/u%d/Ur_Info/Profile.jpg", uid);
        FILE *cf = fopen(cand, "rb");
        if (cf) { fclose(cf); snprintf(jpg_path, sizeof(jpg_path), "%s", cand); }
        else xTaskCreate(ur_info_fetch_task, "urinfo", 8192, (void*)(intptr_t)uid, 2, NULL);
    }
    ESP_LOGI(TAG, "Profile show: opening JPG %s", jpg_path);
    FILE *fp = fopen(jpg_path, "rb");
    bool has_jpg = false;
    if (!fp && strstr(jpg_path, "/sdcard/User/") != NULL) {
        /* 2026-10-06 首次打开:公共 Ur_Info 未下载 → 显示等待遮罩,下载完成自动显示
           (用户拍板:不要"先关闭等第二次打开") */
        lvgl_port_lock(pdMS_TO_TICKS(500));
        if (!s_profile_wait_ov || !lv_obj_is_valid(s_profile_wait_ov)) {
            s_profile_wait_ov = lv_obj_create(lv_layer_top());
            lv_obj_set_size(s_profile_wait_ov, 480, 800);
            lv_obj_set_pos(s_profile_wait_ov, 0, 0);
            lv_obj_set_style_bg_color(s_profile_wait_ov, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(s_profile_wait_ov, LV_OPA_90, 0);
            lv_obj_set_style_border_width(s_profile_wait_ov, 0, 0);
            lv_obj_clear_flag(s_profile_wait_ov, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *wl = lv_label_create(s_profile_wait_ov);
            lv_label_set_text(wl, "正在下载主页资源…\n(首次约十几秒)");
            lv_obj_set_style_text_color(wl, lv_color_white(), 0);
            lv_obj_set_style_text_font(wl, s_chat_font, 0);
            lv_obj_set_style_text_align(wl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(wl);
            lv_obj_add_event_cb(s_profile_wait_ov, [](lv_event_t *e) {
                /* 点击遮罩取消等待(恢复按钮状态) */
                s_profile_waiting = false;
                if (s_profile_wait_ov) { lv_obj_del(s_profile_wait_ov); s_profile_wait_ov = NULL; }
                profile_hide();
            }, LV_EVENT_CLICKED, NULL);
        }
        lvgl_port_unlock();
        s_profile_waiting = true;
        xTaskCreate(ur_info_public_fetch_task, "urinfo_pub", 8192, (void*)1, 2, NULL);
        ESP_LOGW(TAG, "Profile: JPG 不存在 %s,等待下载完成后自动显示", jpg_path);
        return;   // 不关闭:遮罩在,下载完成重入 profile_show
    }
    if (fp) {
        fclose(fp);
        char lv_path[300];
        /* load_jpg_thumbnail 内部拼 "/sdcard"+path+2,故传 S: + 去前缀路径 */
        snprintf(lv_path, sizeof(lv_path), "S:%s", jpg_path + 7);
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

    // ② "动图"按钮（右下角, 用户手动触发加载;2026-10-06 用户仓库优先,无视频无按钮）
    char mjpeg_path[96];
    snprintf(mjpeg_path, sizeof(mjpeg_path), "/sdcard/User/Ur_Info/Profile.mjpeg");
    if (uid > 0) {
        char cand[96];
        snprintf(cand, sizeof(cand), "/sdcard/_users/u%d/Ur_Info/Profile.mjpeg", uid);
        FILE *cf = fopen(cand, "rb");
        if (cf) { fclose(cf); snprintf(mjpeg_path, sizeof(mjpeg_path), "%s", cand); }
    }
    FILE *mjpeg = fopen(mjpeg_path, "rb");
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
        /* 2026-10-06 创建时拷贝路径(点击回调触发时 profile_show 栈已返回,
           引用局部变量会悬垂);按钮删除时释放 */
        char *mj_path_copy = strdup(mjpeg_path);
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            free((void*)lv_event_get_user_data(e));
        }, LV_EVENT_DELETE, mj_path_copy);
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            if (s_profile_loading) return;
            const char *mj_path = (const char*)lv_event_get_user_data(e);
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
            /* 2026-10-06 路径随 arg 进任务堆拷贝(回调触发时局部变量已失效) */
            void *mj_arg = malloc(sizeof(int) + 96);
            *(int*)mj_arg = gen;
            snprintf((char*)mj_arg + sizeof(int), 96, "%s", mj_path);
            xTaskCreate([](void *arg) {
                int gen = *(int*)arg;
                char path[96];
                snprintf(path, sizeof(path), "%s", (char*)arg + sizeof(int));
                free(arg);
                int n = ppa_preload_profile(path);
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
            }, "pfl", 8192, mj_arg, 2, NULL);
        }, LV_EVENT_CLICKED, mj_path_copy);
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
    /* 2026-10-06 等待遮罩清理(首次下载等待中退出,防残留) */
    s_profile_waiting = false;
    if (s_profile_wait_ov && lv_obj_is_valid(s_profile_wait_ov)) {
        lv_obj_del(s_profile_wait_ov);
        s_profile_wait_ov = NULL;
    }
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
        /* 2026-10-07 修复:曾一律隐藏罗德岛/立牌按钮且无恢复路径
           (注释称 cover_display_start 恢复,但 profile 返回不走该函数)
           → 返回竖屏通行证时"横屏立牌/时装/罗德岛按钮丢失" */
        if (s_rhodes_btn) {
            if (s_profile_was_cover) lv_obj_remove_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_rhodes_btn, LV_OBJ_FLAG_HIDDEN);
        }
        fashion_btn_sync();
        if (s_settings_btn) lv_obj_remove_flag(s_settings_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_menu_btn)     lv_obj_remove_flag(s_menu_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_mode_label)  lv_obj_remove_flag(lv_obj_get_parent(s_mode_label), LV_OBJ_FLAG_HIDDEN);
        if (s_btn_labels[0]) lv_obj_remove_flag(lv_obj_get_parent(s_btn_labels[0]), LV_OBJ_FLAG_HIDDEN);
        if (!s_profile_was_cover) {  // 回 expression 模式才恢复
            if (s_lv2_interact_btn) lv_obj_remove_flag(s_lv2_interact_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_ppd_interact_btn) lv_obj_remove_flag(s_ppd_interact_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_kb_btn) lv_obj_remove_flag(s_kb_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_standee_btn) lv_obj_add_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);   // profile 返回对话模式:立牌隐藏
        } else {
            /* 2026-10-07 返回竖屏通行证(cover):立牌按钮恢复 */
            if (s_standee_btn) lv_obj_remove_flag(s_standee_btn, LV_OBJ_FLAG_HIDDEN);
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
    char path[300];      // "S:/Arknights/main/operator/INDEX/CASTER_108x228/5STAR/Amiya.jpg"
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
static int64_t s_building_since = 0;   // s_index_building 置位时间(卡死看门狗)

// ── 确认弹窗(半透明,建在索引页上):第二档/第三档 ──
static lv_obj_t *s_dl_popup = NULL;
static lv_obj_t *s_dl_title = NULL, *s_dl_sub = NULL, *s_dl_bar = NULL;
static lv_obj_t *s_dl_yes = NULL, *s_dl_yes_lbl = NULL, *s_dl_no = NULL, *s_dl_no_lbl = NULL;
static char s_dl_voc[32], s_dl_star[16], s_dl_name[64], s_dl_path[300];
static volatile bool s_dl_cancel = false;
static int s_dl_mode = 0;   // 0=第三档(无立绘) 1=第二档(部分资源)
static int s_dl_state = 0;  // 0=确认下载 1=云端未就绪 2=检查中/网络失败 3=第二档(继续展示)
// ── 用户仓库下载(索引页卡片也可能是用户自己做的角色)──
static int s_dl_user_uid = 0;         // >0:按用户仓库探测/下载
static char s_dl_user_rel[160];       // 用户根下 rel(Arknights/main/operator/...)
static bool s_dl_user_mode = false;   // 探测命中的下载模式(true=用户仓库)
// ── 下载中状态:统一进度弹窗 / 顶部小条(后台下载) / 页面锁定 ──
static bool s_dl_locked = false;                                          // 下载中:禁止切换页面
static int64_t s_upd_total = 0;   /* 2026-10-07 更新下载总量(速度统计用) */

/* 2026-10-06 下载速度统计(pct 驱动,1 秒窗口):下载框显示 xx% · xxKB/s,
   用户可判断是网络慢还是真卡死(速度持续为 0 = 卡死) */
/* 2026-10-07 速度统计改真实累计字节(role_downloader 的 g_dl_bytes_done):
   多文件下载 pct 按单文件重置,曾按 总量×pct 算出假速度 100MB/s */
extern int64_t g_dl_bytes_done;
static int64_t s_dl_speed_last_bytes = 0;
static int64_t s_dl_speed_ts = 0;
static int s_dl_speed_kbps = 0;   // 2026-10-07 窗口内缓存值(reset 清 0)
static void dl_speed_reset(int64_t total) {
    (void)total;
    s_dl_speed_last_bytes = g_dl_bytes_done;
    s_dl_speed_ts = 0;
    s_dl_speed_kbps = 0;
}
static void dl_speed_fmt(int pct, char *out, size_t n) {
    /* 2026-10-07 窗口内缓存:小条与弹窗每步都调用本函数,若每次调用都重置
       窗口/字节基准,两个调用者互相抢窗口 → 速度恒 0(展开弹窗"速度停止"的另一半原因)。
       改为 1 秒窗口内首个调用计算并缓存,其余调用复用缓存值 */
    int64_t now = esp_timer_get_time();
    if (s_dl_speed_ts > 0 && now > s_dl_speed_ts) {
        int64_t bytes = g_dl_bytes_done - s_dl_speed_last_bytes;
        s_dl_speed_kbps = (int)(bytes * 1000000 / (now - s_dl_speed_ts) / 1024);
        if (s_dl_speed_kbps < 0) s_dl_speed_kbps = 0;
        s_dl_speed_ts = now;
        s_dl_speed_last_bytes = g_dl_bytes_done;
        /* 2026-10-07 速度入日志(用户要求:进度条不显示时可从串口判断) */
        if (s_dl_speed_kbps > 0) ESP_LOGI(TAG, "dl speed: %d%% %dKB/s", pct, s_dl_speed_kbps);
    } else if (s_dl_speed_ts == 0) {
        s_dl_speed_ts = now;   // 首帧只建窗口
    }
    /* 2026-10-06 -Werror=format-truncation 可证安全写法:
       n>=28 才拼速度(最坏 27 字节),否则退化为纯百分比。
       2026-10-07 窗口建立后无条件拼速度(可为 0KB/s):停滞可见 */
    if (s_dl_speed_ts > 0 && n >= 28) snprintf(out, n, "%d%% %dKB/s", pct, s_dl_speed_kbps);
    else snprintf(out, n, "%d%%", pct);
}
static lv_obj_t *s_pp_popup = NULL, *s_pp_title = NULL, *s_pp_bar = NULL; // 前台进度弹窗(lv_layer_top)
static lv_obj_t *s_pp_speed = NULL; // 2026-10-07 弹窗内速度标签(展开弹窗也能看速度)
static lv_obj_t *s_pp_mini = NULL, *s_pp_mini_bar = NULL, *s_pp_mini_lbl = NULL; // 顶部小条
static void pp_popup_show(void);     // 前台进度弹窗(含[后台下载][取消下载])
static void pp_mini_show(void);      // 顶部小条(点击展开回弹窗)
static void pp_hide_all(void);       // 关闭进度弹窗+小条
static void dl_lock_buttons(bool lock);   // 下载期间禁用侧边按钮列,完成自动解锁
static void index_dl_popup_show(int ai, int mode, int64_t total);   // 前向声明(确认回调引用)
static void index_check_task(void *arg);   // 有立绘无 .done:核对云端清单
static void index_update_check_task(void *arg);   // .done 完整:后台核对新增资源(更新提示)

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

/* 下载器完整下载标记(.done):有则直接按齐全处理,不再联网核对 */
static bool agent_has_done(const char *agent_path) {
    char chk[340];
    snprintf(chk, sizeof(chk), "%.255s/.done", agent_path);
    FILE *df = fopen(chk, "r");
    if (df) { fclose(df); return true; }
    return false;
}

/* 当前绑定用户的 uid:优先词表下发的 bound_user_uid(换绑后自动更新);
 * 词表未就绪(离线/首次开机)时兜底扫本地 /sdcard/_users/u<数字>。 */
static int scan_bound_user_uid(void) {
    int uid = Application::GetInstance().bound_user_uid();
    if (uid > 0) return uid;
    DIR *d = opendir("/sdcard/_users");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == 'u' && e->d_name[1] >= '0' && e->d_name[1] <= '9') {
            uid = atoi(e->d_name + 1);
            if (uid > 0) break;
        }
    }
    closedir(d);
    return uid;
}

/* 竖屏立绘(cover 目录下的 .mjpeg)是否存在 */
static bool agent_has_cover(const char *agent_path) {
    char chk[340];
    snprintf(chk, sizeof(chk), "%.255s/cover", agent_path);
    DIR *cd = opendir(chk);
    if (!cd) return false;
    struct dirent *ce;
    bool has_mjpeg = false;
    while ((ce = readdir(cd)) != NULL) {
        const char *cext = strrchr(ce->d_name, '.');
        if (cext && strcasecmp(cext, ".mjpeg") == 0) { has_mjpeg = true; break; }
    }
    closedir(cd);
    return has_mjpeg;
}

static int scan_sd_agents(void) {
    const char *base = "/sdcard/Arknights/main/operator/INDEX";
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
                snprintf(a->path, sizeof(a->path), "S:/Arknights/main/operator/INDEX/%s_108x228/%s/%s",
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
    // 弹窗下载任务取消(目录自动恢复);弹窗是索引页子对象,随页面级联删除,指针置空即可
    s_dl_cancel = true;
    s_dl_popup = s_dl_title = s_dl_sub = s_dl_bar = s_dl_yes = s_dl_yes_lbl = s_dl_no = s_dl_no_lbl = NULL;
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
        /* 2026-09-28:此处不再开检测——索引页关闭到立绘显示完成之间
           唤醒会抢流程(莱伊未克隆也曾在此窗口触发)。检测改在
           cover_display_start/cover_restore 完成("对话模式"就绪)时开。 */
        // 重新加载 cover（索引页打开时播放缓存已释放）
        if (s_agent_path[0]) cover_display_start_async(s_agent_path);
    }
}

/* 对外包装:角色下载页打开前调用(两页都在 top layer,叠加会串事件)。
 * 索引页未开时调用无害(仅移除不存在的 indev 回调)。 */
void agent_index_hide_for_app(void) {
    agent_index_hide();
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
    if (!ppa_jpg_lock()) {   // 2026-10-06 超时不等待(悬挂保护):menu→profile 曾在此永久卡死
        free(tx_buf); free(rx_buf);
        thumb_fail_log("lock timeout", idx, fs_path);
        return NULL;
    }
    esp_err_t e = jpeg_decoder_process(handle, &s_thumb_jpg_cfg, tx_buf, tx_sz, rx_buf, rx_sz, &dec);
    ppa_jpg_unlock();
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
    if (!ppa_jpg_lock()) {   // 2026-10-06 超时不等待(悬挂保护)
        ESP_LOGE(TAG, "thumb engine lock timeout");
        return NULL;
    }
    for (int retry = 0; retry < 3; retry++) {
        if (jpeg_new_decoder_engine(&eng_cfg, slot) == ESP_OK) { ppa_jpg_unlock(); return *slot; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ppa_jpg_unlock();
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
    if (s_fashion_panel) return;   // 2026-09-28 时装面板开着:手势不翻页(面板上滑动→底层索引页翻页→画面错乱卡死)
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

/* 428 全量缩略图同步(2026-09-10 统一架构,用户拍板语义):
   - 首次(INDEX 无标志文件):进罗德岛 → 阻塞式同步(loading 遮罩+进度,完成前不可操作)
   - 完成后写标志文件 .thumbs_sync_done
   - 之后只在开机时后台增量检查(补新角色缩略图);切回罗德岛不再触发任何检查/下载
   - 已存在的缩略图逐张 stat 跳过,重复同步成本仅 427 次 stat(1-2 秒) */

static bool thumbs_sync_done(void) {
    struct stat st;
    return stat(THUMBS_DONE_FLAG, &st) == 0;
}

static void thumbs_sync_task(void *arg) {
    bool blocking = (intptr_t)arg != 0;   // true=首次阻塞模式(loading 进度) false=开机后台静默
    if (!blocking) {
        // 2026-09-17:后台补缺延迟 8s 启动——开机时 427 张 stat 与索引页扫描抢
        // SD 总线,曾导致进索引页卡顿 ~20 秒
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
    std::vector<CloudRole> thumbs;
    if (role_download_thumbs(thumbs) != 0) {
        ESP_LOGW(TAG, "thumbs sync: 云端清单获取失败");
        if (blocking) { loading_hide(); s_index_building = false; s_thumbs_blocking = false; }
        s_thumbs_syncing = false;
        vTaskDelete(NULL);
        return;
    }
    int fresh = 0;
    int downloaded = 0;
    int fails = 0;
    int fail_streak = 0;
    s_thumbs_done = 0;
    s_thumbs_total = (int)thumbs.size();
    for (size_t i = 0; i < thumbs.size(); i++) {
        /* 用户操作让路:确认弹窗/角色下载期间暂停同步(网络与 SD 总线让给前台) */
        if (s_dl_locked || s_dl_popup) {
            ESP_LOGW(TAG, "thumbs sync 让路等待:dl_locked=%d popup=%d",
                     (int)s_dl_locked, s_dl_popup != NULL);
            while (s_dl_locked || s_dl_popup) vTaskDelay(pdMS_TO_TICKS(500));
        }
        const CloudRole &t = thumbs[i];
        /* 先本地判定:已存在 → 秒跳(无 delay,无网络);
           缺失 → 下载 + 150ms 让共享 SDMMC 总线喘息 */
        char dst[320];
        snprintf(dst, sizeof(dst), "/sdcard/Arknights/main/operator/INDEX/%.31s_108x228/%.15s/%.63s.jpg",
                 t.voc.c_str(), t.star.c_str(), t.name.c_str());
        struct stat st;
        bool exists = (stat(dst, &st) == 0 && st.st_size > 0);
        int tr = exists ? 0 : role_download_thumb(t.voc.c_str(), t.star.c_str(), t.name.c_str());
        s_thumbs_done = (int)(i + 1);
        if (tr == 0) { fresh++; fail_streak = 0; if (!exists) downloaded++; }
        else if (tr == 1) {
            /* 网络/SD 写失败:连续 10 张失败即中止,下次重试 */
            fails++;
            if (++fail_streak >= 10) {
                ESP_LOGE(TAG, "thumbs sync 中止:连续 %d 张失败(SD 卡写入异常?)", fail_streak);
                if (blocking) { loading_hide(); s_index_building = false; s_thumbs_blocking = false; }
                break;
            }
        } else {
            fail_streak = 0;   // -1 = 服务器无此缩略图,正常跳过
        }
        if (blocking) {
            /* 每张刷新:loading 15s 超时兜底要求刷新间隔 <15s,慢网下 25 张可能超时
               导致遮罩提前消失、空列表像"下载完啥也没有"(2026-09-12 修复) */
            char title[48];
            snprintf(title, sizeof(title), "首次同步缩略图 %d/%d", (int)(i + 1), (int)thumbs.size());
            loading_show(title);
            loading_set_stage("请稍候,完成后即可浏览全部干员",
                              (int)(i + 1) * 100 / (int)thumbs.size());
        } else if ((i + 1) % 25 == 0 || i + 1 == thumbs.size()) {
            if (downloaded > 0 || fails > 0) {
                ESP_LOGI(TAG, "thumbs sync: %d/%d(已下 %d,失败 %d)",
                         (int)(i + 1), (int)thumbs.size(), downloaded, fails);
            }
        }
        if (!exists) vTaskDelay(pdMS_TO_TICKS(150));   // 仅下载后让总线喘息;秒跳零延迟
    }
    /* 完成:写标志(失败重试机制 = 标志缺失时下次阻塞/开机任务重跑,已存在文件秒跳) */
    if (fail_streak < 10 && !thumbs.empty()) {
        FILE *f = fopen(THUMBS_DONE_FLAG, "w");
        if (f) { fprintf(f, "ok"); fclose(f); }
    }
    if (blocking) {
        loading_hide();
        s_thumbs_blocking = false;
        if (lvgl_port_lock(pdMS_TO_TICKS(3000))) {
            s_index_building = false;
            agent_index_refresh();   // 列表全量就绪,一次性刷新
            if (s_total_agents > 0) {
                /* 默认筛选"全部"不输出列表(全量缩略图压力大)→ 弹提示引导用户选筛选,
                   否则空列表像卡死 */
                lv_obj_t* tip = lv_label_create(lv_layer_top());
                lv_label_set_text(tip, "角色已就绪!\n请通过上方职业/星级\n筛选查看干员");
                lv_obj_set_style_bg_color(tip, lv_color_hex(0x222222), 0);
                lv_obj_set_style_bg_opa(tip, LV_OPA_90, 0);
                lv_obj_set_style_text_color(tip, lv_color_white(), 0);
                lv_obj_set_style_text_font(tip, s_chat_font, 0);
                lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_pad_all(tip, 16, 0);
                lv_obj_set_style_radius(tip, 12, 0);
                lv_obj_center(tip);
                lv_obj_move_foreground(tip);
                lv_obj_t* t = tip;
                lvgl_port_unlock();
                xTaskCreate([](void* p) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
                        if (lv_obj_is_valid((lv_obj_t*)p)) lv_obj_del((lv_obj_t*)p);
                        lvgl_port_unlock();
                    }
                    vTaskDelete(NULL);
                }, "sync_done_tip", 4096, t, 5, NULL);
                vTaskDelete(NULL);   // 提示任务已起,本任务直接结束
                return;
            }
            lvgl_port_unlock();
        }
    } else {
        /* 后台模式:只有真实下载过才刷新/留痕;全跳过(常态)完全静默,
           不打扰用户浏览(曾无条件 refresh 把用户正在看的页踢回第 0 页) */
        if (downloaded > 0) {
            ESP_LOGI(TAG, "thumbs sync 完成: 补缺 %d 张", downloaded);
            if (lvgl_port_lock(pdMS_TO_TICKS(3000))) {
                if (s_index_page && !s_index_building && !s_dl_locked) {
                    agent_index_refresh();
                }
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGI(TAG, "thumbs sync 完成: 全部已存在,无下载");
        }
    }
    s_thumbs_syncing = false;
    s_thumbs_done = 0;
    s_thumbs_total = 0;
    /* 恢复提示文本:曾因先 refresh(仍 syncing=true 显示"同步中")后置 false,
       hint 停留在"同步中 427/427"不消失 */
    if (lvgl_port_lock(pdMS_TO_TICKS(2000))) {
        index_hint_update();
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

/* 索引页空列表提示:同步期间显示进度,消除"列表空=坏"的困惑 */
static void index_hint_update(void) {
    if (!s_index_hint || !lv_obj_is_valid(s_index_hint)) return;
    if (s_thumbs_syncing && s_thumbs_total > 0) {
        char buf[72];
        snprintf(buf, sizeof(buf), "缩略图同步中 %d/%d\n请稍候,边下边显示",
                 s_thumbs_done, s_thumbs_total);
        lv_label_set_text(s_index_hint, buf);
    } else {
        lv_label_set_text(s_index_hint, "请选择职业或星级\n查看干员");
    }
}

void agent_index_show(void) {
    if (s_index_page) { agent_index_hide(); return; }

    bg_music_stop();   /* 2026-10-04 开机音乐(生命流)播到配网结束、进入选择角色页面的这一刻即停 */

    s_index_building = true;
    s_building_since = esp_timer_get_time();  // 构建完成前屏蔽手势（防点击/滑动重入崩溃）

    /* 2026-09-27 修正:页面框架先于 scan_sd_agents 创建——配网结束直接显示
       黑底索引页(框架+顶栏),扫描/缩略图在框架之后后台填充。
       曾:scan(数秒)在页面创建之前 → 中间黑屏一瞬(用户报"经过了其他页面") */
    lvgl_port_lock(0);
    lv_obj_t *page = lv_obj_create(lv_layer_top());
    lv_obj_set_size(page, 480, 800);
    lv_obj_set_pos(page, 0, 0);
    /* 2026-09-26 用户拍板:索引页直接纯黑不透明背景(不再叠半透明蒙版——
       开机无角色时蒙版感多余;有立绘时退出索引页仍无缝回立绘,只是不再透出) */
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    s_index_page = page;
    lvgl_port_unlock();   /* 框架已显示;scan/缩略图在锁外执行,不阻塞渲染 */

    // 触摸判定挂到 indev 级（grid 对象级也在构建时挂载，双保险）：
    // 从卡片上按下的事件可能不冒泡到 grid 对象，indev 事件全局可达
    // （构建期由 s_index_building 屏蔽，关闭页由 s_index_page 判空屏蔽）
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_add_event_cb(indev, on_index_gesture, LV_EVENT_ALL, NULL);
        ESP_LOGI(TAG, "Indev touch listener attached (indev=%p)", (void*)indev);
    } else {
        /* 启动早期触摸驱动可能尚未注册(自动回退索引页时常见):
           延迟重试一次,保证滑动翻页/手势可用 */
        ESP_LOGW(TAG, "Indev touch listener attach FAILED (no active indev) → 2s 后重试");
        xTaskCreate([](void* arg) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            lv_indev_t *in = lv_indev_active();
            if (in) {
                lv_indev_add_event_cb(in, on_index_gesture, LV_EVENT_ALL, NULL);
                ESP_LOGI(TAG, "Indev touch listener retry attached (indev=%p)", (void*)in);
            } else {
                ESP_LOGW(TAG, "Indev touch listener retry FAILED");
            }
            vTaskDelete(NULL);
        }, "indev_retry", 4096, NULL, 2, NULL);
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
    // 428 缩略图首同步(用户拍板:INDEX 无标志 → 阻塞同步,loading 进度,
    // 完成前不可操作;完成写标志。之后切回罗德岛不触发,只开机时后台增量检查)
    if (!s_thumbs_syncing && !thumbs_sync_done()) {
        s_thumbs_syncing = true;
        s_thumbs_blocking = true;   // 仅阻塞同步期间屏蔽手势(后台静默检查不屏蔽)
        s_index_building = true;
        /* 2026-09-26 去掉"首次同步缩略图…"loading 遮罩(用户拍板:WiFi 连接后直接
           是角色选择页,不再闪其它页面)——索引页自身有同步进度提示(index_hint) */
        xTaskCreate(thumbs_sync_task, "thumbs_sync", 16384, (void*)1, 2, NULL);
    }
    // 初始筛选 = 全部：不输出列表（427 干员全量缩略图压力大），提示用户选择筛选
    s_filter_prof = 0;
    s_filter_rarity = 0;
    s_filtered_count = 0;
    if (s_filtered) { heap_caps_free(s_filtered); s_filtered = NULL; }
    s_index_page_total = (s_filtered_count + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
    if (s_index_page_total < 1) s_index_page_total = 1;
    s_index_page_cur = 0;
    ESP_LOGI(TAG, "Index: %d agents, %d pages", s_total_agents, s_index_page_total);

    /* page 框架已提前创建(见函数开头,2026-09-27);此处仅补 loading 置顶 */
    lvgl_port_lock(0);
    if (s_thumbs_syncing) loading_raise();
    page = s_index_page;

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
        /* 2026-09-29 无角色可返回(未加载过任何角色)时不响应——
           否则会退回"中间笑脸+右上四按钮"的空主页且无法再返回索引页 */
        if (s_agent_path[0] == '\0') return;
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
            if (s_dl_locked) return;   // 下载中:禁止切换角色(下载完自动解锁)
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

    // ── 页码指示器（2026-10-06 左移避让右上角电量胶囊 [374,474]×[2,34]）──
    s_index_pg_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_index_pg_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_index_pg_label, s_chat_font, 0);
    lv_obj_align(s_index_pg_label, LV_ALIGN_RIGHT_MID, -120, 0);

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
        if (s_index_building) {
            // 卡死看门狗:检查任务异常退出未复位标志时,确认按钮会永久静默失效。
            // 超过 90 秒强制复位,允许再次切换(并留日志排查)
            if (esp_timer_get_time() - s_building_since > 90000000) {
                ESP_LOGW(TAG, "Confirm switch: s_index_building 卡死 90s,强制复位");
                s_index_building = false;
                loading_hide();
            } else {
                ESP_LOGW(TAG, "Confirm switch: 检查/下载进行中,忽略(%lld ms)",
                         (long long)(esp_timer_get_time() - s_building_since) / 1000);
                return;
            }
        }
        if (s_dl_locked) { ESP_LOGW(TAG, "Confirm switch: 下载锁中,忽略"); return; }
        if (ai < 0 || ai >= s_total_agents) { ESP_LOGW(TAG, "Confirm switch: 未选中角色"); return; }
        /* 统一文件系统(2026-09-10):设备端单一资源根,不分公共/用户路径。
           角色源=绑定用户仓库(云端克隆后下载),本地路径统一。 */
        char agent_path[300];
        snprintf(agent_path, sizeof(agent_path), "/sdcard/Arknights/main/operator/%s/%s/%s",
                 PROF_EN[s_agents[ai].prof], RARITY_DIR[s_agents[ai].rarity], s_agents[ai].name);
        if (agent_has_done(agent_path)) {
            // 第一档:完整(下载器 .done 标记)→ 先核对云端清单,再决定展示/弹更新。
            // 更新提示优先于跳立绘;检查期间 loading 覆盖 + 屏蔽索引页手势。
            ESP_LOGI(TAG, "Confirm switch: %s (.done) → 核对云端清单…", s_agents[ai].name);
            // 注意:agent_index_hide() 会释放 s_agents 并清零 s_total_agents,
            // 更新检查任务必须在此之前把 voc/star/name 拷走(任务参数不能引用 s_agents)
            char *upd_key = (char *)malloc(128);
            if (upd_key) {
                snprintf(upd_key, 128, "%s|%s|%s",
                         PROF_EN[s_agents[ai].prof], RARITY_DIR[s_agents[ai].rarity],
                         s_agents[ai].name);
            }
            s_index_building = true;
            s_building_since = esp_timer_get_time();   // 检查期间屏蔽索引页手势
            loading_show("检查资源中…");
            if (upd_key) {
                BaseType_t ok = xTaskCreate(index_update_check_task, "dl_upd_chk",
                                            10240, upd_key, 5, NULL);
                if (ok != pdPASS) {
                    /* 任务创建失败:退回现状行为(直接展示) */
                    ESP_LOGE(TAG, "update check: 任务创建失败 %d", (int)ok);
                    free(upd_key);
                    s_index_building = false;
                    loading_hide();
                    agent_index_hide();
                    cover_display_start_async(agent_path);
                }
            }
        } else if (agent_has_cover(agent_path)) {
            // 有立绘无 .done:异步核对云端清单
            // (齐全/手工角色/网络未知 → 直接展示;部分缺失 → 第二档弹窗)
            ESP_LOGI(TAG, "Confirm switch: %s 核对云端清单…", s_agents[ai].name);
            s_index_building = true;
            s_building_since = esp_timer_get_time();   // 检查期间屏蔽索引页手势
            loading_show("检查资源中…");
            xTaskCreate(index_check_task, "dl_check", 10240, (void *)(intptr_t)ai, 5, NULL);
        } else {
            // 第三档:连竖屏立绘都没有 → 弹"下载完整资源 / 取消选中"窗
            ESP_LOGI(TAG, "Confirm switch: %s 未缓存 → 弹下载窗", s_agents[ai].name);
            index_dl_popup_show(ai, 0, -1);
        }
    }, LV_EVENT_CLICKED, NULL);

    // ── 空列表提示（两个筛选均为"全部"时显示;缩略图同步期间显示进度）──
    s_index_hint = lv_label_create(page);
    lv_obj_set_style_text_color(s_index_hint, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_index_hint, s_chat_font, 0);
    lv_obj_set_style_text_align(s_index_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_index_hint, LV_ALIGN_CENTER, 0, 30);
    index_hint_update();
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

    // 显示第一页(首次缩略图阻塞同步进行中时保持手势屏蔽,完成后任务内放开。
    // 注意:后台静默检查(s_thumbs_blocking=false)绝不屏蔽——曾因 syncing=true
    // 不复位 building,后台检查的 2 秒窗口内所有点击被当滑动拦截)
    if (!s_thumbs_blocking) s_index_building = false;
    agent_index_show_page(0);


    lvgl_port_unlock();
}

// ─── 索引页"未缓存"下载弹窗 ───
// ─── 确认弹窗(第二档/第三档)───
static void index_dl_fetch_task(void *arg);
static void index_clone_start(void);
static void index_clone_fetch_task(void *arg);

/* 第三档检查任务:拉 manifest 拿总大小 → 更新弹窗
 * 用户仓库优先(同名角色用户版本遮蔽公共库);404/网络失败回落公共库。 */
static void index_dl_probe_task(void *arg) {
    /* 统一文件系统(2026-09-10):角色源=绑定用户仓库。
       user ≥0 → 下载模式;user==-2(未克隆)且公共库有 → 克隆模式;否则未就绪/重试 */
    int64_t user_total = -2, pub_total = -2;
    bool user_unbound = false;
    if (s_dl_user_uid > 0 && s_dl_user_rel[0]) {
        user_total = role_download_probe_user(s_dl_user_uid, s_dl_user_rel);
    } else {
        user_unbound = true;
    }
    if (user_total == -2) {
        pub_total = role_download_probe(s_dl_voc, s_dl_star, s_dl_name);
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) { vTaskDelete(NULL); return; }
    if (!s_dl_popup) { lvgl_port_unlock(); vTaskDelete(NULL); return; }  // 用户已离开索引页
    if (user_total >= 0) {
        s_dl_user_mode = true;
        int mins = role_download_estimate_minutes(user_total);
        char buf[64];
        s_dl_state = 0;   // 确认下载(用户仓库)
        lv_label_set_text(s_dl_title, "该角色未缓存");
        snprintf(buf, sizeof(buf), "预计约 %d 分钟", mins);
        lv_label_set_text(s_dl_sub, buf);
        lv_label_set_text(s_dl_yes_lbl, "下载完整资源");
        lv_obj_remove_flag(s_dl_yes, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_dl_no_lbl, "取消选中");
        lv_obj_remove_flag(s_dl_no, LV_OBJ_FLAG_HIDDEN);
    } else if (user_total == -2 && pub_total >= 0) {
        // 2026-09-30 方案B(用户拍板):设备端不再自助克隆,仅提示到网页/后台克隆
        s_dl_state = 6;   // 未克隆:仅提示(无克隆按钮)
        lv_label_set_text(s_dl_title, "该角色尚未克隆到\n您的私人仓库");
        lv_label_set_text(s_dl_sub, "请前往网站后台【仓库】\n克隆后方可下载");
        lv_obj_add_flag(s_dl_yes, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_dl_no_lbl, "知道了");
        lv_obj_remove_flag(s_dl_no, LV_OBJ_FLAG_HIDDEN);
    } else if (user_total == -2 || user_unbound) {
        s_dl_state = 1;   // 用户仓库无且公共库也无/未绑定
        if (user_unbound) {
            lv_label_set_text(s_dl_title, "设备未绑定用户,\n请先在后台绑定");
        } else {
            lv_label_set_text(s_dl_title, "资源组制作中,\n敬请期待");
        }
        lv_obj_add_flag(s_dl_yes, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_dl_no_lbl, "取消选中");
        lv_obj_remove_flag(s_dl_no, LV_OBJ_FLAG_HIDDEN);
    } else {
        s_dl_state = 2;   // 网络失败:重试
        lv_label_set_text(s_dl_title, "检查失败,请确认网络");
        lv_label_set_text(s_dl_yes_lbl, "重试");
        lv_obj_remove_flag(s_dl_yes, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_dl_no_lbl, "取消选中");
        lv_obj_remove_flag(s_dl_no, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void index_dl_reprobe(void) {
    if (!s_dl_popup) return;
    s_dl_state = 2;
    lv_label_set_text(s_dl_title, "正在检查角色…");
    lv_obj_add_flag(s_dl_yes, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dl_no, LV_OBJ_FLAG_HIDDEN);
    xTaskCreate(index_dl_probe_task, "dl_probe", 10240, NULL, 5, NULL);
}

/* ─── 统一下载进度 UI:前台弹窗(含后台/取消) + 顶部小条(后台下载,点击展开)─── */

static void dl_lock_buttons(bool lock) {
    /* 下载期间锁定页面切换:禁用侧边按钮列(菜单/设置/罗德岛/时装/交互/模式切换等),下载完自动解锁 */
    lv_obj_t* btns[] = {s_lv2_interact_btn, s_ppd_interact_btn, s_rhodes_btn, s_fashion_btn, s_kb_btn,
                        s_hide_btn, s_settings_btn, s_menu_btn, s_standee_btn,
                        s_bg_unlock_btn, s_bg_switch_btn, s_bg_play_btn,
                        s_mode_label ? lv_obj_get_parent(s_mode_label) : NULL};
    for (int i = 0; i < (int)(sizeof(btns) / sizeof(btns[0])); i++) {
        if (btns[i] && lv_obj_is_valid(btns[i])) {
            if (lock) lv_obj_add_state(btns[i], LV_STATE_DISABLED);
            else lv_obj_remove_state(btns[i], LV_STATE_DISABLED);
        }
    }
    s_dl_locked = lock;
}

static void pp_hide_all(void) {
    if (s_pp_popup) { lv_obj_del(s_pp_popup); s_pp_popup = s_pp_title = s_pp_bar = s_pp_speed = NULL; }
    if (s_pp_mini) { lv_obj_del(s_pp_mini); s_pp_mini = s_pp_mini_bar = s_pp_mini_lbl = NULL; }
}

static void pp_popup_show(void) {
    /* 前台下载进度弹窗(lv_layer_top):标题+进度条+[后台下载][取消下载] */
    pp_hide_all();
    s_pp_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_pp_popup, 320, 170);
    lv_obj_center(s_pp_popup);
    lv_obj_set_style_bg_color(s_pp_popup, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_pp_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_pp_popup, 10, 0);
    lv_obj_set_style_border_width(s_pp_popup, 0, 0);
    lv_obj_set_style_pad_all(s_pp_popup, 10, 0);

    s_pp_title = lv_label_create(s_pp_popup);
    lv_label_set_text(s_pp_title, "下载中…");
    lv_obj_set_style_text_color(s_pp_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pp_title, s_chat_font, 0);
    lv_obj_set_style_text_align(s_pp_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pp_title, LV_ALIGN_TOP_MID, 0, 12);

    s_pp_bar = lv_bar_create(s_pp_popup);
    lv_obj_set_size(s_pp_bar, 280, 12);
    lv_obj_align(s_pp_bar, LV_ALIGN_TOP_MID, 0, 56);
    lv_bar_set_value(s_pp_bar, 0, LV_ANIM_OFF);

    /* 2026-10-07 弹窗内速度标签:小条收起时弹窗也能看速度(用户:展开后速度显示"停止") */
    s_pp_speed = lv_label_create(s_pp_popup);
    lv_label_set_text(s_pp_speed, "");
    lv_obj_set_style_text_color(s_pp_speed, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(s_pp_speed, s_chat_font, 0);
    lv_obj_align(s_pp_speed, LV_ALIGN_TOP_MID, 0, 76);

    lv_obj_t* bg_btn = lv_btn_create(s_pp_popup);
    lv_obj_set_size(bg_btn, 120, 34);
    lv_obj_align(bg_btn, LV_ALIGN_BOTTOM_LEFT, 24, -12);
    lv_obj_set_style_bg_color(bg_btn, lv_color_hex(0x2b6cb0), 0);
    lv_obj_set_style_radius(bg_btn, 6, 0);
    lv_obj_set_style_border_width(bg_btn, 0, 0);
    lv_obj_t* bg_lbl = lv_label_create(bg_btn);
    lv_label_set_text(bg_lbl, "后台下载");
    lv_obj_set_style_text_color(bg_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bg_lbl, s_chat_font, 0);
    lv_obj_center(bg_lbl);
    lv_obj_add_event_cb(bg_btn, [](lv_event_t* e) {
        /* 缩成顶部小条,下载继续。2026-10-07 去掉"切立绘"逻辑:
           agent_index_hide() 会置 s_dl_cancel=true → 下载任务立即取消
           (实测:展开小条再点后台 → "Logos 取消: 1 ok / 412 skip")。
           cover 本就在展示(更新下载从索引页点角色进入),无需重载 */
        if (s_pp_popup) {
            lv_obj_del(s_pp_popup);
            s_pp_popup = s_pp_title = s_pp_bar = s_pp_speed = NULL;
        }
        pp_mini_show();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* cn_btn = lv_btn_create(s_pp_popup);
    lv_obj_set_size(cn_btn, 120, 34);
    lv_obj_align(cn_btn, LV_ALIGN_BOTTOM_RIGHT, -24, -12);
    lv_obj_set_style_bg_color(cn_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(cn_btn, 6, 0);
    lv_obj_set_style_border_width(cn_btn, 0, 0);
    lv_obj_t* cn_lbl = lv_label_create(cn_btn);
    lv_label_set_text(cn_lbl, "取消下载");
    lv_obj_set_style_text_color(cn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cn_lbl, s_chat_font, 0);
    lv_obj_center(cn_lbl);
    static int64_t s_pp_popup_ts = 0;
    s_pp_popup_ts = esp_timer_get_time();
    lv_obj_add_event_cb(cn_btn, [](lv_event_t* e) {
        /* 2026-10-07 防点击穿透:弹窗刚创建 800ms 内的点击视为穿透忽略 */
        if (esp_timer_get_time() - s_pp_popup_ts < 800000) return;
        s_dl_cancel = true;   // 下载任务回调检测后中止;收尾恢复锁
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_pp_popup);
}

static void pp_mini_show(void) {
    /* 顶部小条(后台下载):点击展开回完整弹窗 */
    if (s_pp_mini) return;
    s_pp_mini = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_pp_mini, 320, 32);
    lv_obj_align(s_pp_mini, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(s_pp_mini, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_pp_mini, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_pp_mini, 8, 0);
    lv_obj_set_style_border_width(s_pp_mini, 0, 0);
    lv_obj_set_style_pad_all(s_pp_mini, 4, 0);
    s_pp_mini_bar = lv_bar_create(s_pp_mini);
    lv_obj_set_size(s_pp_mini_bar, 216, 10);
    lv_obj_align(s_pp_mini_bar, LV_ALIGN_LEFT_MID, 6, 0);
    lv_bar_set_value(s_pp_mini_bar, 0, LV_ANIM_OFF);
    s_pp_mini_lbl = lv_label_create(s_pp_mini);
    lv_label_set_text(s_pp_mini_lbl, "0%");
    lv_obj_set_style_text_color(s_pp_mini_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pp_mini_lbl, s_chat_font, 0);
    lv_obj_align(s_pp_mini_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(s_pp_mini, [](lv_event_t* e) {
        /* 点小条 → 展开完整弹窗(2026-10-07 延迟到下一事件循环:
           点击事件可能穿透到刚弹出的"取消下载"按钮,曾展开即中断下载) */
        if (s_pp_mini) {
            lv_obj_del(s_pp_mini);
            s_pp_mini = s_pp_mini_bar = s_pp_mini_lbl = NULL;
        }
        lv_async_call([](void*) { pp_popup_show(); }, NULL);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_pp_mini);
}

static void index_dl_start(void) {
    /* 关确认弹窗 → 统一进度弹窗 + 锁定页面 → 起下载任务 */
    if (s_dl_popup) {
        lv_obj_del(s_dl_popup);
        s_dl_popup = s_dl_title = s_dl_sub = s_dl_bar = s_dl_yes = s_dl_yes_lbl = s_dl_no = s_dl_no_lbl = NULL;
    }
    s_dl_cancel = false;
    pp_popup_show();
    dl_lock_buttons(true);
    video_playback_stop();   /* 2026-10-07 下载优先:暂停封面播放(30FPS JPEG 解码
                                抢 PSRAM/CPU,实测下载仅 20KB/s;恢复在 fetch 收尾) */
    ppa_release_expendable_caches();   /* 2026-10-07 实测下载前 PSRAM 仅剩 1.4MB
                                          (largest 557KB)→ 任务/缓冲分配失败卡死;
                                          释放可牺牲缓存给下载让路 */
    xTaskCreate(index_dl_fetch_task, "dl_fetch", 16384, NULL, 5, NULL);
}

/* 弹窗(两档):
 *   mode 0 = 第三档(连竖屏立绘都没有):检查 → [下载完整资源] [取消选中] / 未就绪提示
 *   mode 1 = 第二档(有立绘,其他资源不完整):[继续展示] [下载完整资源]
 */
static void index_dl_popup_show(int ai, int mode, int64_t total) {
    if (!s_index_page || s_dl_popup) return;
    s_dl_cancel = false;
    dl_speed_reset(total);   /* 2026-10-06 速度统计窗口重置 */
    s_dl_mode = mode;
    snprintf(s_dl_voc, sizeof(s_dl_voc), "%s", PROF_EN[s_agents[ai].prof]);
    snprintf(s_dl_star, sizeof(s_dl_star), "%s", RARITY_DIR[s_agents[ai].rarity]);
    snprintf(s_dl_name, sizeof(s_dl_name), "%s", s_agents[ai].name);
    snprintf(s_dl_path, sizeof(s_dl_path), "/sdcard/Arknights/main/operator/%s/%s/%s",
             s_dl_voc, s_dl_star, s_dl_name);
    // 用户仓库候选(probe 优先探测;本地已缓存的用户角色在确认回调已直接展示,不会到这里)
    s_dl_user_uid = scan_bound_user_uid();
    s_dl_user_rel[0] = 0;
    s_dl_user_mode = false;
    if (s_dl_user_uid > 0) {
        snprintf(s_dl_user_rel, sizeof(s_dl_user_rel),
                 "Arknights/main/operator/%s/%s/%s", s_dl_voc, s_dl_star, s_dl_name);
    }

    s_dl_popup = lv_obj_create(s_index_page);
    lv_obj_set_size(s_dl_popup, 380, 280);
    lv_obj_align(s_dl_popup, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_color(s_dl_popup, lv_color_hex(0x14141c), 0);
    lv_obj_set_style_bg_opa(s_dl_popup, LV_OPA_80, 0);   // 半透明
    lv_obj_set_style_border_width(s_dl_popup, 2, 0);
    lv_obj_set_style_border_color(s_dl_popup, lv_color_hex(0x556688), 0);
    lv_obj_set_style_radius(s_dl_popup, 12, 0);
    lv_obj_set_style_pad_all(s_dl_popup, 0, 0);

    s_dl_title = lv_label_create(s_dl_popup);
    lv_label_set_text(s_dl_title, "正在检查角色…");
    lv_obj_set_style_text_color(s_dl_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_dl_title, s_chat_font, 0);
    lv_obj_set_width(s_dl_title, 360);                       /* 2026-10-05 文案居中 */
    lv_obj_set_style_text_align(s_dl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_dl_title, LV_ALIGN_TOP_MID, 0, 20);

    s_dl_sub = lv_label_create(s_dl_popup);
    lv_label_set_text(s_dl_sub, "");
    lv_obj_set_style_text_color(s_dl_sub, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(s_dl_sub, s_chat_font, 0);
    lv_obj_set_width(s_dl_sub, 360);                         /* 2026-10-05 文案居中 */
    lv_obj_set_style_text_align(s_dl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_dl_sub, LV_ALIGN_TOP_MID, 0, 84);

    s_dl_bar = lv_bar_create(s_dl_popup);
    lv_obj_set_size(s_dl_bar, 320, 12);
    lv_obj_align(s_dl_bar, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_add_flag(s_dl_bar, LV_OBJ_FLAG_HIDDEN);

    /* 按钮垂直堆叠(大按钮,小学生好点) */
    s_dl_yes = lv_btn_create(s_dl_popup);
    lv_obj_set_size(s_dl_yes, 200, 46);
    lv_obj_align(s_dl_yes, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_bg_color(s_dl_yes, lv_color_hex(0x00AA55), 0);
    lv_obj_set_style_radius(s_dl_yes, 8, 0);
    s_dl_yes_lbl = lv_label_create(s_dl_yes);
    lv_label_set_text(s_dl_yes_lbl, "下载完整资源");
    lv_obj_set_style_text_color(s_dl_yes_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_dl_yes_lbl, s_chat_font, 0);
    lv_obj_center(s_dl_yes_lbl);
    lv_obj_add_flag(s_dl_yes, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_dl_yes, [](lv_event_t *e) {
        if (s_dl_state == 3) {
            /* 第二档"继续展示":关弹窗直接进立绘 */
            if (s_dl_popup) {
                lv_obj_del(s_dl_popup);
                s_dl_popup = s_dl_title = s_dl_sub = s_dl_bar = s_dl_yes = s_dl_yes_lbl = s_dl_no = s_dl_no_lbl = NULL;
            }
            agent_index_hide();
            cover_display_start_async(s_dl_path);
        } else if (s_dl_state == 2) {
            index_dl_reprobe();   // 网络失败:重新检查
        } else if (s_dl_state == 4) {
            index_clone_start();  // 一键克隆并下载
        } else {
            index_dl_start();     // 确认下载
        }
    }, LV_EVENT_CLICKED, NULL);

    s_dl_no = lv_btn_create(s_dl_popup);
    lv_obj_set_size(s_dl_no, 200, 46);
    lv_obj_align(s_dl_no, LV_ALIGN_TOP_MID, 0, 206);
    lv_obj_set_style_bg_color(s_dl_no, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(s_dl_no, 8, 0);
    s_dl_no_lbl = lv_label_create(s_dl_no);
    lv_label_set_text(s_dl_no_lbl, "取消选中");
    lv_obj_set_style_text_color(s_dl_no_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_dl_no_lbl, s_chat_font, 0);
    lv_obj_center(s_dl_no_lbl);
    lv_obj_add_flag(s_dl_no, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_dl_no, [](lv_event_t *e) {
        if (s_dl_mode == 1 && s_dl_state == 3) {
            index_dl_start();   // 第二档"下载完整资源"
        } else {
            /* 取消选中 */
            if (s_dl_popup) {
                lv_obj_del(s_dl_popup);
                s_dl_popup = s_dl_title = s_dl_sub = s_dl_bar = s_dl_yes = s_dl_yes_lbl = s_dl_no = s_dl_no_lbl = NULL;
            }
            s_selected_ai = -1;
            for (int k = 0; k < CARDS_PER_PAGE; k++) {
                lv_obj_t *ck = s_card_objs[k];
                if (!ck) continue;
                lv_obj_set_style_border_color(ck, lv_color_hex(0x555555), 0);
                lv_obj_set_style_border_width(ck, 1, 0);
            }
            if (s_confirm_btn) lv_obj_add_flag(s_confirm_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, NULL);

    if (mode == 1) {
        /* 第二档:有立绘,其他资源不完整(check 任务已 probe,total 有效) */
        s_dl_state = 3;
        lv_label_set_text(s_dl_title, "该角色立绘可用,\n其他资源不完整");
        if (total > 0) {
            int mins = role_download_estimate_minutes(total);
            char buf[64];
            snprintf(buf, sizeof(buf), "下载完整资源预计约 %d 分钟", mins);
            lv_label_set_text(s_dl_sub, buf);
        }
        lv_label_set_text(s_dl_yes_lbl, "继续展示");
        lv_obj_remove_flag(s_dl_yes, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_dl_no_lbl, "下载完整资源");
        lv_obj_remove_flag(s_dl_no, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 第三档:无立绘 → 检查后给下载/取消 */
        s_dl_state = 2;
        lv_label_set_text(s_dl_title, "正在检查角色…");
        xTaskCreate(index_dl_probe_task, "dl_probe", 10240, NULL, 5, NULL);
    }
}

/* 有立绘无 .done:异步核对云端清单(方案 A) */
static void index_check_task(void *arg) {
    int ai = (int)(intptr_t)arg;
    if (ai < 0 || ai >= s_total_agents) { vTaskDelete(NULL); return; }
    char voc[32], star[16];
    snprintf(voc, sizeof(voc), "%s", PROF_EN[s_agents[ai].prof]);
    snprintf(star, sizeof(star), "%s", RARITY_DIR[s_agents[ai].rarity]);
    int64_t total = -1;
    /* 统一文件系统:角色清单源=绑定用户仓库(未克隆时 -2,按齐全处理直接展示旧文件) */
    int uid = scan_bound_user_uid();
    char rel[160];
    snprintf(rel, sizeof(rel), "Arknights/main/operator/%s/%s/%s", voc, star, s_agents[ai].name);
    int r = (uid > 0) ? role_download_check_user(uid, rel, &total) : -1;
    ESP_LOGI(TAG, "dl check: %s r=%d(0=部分缺失 -1=网络失败 -2=云端无包 1=齐全)",
             s_agents[ai].name, r);
    /* 弹窗/切换需要 LVGL 锁;曾单次 2 秒拿不到锁直接放弃(用户点确认后无反应)——改循环等 15s */
    bool got = false;
    for (int w = 0; w < 30 && !got; w++) {
        got = lvgl_port_lock(pdMS_TO_TICKS(500));
    }
    if (!got) {
        ESP_LOGW(TAG, "dl check: LVGL 锁超时,放弃弹窗/展示");
        s_index_building = false;   // 防索引页手势永久屏蔽
        vTaskDelete(NULL);
        return;
    }
    loading_hide();
    s_index_building = false;
    if (s_index_page) {
        if (r == 0) {
            /* 部分缺失 → 第二档弹窗 */
            ESP_LOGI(TAG, "dl check: %s 部分缺失 → 第二档", s_agents[ai].name);
            index_dl_popup_show(ai, 1, total);
        } else {
            /* 齐全/手工角色(404)/网络未知 → 有立绘就直接展示 */
            char agent_path[300];
            snprintf(agent_path, sizeof(agent_path), "/sdcard/Arknights/main/operator/%s/%s/%s",
                     voc, star, s_agents[ai].name);
            ESP_LOGI(TAG, "dl check: %s r=%d → 展示", s_agents[ai].name, r);
            agent_index_hide();
            cover_display_start_async(agent_path);
        }
    }
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

/* .done 完整角色:先核对云端清单,再决定展示/弹更新(更新提示优先于跳立绘)。
   任务参数 = "VOC|STAR|NAME" 字符串(malloc 分配,任务负责 free):
   确认回调先 agent_index_hide()(会释放 s_agents),不能再把 ai 下标传给任务。 */
static lv_obj_t* s_upd_popup = NULL;
static char s_upd_voc[32], s_upd_star[16], s_upd_name[64];   // 弹窗期间的角色键(回调引用,不经 s_agents)

static void index_update_popup_show(const char* name, const char* voc, const char* star);
static void index_update_check_task(void *arg) {
    char key[300];
    snprintf(key, sizeof(key), "%s", (const char *)arg ? (const char *)arg : "");
    free(arg);
    /* 统一文件系统(2026-09-10):key = "VOC|STAR|NAME";角色清单源=绑定用户仓库
       (云端克隆后设备下载;公共库只是克隆源)。r==-2 = 用户仓库无此角色(未克隆),
       按"齐全"处理直接展示旧文件(异常态:换绑未清等)。 */
    char voc[32], star[16], name[64];
    int64_t total = -1;
    int r;
    {
        char *p1 = strchr(key, '|');
        char *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
        if (!p1 || !p2) { ESP_LOGW(TAG, "update check: 参数非法"); vTaskDelete(NULL); return; }
        *p1 = 0; *p2 = 0;
        snprintf(voc, sizeof(voc), "%.31s", key);
        snprintf(star, sizeof(star), "%.15s", p1 + 1);
        snprintf(name, sizeof(name), "%.63s", p2 + 1);
        int uid = scan_bound_user_uid();
        char rel[160];
        snprintf(rel, sizeof(rel), "Arknights/main/operator/%s/%s/%s", voc, star, name);
        if (uid > 0) {
            r = role_download_check_user(uid, rel, &total);
            ESP_LOGI(TAG, "update check(user repo): %s r=%d(0=有更新 -1=网络失败 -2=云端无包 1=齐全)",
                     name, r);
        } else {
            r = -1;   // 未绑定用户:未知,按齐全处理直接展示
            ESP_LOGI(TAG, "update check: 未绑定用户,直接展示 %s", name);
        }
    }
    /* 弹窗/切换需要 LVGL 锁;循环等 15s(曾单次 3 秒拿不到静默放弃) */
    bool got = false;
    for (int w = 0; w < 30 && !got; w++) {
        got = lvgl_port_lock(pdMS_TO_TICKS(500));
    }
    if (!got) {
        ESP_LOGW(TAG, "update check: LVGL 锁超时,放弃弹窗/展示");
        s_index_building = false;   // 防索引页手势永久屏蔽(loading 残留由 loading_hide 幂等清理)
        vTaskDelete(NULL);
        return;
    }
    loading_hide();
    s_index_building = false;   // 检查完成,索引页恢复手势
    if (r == 0) {
        /* 有更新:弹更新提示(索引页保留),由用户选更新/后台下载/取消 */
        s_upd_total = total;   /* 2026-10-07 速度统计总量 */
        snprintf(s_dl_path, sizeof(s_dl_path), "/sdcard/Arknights/main/operator/%s/%s/%s",
                 voc, star, name);
        ESP_LOGI(TAG, "update check: %s 有更新资源 → 弹窗", name);
        index_update_popup_show(name, voc, star);
    } else {
        /* 齐全/网络失败/云端无包 → 直接展示立绘 */
        char agent_path[300];
        snprintf(agent_path, sizeof(agent_path), "/sdcard/Arknights/main/operator/%s/%s/%s",
                 voc, star, name);
        ESP_LOGI(TAG, "update check: %s r=%d → 展示", name, r);
        agent_index_hide();
        cover_display_start_async(agent_path);
    }
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void index_update_popup_show(const char* name, const char* voc, const char* star) {
    /* 更新提示弹窗(lv_layer_top,索引页保留):[更新下载][后台下载][取消] */
    if (s_upd_popup) return;   // 已有弹窗
    // 回调闭包经 s_upd_* 静态缓冲取角色键(s_agents 在确认回调里已被 agent_index_hide 释放)
    snprintf(s_upd_voc, sizeof(s_upd_voc), "%s", voc);
    snprintf(s_upd_star, sizeof(s_upd_star), "%s", star);
    snprintf(s_upd_name, sizeof(s_upd_name), "%s", name);
    snprintf(s_dl_voc, sizeof(s_dl_voc), "%s", voc);
    snprintf(s_dl_star, sizeof(s_dl_star), "%s", star);
    snprintf(s_dl_name, sizeof(s_dl_name), "%s", name);
    // s_dl_path 由 update_check_task 按模式(公共库/用户仓库)先填好,此处不覆盖
    s_upd_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_upd_popup, 340, 150);
    lv_obj_center(s_upd_popup);
    lv_obj_set_style_bg_color(s_upd_popup, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_upd_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_upd_popup, 10, 0);
    lv_obj_set_style_border_width(s_upd_popup, 0, 0);
    lv_obj_set_style_pad_all(s_upd_popup, 10, 0);

    lv_obj_t* title = lv_label_create(s_upd_popup);
    lv_label_set_text_fmt(title, "「%s」有更新资源\n是否更新?", name);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 三按钮横排:更新下载(绿) 后台下载(蓝) 取消(灰) */
    lv_obj_t* yes_btn = lv_btn_create(s_upd_popup);
    lv_obj_set_size(yes_btn, 96, 34);
    lv_obj_align(yes_btn, LV_ALIGN_BOTTOM_LEFT, 14, -14);
    lv_obj_set_style_bg_color(yes_btn, lv_color_hex(0x00AA55), 0);
    lv_obj_set_style_radius(yes_btn, 6, 0);
    lv_obj_set_style_border_width(yes_btn, 0, 0);
    lv_obj_t* yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "更新下载");
    lv_obj_set_style_text_color(yes_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yes_lbl, s_chat_font, 0);
    lv_obj_center(yes_lbl);
    lv_obj_add_event_cb(yes_btn, [](lv_event_t* e) {
        /* 前台下载:进度弹窗 + 锁定页面 + 暂停封面(下载优先) */
        if (s_upd_popup) { lv_obj_del(s_upd_popup); s_upd_popup = NULL; }
        s_dl_cancel = false;
        pp_popup_show();
        dl_lock_buttons(true);
        video_playback_stop();
        ppa_release_expendable_caches();   // 2026-10-07 PSRAM 让路(见 index_dl_start)
        xTaskCreate(index_dl_fetch_task, "dl_upd", 16384, NULL, 5, NULL);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* bg_btn = lv_btn_create(s_upd_popup);
    lv_obj_set_size(bg_btn, 96, 34);
    lv_obj_align(bg_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(bg_btn, lv_color_hex(0x2b6cb0), 0);
    lv_obj_set_style_radius(bg_btn, 6, 0);
    lv_obj_set_style_border_width(bg_btn, 0, 0);
    lv_obj_t* bg_lbl = lv_label_create(bg_btn);
    lv_label_set_text(bg_lbl, "后台下载");
    lv_obj_set_style_text_color(bg_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bg_lbl, s_chat_font, 0);
    lv_obj_center(bg_lbl);
    lv_obj_add_event_cb(bg_btn, [](lv_event_t* e) {
        /* 后台下载:顶部小条 + 锁定页面。2026-10-07 不再切立绘(重载 cover
           抢 PSRAM/CPU 拖慢下载,实测 20KB/s),并暂停封面播放(下载优先);
           下载完成收尾自动进新立绘 */
        if (s_upd_popup) { lv_obj_del(s_upd_popup); s_upd_popup = NULL; }
        pp_mini_show();
        dl_lock_buttons(true);
        s_dl_cancel = false;
        video_playback_stop();
        ppa_release_expendable_caches();   // 2026-10-07 PSRAM 让路(见 index_dl_start)
        xTaskCreate(index_dl_fetch_task, "dl_upd_bg", 16384, NULL, 5, NULL);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* no_btn = lv_btn_create(s_upd_popup);
    lv_obj_set_size(no_btn, 96, 34);
    lv_obj_align(no_btn, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    lv_obj_set_style_bg_color(no_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(no_btn, 6, 0);
    lv_obj_set_style_border_width(no_btn, 0, 0);
    lv_obj_t* no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "取消");
    lv_obj_set_style_text_color(no_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(no_lbl, s_chat_font, 0);
    lv_obj_center(no_lbl);
    lv_obj_add_event_cb(no_btn, [](lv_event_t* e) {
        /* 不更新:照常展示旧立绘 */
        if (s_upd_popup) { lv_obj_del(s_upd_popup); s_upd_popup = NULL; }
        agent_index_hide();
        cover_display_start_async(s_dl_path);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_upd_popup);
}

static void index_dl_fetch_task(void *arg) {
    dl_speed_reset(s_upd_total);   /* 2026-10-07 更新下载也要速度统计(曾恒 0 不显示) */
    auto step_cb = [](int pct, const char *file, void *ud) -> bool {
        if (s_dl_cancel) return false;
        if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return true;
        if (pct >= 0) {
            if (s_pp_bar && lv_obj_is_valid(s_pp_bar))
                lv_bar_set_value(s_pp_bar, pct, LV_ANIM_OFF);
            if (s_pp_mini_bar && lv_obj_is_valid(s_pp_mini_bar))
                lv_bar_set_value(s_pp_mini_bar, pct, LV_ANIM_OFF);
            if (s_pp_mini_lbl && lv_obj_is_valid(s_pp_mini_lbl)) {
                char pb[32];
                dl_speed_fmt(pct, pb, sizeof(pb));   /* 2026-10-06 xx% xxKB/s(32 字节满足 -Werror=format-truncation 最坏 27) */
                lv_label_set_text(s_pp_mini_lbl, pb);
            }
            if (s_pp_speed && lv_obj_is_valid(s_pp_speed)) {   /* 2026-10-07 弹窗内速度 */
                char sp[32];
                dl_speed_fmt(pct, sp, sizeof(sp));
                lv_label_set_text(s_pp_speed, sp);
            }
        }
        if (file && s_pp_title && lv_obj_is_valid(s_pp_title)) {
            lv_label_set_text_fmt(s_pp_title, "下载中…\n%.40s", file);
        }
        lvgl_port_unlock();
        return true;
    };
    int r;
    if (s_dl_user_mode) {
        /* 用户仓库下载:Arknights 类落位统一路径 /sdcard/<rel>/(OC 仍落 _users) */
        r = role_download_fetch_user(s_dl_user_uid, s_dl_user_rel, step_cb, nullptr);
        if (strncmp(s_dl_user_rel, "Arknights/", 10) == 0)
            snprintf(s_dl_path, sizeof(s_dl_path), "/sdcard/%s", s_dl_user_rel);
        else
            snprintf(s_dl_path, sizeof(s_dl_path), "/sdcard/_users/u%d/%s",
                     s_dl_user_uid, s_dl_user_rel);
    } else {
        r = role_download_fetch(s_dl_voc, s_dl_star, s_dl_name, step_cb, nullptr);
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
        if (r == 0) {
            /* 成功:解锁 + 自动进立绘(下载的新角色) */
            ESP_LOGI(TAG, "dl: %s 下载成功 → 进立绘", s_dl_name);
            pp_hide_all();
            dl_lock_buttons(false);
            agent_index_hide();                    // 索引页已关时无害
            cover_display_start_async(s_dl_path);
        } else if (r == 1) {
            /* 失败:解锁 + 提示(.part 保留,下次续传);恢复封面播放(下载期暂停) */
            ESP_LOGE(TAG, "dl: %s 失败(.part 保留可续传)", s_dl_name);
            pp_hide_all();
            dl_lock_buttons(false);
            video_playback_start(30);   // 2026-10-07 下载期暂停的封面恢复
            lv_obj_t* tip = lv_label_create(lv_layer_top());
            lv_label_set_text(tip, "下载失败,请检查网络\n(已下载部分已保留)");
            lv_obj_set_style_bg_color(tip, lv_color_hex(0x222222), 0);
            lv_obj_set_style_bg_opa(tip, LV_OPA_90, 0);
            lv_obj_set_style_text_color(tip, lv_color_white(), 0);
            lv_obj_set_style_text_font(tip, s_chat_font, 0);
            lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_all(tip, 16, 0);
            lv_obj_set_style_radius(tip, 12, 0);
            lv_obj_center(tip);
            lv_obj_move_foreground(tip);
            lv_obj_t* t = tip;
            lvgl_port_unlock();
            xTaskCreate([](void* p) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
                    if (lv_obj_is_valid((lv_obj_t*)p)) lv_obj_del((lv_obj_t*)p);
                    lvgl_port_unlock();
                }
                vTaskDelete(NULL);
            }, "dl_fail_tip", 4096, t, 5, NULL);
            vTaskDelete(NULL);
            return;
        } else {
            /* r==2 取消:解锁,留在当前页面(索引页或立绘);恢复封面播放 */
            ESP_LOGI(TAG, "dl: %s 已取消(.part 保留可续传)", s_dl_name);
            pp_hide_all();
            dl_lock_buttons(false);
            video_playback_start(30);   // 2026-10-07 下载期暂停的封面恢复
        }
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

/* ─── 一键克隆并下载(2026-09-10):云端复制公共库角色到用户仓库 → 用户仓库下载 ─── */

static void index_clone_start(void) {
    /* 关确认弹窗 → 进度弹窗 + 锁页面 → 克隆+下载任务 */
    if (s_dl_popup) {
        lv_obj_del(s_dl_popup);
        s_dl_popup = s_dl_title = s_dl_sub = s_dl_bar = s_dl_yes = s_dl_yes_lbl = s_dl_no = s_dl_no_lbl = NULL;
    }
    s_dl_cancel = false;
    s_dl_user_mode = true;
    pp_popup_show();
    dl_lock_buttons(true);
    video_playback_stop();   // 2026-10-07 下载优先(恢复在 fetch 收尾)
    ppa_release_expendable_caches();   // 2026-10-07 PSRAM 让路(见 index_dl_start)
    xTaskCreate(index_clone_fetch_task, "dl_clone", 16384, NULL, 5, NULL);
}

static void index_clone_fetch_task(void *arg) {
    auto step_cb = [](int pct, const char *file, void *ud) -> bool {
        if (s_dl_cancel) return false;
        if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return true;
        if (pct >= 0) {
            if (s_pp_bar && lv_obj_is_valid(s_pp_bar))
                lv_bar_set_value(s_pp_bar, pct, LV_ANIM_OFF);
            if (s_pp_mini_bar && lv_obj_is_valid(s_pp_mini_bar))
                lv_bar_set_value(s_pp_mini_bar, pct, LV_ANIM_OFF);
            if (s_pp_mini_lbl && lv_obj_is_valid(s_pp_mini_lbl)) {
                char pb[32];
                dl_speed_fmt(pct, pb, sizeof(pb));   /* 2026-10-06 xx% xxKB/s(32 字节满足 -Werror=format-truncation 最坏 27) */
                lv_label_set_text(s_pp_mini_lbl, pb);
            }
        }
        if (file && s_pp_title && lv_obj_is_valid(s_pp_title))
            lv_label_set_text_fmt(s_pp_title, "下载中…\n%.40s", file);
        lvgl_port_unlock();
        return true;
    };

    /* 1. 云端克隆(公共库 → 绑定用户仓库,服务器同时建 agent 行) */
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (s_pp_title && lv_obj_is_valid(s_pp_title))
            lv_label_set_text_fmt(s_pp_title, "云端克隆中…\n%s", s_dl_name);
        lvgl_port_unlock();
    }
    int cr = role_download_clone(s_dl_voc, s_dl_star, s_dl_name);
    if (cr != 0 && cr != 3) {
        if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
            pp_hide_all();
            dl_lock_buttons(false);
            lv_obj_t* tip = lv_label_create(lv_layer_top());
            if (cr == 2)
                lv_label_set_text(tip, "存储配额不足,\n请到后台清理仓库后重试");
            else if (cr == 4)
                lv_label_set_text(tip, "云端无此角色资源");
            else if (cr == 5)
                lv_label_set_text(tip, "该角色尚未验收,\n请等待管理员验收后克隆");
            else
                lv_label_set_text(tip, "克隆失败,请到后台\n公共仓库手动克隆");
            lv_obj_set_style_bg_color(tip, lv_color_hex(0x222222), 0);
            lv_obj_set_style_bg_opa(tip, LV_OPA_90, 0);
            lv_obj_set_style_text_color(tip, lv_color_white(), 0);
            lv_obj_set_style_text_font(tip, s_chat_font, 0);
            lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_all(tip, 16, 0);
            lv_obj_set_style_radius(tip, 12, 0);
            lv_obj_center(tip);
            lv_obj_move_foreground(tip);
            lvgl_port_unlock();
            xTaskCreate([](void* p) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
                    if (lv_obj_is_valid((lv_obj_t*)p)) lv_obj_del((lv_obj_t*)p);
                    lvgl_port_unlock();
                }
                vTaskDelete(NULL);
            }, "clone_fail_tip", 4096, tip, 5, NULL);
        }
        vTaskDelete(NULL);
        return;
    }

    /* 2. 用户仓库下载(克隆后 manifest 实时可见) */
    int r = role_download_fetch_user(s_dl_user_uid, s_dl_user_rel, step_cb, nullptr);
    snprintf(s_dl_path, sizeof(s_dl_path), "/sdcard/%s", s_dl_user_rel);

    /* 3. 收尾同 index_dl_fetch_task */
    if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
        if (r == 0) {
            ESP_LOGI(TAG, "clone+dl: %s 完成 → 进立绘", s_dl_name);
            pp_hide_all();
            dl_lock_buttons(false);
            agent_index_hide();
            cover_display_start_async(s_dl_path);
        } else if (r == 1) {
            ESP_LOGE(TAG, "clone+dl: %s 下载失败(.part 保留)", s_dl_name);
            pp_hide_all();
            dl_lock_buttons(false);
            video_playback_start(30);   // 2026-10-07 下载期暂停的封面恢复
            lv_obj_t* tip = lv_label_create(lv_layer_top());
            lv_label_set_text(tip, "下载失败,请检查网络\n(克隆已完成,可直接重试下载)");
            lv_obj_set_style_bg_color(tip, lv_color_hex(0x222222), 0);
            lv_obj_set_style_bg_opa(tip, LV_OPA_90, 0);
            lv_obj_set_style_text_color(tip, lv_color_white(), 0);
            lv_obj_set_style_text_font(tip, s_chat_font, 0);
            lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_all(tip, 16, 0);
            lv_obj_set_style_radius(tip, 12, 0);
            lv_obj_center(tip);
            lv_obj_move_foreground(tip);
            lvgl_port_unlock();
            xTaskCreate([](void* p) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
                    if (lv_obj_is_valid((lv_obj_t*)p)) lv_obj_del((lv_obj_t*)p);
                    lvgl_port_unlock();
                }
                vTaskDelete(NULL);
            }, "clone_dl_tip", 4096, tip, 5, NULL);
            vTaskDelete(NULL);
            return;
        } else {
            pp_hide_all();   // r==2 取消
            dl_lock_buttons(false);
            video_playback_start(30);   // 2026-10-07 下载期暂停的封面恢复
        }
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void agent_index_refresh(void) {
    // 筛选变化:关闭弹窗 + 取消选中(选中角色可能已不在新列表)
    s_dl_cancel = true;
    if (s_dl_popup) {
        lv_obj_del(s_dl_popup);
        s_dl_popup = s_dl_title = s_dl_sub = s_dl_bar = s_dl_yes = s_dl_yes_lbl = s_dl_no = s_dl_no_lbl = NULL;
    }
    s_selected_ai = -1;
    if (s_confirm_btn) lv_obj_add_flag(s_confirm_btn, LV_OBJ_FLAG_HIDDEN);
    // 1. 释放旧缩略图
    for (int i = 0; i < s_total_agents; i++) {
        if (s_agent_dsc[i]) {
            if (s_agent_dsc[i]->data) heap_caps_free((void*)s_agent_dsc[i]->data);
            heap_caps_free(s_agent_dsc[i]);
            s_agent_dsc[i] = NULL;
        }
    }
    // 1.5 重新扫描(2026-09-30 修复):缩略图同步/下载完成后文件系统已变化,
    // 不重扫则 s_total_agents 停留在旧值(空卡首次同步后为 0,筛选列表永远空)
    s_total_agents = scan_sd_agents();
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
    // 空列表提示切换(文本含同步进度,由 index_hint_update 统一维护)
    if (s_index_hint) {
        index_hint_update();
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

    // 表头(2026-09-28 动态显示绑定用户账号名;唤醒后 chat_overlay_set_identity 更新)
    lv_obj_t *hdr = lv_label_create(s_chat_user_box);
    lv_label_set_text(hdr, "用户：");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(hdr, s_chat_font, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    s_chat_user_hdr_label = hdr;

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

    // 表头(2026-09-28 动态显示干员中文名;唤醒后 chat_overlay_set_identity 更新)
    hdr = lv_label_create(s_chat_assistant_box);
    lv_label_set_text(hdr, "角色：");
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
    lv_obj_set_pos(btn, 366, 155);   /* 2026-10-02 下移:右上角让给电量胶囊 */
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
    lv_obj_set_pos(btn, 366, 195);
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

    // ── ②b 时装（竖屏 cover + cover 目录含 fashion_*.mjpeg 时显示;槽位在罗德岛下方）──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 275);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    s_fashion_lbl = lv_label_create(btn);
    lv_label_set_text(s_fashion_lbl, "时装");
    lv_obj_set_style_text_color(s_fashion_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_fashion_lbl, s_chat_font, 0);
    lv_obj_center(s_fashion_lbl);
    s_fashion_btn = btn;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);  // fashion_btn_sync 控制显隐
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        if (s_fashion_panel) { fashion_panel_hide(); return; }
        fashion_panel_show();   /* 2026-09-26 无条件显示(无时装时面板仅"默认精二"一项) */
    }, LV_EVENT_CLICKED, NULL);

    // ── ③ 对话模式/通行证模式 ──
    btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 110, 35);
    lv_obj_set_pos(btn, 366, 115);
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
                s_show_talk_hint = true;   // 进入后显示"请说你好XXX"引导提示条
                loading_show("进入对话模式");  // mode_switch_task 完成时统一隐藏
            }
        } else {
            // Expression → cover: close Live2D/PPD interaction first
            bool no_emoji_fb = s_no_emoji_fallback;   // 无 emoji 角色：stop 内部已异步回 cover，无需再 s_req_cover
            if (s_lv2_interaction) lv2_interaction_stop();
            if (s_pd_interaction) pd_interaction_stop();
            if (s_standee_mode) standee_exit(false);
            if (!no_emoji_fb) {
                s_req_cover = true;
                loading_show("返回展示模式");
            }
        }
    }, LV_EVENT_CLICKED, NULL);

    // ── ④ Live2D 交互按钮(2026-10-02 已移除:暂时用不到,接口保留;
    //    s_lv2_interact_btn 保持 NULL,各处 if 检查自动跳过)──

    // ── ④b PPD 交互（expression 模式可见；槽位 (366,205)，弹出键盘下移至 (366,245)）──
    s_ppd_interact_btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(s_ppd_interact_btn, 110, 35);
    lv_obj_set_pos(s_ppd_interact_btn, 366, 235);   /* 2026-10-02 下移(Live2D 按钮已删,PPD 下挪) */
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
    lv_obj_set_pos(btn, 366, 35);
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
    lv_obj_set_pos(btn, 366, 75);
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
    lv_obj_set_pos(btn, 366, 275);   /* 2026-10-02 键盘:与 PPD(235) 间隔 40 */
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
    lv_obj_set_pos(s_standee_btn, 366, 315);
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
        // 回复框表头:优先干员中文名(2026-09-28 唤醒时已设置);否则回退路径最后一段(如 "Amiya")
        if (s_chat_assistant_hdr) {
            if (s_chat_agent_hdr[0]) {
                lv_label_set_text(s_chat_assistant_hdr, s_chat_agent_hdr);
            } else if (s_agent_path[0]) {
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

void chat_overlay_set_identity(const char *username, const char *agent_cn) {
    /* 2026-09-28 聊天框表头:输入框=绑定用户账号名,回复框=干员中文名。
       唤醒词命中时由 application.cc 调用(UTF-8 直存直显,LVGL 中文字体)。 */
    if (username && username[0]) {
        size_t nl = strlen(username);
        if (nl > sizeof(s_chat_user_hdr) - 2) nl = sizeof(s_chat_user_hdr) - 2;
        memcpy(s_chat_user_hdr, username, nl);
        s_chat_user_hdr[nl] = ':';
        s_chat_user_hdr[nl + 1] = 0;
    }
    if (agent_cn && agent_cn[0]) {
        size_t nl = strlen(agent_cn);
        if (nl > sizeof(s_chat_agent_hdr) - 2) nl = sizeof(s_chat_agent_hdr) - 2;
        memcpy(s_chat_agent_hdr, agent_cn, nl);
        s_chat_agent_hdr[nl] = ':';
        s_chat_agent_hdr[nl + 1] = 0;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_chat_user_hdr_label && s_chat_user_hdr[0]) {
        lv_label_set_text(s_chat_user_hdr_label, s_chat_user_hdr);
    }
    if (s_chat_assistant_hdr && s_chat_agent_hdr[0]) {
        lv_label_set_text(s_chat_assistant_hdr, s_chat_agent_hdr);
    }
    lvgl_port_unlock();
}

// ── OTA 升级确认面板(2026-09-28 用户拍板:升级前询问,选择后阻塞执行) ──
static lv_obj_t *s_ota_panel = NULL;
static volatile int s_ota_choice = 0;   // 0=未选择 1=升级 -1=暂不

static void ota_panel_pick(int v) {
    s_ota_choice = v;
    lvgl_port_lock(pdMS_TO_TICKS(500));
    if (s_ota_panel) {
        lv_obj_del(s_ota_panel);
        s_ota_panel = NULL;
    }
    lvgl_port_unlock();
}

void ota_confirm_show(const char *version) {
    /* 启动阶段(索引页,PPA 未直写)调用,无保护矩形冲突 */
    if (s_ota_panel) return;
    s_ota_choice = 0;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, 480, 800);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    s_ota_panel = bg;

    lv_obj_t *title = lv_label_create(bg);
    lv_label_set_text(title, "发现新版本");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 200);

    char vbuf[96];
    snprintf(vbuf, sizeof(vbuf), "%.*s", (int)strnlen(version, 60), version);
    lv_obj_t *ver = lv_label_create(bg);
    lv_label_set_text(ver, vbuf);
    lv_obj_set_style_text_color(ver, lv_color_hex(0xFFD27F), 0);
    lv_obj_set_style_text_font(ver, s_chat_font, 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 260);

    lv_obj_t *hint = lv_label_create(bg);
    lv_label_set_text(hint, "是否升级?");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(hint, s_chat_font, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 320);

    lv_obj_t *byes = lv_btn_create(bg);
    lv_obj_set_size(byes, 180, 50);
    lv_obj_set_pos(byes, 60, 400);
    lv_obj_set_style_bg_color(byes, lv_color_hex(0x2b6cb0), 0);
    lv_obj_t *lyes = lv_label_create(byes);
    lv_label_set_text(lyes, "升级");
    lv_obj_set_style_text_color(lyes, lv_color_white(), 0);
    lv_obj_set_style_text_font(lyes, s_chat_font, 0);
    lv_obj_center(lyes);
    lv_obj_add_event_cb(byes, [](lv_event_t *e) { ota_panel_pick(1); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bno = lv_btn_create(bg);
    lv_obj_set_size(bno, 180, 50);
    lv_obj_set_pos(bno, 260, 400);
    lv_obj_set_style_bg_color(bno, lv_color_hex(0x555555), 0);
    lv_obj_t *lno = lv_label_create(bno);
    lv_label_set_text(lno, "暂不");
    lv_obj_set_style_text_color(lno, lv_color_white(), 0);
    lv_obj_set_style_text_font(lno, s_chat_font, 0);
    lv_obj_center(lno);
    lv_obj_add_event_cb(bno, [](lv_event_t *e) { ota_panel_pick(-1); }, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
}

int ota_confirm_wait(int timeout_ms) {
    /* application 线程轮询等待用户选择;超时返回 0(视为暂不,下次启动再问) */
    int waited = 0;
    while (s_ota_choice == 0 && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_ota_choice;
}

void ota_confirm_hide(void) {
    if (!s_ota_panel) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_ota_panel) {
        lv_obj_del(s_ota_panel);
        s_ota_panel = NULL;
    }
    lvgl_port_unlock();
}

// ── OTA 升级进度面板(2026-09-28 用户要求图形进度条) ──
static lv_obj_t *s_ota_prog_panel = NULL;
static lv_obj_t *s_ota_prog_bar = NULL;
static lv_obj_t *s_ota_prog_label = NULL;

void ota_progress_show(const char *version) {
    if (s_ota_prog_panel) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, 480, 800);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    s_ota_prog_panel = bg;

    lv_obj_t *title = lv_label_create(bg);
    lv_label_set_text(title, "正在升级固件...");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 200);

    char vbuf[96];
    snprintf(vbuf, sizeof(vbuf), "版本 %.*s", (int)strnlen(version, 60), version);
    lv_obj_t *ver = lv_label_create(bg);
    lv_label_set_text(ver, vbuf);
    lv_obj_set_style_text_color(ver, lv_color_hex(0xFFD27F), 0);
    lv_obj_set_style_text_font(ver, s_chat_font, 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 260);

    lv_obj_t *bar = lv_bar_create(bg);
    lv_obj_set_size(bar, 360, 22);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 330);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    s_ota_prog_bar = bar;

    lv_obj_t *pct = lv_label_create(bg);
    lv_label_set_text(pct, "0%");
    lv_obj_set_style_text_color(pct, lv_color_white(), 0);
    lv_obj_set_style_text_font(pct, s_chat_font, 0);
    lv_obj_align(pct, LV_ALIGN_TOP_MID, 0, 370);
    s_ota_prog_label = pct;
    lvgl_port_unlock();
}

void ota_progress_update(int percent) {
    if (!s_ota_prog_panel || !s_ota_prog_bar) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    lv_bar_set_value(s_ota_prog_bar, percent, LV_ANIM_OFF);
    if (s_ota_prog_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        lv_label_set_text(s_ota_prog_label, buf);
    }
    lvgl_port_unlock();
}

void ota_progress_hide(void) {
    if (!s_ota_prog_panel) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (s_ota_prog_panel) {
        lv_obj_del(s_ota_prog_panel);
        s_ota_prog_panel = NULL;
        s_ota_prog_bar = NULL;
        s_ota_prog_label = NULL;
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

// Live2D 交互支持的角色白名单（现有 lv2 模型仅 Theresia/Amiya；
// 无 emoji 角色的降级链也用此判断）
static bool lv2_agent_supported(const char* agent) {
    return strstr(agent, "Theresia") || strstr(agent, "theresia") ||
           strstr(agent, "Amiya") || strstr(agent, "amiya");
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
static volatile bool s_pd_task_exited = false;   // pd_anim_task 退出确认(vTaskDelete 前置位;stop 轮询防竞态)
static uint16_t* s_pd_fb[3] = {NULL, NULL, NULL};  // 480×800 RGB565：fb[0/1]=渲染双缓冲（防撕裂）
                                                    // fb[2]=干净底图（PPA 每帧拷入，canvas 指向它——
                                                    // LVGL 重绘 UI 矩形时读干净帧再叠半透明，不会自叠加）
static int s_pd_fb_idx = 0;
static char s_pd_dirs[64][160];           // 角色目录完整路径(统一根 /sdcard/Arknights/main/operator/...,2026-09-10)
                                           // 上限 64:曾限 8,VANGUARD 等靠后职业凑满 8 个后永远扫不到
static int s_pd_count = 0, s_pd_index = 0;
// s_pd_interaction_bg 声明上移至文件头部声明区（profile 动图播放需前向引用）
static volatile bool s_pd_throttle = false;   /* 2026-10-03 volatile:强制绝对地址读,绕开编译器 s11 相对寻址 bug(崩溃 lbu -552(s11) */            // true=交互模式 5fps 节流（默认）；false=测试页快节奏（无 AFE 压力）

// ═══════ PPA 直写上屏管线（PPD 交互专用）═══════
// 绕过 LVGL 全屏重绘（partial 模式 ~100ms/帧是 4fps 的元凶）：渲染帧经 PPA SRM（1:1 硬件拷贝）
// DMA 直写面板 fb，按钮列/画质按钮矩形跳过（保留 fb 上 LVGL 画的按钮像素，按钮不闪不遮）。
// LVGL 只做按钮状态重绘：canvas 缓冲直接指向面板 fb，按钮矩形重绘时底图自洽（同址自拷）。
// CPU 0 每帧零参与（PPA 是硬件 DMA），帧率 = 渲染 58ms + 2×msync ~4ms ≈ 16fps。
static volatile bool s_pd_direct = false;     /* 2026-10-03 volatile 同上 */              // PPA 直写模式激活（panel fb 可用 + PPA 注册成功）
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
        /* 2026-10-02 右上角按钮列整体下移(Live2D 按钮已删),保护矩形同步 */
        {374, 0,   480, 34},    // 电量胶囊(2026-10-06 补全:胶囊实际 [374,474]×[2,34],
                                // 曾只护 x≥396 → 左 22px/下 2px 每帧被直写覆盖 → 左/下边缘闪烁)
        {366, 35,  476, 70},    // 设置
        {366, 75,  476, 110},   // 菜单
        {366, 115, 476, 150},   // 模式
        {366, 155, 476, 190},   // 隐藏
        {366, 195, 476, 230},   // 罗德岛(原 Live2D 槽)
        {366, 235, 476, 270},   // PPD 交互
        {366, 275, 476, 310},   // 时装/弹出键盘
        {366, 315, 476, 350},   // 立牌
        {366, 355, 466, 387},   // 形态切换(竖屏 PPD 交互)
        {0,   505, 480, 800},   // 聊天区(2026-10-03 扩到全宽:对话框两侧到屏幕
                               // 边缘的条带此前不在保护区/失效区,残留旧背景)
    };
    // 横屏 Q 版互动：4 按钮旋转排布（pos 190+i*40,10 + pivot(0,0) + 顺时针 90°）
    // → 屏幕 x∈[pos-35,pos], y∈[10,120]；动作按钮（i=4：x∈[315,350]）与动作列表面板
    // （旋转后顶部横条 x∈[0,480] y∈[10,160]）同样保护。
    // 横屏聊天框侧边栏（旋转后 x∈[350,480] y∈[360,780]）在下方动态追加（仅可见时保护，
    // 隐藏时不保护——否则直写跳过的区域纸偶画面冻结）
    static const protect_rect_t PROTECT_LANDSCAPE[] = {
        {448, 700, 480, 800},  // 电量胶囊(横屏 rot 900,视觉右上角=逻辑右下角,2026-10-06 用户拍板)
        {155, 10, 190, 120},   // 设置
        {195, 10, 230, 120},   // 菜单
        {235, 10, 270, 120},   // 模式
        {275, 10, 310, 120},   // 横屏立牌
        {315, 10, 350, 120},   // 显示/隐藏聊天框
        {355, 10, 390, 120},   // 动作按钮
        {395, 10, 430, 120},   // 形态切换(横屏 Q 版,顶部横条第 7 位)
        {0,   10, 480, 160},   // 动作列表面板（展开时顶部横条）
    };
    protect_rect_t rects[14];
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
        // 背景系统按钮(竖屏 PPD 交互,2026-09-11 左上竖排):解锁背景常驻;
        // 切换/播放仅解锁后可见时保护(隐藏时不保护,否则该区域纸偶画面冻结)
        if (!s_pdq_mode && s_bg_unlock_btn && !lv_obj_has_flag(s_bg_unlock_btn, LV_OBJ_FLAG_HIDDEN))
            rects[nr++] = {6, 6, 116, 38};      // 解锁背景 110×32
        if (!s_pdq_mode && s_bg_unlocked && s_bg_switch_btn &&
            !lv_obj_has_flag(s_bg_switch_btn, LV_OBJ_FLAG_HIDDEN))
            rects[nr++] = {6, 42, 116, 74};     // 切换背景 110×32
        if (!s_pdq_mode && s_bg_unlocked && s_bg_play_btn &&
            !lv_obj_has_flag(s_bg_play_btn, LV_OBJ_FLAG_HIDDEN))
            rects[nr++] = {6, 78, 116, 110};    // 播放·停止 110×32
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
    int ys[28]; int ny = 0;   // 2 + 2×N(N≤12) = 26 上限
    ys[ny++] = 0; ys[ny++] = 800;
    for (int i = 0; i < N; i++) { ys[ny++] = PROTECT[i].y0; ys[ny++] = PROTECT[i].y1; }
    for (int i = 0; i < ny; i++)
        for (int j = i + 1; j < ny; j++)
            if (ys[j] < ys[i]) { int t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
    int uniq[28]; int nu = 0;
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
   /sdcard/Arknights/main/operator/<职业>/<星级>/<干员>/PPD/{scene.json,*.raw}
   职业白名单 7 个（INDEX/PRTS/REINSTALL 是系统目录，不扫）；每职业 1STAR..6STAR */
static const char* s_pd_professions[] = {"CASTER","GUARD","MEDIC","SNIPER","SPECIALIST","SUPPORTER","VANGUARD","REINSTALL"};
static const char* s_pd_rarities[] = {"1STAR","2STAR","3STAR","4STAR","5STAR","6STAR"};

// 扫描 <职业/星级目录>/<干员>/<sub>/scene.json 存在的角色（sub = "PPD" 竖屏 / "PPD_Q" 横屏 Q 版）
static void pd_scan_prof_dir(const char* prof_path, const char* sub) {
    DIR* d = opendir(prof_path);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && s_pd_count < 64) {
        if (e->d_name[0] == '.') continue;
        size_t pp = strlen(prof_path), nl = strlen(e->d_name);
        if (pp + 1 + nl >= sizeof(s_pd_dirs[0])) continue;
        char full[sizeof(s_pd_dirs[0])];
        memcpy(full, prof_path, pp);
        full[pp] = '/';
        memcpy(full + pp + 1, e->d_name, nl + 1);
        char scene[sizeof(s_pd_dirs[0]) + 20];
        snprintf(scene, sizeof(scene), "%s/%s/scene.json", full, sub);
        struct stat st;
        if (stat(scene, &st) != 0) continue;
        memcpy(s_pd_dirs[s_pd_count], full, pp + 1 + nl + 1);
        s_pd_count++;
    }
    closedir(d);
}

// 扫描 PPD 角色:统一根 /sdcard/Arknights/main/operator(8 职业全扫,2026-09-10)
static void pd_scan_chars_sub(const char* sub) {
    /* 统一文件系统(2026-09-10):单树扫描 /sdcard/Arknights/main/operator,
       8 职业全扫(REINSTALL 角色也支持,如塞雷娅)。 */
    s_pd_count = 0;
    for (int p = 0; p < 8 && s_pd_count < 64; p++) {
        for (int r = 0; r < 6 && s_pd_count < 64; r++) {
            char prof_path[80];
            snprintf(prof_path, sizeof(prof_path), "/sdcard/Arknights/main/operator/%s/%s",
                     s_pd_professions[p], s_pd_rarities[r]);
            pd_scan_prof_dir(prof_path, sub);
        }
    }
    ESP_LOGI("PD", "scan: %d chars under /sdcard/Arknights/main/operator/%s", s_pd_count, sub);
}

static void pd_scan_chars(void) { pd_scan_chars_sub("PPD"); }

/* 2026-10-01 拍照临时卸载/恢复立绘:PD 层纹理(RGBA,十几 MB)占满 PSRAM,
   摄像头 1.87MB 连续缓冲无法分配 → 拍照前卸载、拍完恢复(屏幕短暂空白) */
static bool pd_ci_strstr(const char* hay, const char* needle);   /* 前向声明(定义在 6667) */
static void bg_attach_to_model(void);   /* 前向声明(定义在 6009,拍照恢复背景用) */
void pd_unload_for_camera(void) {
    if (!s_pd_mutex) return;   // cover 模式 PD 未启动
    xSemaphoreTake(s_pd_mutex, portMAX_DELAY);
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    xSemaphoreGive(s_pd_mutex);
    ESP_LOGI("PD", "unloaded for camera");
}
void pd_restore_after_camera(void) {
    if (!s_pd_mutex) return;   // cover 模式 PD 未启动
    // 2026-10-02 pd_load 栈需求大(PPD 解析+纹理),不能在调用线程
    // (tool_call 6KB pthread 栈)直接跑 → Stack protection fault;
    // 丢独立大栈 FreeRTOS 任务异步恢复(持锁加载,渲染冻结到完成,语音不受影响)
    BaseType_t ret = xTaskCreate([](void* arg) {
        xSemaphoreTake(s_pd_mutex, portMAX_DELAY);
        if (!s_pd_model && s_pd_count > 0) {
            // 2026-10-02 恢复必须按当前角色定位:s_pd_index 是纸偶测试页
            // "切换角色"按钮的残留索引,直接用它会在测试页切过角色后
            // 拍照恢复成别的角色(Mon3tr 之谜)。找不到当前角色的 PPD 则不恢复。
            int idx = -1;
            if (s_agent_path[0]) {
                for (int i = 0; i < s_pd_count; i++) {
                    if (pd_ci_strstr(s_agent_path, s_pd_dirs[i])) { idx = i; break; }
                }
            }
            if (idx >= 0) {
                s_pd_index = idx;
                /* 2026-10-04 背景解码必须在 pd_load 之前:模型加载后 PSRAM
                   仅剩 ~140KB,JPEG tx 缓冲(~1-2MB)重分配失败 → 静默 -1
                   → 拍照后背景回默认(澄空)的根因。 */
                bool bg_ok = false;
                if (s_bg_cur >= 0 && s_bg_cur < s_bg_count) {
                    char bgpath[160];
                    snprintf(bgpath, sizeof(bgpath),
                             "/sdcard/Arknights/main/background/%.*s.jpg",
                             (int)sizeof(s_bg_names[0]) - 1, s_bg_names[s_bg_cur]);
                    int w = ppa_long_bg_decode(bgpath, 0);
                    if (w > 0) {
                        s_bg_slot_active = 0;
                        s_bg_off_x = (w - 480) / 2;
                        if (s_bg_off_x < 0) s_bg_off_x = 0;
                        bg_ok = true;
                    } else {
                        ESP_LOGW("PD", "bg restore decode failed (%d): cur=%d count=%d",
                                 w, (int)s_bg_cur, (int)s_bg_count);
                    }
                }
                char path[sizeof(s_pd_dirs[0]) + 8];
                snprintf(path, sizeof(path), "%s/PPD", s_pd_dirs[idx]);
                s_pd_model = pd_load(path, s_pdq_mode ? 1 : 0);
                // 2026-10-03 拍照前后背景必须不变:拍照前对话模式 PPA 显示
                // background.jpg;恢复后确保 PPA 背景仍在(blend 路径曾丢背景)
                if (s_pd_model && !ppa_has_background() && !s_pdq_mode) {
                    ppa_load_background("/sdcard/Arknights/main/background/background.jpg");
                }
                // 2026-10-02 恢复拍照前用户选的背景(挂载+PPA 缓冲同步需模型已加载)
                if (s_pd_model && bg_ok) {
                    bg_attach_to_model();
                    /* 2026-10-04 与 bg_switch 对齐:同步 PPA 混合背景缓冲
                       (对话模式的 blend 底图),否则显示仍是默认 background.jpg */
                    const uint8_t* bgbuf = ppa_long_bg_buffer(0);
                    if (bgbuf) {
                        ppa_background_set_from_rgb565(bgbuf, ppa_long_bg_width(0),
                                                       ppa_long_bg_stride(0), s_bg_off_x);
                    }
                    ESP_LOGI("PD", "bg restored after camera: %s", s_bg_names[s_bg_cur]);
                }
            } else {
                ESP_LOGW("PD", "restore after camera: 当前角色无 PPD 资源,不恢复");
            }
        }
        xSemaphoreGive(s_pd_mutex);
        ESP_LOGI("PD", "restored after camera %p", (void*)s_pd_model);
        ESP_LOGI("PD", "pd_restore stack hw: %u / 32768", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelete(NULL);
    }, "pd_restore", 32768, NULL, 2, NULL);
    if (ret != pdPASS) {
        ESP_LOGE("PD", "pd_restore task create failed, PD stays unloaded");
    }
}

static void pd_load_current(void) {
    ESP_LOGI("PD", "switch: waiting mutex...");
    xSemaphoreTake(s_pd_mutex, portMAX_DELAY);   // 渲染/触摸暂停（切角色加载期间画面冻结在上一帧）
    ESP_LOGI("PD", "switch: mutex taken, loading %s", s_pd_dirs[s_pd_index]);
    loading_show("切换角色");
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    char path[sizeof(s_pd_dirs[0]) + 8];
    snprintf(path, sizeof(path), "%s/PPD", s_pd_dirs[s_pd_index]);
    s_pd_model = pd_load(path, s_pdq_mode ? 1 : 0);   /* 2026-09-26 mesh_only:省层纹理+anims 开销(黑键154层减载爆内存) */   // 同步加载（分块读 + 让步）
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

// ══════════════════════════════════════════════════
// 背景系统（PRTS 横屏长图竖屏切片：解锁→滑动/切换；未解锁触摸=视线/摸头）
// 云端:公共库 Arknights/main/background(public 三件套,全局一份,2026-09-10 统一架构)
// 渲染:pd_model.long_bg 偏移渲染(draw_band fill_bg 最高优先级分支)
// ══════════════════════════════════════════════════

static void bg_panel_hide(void);
static void bg_check_task(void *arg);
static void bg_fetch_task(void *arg);
static void bg_switch_task(void *arg);
static void bg_update_popup_show(int64_t total);

// 扫描本地已下载背景(/sdcard/Arknights/main/background/*.jpg,排除旧 background*)
static void bg_scan_list(void) {
    s_bg_count = 0;
    const char *dir = "/sdcard/Arknights/main/background";
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_bg_count < 40) {
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || strcasecmp(ext, ".jpg") != 0) continue;
        if (strncasecmp(e->d_name, "background", 10) == 0) continue;   // 旧 background.jpg/p 排除
        int stem = (int)(ext - e->d_name);
        if (stem <= 0 || stem >= (int)sizeof(s_bg_names[0])) continue;
        memcpy(s_bg_names[s_bg_count], e->d_name, stem);
        s_bg_names[s_bg_count][stem] = '\0';
        s_bg_count++;
    }
    closedir(d);
    // 排序保证"第一张"稳定(readdir 顺序不定)
    qsort(s_bg_names, s_bg_count, sizeof(s_bg_names[0]),
          [](const void *a, const void *b) { return strcmp((const char*)a, (const char*)b); });
    ESP_LOGI(TAG, "bg scan: %d 张背景", s_bg_count);
}

// 挂当前活跃槽到模型(锁内调用;形态切换 pd_load 重载后同样调用重挂)
static void bg_attach_to_model(void) {
    if (!s_pd_model || s_bg_slot_active < 0) return;
    s_pd_model->long_bg = (uint16_t*)ppa_long_bg_buffer(s_bg_slot_active);
    s_pd_model->long_bg_w = ppa_long_bg_width(s_bg_slot_active);
    s_pd_model->long_bg_h = ppa_long_bg_height(s_bg_slot_active);
    s_pd_model->long_bg_stride = ppa_long_bg_stride(s_bg_slot_active);
    s_pd_model->long_bg_off_x = s_bg_off_x;
}

/* ── 背景音乐(2026-09-11):播放当前背景对应的场景 BGM(同名 wav);对话/唤醒时停止 ── */
void bg_music_stop(void) {
    /* 全局导出:application.cc 唤醒词回调调用(PPD 对话模式下音乐与对话不共存) */
    s_music_cancel = true;
    bool was = s_bg_music_playing || s_music_task;
    s_bg_music_playing = false;
    if (!was) return;   // 本来就没在播,跳过 UI 更新(唤醒词高频路径零开销)
    if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
        if (s_bg_play_lbl && lv_obj_is_valid(s_bg_play_lbl))
            lv_label_set_text(s_bg_play_lbl, "播放音乐");
        lvgl_port_unlock();
    }
}

static void bg_music_toggle(void) {
    if (s_music_task) {   // 播放中(含音乐页/背景乐)→ 停止;播完自动结束则重播
        bg_music_stop();
        return;
    }
    if (s_bg_cur < 0 || s_bg_cur >= s_bg_count) { ui_toast("请先选择背景"); return; }
    /* 从本地清单查当前场景的实际音乐文件名(轮替场景按设备时间选 day/night,2026-09-11) */
    char bg_short[80];
    snprintf(bg_short, sizeof(bg_short), "%s", s_bg_names[s_bg_cur]);
    /* "场景_" 是 7 个字节(场3+景3+_1):曾误用 6 只剥掉"场景"两字,
       剩 "_假日" 与清单的"假日"匹配失败 → "该场景暂无对应音乐"(2026-09-13 修) */
    if (strncmp(bg_short, "场景_", 7) == 0) memmove(bg_short, bg_short + 7, strlen(bg_short + 7) + 1);
    std::string track_file;
    {
        FILE *f = fopen("/sdcard/Arknights/main/music/music_manifest.json", "rb");
        if (!f) { ui_toast("音乐清单不存在,请先同步背景"); return; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char*)malloc(sz + 1);
        if (!buf) { fclose(f); ui_toast("内存不足"); return; }
        if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); ui_toast("清单读取失败"); return; }
        buf[sz] = 0;
        fclose(f);
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) { ui_toast("清单解析失败"); return; }
        cJSON *tracks = cJSON_GetObjectItem(root, "tracks");
        if (tracks && cJSON_IsArray(tracks)) {
            time_t tnow = time(NULL);
            struct tm tmv;
            localtime_r(&tnow, &tmv);
            const char *want_time = (tmv.tm_hour >= 6 && tmv.tm_hour < 18) ? "day" : "night";
            int n = cJSON_GetArraySize(tracks);
            for (int i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(tracks, i);
                cJSON *bj = it ? cJSON_GetObjectItem(it, "bg") : NULL;
                if (!bj || !cJSON_IsString(bj) || strcmp(bj->valuestring, bg_short) != 0) continue;
                cJSON *tj = cJSON_GetObjectItem(it, "time");
                const char *tt = (tj && cJSON_IsString(tj)) ? tj->valuestring : "";
                cJSON *fj = cJSON_GetObjectItem(it, "file");
                if (!fj || !cJSON_IsString(fj)) continue;
                if (track_file.empty()) track_file = fj->valuestring;   // 兜底第一条
                if (tt[0] == '\0' || strcmp(tt, want_time) == 0) {
                    track_file = fj->valuestring;
                    break;
                }
            }
        }
        cJSON_Delete(root);
    }
    if (track_file.empty()) { ui_toast("该场景暂无对应音乐"); return; }
    char path[220];
    snprintf(path, sizeof(path), "/sdcard/Arknights/main/music/%s", track_file.c_str());
    char *p = strdup(path);
    if (!p) { ui_toast("内存不足"); return; }
    if (xTaskCreate(music_play_task, "bg_music", 8192, p, 3, &s_music_task) != pdPASS) {
        free(p);
        s_music_task = NULL;
        ui_toast("音乐播放启动失败");
        return;
    }
    s_bg_music_playing = true;
    if (s_bg_play_lbl && lv_obj_is_valid(s_bg_play_lbl))
        lv_label_set_text(s_bg_play_lbl, "停止音乐");
    ESP_LOGI(TAG, "bg music play: %s", path);
}

// 重绘全部 UI 保护区(两大块:底部对话框区 + 右侧按钮列区):
// panel 上这些矩形的像素只在 LVGL flush 时从 fb[2](canvas 底图)自拷更新,
// 背景切换/滑动后必须主动 invalidate,否则透过半透明 UI 看到旧背景。
// 用区域级 lv_inv_area 而非对象 invalidate——已隐藏按钮的残留位置(时装/键盘槽位等)
// 也必须刷新,对象级 invalidate 覆盖不到 HIDDEN 对象。任意区域重绘都会 blit
// 全屏 canvas 的 fb[2](已同步新帧)。
// 调用方须已持 LVGL 锁或在 LVGL 回调上下文。
static void bg_invalidate_ui(void) {
    /* 与 pd_ppa_present 的保护区精确一致的矩形(+立牌/切换背景槽位):
       仅 invalidate 保护区——flush 与 PPA 直拷写 panel 不同区,零并发撕裂。
       重绘时 canvas blit fb[2](已同步新帧)+ 可见 UI 重画;隐藏对象不画,
       残留区(时装/键盘/立牌槽位)直接变回新背景。
       2026-09-17:按 s_pdq_mode 选列表——曾只 invalidate 竖屏矩形,
       横屏 Q 版顶部按钮条不重绘 → 人物动作入区后残影冻结 */
    static const lv_area_t rects_portrait[] = {
        /* 2026-10-03 与 pd_ppa_present 的 PROTECT_PORTRAIT 精确同步(曾偏移 30px:
           2026-10-02 按钮列下移只改了跳过列表,失效列表还是旧坐标 → 按钮周围
           留边区永不重绘,切背景/滑动后残留旧背景) */
        {374, 0,   480, 34},    // 电量胶囊(与 PROTECT_PORTRAIT 同步,2026-10-06 补全)
        {366, 35,  476, 70},    // 设置
        {366, 75,  476, 110},   // 菜单
        {366, 115, 476, 150},   // 模式
        {366, 155, 476, 190},   // 隐藏
        {366, 195, 476, 230},   // 罗德岛(原 Live2D 槽)
        {366, 235, 476, 270},   // PPD 交互
        {366, 275, 476, 310},   // 键盘/时装
        {366, 315, 476, 350},   // 立牌(横屏残留槽位)
        {366, 355, 466, 387},   // 形态切换
        {6, 6, 116, 38},        // 解锁背景(左上竖排,2026-09-11)
        {6, 42, 116, 74},       // 切换背景
        {6, 78, 116, 110},      // 播放·停止
        {0,   505, 480, 800},   // 聊天区(与 PROTECT_PORTRAIT 同步全宽)
    };
    static const lv_area_t rects_landscape[] = {
        {448, 700, 480, 800},  // 电量胶囊(与 PROTECT_LANDSCAPE 同步,2026-10-06)
        {155, 10, 190, 120},   // 设置
        {195, 10, 230, 120},   // 菜单
        {235, 10, 270, 120},   // 模式
        {275, 10, 310, 120},   // 横屏立牌
        {315, 10, 350, 120},   // 显示/隐藏聊天框
        {355, 10, 390, 120},   // 动作按钮
        {395, 10, 430, 120},   // 形态切换(横屏 Q 版)
        {435, 10, 467, 120},   // 时装(横屏 Q 版第 8 位,2026-09-26)
        {0,   10, 480, 160},   // 动作列表面板(顶部横条)
    };
    const lv_area_t *rects = s_pdq_mode ? rects_landscape : rects_portrait;
    int n = s_pdq_mode ? (int)(sizeof(rects_landscape) / sizeof(rects_landscape[0]))
                       : (int)(sizeof(rects_portrait) / sizeof(rects_portrait[0]));
    /* 宿主=全屏 bg(位于 (0,0),区域坐标=绝对坐标) */
    lv_obj_t *host = s_pd_interaction_bg;
    if (!host || !lv_obj_is_valid(host)) return;
    for (int i = 0; i < n; i++) lv_obj_invalidate_area(host, &rects[i]);
}

// 3 秒 toast(需已持 LVGL 锁)
static void bg_toast_locked(const char *msg) {
    lv_obj_t* tip = lv_label_create(lv_layer_top());
    lv_label_set_text(tip, msg);
    lv_obj_set_style_bg_color(tip, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_90, 0);
    lv_obj_set_style_text_color(tip, lv_color_white(), 0);
    lv_obj_set_style_text_font(tip, s_chat_font, 0);
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(tip, 16, 0);
    lv_obj_set_style_radius(tip, 12, 0);
    lv_obj_center(tip);
    lv_obj_move_foreground(tip);
    xTaskCreate([](void* p) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
            if (lv_obj_is_valid((lv_obj_t*)p)) lv_obj_del((lv_obj_t*)p);
            lvgl_port_unlock();
        }
        vTaskDelete(NULL);
    }, "bg_tip", 4096, tip, 5, NULL);
}

static void ui_toast(const char *msg) {
    if (lvgl_port_lock(pdMS_TO_TICKS(2000))) { bg_toast_locked(msg); lvgl_port_unlock(); }
}

// 解锁生效(下载完成/检查齐全后调用;LVGL 锁内):挂第一张(如未挂)+ 按钮切换
static void bg_unlock_finish(void) {
    if (!s_pd_interaction) return;
    if (s_bg_count == 0) bg_scan_list();
    s_bg_unlocked = true;
    if (s_bg_unlock_lbl && lv_obj_is_valid(s_bg_unlock_lbl)) lv_label_set_text(s_bg_unlock_lbl, "锁定背景");
    if (s_bg_switch_btn && lv_obj_is_valid(s_bg_switch_btn))
        lv_obj_remove_flag(s_bg_switch_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_bg_play_btn && lv_obj_is_valid(s_bg_play_btn))
        lv_obj_remove_flag(s_bg_play_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_bg_cur < 0 && s_bg_count > 0) {
        /* 进入时无背景文件(刚下载完):后台解码第一张挂载 */
        s_bg_pending_idx = 0;
        s_bg_task_running = true;
        BaseType_t mret = xTaskCreate(bg_switch_task, "bg_mount", 16384, NULL, 5, NULL);   /* 2026-10-03 48K 回退 16K:0x6e 是 s11 被踩非栈溢出;48K 超堆最大连续块(46K)分配必败致切换无响应 */
        if (mret != pdPASS) {   /* 2026-10-03 失败必须可见 */
            ESP_LOGE(TAG, "bg_mount create FAILED (free %u, maxblk %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            s_bg_task_running = false;
        }
    }
}

// 检查中弹窗(角色检查同款:小弹窗"正在检查…",不用全屏 loading 遮罩)
static void bg_check_popup_show(void) {
    if (s_bg_check_popup) return;
    s_bg_check_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_bg_check_popup, 340, 130);
    lv_obj_center(s_bg_check_popup);
    lv_obj_set_style_bg_color(s_bg_check_popup, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_bg_check_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_bg_check_popup, 10, 0);
    lv_obj_set_style_border_width(s_bg_check_popup, 0, 0);
    lv_obj_set_style_pad_all(s_bg_check_popup, 10, 0);
    lv_obj_t* title = lv_label_create(s_bg_check_popup);
    lv_label_set_text(title, "正在检查背景…");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(title);
    lv_obj_move_foreground(s_bg_check_popup);
}

static void bg_check_popup_hide_core(void) {
    /* 无锁版:调用方已持 LVGL 锁时用(bg_check_task 收尾;lvgl 互斥锁非递归,嵌套会死锁) */
    if (!s_bg_check_popup) return;
    lv_obj_del(s_bg_check_popup);
    s_bg_check_popup = NULL;
}

static void bg_check_popup_hide(void) {
    if (!s_bg_check_popup) return;
    lvgl_port_lock(0);
    bg_check_popup_hide_core();
    lvgl_port_unlock();
}

// 解锁/锁定按钮回调
static void bg_unlock_click(lv_event_t *e) {
    if (s_bg_unlocked) {
        /* 锁定:触摸恢复视线/摸头;停音乐并隐藏切换/播放按钮 */
        s_bg_unlocked = false;
        s_bg_last_x = -1;
        bg_music_stop();
        bg_panel_hide();
        if (s_bg_unlock_lbl) lv_label_set_text(s_bg_unlock_lbl, "解锁背景");
        if (s_bg_switch_btn) lv_obj_add_flag(s_bg_switch_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_bg_play_btn) lv_obj_add_flag(s_bg_play_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_bg_checking || s_bg_task_running) return;
    s_bg_checking = true;
    bg_check_popup_show();
    /* 2026-10-05 创建失败必须恢复状态:此前 32768 栈在碎片堆分配失败时
       任务不存在 → s_bg_checking 永远 true → 卡死在"正在检查背景…" */
    BaseType_t cret = xTaskCreate(bg_check_task, "bg_check", 32768, NULL, 5, NULL);
    if (cret != pdPASS) {
        ESP_LOGE(TAG, "bg_check create FAILED (free %u, maxblk %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        /* 降级:16K 栈再试一次(HTTP 检查路径实测浅调用) */
        cret = xTaskCreate(bg_check_task, "bg_check", 16384, NULL, 5, NULL);
        if (cret != pdPASS) {
            s_bg_checking = false;
            bg_check_popup_hide();
        }
    }
}

// 解锁时阻塞检查云端背景+音乐(公共库;四档:1齐全/0缺失/-2云端无/-1网络失败)
static void bg_check_task(void *arg) {
    int64_t total = 0;
    int r = role_download_check_public("background", &total);
    s_bg_missing_music = role_download_check_music_missing();   // ≥0 缺失首数 / -1 网络失败
    if (!s_pd_interaction) {   // 检查期间已退出互动:丢弃
        bg_check_popup_hide();
        s_bg_checking = false;
        vTaskDelete(NULL);
        return;
    }
    if (r == 1 && s_bg_missing_music <= 0) {
        s_bg_checking = false;
        if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
            bg_check_popup_hide_core();   // 已持锁:用无锁核心
            bg_unlock_finish();
            lvgl_port_unlock();
        }
    } else if (r == 0 || s_bg_missing_music > 0) {
        s_bg_checking = false;
        if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
            bg_check_popup_hide_core();
            bg_update_popup_show(total);
            lvgl_port_unlock();
        }
    } else if (r == -2 && s_bg_missing_music <= 0) {
        bg_check_popup_hide();
        s_bg_checking = false;
        ui_toast("云端暂无背景资源");
    } else {
        bg_check_popup_hide();
        s_bg_checking = false;
        ui_toast("网络连接失败,请重试");
    }
    ESP_LOGI(TAG, "bg_check stack hw: %u / 32768", (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* 2026-10-03 诊断 */
    vTaskDelete(NULL);
}

// 新背景三键弹窗([更新下载]绿 [后台下载]蓝 [取消]灰;与角色更新弹窗同款)
static void bg_update_popup_show(int64_t total) {
    if (s_bg_upd_popup) return;
    dl_speed_reset(total);   /* 2026-10-06 速度统计窗口重置 */
    s_bg_upd_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_bg_upd_popup, 340, 150);
    lv_obj_center(s_bg_upd_popup);
    lv_obj_set_style_bg_color(s_bg_upd_popup, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_bg_upd_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_bg_upd_popup, 10, 0);
    lv_obj_set_style_border_width(s_bg_upd_popup, 0, 0);
    lv_obj_set_style_pad_all(s_bg_upd_popup, 10, 0);

    (void)total;
    lv_obj_t* title = lv_label_create(s_bg_upd_popup);
    if (s_bg_missing_music > 0)
        lv_label_set_text_fmt(title, "发现新背景/音乐资源\n(音乐缺失 %d 首)\n是否更新?", s_bg_missing_music);
    else
        lv_label_set_text(title, "发现新背景资源\n是否更新?");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* yes_btn = lv_btn_create(s_bg_upd_popup);
    lv_obj_set_size(yes_btn, 96, 34);
    lv_obj_align(yes_btn, LV_ALIGN_BOTTOM_LEFT, 14, -14);
    lv_obj_set_style_bg_color(yes_btn, lv_color_hex(0x00AA55), 0);
    lv_obj_set_style_radius(yes_btn, 6, 0);
    lv_obj_set_style_border_width(yes_btn, 0, 0);
    lv_obj_t* yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "更新下载");
    lv_obj_set_style_text_color(yes_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yes_lbl, s_chat_font, 0);
    lv_obj_center(yes_lbl);
    lv_obj_add_event_cb(yes_btn, [](lv_event_t* e) {
        /* 前台下载:进度弹窗 + 锁定页面 */
        if (s_bg_upd_popup) { lv_obj_del(s_bg_upd_popup); s_bg_upd_popup = NULL; }
        s_dl_cancel = false;
        pp_popup_show();
        dl_lock_buttons(true);
        s_bg_task_running = true;
        xTaskCreate(bg_fetch_task, "bg_fetch", 32768, NULL, 5, NULL);   /* 2026-10-03 16K→32K:下载回调链+弹窗深调用,自删任务金丝雀盲区 */
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* bg_btn = lv_btn_create(s_bg_upd_popup);
    lv_obj_set_size(bg_btn, 96, 34);
    lv_obj_align(bg_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(bg_btn, lv_color_hex(0x2b6cb0), 0);
    lv_obj_set_style_radius(bg_btn, 6, 0);
    lv_obj_set_style_border_width(bg_btn, 0, 0);
    lv_obj_t* bg_lbl = lv_label_create(bg_btn);
    lv_label_set_text(bg_lbl, "后台下载");
    lv_obj_set_style_text_color(bg_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bg_lbl, s_chat_font, 0);
    lv_obj_center(bg_lbl);
    lv_obj_add_event_cb(bg_btn, [](lv_event_t* e) {
        /* 后台下载:顶部小条 + 锁定页面 */
        if (s_bg_task_running) {   /* 2026-10-04 重入保护:下载已在跑,再点会起第二个任务
                                      互相踩(判定中断);已在下载时只提示,继续等待原任务 */
            if (s_bg_upd_popup) { lv_obj_del(s_bg_upd_popup); s_bg_upd_popup = NULL; }
            ui_toast("下载已在进行中,请等待完成");
            return;
        }
        if (s_bg_upd_popup) { lv_obj_del(s_bg_upd_popup); s_bg_upd_popup = NULL; }
        pp_mini_show();
        dl_lock_buttons(true);
        s_dl_cancel = false;
        s_bg_task_running = true;
        xTaskCreate(bg_fetch_task, "bg_fetch_bg", 32768, NULL, 5, NULL);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* no_btn = lv_btn_create(s_bg_upd_popup);
    lv_obj_set_size(no_btn, 96, 34);
    lv_obj_align(no_btn, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    lv_obj_set_style_bg_color(no_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(no_btn, 6, 0);
    lv_obj_set_style_border_width(no_btn, 0, 0);
    lv_obj_t* no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "取消");
    lv_obj_set_style_text_color(no_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(no_lbl, s_chat_font, 0);
    lv_obj_center(no_lbl);
    lv_obj_add_event_cb(no_btn, [](lv_event_t* e) {
        /* 取消:不下载,保持锁定 */
        if (s_bg_upd_popup) { lv_obj_del(s_bg_upd_popup); s_bg_upd_popup = NULL; }
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_bg_upd_popup);
}

// 下载背景(public 三件套;进度走 pp_popup/pp_mini;完成→重扫+解锁)
static void bg_fetch_task(void *arg) {
    auto step_cb = [](int pct, const char *file, void *ud) -> bool {
        if (s_dl_cancel) return false;
        if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return true;
        if (pct >= 0) {
            if (s_pp_bar && lv_obj_is_valid(s_pp_bar)) lv_bar_set_value(s_pp_bar, pct, LV_ANIM_OFF);
            if (s_pp_mini_bar && lv_obj_is_valid(s_pp_mini_bar)) lv_bar_set_value(s_pp_mini_bar, pct, LV_ANIM_OFF);
            if (s_pp_mini_lbl && lv_obj_is_valid(s_pp_mini_lbl)) {
                char pb[32];
                dl_speed_fmt(pct, pb, sizeof(pb));   /* 2026-10-06 xx% xxKB/s(32 字节满足 -Werror=format-truncation 最坏 27) */
                lv_label_set_text(s_pp_mini_lbl, pb);
            }
            if (s_pp_speed && lv_obj_is_valid(s_pp_speed)) {   /* 2026-10-07 弹窗内速度 */
                char sp[32];
                dl_speed_fmt(pct, sp, sizeof(sp));
                lv_label_set_text(s_pp_speed, sp);
            }
        }
        if (file && s_pp_title && lv_obj_is_valid(s_pp_title))
            lv_label_set_text_fmt(s_pp_title, "下载中…\n%.40s", file);   // 角色下载同款标题
        lvgl_port_unlock();
        return true;
    };
    int r = role_download_fetch_public("background", step_cb, nullptr);
    /* 背景下载后继续下载缺失音乐。注意 fetch_public 内部会重建共享清单 s_files
       (parse_manifest),把背景文件名覆盖成背景清单——必须重新检查音乐缺失,
       否则会把背景文件当音乐下载(2026-09-13 修:404 风暴根因) */
    if (r == 0 && s_bg_missing_music > 0) {
        s_bg_missing_music = role_download_check_music_missing();
        if (s_bg_missing_music > 0) {
            int mr = role_download_fetch_music_missing(step_cb, nullptr);
            if (mr != 0) ESP_LOGW(TAG, "bg 音乐下载不完整: %d", mr);
        }
    }
    if (lvgl_port_lock(pdMS_TO_TICKS(5000))) {
        if (!s_pd_interaction) {   // 下载期间退出互动:仅清理弹窗
            pp_hide_all();
            dl_lock_buttons(false);
        } else if (r == 0) {
            pp_hide_all();
            dl_lock_buttons(false);
            // 2026-10-03 下载完成的重扫移入独立任务(直接重扫与 PD 渲染并发
            // 触发 Load access fault;bg.raw padding 已去,此路径本应极少再触发)
            xTaskCreate([](void*) {
                vTaskDelay(pdMS_TO_TICKS(300));
                bg_scan_list();
                if (s_pd_interaction) bg_unlock_finish();
                vTaskDelete(NULL);
            }, "bg_post", 8192, NULL, 5, NULL);
        } else if (r == 1) {
            pp_hide_all();
            dl_lock_buttons(false);
            bg_toast_locked("下载失败,请检查网络\n(已下载部分已保留)");
        } else {
            pp_hide_all();   // r==2 取消
            dl_lock_buttons(false);
        }
        lvgl_port_unlock();
    }
    s_bg_task_running = false;
    vTaskDelete(NULL);
}

// 切换/首挂背景:解码到非活跃槽(渲染继续用活跃槽)→ 持锁换指针+居中
static void bg_switch_task(void *arg) {
    int idx = s_bg_pending_idx;
    if (idx < 0 || idx >= s_bg_count) { s_bg_task_running = false; vTaskDelete(NULL); return; }
    char path[160];   // 前缀43+stem79+".jpg"+NUL 上限≈127,缓冲足够 GCC 可证不越界
    snprintf(path, sizeof(path), "/sdcard/Arknights/main/background/%.*s.jpg",
             (int)sizeof(s_bg_names[0]) - 1, s_bg_names[idx]);
    int slot = (s_bg_slot_active == 0) ? 1 : 0;
    int w = ppa_long_bg_decode(path, slot);
    if (w > 0 && s_pd_interaction && s_pd_mutex &&
        xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
        s_bg_slot_active = slot;
        s_bg_off_x = (w - 480) / 2;
        if (s_bg_off_x < 0) s_bg_off_x = 0;
        s_bg_cur = idx;
        bg_attach_to_model();
        // 2026-10-03 选背景全局持久化:用户选的背景对对话/拍照/恢复全部生效。
        // ① PPA 背景缓冲同步(对话模式 BLEND 底图)② 写 bg.raw(PD 模型背景,
        // 下次开机/拍照恢复也读它)→ 所有显示路径背景一致,不再"切回默认"
        {
            const uint8_t* bgbuf = ppa_long_bg_buffer(slot);
            if (bgbuf) {
                int bw = ppa_long_bg_width(slot);
                int bstride = ppa_long_bg_stride(slot);
                int off = s_bg_off_x;
                ppa_background_set_from_rgb565(bgbuf, bw, bstride, off);
                /* 2026-10-03 移除 bg.raw 文件写:SDMMC DMA 写 768KB 疑破坏 .bss
                   (fb_idx 被写坏)。背景一致性由 PPA 缓冲同步 + 下次启动
                   PPA 加载时同步写(同样移除后改小缓冲?)——先纯内存方案 */
            }
        }
        /* 同步渲染一帧新背景到 fb[2](canvas 底图源):invalidate 后 LVGL 会立刻
           blit fb[2] 画 UI——若等 pd_anim 下一帧 present 才更新 fb[2],竞态窗口内
           LVGL 读到旧帧 → 对话框/按钮区显示旧背景("不透明旧照片"根因)。
           freeze(切换面板打开)期间 present 暂停,fb[2] 更不会更新,必须主动渲染 */
        if (s_pd_fb[2]) {
            pd_render(s_pd_model, s_pd_fb[2], 480, 800,
                      s_pd_model->last_ms);   /* 2026-09-27 相对时间基准:曾传绝对时间 → last_ms 被污染,
                                                 pd_set_anim 的 t0=绝对,循环 t_ms=相对 → t_rel 恒 0 → 动画卡第 0 帧 */
            esp_cache_msync((void*)s_pd_fb[2], 480 * 800 * 2,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        xSemaphoreGive(s_pd_mutex);
        ESP_LOGI(TAG, "bg switch → %s (%dpx 可见, off %d)", s_bg_names[idx], w, s_bg_off_x);
        /* 切换背景 → 停旧曲(重新点播放切新曲,避免串场) */
        bg_music_stop();
        /* 重绘 UI 保护区:flush 从 fb[2](已同步新帧)自拷 */
        if (lvgl_port_lock(pdMS_TO_TICKS(2000))) {
            bg_invalidate_ui();
            lvgl_port_unlock();
        }
    } else if (w <= 0 && s_pd_interaction) {
        ui_toast("背景加载失败");
    }
    s_bg_task_running = false;
    ESP_LOGI(TAG, "bg_switch stack hw: %u / 16384", (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* 2026-10-03 诊断 */
    vTaskDelete(NULL);
}

// 切换面板列表项点击(普通函数指针 + user_data 传下标)
static void bg_panel_item_click(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bg_panel_hide();
    if (idx < 0 || idx >= s_bg_count || idx == s_bg_cur) return;
    s_bg_pending_idx = idx;
    s_bg_task_running = true;
    BaseType_t ret = xTaskCreate(bg_switch_task, "bg_switch", 16384, NULL, 5, NULL);   /* 2026-10-03 48K 回退 16K:同 bg_mount;原值从未栈溢出 */   /* 2026-10-03 栈 8K->16K+钉核0 */
    if (ret != pdPASS) {   /* 2026-10-03 失败必须可见:曾 48K 静默失败致切换无响应 20 秒 */
        ESP_LOGE(TAG, "bg_switch create FAILED (free %u, maxblk %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_bg_task_running = false;
        ui_toast("内存不足,请重启设备");
    }
}

// 切换背景面板(时装面板同款:全屏半透明底+标题+滚动列表)
static void bg_panel_show(void) {
    if (s_bg_panel) return;
    lvgl_port_lock(0);
    lv_obj_t *bgp = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bgp, 480, 800);
    lv_obj_set_pos(bgp, 0, 0);
    lv_obj_set_style_bg_color(bgp, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bgp, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bgp, 0, 0);
    lv_obj_clear_flag(bgp, LV_OBJ_FLAG_SCROLLABLE);
    s_bg_panel = bgp;
    lv_obj_add_event_cb(bgp, [](lv_event_t *e) {
        bg_panel_hide();   // 点空白关闭(列表项点击冒泡到 bgp 时已 hide,幂等)
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *title = lv_label_create(bgp);
    lv_label_set_text(title, "切换背景");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, s_chat_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_t *cont = lv_obj_create(bgp);
    lv_obj_set_size(cont, 420, 640);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
    for (int i = 0; i < s_bg_count; i++) {
        lv_obj_t *b = lv_btn_create(cont);
        lv_obj_set_size(b, 340, 44);
        lv_obj_set_pos(b, 40, i * 52);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2a2f3a), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, s_bg_names[i]);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, s_chat_font, 0);
        lv_obj_center(l);
        if (i == s_bg_cur) lv_obj_set_style_bg_color(b, lv_color_hex(0x886644), 0);
        lv_obj_add_event_cb(b, bg_panel_item_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    lvgl_port_unlock();
}

static void bg_panel_hide(void) {
    if (!s_bg_panel) return;
    lvgl_port_lock(0);
    lv_obj_del(s_bg_panel);
    s_bg_panel = NULL;
    lvgl_port_unlock();
}

/* 2026-10-03 s11 之谜破解:不是"写者"!GCC section-anchor 优化把 .bss 锚基址
   缓存在 s11,而插桩的 asm volatile 读 s11 未声明 clobber → GCC 在检查点附近
   自由复用 s11 存临时值(0x6e=110 之类循环中间量),旧构建里 s_pd_direct 的
   锚访问(-544(s11))的锚加载在罕见分支,主路径用的是陈旧临时值 → 崩。
   修复:本文件禁 -fno-section-anchors(main/CMakeLists.txt)+ 删除全部插桩。
   证据链:反汇编见 mv s11,a1 / sub s11,s5,s11 / lui s11,0x4ff46(仅罕见分支);
   新构建同值 0x6e 不崩(锚访问换了寄存器),"写者跟着栈走"= 每循环同段算术。 */

/* 2026-10-03 二轮破解(禁用锚优化后仍崩):反汇编证实编译器代码正确
   (lui s11 支配全部访问),三个崩溃 dump 统计:s4/s5/s10 锚从不坏、唯独 s11
   每次被踩成 0x6e——s11 被某个罕见路径当草稿寄存器用(CLIC 嵌套中断出口
   rtos_int_exit 的 mv s11,a0 是头号嫌疑)。不再追汇编,改用零成本免疫:
   屏障声明 s11 死亡,GCC 在每次基址访问前重新 lui——加载与使用只隔一两条
   指令,踩不到;语义无损(纯寄存器分配约束)。 */
#define PD_S11_SAFE() asm volatile("" ::: "s11")

static void pd_anim_task(void*) {
    int64_t start_us = esp_timer_get_time();
    int fps_frame = 0; int64_t fps_last = 0;
    while (s_pd_running) {
        PD_S11_SAFE();
        uint32_t t_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        uint16_t* cur = NULL;
        if (s_pd_fb_idx < 0 || s_pd_fb_idx >= 3 || !s_pd_fb[s_pd_fb_idx]) {
            /* 2026-10-03 诊断:.bss 被 DMA/越界写破坏时打印现场(曾 s_pd_fb_idx
               被写坏 → 读数组外 Load fault);修复为钳制,避免崩溃 */
            ESP_LOGE("PD", "fb_idx corrupted: %d (fb=%p,%p,%p)",
                     (int)s_pd_fb_idx, (void*)s_pd_fb[0], (void*)s_pd_fb[1], (void*)s_pd_fb[2]);
            s_pd_fb_idx = 0;
            cur = s_pd_fb[0];
        } else {
            cur = s_pd_fb[s_pd_fb_idx];
        }
        int64_t t0 = esp_timer_get_time();
        if (s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            PD_S11_SAFE();
            if (s_pd_model && cur) pd_render(s_pd_model, cur, 480, 800, t_ms);
            PD_S11_SAFE();
            xSemaphoreGive(s_pd_mutex);
        }
        PD_S11_SAFE();
        int64_t t1 = esp_timer_get_time();
        fps_frame++;
        if (fps_last == 0) fps_last = t1;
        if (t1 - fps_last > 5000000) {
            ESP_LOGI("PD", "FPS: %.1f (render: %d ms) stack hw %u",
                     fps_frame * 1000000.0f / (t1 - fps_last), (int)((t1 - t0) / 1000),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* 2026-10-03 诊断:验证 16K 栈余量 */
            fps_frame = 0; fps_last = t1;
        }
        if (s_pd_direct) {
            /* 面板打开（设置/菜单/键盘/语音/音乐）或全屏应用抽屉（拼豆/蟑螂/Live2D测试，
               s_pd_suspend）时暂停直写：大面积 UI 的 LVGL 重绘/滚动与 PPA 每帧拷贝并发
               会互相覆盖（闪烁看不清）。期间画面冻结在最后一帧（渲染继续推进动画参数），
               关闭后恢复 */
            /* 背景系统弹窗/加载遮罩出现时同样暂停直写：它们在 lv_layer_top，不在
               pd_ppa_present 的硬编码保护区列表 → 不暂停会被每帧 PPA 直拷冲掉
               （"弹窗闪现又消失"根因）。弹窗删除后恢复直写。
               bg_ui 与 ui_open 分离：背景弹窗频繁开关，若走 ui_open 的恢复序列
               （video_playback_stop+150ms）会闪屏并破坏保护区按钮像素（黑矩形根因）——
               bg_ui 只暂停 present，恢复时直接画下一帧渲染结果，零附加动作。 */
            bool ui_open = settings_ui_is_open() || menu_ui_is_open() || s_kb_overlay || s_voice_overlay ||
                           s_music_overlay || s_pd_suspend;
            bool bg_ui = s_bg_panel || s_bg_upd_popup || s_bg_check_popup ||
                         s_pp_popup || s_pp_mini || loading_is_active() ||
                         s_pd_skin_panel;   /* 2026-09-27 时装面板:暂停直写防人物图层冲掉面板 */
            bool freeze = ui_open || bg_ui;
            static bool freeze_was = false;   /* 2026-10-03 诊断:冻结切换打点,定位卡死条件 */
            if (freeze != freeze_was) {
                ESP_LOGI(TAG, "PD freeze %d->%d (ui=%d: set=%d menu=%d kb=%d vc=%d music=%d susp=%d | bg=%d: panel=%d upd=%d chk=%d pp=%d mini=%d load=%d skin=%d)",
                         (int)freeze_was, (int)freeze, (int)ui_open,
                         (int)settings_ui_is_open(), (int)menu_ui_is_open(),
                         (int)(s_kb_overlay != NULL), (int)(s_voice_overlay != NULL),
                         (int)(s_music_overlay != NULL), (int)s_pd_suspend, (int)bg_ui,
                         (int)(s_bg_panel != NULL), (int)(s_bg_upd_popup != NULL),
                         (int)(s_bg_check_popup != NULL), (int)(s_pp_popup != NULL),
                         (int)(s_pp_mini != NULL), (int)loading_is_active(),
                         (int)(s_pd_skin_panel != NULL));
                freeze_was = freeze;
            }
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
            if (!freeze) {
                /* 2026-10-04 模型缺失(拍照恢复失败)时空转优化:跳过无意义的
                   直写呈现与逐帧失效,循环退化为 5ms 节拍,零 CPU 负载。
                   曾 28.5fps 空转 + 每帧 PPA 直写 + 胶囊失效,推高 CPU/LVGL 负载
                   (拍照后 WDT 饿死 IDLE 的帮凶之一)。 */
                if (s_pd_model) {
                /* PPA 直写上屏：等上一帧 DMA 完成 → msync（CPU 写 s_pd_fb 对 PPA 可见 /
                   PPA 写 fb 对扫描 DMA 可见）→ 行带扫描硬件拷贝（跳过 UI 保护矩形）。
                   CPU 0 零参与；上帧 DMA 与下帧渲染重叠（双缓冲异区），帧率 ~16fps。
                   撕裂防护：渲染写 idx 与 DMA 读 idx^1 异区；写回同区要等 2 帧周期 */
                pd_ppa_wait_idle();
                esp_cache_msync((void*)s_pd_panel_fb, 480 * 800 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                esp_cache_msync((void*)cur, 480 * 800 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                pd_ppa_present(cur, s_pd_panel_fb);
                /* 同步干净底图 fb[2](canvas buffer):侧边按钮栏/时钟等 UI 矩形的 LVGL
                   重绘 flush 会从 fb[2] 自拷覆盖面板 fb——不更新则按钮栏区域画面冻结在
                   首帧(新约能天使的枪在按钮栏附近被"切断"的根因:注释曾声称每帧拷入,
                   实现漏了)。768KB memcpy ~1-2ms,帧率影响可忽略。 */
                if (s_pd_fb[2] && s_pd_fb[2] != cur) {
                    /* 2026-10-06 加锁:bg_switch_task 持锁 pd_render(fb[2]) 与本循环
                       memcpy 并发写同一缓冲(此前 memcpy 不持锁),数据竞争 →
                       背景切换瞬间胶囊/UI 区读到撕裂混合帧(闪烁)。互斥后 fb[2]
                       内容始终完整,胶囊逐帧失效看到的面板与底图一致 */
                    if (s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
                        memcpy(s_pd_fb[2], cur, 480 * 800 * 2);
                        esp_cache_msync((void*)s_pd_fb[2], 480 * 800 * 2,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                        xSemaphoreGive(s_pd_mutex);
                    }
                }
                /* 2026-09-13 修"上动下静"割裂:UI 保护区(半透明对话框/按钮列)的像素
                   只在 LVGL flush 时从 fb[2] 自拷——静止时无 invalidate 就不重绘,
                   被对话框遮挡的人物下半部分帧冻结(滑动时 bg_invalidate_ui 每帧
                   刷新所以正常)。每帧 invalidate 保护区,遮挡区跟上动画;
                   滑动路径已证明该负载可承受(480×800 双拷 ~1-2ms)。
                   2026-09-26:改时间基节流(300ms 一次)——曾按"每 3 帧"节流,但
                   mesh 渲染帧率 21fps(层渲染 9fps)使 UI 刷新频率 7 次/秒,
                   taskLVGL 重绘大面板+按钮图标过载 → IDLE0 饿死 watchdog。
                   300ms 固定节拍与帧率解耦,视觉无感,负载恒定 */
                /* 2026-10-03 电量胶囊逐帧失效:保护区像素只在 flush 时从 fb[2]
                   自拷,300ms 节流让胶囊背景与滑动背景不同步(闪烁)。
                   84×32 小区域重绘代价可忽略。
                   必须持 LVGL 锁:跨核无锁 invalidate 会破坏失效链表
                   (lv_inv_area 死循环 → IDLE1 饿死 WDT)。 */
                if (s_batt_box && lvgl_port_lock(pdMS_TO_TICKS(4))) {
                    if (s_batt_box && lv_obj_is_valid(s_batt_box)) {
                        if ((s_pdq_mode || s_standee_mode) != s_batt_land) batt_apply_orientation();   /* 2026-10-06 横屏跟随(含立牌) */
                        lv_obj_invalidate(s_batt_box);
                    }
                    lvgl_port_unlock();
                }
                static int64_t s_inv_last = 0;
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_inv_last >= 300000) {
                    s_inv_last = now_us;
                    if (lvgl_port_lock(pdMS_TO_TICKS(4))) {
                        bg_invalidate_ui();
                        lvgl_port_unlock();
                    }
                }
                }   /* if (s_pd_model) 结束:模型缺失时以上全部跳过 */
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
        PD_S11_SAFE();
        s_pd_fb_idx ^= 1;   // 下一帧写另一个缓冲，本帧源不被改写
        /* 帧率控制：直写模式不限帧（渲染 ~56ms 自定节奏 ~17fps）；
           老路：交互节流 5fps / 测试页快节奏。
           直写模式下渲染占满 CPU 1，vTaskDelay(5) 给 IDLE1 留 5ms/帧 喂 watchdog
           （1ms 窗口被 audio_detection 等就绪任务抢走 → IDLE1 5s 无运行 → task_wdt 崩溃） */
        int64_t t2 = esp_timer_get_time();
        PD_S11_SAFE();
        int64_t frame_period_us = s_pd_direct ? 0 : (s_pd_throttle ? 200000 : 56000);
        if (s_pd_direct) {
            vTaskDelay(5);
        } else if (t2 - t1 < frame_period_us) {
            vTaskDelay((frame_period_us - (t2 - t1)) / 1000);
        }
        PD_S11_SAFE();
    }
    s_pd_task_exited = true;   // 退出确认:stop 侧轮询此标志后才释放 mutex/fb/model
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

/* ── 多形态(forms.json):角色目录 PPD/PPD_Q 下的 forms/<形态>/ 子场景 ── */
static char s_pd_form_names[8][32];   // 形态名(如 背面/基建)
static char s_pd_form_dirs[8][64];    // 形态相对目录(如 forms/背面)
static int s_pd_form_count = 0;
static int s_pd_form_idx = 0;         // 0=主形态(正面)
static char s_pd_base_dir[160];       // 当前角色目录完整路径(公共库或用户仓库)
static lv_obj_t* s_pd_form_lbl = NULL;   // s_pd_form_btn 声明已前移(背景系统 bg_invalidate_ui 引用)

static int pd_scan_forms(const char* base_path) {
    /* 读 base_path/forms.json → 填 s_pd_form_names/dirs,返回形态数 */
    s_pd_form_count = 0;
    char fp[192];   // 完整路径(≤160) + "/forms.json"(12) 上限内
    if (strlen(base_path) + 12 >= sizeof(fp)) return 0;
    snprintf(fp, sizeof(fp), "%s/forms.json", base_path);
    FILE* f = fopen(fp, "r");
    if (!f) {
        ESP_LOGI("PD", "forms.json 不存在: %s", fp);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(f); return 0; }
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return 0;
    int n = 0;
    cJSON* forms = cJSON_GetObjectItem(root, "forms");
    cJSON* it = forms ? forms->child : NULL;
    for (; it && n < 8; it = it->next) {
        cJSON* nm = cJSON_GetObjectItem(it, "name");
        cJSON* dr = cJSON_GetObjectItem(it, "dir");
        if (!nm || !dr || !cJSON_IsString(nm) || !cJSON_IsString(dr)) continue;
        /* 2026-09-26 形态目录存在性检查:转换中断残迹(如黑键 forms.json 声明
           背面/基建但目录未生成)点击切换必失败——无 scene.json 的形态不显示按钮 */
        {
            char chk[256];
            snprintf(chk, sizeof(chk), "%s/%s/scene.json", base_path, dr->valuestring);
            if (access(chk, F_OK) != 0) {
                ESP_LOGW(TAG, "form %s 目录缺失,跳过: %s", nm->valuestring, chk);
                continue;
            }
        }
        strncpy(s_pd_form_names[n], nm->valuestring, 31);
        s_pd_form_names[n][31] = 0;
        strncpy(s_pd_form_dirs[n], dr->valuestring, 63);
        s_pd_form_dirs[n][63] = 0;
        n++;
    }
    cJSON_Delete(root);
    s_pd_form_count = n;
    ESP_LOGI(TAG, "forms: %d 个形态 @ %s", n, base_path);
    return n;
}

static volatile bool s_pd_form_switching = false;   // 形态加载中防连点
static int s_pd_form_prev_idx = 0;                  // 切换前的形态(失败回退用)

/* ── 时装(2026-09-26 用户拍板:横屏对话模式 PPDQ 时装切换)──
   PPD_Q/skins.json = {"skins":[{"name":"显示名","dir":"skins/skin1"},...]}
   时装×形态独立:时装目录内缺某形态时回退默认皮肤对应形态 */
static int s_pd_skin_count = 0;
static char s_pd_skin_names[8][64];
static char s_pd_skin_dirs[8][64];
static int s_pd_skin_idx = 0;                        // 0=默认;>0=skins 下标+1
static lv_obj_t *s_pd_skin_btn = NULL;
static lv_obj_t *s_pd_skin_lbl = NULL;
static volatile bool s_pd_skin_switching = false;
static void pd_form_path(char *out, size_t cap, bool fallback_default);   /* 前向声明(pd_form_switch_task 使用) */
static void pd_skin_switch_task(void *arg);
static void pd_skin_panel_show(void);
static void pd_skin_panel_hide(void);

static void pd_scan_skins(const char *base_path) {
    s_pd_skin_count = 0;
    char fp[192];
    if (strlen(base_path) + 12 >= sizeof(fp)) return;
    snprintf(fp, sizeof(fp), "%s/skins.json", base_path);
    FILE *f = fopen(fp, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(f); return; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    int n = 0;
    cJSON *skins = cJSON_GetObjectItem(root, "skins");
    cJSON *it = skins ? skins->child : NULL;
    for (; it && n < 8; it = it->next) {
        cJSON *nm = cJSON_GetObjectItem(it, "name");
        cJSON *dr = cJSON_GetObjectItem(it, "dir");
        if (!nm || !dr || !cJSON_IsString(nm) || !cJSON_IsString(dr)) continue;
        strncpy(s_pd_skin_names[n], nm->valuestring, 63);
        s_pd_skin_names[n][63] = 0;
        strncpy(s_pd_skin_dirs[n], dr->valuestring, 63);
        s_pd_skin_dirs[n][63] = 0;
        n++;
    }
    cJSON_Delete(root);
    s_pd_skin_count = n;
    ESP_LOGI(TAG, "skins: %d 套时装 @ %s", n, base_path);
}

/* 形态加载放独立任务(大形态 pd_load 可达 10 秒):曾在 LVGL 任务里同步做,
   阻塞 UI/渲染整核,AFE feed 消费任务被饿死 → Ringbuffer full 刷屏。 */
static void pd_form_switch_task(void *arg) {
    char path[sizeof(s_pd_base_dir) + 200];
    pd_form_path(path, sizeof(path), true);   /* 2026-09-26 时装×形态独立:当前皮肤内切形态,时装缺形态回退默认 */
    char prev_path[sizeof(s_pd_base_dir) + 200];
    int save_idx = s_pd_form_idx;
    s_pd_form_idx = s_pd_form_prev_idx;
    pd_form_path(prev_path, sizeof(prev_path), true);
    s_pd_form_idx = save_idx;
    ESP_LOGI(TAG, "form switch → %s", path);
    /* 2026-09-17b:先 free 旧模型再 load 新——大形态(背面 200+ 层)与正面同时驻留
       直接 PSRAM 耗尽(alloc fail 海量 + multinet 野指针崩溃);
       load 失败则回退加载切换前形态;再失败才放弃(交互退出)。 */
    if (s_pd_mutex) xSemaphoreTake(s_pd_mutex, portMAX_DELAY);
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    pd_model_t *new_model = pd_load(path, s_pdq_mode ? 1 : 0);   /* 2026-09-26 mesh_only:省层纹理+anims 开销(黑键154层减载爆内存) */
    if (!new_model) {
        ESP_LOGW(TAG, "form switch: 目标形态加载失败 → 回退上一形态 %s", prev_path);
        s_pd_form_idx = s_pd_form_prev_idx;
        new_model = pd_load(prev_path, s_pdq_mode ? 1 : 0);   /* 2026-09-26 mesh_only 同上 */
    }
    if (new_model) {
        s_pd_model = new_model;
        s_pd_model->half_res = false;
        if (s_bg_slot_active >= 0) bg_attach_to_model();   // 重挂长背景(新模型 long_bg 字段为 0)
        /* 同步渲染新形态帧到 fb[2]:形态切换期间 loading 遮罩 freeze 直写,
           UI 重绘(形态 label 等)blit 的是冻结旧帧——先主动刷 fb[2] 防 UI 区旧画面 */
        if (s_pd_fb[2]) {
            pd_render(s_pd_model, s_pd_fb[2], 480, 800,
                      s_pd_model->last_ms);   /* 2026-09-27 相对时间基准:曾传绝对时间 → last_ms 被污染,
                                                 pd_set_anim 的 t0=绝对,循环 t_ms=相对 → t_rel 恒 0 → 动画卡第 0 帧 */
            esp_cache_msync((void*)s_pd_fb[2], 480 * 800 * 2,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        pd_set_anim(s_pd_model, "Idle");   /* 2026-09-27 切形态后自动待机动画(与时装切换一致) */
    }
    if (s_pd_mutex) xSemaphoreGive(s_pd_mutex);
    if (!new_model) {
        /* 2026-09-17:两形态都加载失败——资源缺失,退出交互避免空转 */
        ESP_LOGW(TAG, "form switch: 形态加载彻底失败(资源缺失?)");
        if (s_pd_form_lbl && lv_obj_is_valid(s_pd_form_lbl)) {
            lv_label_set_text_fmt(s_pd_form_lbl, "形态:%s(资源缺失,请更新下载)",
                                  s_pd_form_idx == 0 ? "正面" : s_pd_form_names[s_pd_form_idx - 1]);
        }
        s_pd_form_idx = s_pd_form_prev_idx;
        loading_hide();
        s_pd_form_switching = false;
        vTaskDelete(NULL);
        return;
    }
    if (!s_pd_interaction) {
        /* 加载期间用户已退出互动:丢弃加载结果(互动已停,anim task 已退,防泄漏) */
        if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
        s_pd_form_switching = false;
        vTaskDelete(NULL);
        return;
    }
    if (lvgl_port_lock(pdMS_TO_TICKS(15000))) {
        if (s_pd_form_lbl && lv_obj_is_valid(s_pd_form_lbl)) {
            lv_label_set_text_fmt(s_pd_form_lbl, "形态:%s",
                                  s_pd_form_idx == 0 ? "正面" : s_pd_form_names[s_pd_form_idx - 1]);
        }
        /* 动作列表来自各形态自己的 anims.json:重建动作按钮+面板(曾只换模型不换列表,
           切到基建仍显示正面的 Attack/Die/Idle/Start) */
        if (s_pdq_mode && s_pd_interaction_bg && lv_obj_is_valid(s_pd_interaction_bg)) {
            pdq_anim_panel_set(false);   // 收起面板 + 恢复形态按钮显示
            if (s_anim_panel) { lv_obj_del(s_anim_panel); s_anim_panel = NULL; }
            if (s_anim_btn) { lv_obj_del(s_anim_btn); s_anim_btn = NULL; }
            pdq_anim_ui_create(s_pd_interaction_bg);
        }
        if (!s_pdq_mode) bg_invalidate_ui();   // UI 保护区重绘:fb[2] 已同步新形态帧
        loading_hide();
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "form switch: LVGL 锁超时");
    }
    s_pd_form_switching = false;
    ESP_LOGI(TAG, "pd_form_sw stack hw: %u / 49152", (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* 2026-10-03 诊断 */
    vTaskDelete(NULL);
}

static void pd_form_switch(void) {
    /* 循环切换形态:主形态 ↔ forms/<形态>。 */
    if (!s_pd_model || s_pd_form_count == 0 || s_pd_form_switching) return;
    s_pd_form_prev_idx = s_pd_form_idx;
    s_pd_form_idx = (s_pd_form_idx + 1) % (s_pd_form_count + 1);
    s_pd_form_switching = true;
    loading_show("切换形态中…");
    xTaskCreate(pd_form_switch_task, "pd_form_sw", 24576, NULL, 5, NULL);   /* 2026-10-03 48K 回退 24K:48K 超堆最大连续块(46K)分配必败 */   /* 2026-09-26 栈加大:形态切换同样调 pd_load(used_names 16KB 栈数组) */
}

/* ── 时装切换(2026-09-26 用户拍板:横屏对话模式时装×形态独立)── */
static void pd_skin_base(char *out, size_t cap) {
    /* 当前皮肤基础目录:PPD_Q 或 PPD_Q/skins/skin<N> */
    const char *sub = s_pdq_mode ? "PPD_Q" : "PPD";
    snprintf(out, cap, "%s/%s", s_pd_base_dir, sub);
    if (s_pd_skin_idx > 0 && s_pd_skin_idx <= s_pd_skin_count) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "/%s", s_pd_skin_dirs[s_pd_skin_idx - 1]);
    }
}

static void pd_form_path(char *out, size_t cap, bool fallback_default) {
    /* 当前皮肤下第 s_pd_form_idx 形态的完整路径;fallback_default=时装缺形态回退默认皮肤 */
    pd_skin_base(out, cap);
    if (s_pd_form_idx > 0) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "/%s", s_pd_form_dirs[s_pd_form_idx - 1]);
    }
    if (fallback_default && s_pd_skin_idx > 0 && s_pd_form_idx > 0) {
        char chk[300];
        snprintf(chk, sizeof(chk), "%s/scene.json", out);
        if (access(chk, F_OK) != 0) {
            const char *sub = s_pdq_mode ? "PPD_Q" : "PPD";
            snprintf(out, cap, "%s/%s/%s", s_pd_base_dir, sub,
                     s_pd_form_dirs[s_pd_form_idx - 1]);
        }
    }
}

static void pd_skin_switch_task(void *arg) {
    char path[sizeof(s_pd_base_dir) + 200];
    pd_form_path(path, sizeof(path), true);
    ESP_LOGI(TAG, "skin switch → %s", path);
    if (s_pd_mutex) xSemaphoreTake(s_pd_mutex, portMAX_DELAY);
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    pd_model_t *new_model = pd_load(path, s_pdq_mode ? 1 : 0);
    if (!new_model) {
        /* 时装加载失败 → 回退默认皮肤同形态 */
        int save_idx = s_pd_skin_idx;
        s_pd_skin_idx = 0;
        pd_form_path(path, sizeof(path), false);
        ESP_LOGW(TAG, "skin load fail → 回退默认 %s", path);
        new_model = pd_load(path, s_pdq_mode ? 1 : 0);
        if (!new_model) s_pd_skin_idx = save_idx;   // 默认也失败:恢复索引
    }
    if (new_model) {
        s_pd_model = new_model;
        s_pd_model->half_res = false;
        if (s_bg_slot_active >= 0) bg_attach_to_model();
        if (s_pd_fb[2]) {
            pd_render(s_pd_model, s_pd_fb[2], 480, 800,
                      s_pd_model->last_ms);   /* 2026-09-27 相对时间基准:曾传绝对时间 → last_ms 被污染,
                                                 pd_set_anim 的 t0=绝对,循环 t_ms=相对 → t_rel 恒 0 → 动画卡第 0 帧 */
            esp_cache_msync((void*)s_pd_fb[2], 480 * 800 * 2,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        pd_set_anim(s_pd_model, "Idle");   /* 切时装后自动待机动画 */
    }
    if (s_pd_mutex) xSemaphoreGive(s_pd_mutex);
    if (s_pd_skin_lbl && lv_obj_is_valid(s_pd_skin_lbl)) {
        lvgl_port_lock(pdMS_TO_TICKS(100));
        char t[96];
        if (s_pd_skin_idx == 0) snprintf(t, sizeof(t), "时装:默认");
        else snprintf(t, sizeof(t), "时装:%.*s", 8, s_pd_skin_names[s_pd_skin_idx - 1]);
        lv_label_set_text(s_pd_skin_lbl, t);
        lvgl_port_unlock();
    }
    loading_hide();
    s_pd_skin_switching = false;
    ESP_LOGI(TAG, "pd_skin_sw stack hw: %u / 49152", (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* 2026-10-03 诊断 */
    vTaskDelete(NULL);
}

static void pd_skin_panel_hide(void) {
    if (!s_pd_skin_panel) return;
    lvgl_port_lock(0);
    lv_obj_del(s_pd_skin_panel);
    s_pd_skin_panel = NULL;
    lvgl_port_unlock();
}

static void pd_skin_panel_show(void) {
    if (s_pd_skin_panel) { pd_skin_panel_hide(); return; }
    lvgl_port_lock(0);
    lv_obj_t *panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel, 190, 480);
    /* 旋转 900 后视觉 = (x-480, y)-(x, y+190)。曾用 (480,10) → 视觉盖住顶部按钮条;
       改 (480,130) → 视觉 (0,130)-(480,320),按钮条(y 10-120)下方弹出,2026-09-27 */
    lv_obj_set_pos(panel, 480, 130);
    lv_obj_set_style_transform_pivot_x(panel, 0, 0);
    lv_obj_set_style_transform_pivot_y(panel, 0, 0);
    lv_obj_set_style_transform_rotation(panel, 900, 0);   /* 旋转 90°=横屏顶部横条(与动作面板同布局) */
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(panel, lv_opa_t(235), 0);   /* ~92%:LVGL9 无 LV_OPA_92 宏 */
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    s_pd_skin_panel = panel;
    lv_obj_add_event_cb(panel, [](lv_event_t *e) {
        pd_skin_panel_hide();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ptitle = lv_label_create(panel);
    lv_label_set_text(ptitle, "时装");
    lv_obj_set_style_text_color(ptitle, lv_color_white(), 0);
    lv_obj_set_style_text_font(ptitle, s_chat_font, 0);
    lv_obj_set_pos(ptitle, 5, 6);
    int total = s_pd_skin_count + 1;
    for (int i = 0; i < total && i < 7; i++) {
        const char *nm = (i == 0) ? "默认" : s_pd_skin_names[i - 1];
        lv_obj_t *b = lv_btn_create(panel);
        lv_obj_set_size(b, 180, 26);
        lv_obj_set_pos(b, 5, 30 + i * 30);
        lv_obj_set_style_bg_color(b, (s_pd_skin_idx == i) ? lv_color_hex(0x886644)
                                                          : lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_radius(b, 5, 0);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, nm);
        lv_obj_set_style_text_color(bl, lv_color_white(), 0);
        lv_obj_set_style_text_font(bl, s_chat_font, 0);
        lv_obj_center(bl);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            pd_skin_panel_hide();
            if (idx == s_pd_skin_idx || s_pd_skin_switching) return;
            s_pd_skin_idx = idx;
            s_pd_skin_switching = true;
            loading_show("切换时装中…");
            xTaskCreate(pd_skin_switch_task, "pd_skin_sw", 24576, NULL, 5, NULL);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lvgl_port_unlock();
}

static void pd_interaction_task(void* arg) {
    // 角色扫描与选择：必须在本任务里做——SD 卡 opendir/readdir/stat 调用链很深，
    // 在 LVGL 任务（按钮回调）里执行会撑爆其栈（Stack dump → LVGL 状态损坏 → 疯狂重绘 → watchdog）
    const char* sub = s_pdq_mode ? "PPD_Q" : "PPD";
    pd_scan_chars_sub(sub);
    if (s_pd_count == 0) {
        ESP_LOGW("PD", "interaction: 无角色（/sdcard/Arknights/main/operator/<职业>/<星级>/<干员>/%s/scene.json）", sub);
        bool was_pdq = s_pdq_mode;
        loading_hide();
        s_pd_starting = false;
        s_pdq_mode = false;   // 失败复位：防残留导致下次竖屏入口误进 Q 版
        if (!was_pdq) cover_restore();   // 竖屏失败统一回通行证防黑屏（横屏 Q 版维持原兜底）
        vTaskDelete(NULL);
        return;
    }
    int idx = -1;
    for (int i = 0; i < s_pd_count; i++) {
        if (pd_ci_strstr(s_agent_path, s_pd_dirs[i])) { idx = i; break; }
    }
    if (idx < 0) {
        /* 当前角色在本地 SD 卡没有 PPD/PPD_Q 场景:曾回退加载 s_pd_dirs[0](别的角色!),
           用户点新约能天使却显示 Amiya。改为提示后回退立牌。 */
        ESP_LOGW("PD", "interaction: 当前角色无 %s 资源: %s(扫到 %d 个角色)",
                 sub, s_agent_path, s_pd_count);
        bool was_pdq = s_pdq_mode;
        loading_hide();
        s_pd_starting = false;
        s_pdq_mode = false;
        if (lvgl_port_lock(pdMS_TO_TICKS(2000))) {
            lv_obj_t* tip = lv_label_create(lv_layer_top());
            lv_label_set_text(tip, "该角色暂无 Q 版资源\n请先到下载页下载");
            lv_obj_set_style_bg_color(tip, lv_color_hex(0x222222), 0);
            lv_obj_set_style_bg_opa(tip, LV_OPA_90, 0);
            lv_obj_set_style_text_color(tip, lv_color_white(), 0);
            lv_obj_set_style_text_font(tip, s_chat_font, 0);
            lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_all(tip, 16, 0);
            lv_obj_set_style_radius(tip, 12, 0);
            lv_obj_center(tip);
            lv_obj_move_foreground(tip);
            lvgl_port_unlock();
            // 2.5 秒后自动删除(裁剪版 LVGL 无 lv_timer 完整头,用一次性任务延时删)
            xTaskCreate([](void* p) {
                vTaskDelay(pdMS_TO_TICKS(2500));
                if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
                    if (lv_obj_is_valid((lv_obj_t*)p)) lv_obj_del((lv_obj_t*)p);
                    lvgl_port_unlock();
                }
                vTaskDelete(NULL);
            }, "pd_nores_tip", 4096, tip, 5, NULL);
        }
        if (was_pdq) video_playback_start(30);   // 横屏:回立牌
        else cover_restore();                    // 竖屏:回通行证
        vTaskDelete(NULL);
        return;
    }
    char dir[sizeof(s_pd_dirs[0])];
    strncpy(dir, s_pd_dirs[idx], sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    strncpy(s_pd_base_dir, dir, sizeof(s_pd_base_dir) - 1);
    s_pd_base_dir[sizeof(s_pd_base_dir) - 1] = 0;
    s_pd_form_idx = 0;
    ESP_LOGI("PD", "interaction start: agent=%s → dir=%s", s_agent_path, dir);

    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    ppa_release_playback_caches();   //
    mjpeg_free_buffer();   // 2026-10-07 9.3MB 帧缓冲一并让路(曾致 PSRAM 仅 6.6MB 分配失败) MJPEG 播放缓存让路（退出时自动重载）
    ppa_close_mjpeg();
    ppa_release_jpeg_engine();
    /* Live2D 常驻(模型+纹理+fb ≈ 8~11MB)让路——PPD 大图层(handwear/objects 等 1MB+/张)
       需要;Live2D 是后备,重进时 do_switch 全量重载 */
    extern void lv2_free_all(void);
    lv2_free_all();
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
            bool was_pdq = s_pdq_mode;
            s_pd_starting = false;   // 先清标志：video_playback_start 有 s_pd_starting 守卫
            s_pdq_mode = false;
            if (was_pdq) video_playback_start(30);   // 横屏 Q 版：老兜底
            else cover_restore();                    // 竖屏失败统一回通行证防黑屏
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

    // 背景系统:槽分配+扫描+解码第一张必须在 pd_load 之前(PSRAM 碎片化红线:
    // 角色纹理加载后 largest free block 仅几十 KB,此时再分配 2.3MB 必失败)
    s_bg_slot_active = -1;
    s_bg_cur = -1;
    s_bg_off_x = 0;
    if (!s_pdq_mode) {
        int slots = ppa_long_bg_alloc();
        bg_scan_list();
        if (slots > 0 && s_bg_count > 0) {
            char bgpath[160];   // 前缀43+stem79+".jpg"+NUL 上限≈127,缓冲足够 GCC 可证不越界
            /* 文件名最长 79>缓冲余量:-Wformat-truncation 判为可能截断报错;
               精度限制+缓冲加大后编译器可证不越界(s_bg_names 必 NUL 结尾) */
            snprintf(bgpath, sizeof(bgpath),
                     "/sdcard/Arknights/main/background/%.*s.jpg",
                     (int)sizeof(s_bg_names[0]) - 1, s_bg_names[0]);
            if (ppa_long_bg_decode(bgpath, 0) > 0) {
                s_bg_slot_active = 0;
                int w = ppa_long_bg_width(0);
                s_bg_off_x = (w - 480) / 2;   // 默认正中间切片(用户决策)
                if (s_bg_off_x < 0) s_bg_off_x = 0;
                s_bg_cur = 0;
            }
        }
        ESP_LOGI(TAG, "bg init: %d 张, 活跃槽 %d (cur=%d)", s_bg_count, s_bg_slot_active, s_bg_cur);
    }

    // 加载角色（持锁；anim task 尚未启动，此锁只防 pd_test 侧并发——互斥页面实际不会发生）
    if (s_pd_mutex) xSemaphoreTake(s_pd_mutex, portMAX_DELAY);
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    char path[sizeof(s_pd_dirs[0]) + 8];
    snprintf(path, sizeof(path), "%s/%s", dir, sub);
    s_pd_model = pd_load(path, s_pdq_mode ? 1 : 0);   /* 2026-09-26 mesh_only:省层纹理+anims 开销(黑键154层减载爆内存) */
    /* 2026-09-26 进入互动自动播放待机动画(用户拍板:页面加载完人物即动起来)。
       注意:此处已在 s_pd_mutex 锁内(上方 pd_load 的 Take/Give 区间)——不能再 Take,
       否则互斥锁不可重入 → 死锁(2026-09-26 卡死教训) */
    if (s_pd_model) pd_set_anim(s_pd_model, "Idle");
    /* 2026-09-17:PDQ 模式 mesh.ppdq 缺失/损坏(已被 pdq_load 删除)→ 后台增量重下,
       修复后重进互动即出现动作按钮;不阻塞本次互动(回退层纹理渲染) */
    if (s_pdq_mode && s_pd_model && !s_pd_model->pdq) {
        char *fixkey = (char *)malloc(192);
        if (fixkey) {
            snprintf(fixkey, 192, "%s", s_pd_base_dir);   // .../operator/VOC/STAR/NAME
            xTaskCreate([](void *arg) {
                const char *base = (const char *)arg;
                const char *p2 = strrchr(base, '/');
                char voc[32] = "", star[16] = "", name[64] = "";
                if (p2) {
                    const char *p0 = p2 - 1;
                    while (p0 > base && *p0 != '/') p0--;
                    const char *px = p0 - 1;
                    while (px > base && *px != '/') px--;
                    size_t nl = p2 - p0 - 1; if (nl > 63) nl = 63;
                    memcpy(name, p0 + 1, nl); name[nl] = 0;
                    size_t sl = p0 - px - 1; if (sl > 15) sl = 15;
                    memcpy(star, px + 1, sl); star[sl] = 0;
                    {   // VOC = px 前一段:从 px-1 往前到上一个 '/'
                        const char *q = px - 1;
                        while (q > base && *q != '/') q--;
                        size_t vv = (px - 1) - q; if (vv > 31) vv = 31;
                        memcpy(voc, q + 1, vv); voc[vv] = 0;
                    }
                }
                int uid = scan_bound_user_uid();
                if (uid > 0 && voc[0] && star[0] && name[0]) {
                    char rel[200];
                    snprintf(rel, sizeof(rel), "Arknights/main/operator/%s/%s/%s",
                             voc, star, name);
                    ESP_LOGW("PD", "pdq 修复: 后台增量重下 %s", rel);
                    role_download_fetch_user(uid, rel, NULL, NULL);
                    ESP_LOGW("PD", "pdq 修复完成: %s(重进互动生效)", rel);
                }
                free(arg);
                vTaskDelete(NULL);
            }, "pdq_fix", 12288, fixkey, 2, NULL);
        }
    }
    pd_scan_forms(path);   // 多形态清单(无 forms.json 则 0)
    pd_scan_skins(path);   // 时装清单(无 skins.json 则 0;2026-09-26)
    s_pd_skin_idx = 0;     // 每次进入互动回默认时装
    if (s_pd_model && s_bg_slot_active >= 0) bg_attach_to_model();   // 挂长背景(锁内)
    if (s_pd_mutex) xSemaphoreGive(s_pd_mutex);
    if (!s_pd_model) {
        ESP_LOGE("PD", "interaction: pd_load fail %s", path);
        bool was_pdq = s_pdq_mode;
        ppa_long_bg_free();   // 释放已分配长背景槽(进入互动失败路径)
        s_bg_slot_active = -1;
        s_bg_cur = -1;
        loading_hide();
        s_pd_starting = false;   // 先清标志：video_playback_start 有 s_pd_starting 守卫
        s_pdq_mode = false;
        if (was_pdq) video_playback_start(30);   // 横屏 Q 版：老兜底
        else cover_restore();                    // 竖屏失败统一回通行证防黑屏
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
    fashion_btn_sync();
    fashion_panel_hide();
    fashion_btn_sync();
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

    // 多形态切换按钮(有 forms.json 才显示;循环切换,点一次换一个形态)
    s_pd_form_btn = NULL;
    s_pd_form_lbl = NULL;
    if (s_pd_form_count > 0) {
        s_pd_form_btn = lv_btn_create(bg);
        lv_obj_set_size(s_pd_form_btn, 100, 32);
        if (s_pdq_mode) {
            /* 横屏 Q 版:与侧边按钮同排布旋转 90°(i=6 空位),视觉=顶部横条第 7 位。
               曾放竖帧 (366,325) 不旋转——旋转后跑到屏幕外,按钮创建了用户看不到 */
            lv_obj_set_pos(s_pd_form_btn, 430, 10);
            lv_obj_set_style_transform_pivot_x(s_pd_form_btn, 0, 0);
            lv_obj_set_style_transform_pivot_y(s_pd_form_btn, 0, 0);
            lv_obj_set_style_transform_rotation(s_pd_form_btn, 900, 0);
        } else {
            lv_obj_set_pos(s_pd_form_btn, 366, 355);
        }
        lv_obj_set_style_bg_color(s_pd_form_btn, lv_color_hex(0x444488), 0);
        lv_obj_set_style_bg_opa(s_pd_form_btn, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_pd_form_btn, 6, 0);
        lv_obj_set_style_border_width(s_pd_form_btn, 0, 0);
        s_pd_form_lbl = lv_label_create(s_pd_form_btn);
        lv_label_set_text(s_pd_form_lbl, "形态:正面");
        lv_obj_set_style_text_color(s_pd_form_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_pd_form_lbl, s_chat_font, 0);
        lv_obj_center(s_pd_form_lbl);
        lv_obj_move_foreground(s_pd_form_btn);
        lv_obj_add_event_cb(s_pd_form_btn, [](lv_event_t* e) {
            pd_form_switch();
        }, LV_EVENT_CLICKED, NULL);
        ESP_LOGI(TAG, "形态按钮已创建(共 %d 个形态)", s_pd_form_count);
    }

    /* 时装按钮(2026-09-26 用户拍板:横屏对话模式恒显示;无时装时面板仅"默认") */
    s_pd_skin_btn = NULL;
    s_pd_skin_lbl = NULL;
    if (s_pdq_mode) {
        s_pd_skin_btn = lv_btn_create(bg);
        lv_obj_set_size(s_pd_skin_btn, 100, 32);
        /* 旋转 900 后视觉=横屏顶部横条第 8 位 x∈[435,467] y∈[10,110]
           (形态按钮 (430,10) 是第 7 位,向右 +40 = 第 8 位;2026-09-27 修正:
           曾误用 (370,435),旋转后跑到屏幕外,按钮创建了用户看不到) */
        lv_obj_set_pos(s_pd_skin_btn, 470, 10);
        lv_obj_set_style_transform_pivot_x(s_pd_skin_btn, 0, 0);
        lv_obj_set_style_transform_pivot_y(s_pd_skin_btn, 0, 0);
        lv_obj_set_style_transform_rotation(s_pd_skin_btn, 900, 0);
        lv_obj_set_style_bg_color(s_pd_skin_btn, lv_color_hex(0x884466), 0);
        lv_obj_set_style_bg_opa(s_pd_skin_btn, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_pd_skin_btn, 6, 0);
        lv_obj_set_style_border_width(s_pd_skin_btn, 0, 0);
        s_pd_skin_lbl = lv_label_create(s_pd_skin_btn);
        lv_label_set_text(s_pd_skin_lbl, "时装:默认");
        lv_obj_set_style_text_color(s_pd_skin_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_pd_skin_lbl, s_chat_font, 0);
        lv_obj_center(s_pd_skin_lbl);
        lv_obj_move_foreground(s_pd_skin_btn);
        lv_obj_add_event_cb(s_pd_skin_btn, [](lv_event_t* e) {
            pd_skin_panel_show();
        }, LV_EVENT_CLICKED, NULL);
        ESP_LOGI(TAG, "时装按钮已创建(%d 套时装)", s_pd_skin_count);
    }

    // 背景系统按钮(仅竖屏;横屏 Q 版为 PC 预旋转布局,长图切片语义不适用,pdq 继续 bg.raw)
    // 2026-09-11:右上侧按钮太多 → 三按钮搬左上竖排(解锁/切换/播放·停止)
    s_bg_unlock_btn = NULL; s_bg_unlock_lbl = NULL;
    s_bg_switch_btn = NULL; s_bg_switch_lbl = NULL;
    s_bg_play_btn = NULL; s_bg_play_lbl = NULL;
    s_bg_unlocked = false;
    s_bg_music_playing = false;
    s_bg_last_x = -1;
    s_bg_pending_idx = -1;
    if (!s_pdq_mode) {
        s_bg_unlock_btn = lv_btn_create(bg);
        lv_obj_set_size(s_bg_unlock_btn, 110, 32);
        lv_obj_set_pos(s_bg_unlock_btn, 6, 6);
        lv_obj_set_style_bg_color(s_bg_unlock_btn, lv_color_hex(0x444488), 0);
        lv_obj_set_style_bg_opa(s_bg_unlock_btn, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_bg_unlock_btn, 6, 0);
        lv_obj_set_style_border_width(s_bg_unlock_btn, 0, 0);
        s_bg_unlock_lbl = lv_label_create(s_bg_unlock_btn);
        lv_label_set_text(s_bg_unlock_lbl, "解锁背景");
        lv_obj_set_style_text_color(s_bg_unlock_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_bg_unlock_lbl, s_chat_font, 0);
        lv_obj_center(s_bg_unlock_lbl);
        lv_obj_move_foreground(s_bg_unlock_btn);
        lv_obj_add_event_cb(s_bg_unlock_btn, [](lv_event_t* e) {
            bg_unlock_click(e);
        }, LV_EVENT_CLICKED, NULL);

        s_bg_switch_btn = lv_btn_create(bg);
        lv_obj_set_size(s_bg_switch_btn, 110, 32);
        lv_obj_set_pos(s_bg_switch_btn, 6, 42);
        lv_obj_set_style_bg_color(s_bg_switch_btn, lv_color_hex(0x445588), 0);
        lv_obj_set_style_bg_opa(s_bg_switch_btn, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_bg_switch_btn, 6, 0);
        lv_obj_set_style_border_width(s_bg_switch_btn, 0, 0);
        s_bg_switch_lbl = lv_label_create(s_bg_switch_btn);
        lv_label_set_text(s_bg_switch_lbl, "切换背景");
        lv_obj_set_style_text_color(s_bg_switch_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_bg_switch_lbl, s_chat_font, 0);
        lv_obj_center(s_bg_switch_lbl);
        lv_obj_add_flag(s_bg_switch_btn, LV_OBJ_FLAG_HIDDEN);   // 解锁后显示
        lv_obj_move_foreground(s_bg_switch_btn);
        lv_obj_add_event_cb(s_bg_switch_btn, [](lv_event_t* e) {
            bg_panel_show();
        }, LV_EVENT_CLICKED, NULL);

        s_bg_play_btn = lv_btn_create(bg);
        lv_obj_set_size(s_bg_play_btn, 110, 32);
        lv_obj_set_pos(s_bg_play_btn, 6, 78);
        lv_obj_set_style_bg_color(s_bg_play_btn, lv_color_hex(0x556644), 0);
        lv_obj_set_style_bg_opa(s_bg_play_btn, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_bg_play_btn, 6, 0);
        lv_obj_set_style_border_width(s_bg_play_btn, 0, 0);
        s_bg_play_lbl = lv_label_create(s_bg_play_btn);
        lv_label_set_text(s_bg_play_lbl, "播放音乐");
        lv_obj_set_style_text_color(s_bg_play_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_bg_play_lbl, s_chat_font, 0);
        lv_obj_center(s_bg_play_lbl);
        lv_obj_add_flag(s_bg_play_btn, LV_OBJ_FLAG_HIDDEN);   // 解锁后显示
        lv_obj_move_foreground(s_bg_play_btn);
        lv_obj_add_event_cb(s_bg_play_btn, [](lv_event_t* e) {
            bg_music_toggle();
        }, LV_EVENT_CLICKED, NULL);
        ESP_LOGI(TAG, "背景按钮已创建(解锁%d,左上竖排)", s_bg_unlocked);
    }

    // 触摸：拖动转头/点头 / 按住头部摸头 / 触点视线追踪（与测试页一致）
    // （画质按钮已删：PPA 直写后 CPU 0 零参与，全分辨率 18fps 稳，无需快模式）
    // 解锁背景后触摸分流为滑动(增量更新 long_bg_off_x,渲染下一帧自动用新偏移)

    lv_obj_add_event_cb(bg, [](lv_event_t* e){
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t pt; lv_indev_get_point(indev, &pt);
        if (s_bg_unlocked) {
            // 解锁:左右滑动背景(跟手;只改 off_x,渲染 fill_bg 每帧自取)
            if (s_bg_last_x >= 0 && s_pd_model && s_pd_mutex &&
                xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
                int w = s_pd_model->long_bg_w;
                if (w > 480) {
                    int nx = s_bg_off_x - (pt.x - s_bg_last_x);
                    if (nx < 0) nx = 0;
                    if (nx > w - 480) nx = w - 480;
                    if (nx != s_bg_off_x) {
                        s_bg_off_x = nx;
                        s_pd_model->long_bg_off_x = nx;
                        /* UI 保护区底图跟随:每次滑动事件都 invalidate(触摸 30Hz,
                           LVGL 自动合并 dirty 重绘,滞后≤1帧)。无节流——此前 150ms
                           节流造成 UI 区慢主区好几帧 */
                        bg_invalidate_ui();   // LVGL 回调上下文,无需锁
                    }
                }
                xSemaphoreGive(s_pd_mutex);
            }
            s_bg_last_x = pt.x;
        } else if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
            pd_touch(s_pd_model, pt.x, pt.y, true);
            xSemaphoreGive(s_pd_mutex);
        }
    }, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(bg, [](lv_event_t*){
        if (s_bg_unlocked) {
            s_bg_last_x = -1;
            bg_invalidate_ui();   // 松手终刷(节流可能漏掉最后一小段位移)
        } else if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
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
    /* 渲染专用核 CPU 1。
       2026-10-03 栈 32768→16384:实测渲染+循环 LVGL 调用高水位仅 ~1.9K,16K=8 倍
       余量;32K 在互动初始化的碎片堆里分配失败(maxblk 实测 22K)→ 渲染任务
       不存在 → 画面冻结/滑动失效。创建失败已日志兜底。 */
    if (!s_pd_task) {
        s_pd_running = true;
        if (xTaskCreatePinnedToCore(pd_anim_task, "pd_anim", 16384, NULL, 3, &s_pd_task, 1) != pdPASS) {
            ESP_LOGE(TAG, "pd_anim create FAILED (free %u, maxblk %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            s_pd_running = false;
            s_pd_task = NULL;
        }
    }
    loading_hide();
    s_pd_starting = false;
    ESP_LOGI(TAG, "pd_start stack hw: %u / 28672", (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* 2026-10-03 诊断 */
    vTaskDelete(NULL);
}

static void pd_interaction_start(void) {
    if (s_pd_interaction || s_pd_starting) return;
    /* 注意：本函数运行在 LVGL 任务（按钮回调）栈上——只做轻量启动，
       任何 SD 卡 IO（扫描/选角色/加载）都在 pd_interaction_task（10240 栈）里执行 */
    s_pd_starting = true;
    loading_show(s_pdq_mode ? "进入 Q 版互动" : "进入 PPD 交互");
    /* 2026-09-26:栈 10240→32768。pd_load 栈上有 used_names[256][64]=16KB
       (减载模式动画引用层名收集)+cJSON 递归帧,实测需求 ~22KB——
       20KB 仍踩金丝雀(SP 超出栈底 ~1KB)。32KB 留足余量,不动引擎内部。
       2026-10-03:48K 回退 32K——P4 内部 SRAM 空闲仅 ~130K(panel fb 占 768K),
       48K 在碎片堆分配失败导致卡加载页;本任务 JPEG 解码/pd_load/pd_render
       串行执行,最大深度 ~22-28K,32K 够。失败必须兜底,不得静默卡死。 */
    BaseType_t ret = xTaskCreate(pd_interaction_task, "pd_start", 28672, NULL, 2, NULL);   /* 2026-10-04 32K→28K:Mon3tr 等角色封面流程碎片化重(maxblk 29K),32K 分不出;实测峰值 24.3K,28K 留 3.7K 余量 */
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "pd_start create FAILED (free %u, maxblk %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        loading_hide();
        ui_toast("内存不足,请重启设备");
        s_pd_starting = false;
    }
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
    // 等形态切换任务结束（持 mutex 中删除信号量会崩;最长 15s）
    guard = 0;
    while (s_pd_form_switching && guard++ < 300) vTaskDelay(pdMS_TO_TICKS(50));
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
    // 停渲染任务并等它真正退出(曾固定等 100ms 就删 mutex/fb/model:渲染一帧超时
    // 或任务被抢占时,anim task 仍在用已释放资源 → xQueueGenericSend 垃圾句柄 /
    // pd_render 读已释放内存 → Load access fault)。此等待在 lvgl_port_lock 之前:
    // 老路分支的 anim task 需要拿 LVGL 锁才能完成最后一帧退出
    s_pd_running = false;
    guard = 0;
    while (!s_pd_task_exited && guard++ < 200) vTaskDelay(pdMS_TO_TICKS(50));   // 最长 10s
    if (!s_pd_task_exited) ESP_LOGW("PD", "anim task 退出超时(10s),继续停止流程");
    s_pd_task = NULL;
    s_pd_task_exited = false;
    // 等背景检查/下载/切换任务退出(其挂载/收尾持 s_pd_mutex,须在删 mutex 前退干净;
    // 先置取消让下载尽快中止,最长 20s 兜底)
    s_dl_cancel = true;
    guard = 0;
    while (s_bg_task_running && guard++ < 400) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_bg_task_running) ESP_LOGW("PD", "bg 任务退出超时(20s),继续停止流程");
    s_bg_checking = false;
    // 提前置位:音频任务的 expression_switch_emotion 先查 s_pd_interaction——
    // 曾置 false 在 free model/mutex 之后,窗口内它可 Take 已删 mutex/用已释放 model
    s_pd_interaction = false;
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
    s_pd_form_btn = NULL;   // 形态按钮随 bg 一并删除(指针先清防悬垂)
    s_pd_form_lbl = NULL;
    s_pd_form_count = 0;
    s_pd_form_idx = 0;
    /* 2026-09-26 时装状态清理 */
    if (s_pd_skin_panel) {
        lvgl_port_lock(0);
        lv_obj_del(s_pd_skin_panel);
        lvgl_port_unlock();
        s_pd_skin_panel = NULL;
    }
    s_pd_skin_btn = NULL;
    s_pd_skin_lbl = NULL;
    s_pd_skin_count = 0;
    s_pd_skin_idx = 0;
    s_pd_skin_switching = false;
    if (s_pd_interaction_bg) {
        lvgl_port_lock(0);
        lv_obj_del(s_pd_interaction_bg);
        lvgl_port_unlock();
        s_pd_interaction_bg = NULL;
    }
    // 背景系统清理:面板(lv_layer_top 独立)/弹窗显式删;按钮随 bg 已删,指针先清防悬垂
    bg_panel_hide();
    bg_check_popup_hide();
    if (s_bg_upd_popup) {
        lvgl_port_lock(0);
        lv_obj_del(s_bg_upd_popup);
        lvgl_port_unlock();
        s_bg_upd_popup = NULL;
    }
    s_bg_unlock_btn = NULL;
    s_bg_unlock_lbl = NULL;
    s_bg_switch_btn = NULL;
    s_bg_switch_lbl = NULL;
    s_bg_play_btn = NULL;
    s_bg_play_lbl = NULL;
    s_bg_unlocked = false;
    s_bg_music_playing = false;
    bg_music_stop();   // 退出互动停背景音乐(音乐与互动/对话不共存)
    s_bg_cur = -1;
    s_bg_off_x = 0;
    s_bg_last_x = -1;
    s_bg_slot_active = -1;
    ppa_long_bg_free();
    if (s_pd_mutex) { vSemaphoreDelete(s_pd_mutex); s_pd_mutex = NULL; }
    if (s_pd_model) { pd_free(s_pd_model); s_pd_model = NULL; }
    for (int i = 0; i < 3; i++) {
        if (s_pd_fb[i]) { heap_caps_free(s_pd_fb[i]); s_pd_fb[i] = NULL; }
    }
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
// 离线模式按钮（开机网络门期间由 application.cc 调用）：
// 显示在 lv_layer_top（配网界面/错误提示之上），点击跳过联网/激活直接进展示页
// ══════════════════════════════════════════════════
static lv_obj_t *s_offline_btn = NULL;
void offline_button_remove(void);

void offline_button_show(const lv_font_t *font) {
    if (s_offline_btn) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    s_offline_btn = lv_btn_create(lv_layer_top());
    lv_obj_set_size(s_offline_btn, 200, 60);
    lv_obj_set_pos(s_offline_btn, 140, 350);
    lv_obj_set_style_bg_color(s_offline_btn, lv_color_hex(0x885522), 0);
    lv_obj_set_style_radius(s_offline_btn, 12, 0);
    lv_obj_t *lbl = lv_label_create(s_offline_btn);
    lv_label_set_text(lbl, "离线模式");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(s_offline_btn, [](lv_event_t *e) {
        extern void application_set_offline_mode(void);
        application_set_offline_mode();
        offline_button_remove();
        /* 立即反馈（LVGL 任务独立于被 HTTP 阻塞的主循环）：清掉"检查新版本失败"
           的 Alert 残留，用户无需等当前 HTTP 超时结束就明确看到离线已生效 */
        Display* disp = Board::GetInstance().GetDisplay();
        if (disp) {
            disp->SetStatus("离线模式");
            disp->SetChatMessage("system", "正在进入离线模式…");
            disp->SetEmotion("neutral");
        }
        ESP_LOGI(TAG, "离线模式按钮点击：跳过网络门");
    }, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
}

void offline_button_remove(void) {
    if (!s_offline_btn) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (lv_obj_is_valid(s_offline_btn)) lv_obj_del(s_offline_btn);
    s_offline_btn = NULL;
    lvgl_port_unlock();
}

// ══════════════════════════════════════════════════
// 设备未绑定弹层:全屏遮罩 + 大字 6 位绑定码。
// 用户在网页输入绑定码 → 设备轮询到已绑定 → 自动隐藏并解锁对话
// ══════════════════════════════════════════════════
static lv_obj_t *s_bind_overlay = NULL;

void bind_code_overlay_show(const lv_font_t *font, const char *code) {
    if (s_bind_overlay) return;   // 已显示
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    lv_obj_t *ovl = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl, 480, 800);
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_style_bg_color(ovl, lv_color_hex(0x10151d), 0);
    lv_obj_set_style_border_width(ovl, 0, 0);
    lv_obj_set_style_pad_all(ovl, 0, 0);
    lv_obj_remove_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ovl);
    lv_label_set_text(title, "设备未绑定");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    if (font) lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_pos(title, 30, 90);

    lv_obj_t *code_lbl = lv_label_create(ovl);
    lv_label_set_text(code_lbl, code);
    lv_obj_set_style_text_color(code_lbl, lv_color_hex(0x7fd), 0);
    if (font) lv_obj_set_style_text_font(code_lbl, font, 0);
    lv_obj_set_pos(code_lbl, 30, 150);

    /* 2026-10-05 二维码(bind_qr.h,login.png 生成 200x200) + 网址 */
    lv_obj_t *qr = lv_image_create(ovl);
    lv_image_set_src(qr, &bind_qr_img);
    lv_obj_set_pos(qr, (480 - 200) / 2, 205);

    // 绑定网址(2026-09-30 用户要求:新用户知道去哪绑定;2026-10-05 与二维码内容一致)
    lv_obj_t *url_lbl = lv_label_create(ovl);
    lv_label_set_text(url_lbl, "http://124.221.186.33:88/login");
    lv_obj_set_style_text_color(url_lbl, lv_color_hex(0x7fd), 0);
    if (font) lv_obj_set_style_text_font(url_lbl, font, 0);
    lv_obj_set_pos(url_lbl, 30, 430);

    lv_obj_t *hint1 = lv_label_create(ovl);
    lv_label_set_text(hint1, "请访问该网址或扫描二维码\n登录后选择【设备】填写六位码");
    lv_obj_set_style_text_color(hint1, lv_color_hex(0xcccccc), 0);
    if (font) lv_obj_set_style_text_font(hint1, font, 0);
    lv_obj_set_pos(hint1, 30, 495);

    lv_obj_t *hint2 = lv_label_create(ovl);
    lv_label_set_text(hint2, "绑定成功后本页自动消失,\n无需重启设备");
    lv_obj_set_style_text_color(hint2, lv_color_hex(0x888888), 0);
    if (font) lv_obj_set_style_text_font(hint2, font, 0);
    lv_obj_set_pos(hint2, 30, 620);

    s_bind_overlay = ovl;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Bind code overlay shown: %s", code);
}

void bind_code_overlay_hide(void) {
    if (!s_bind_overlay) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    if (lv_obj_is_valid(s_bind_overlay)) lv_obj_del(s_bind_overlay);
    s_bind_overlay = NULL;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Bind code overlay hidden");
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

// 播放动作动画：优先 anim/<名>.mjpeg（spine 官方渲染逐帧烘焙——mesh 形变
// 像素级保真，躺下等大幅形变动画的唯一正确解；曾用 anims.json 轨道：
// 整层刚体无顶点形变,头发/耳朵/脸相对位置方向错乱）。
// 无 MJPEG 时回退轨道(兼容旧数据)。
static void pdq_anim_play(const char *name) {
    for (int i = 0; i < s_anim_count; i++) {
        const char *base = strrchr(s_anim_paths[i], '/');
        if (!base) continue;
        base++;
        if (strncasecmp(base, name, strlen(name)) != 0 ||
            strcasecmp(base + strlen(name), ".mjpeg") != 0) continue;
        int count = ppa_preload_mjpeg(s_anim_paths[i]);
        if (count > 0) {
            s_anim_play = true;
            video_playback_start(30);
            ESP_LOGI(TAG, "pdq anim(MJPEG): %s (%d 帧)", s_anim_paths[i], count);
        } else {
            ESP_LOGW(TAG, "pdq anim MJPEG 加载失败: %s", s_anim_paths[i]);
        }
        return;
    }
    // 回退:anims.json 轨道(旧数据无 MJPEG 时)
    if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
        pd_set_anim(s_pd_model, name);
        xSemaphoreGive(s_pd_mutex);
    }
}

// 停止动作播放：回待机（纸偶渲染继续）
static void pdq_anim_stop(void) {
    if (s_anim_play) {
        video_playback_stop();
        s_anim_play = false;
        ESP_LOGI(TAG, "pdq anim stop (MJPEG)");
        return;
    }
    if (s_pd_model && s_pd_mutex && xSemaphoreTake(s_pd_mutex, portMAX_DELAY) == pdTRUE) {
        pd_set_anim(s_pd_model, NULL);
        xSemaphoreGive(s_pd_mutex);
    }
}

// 横屏 Q 版互动内的动作 UI：左侧"动作"按钮（与按钮组同排布 i=5）+
// 动作列表面板（点击展开/收起；选动作 = pd_set_anim 原生播放，选"恢复互动"回待机）
// 面板展开时与形态按钮在顶部横条重叠：展开隐藏形态按钮,收起恢复(防叠压)
static void pdq_anim_panel_set(bool show) {
    if (s_anim_panel && lv_obj_is_valid(s_anim_panel)) {
        if (show) lv_obj_remove_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pd_form_btn && lv_obj_is_valid(s_pd_form_btn)) {
        if (show) lv_obj_add_flag(s_pd_form_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_pd_form_btn, LV_OBJ_FLAG_HIDDEN);
    }
    /* 2026-09-27:时装按钮同样处理——面板展开时隐藏,防叠压(用户报"时装按钮叠在动作栏上方") */
    if (s_pd_skin_btn && lv_obj_is_valid(s_pd_skin_btn)) {
        if (show) lv_obj_add_flag(s_pd_skin_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_pd_skin_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pdq_anim_ui_create(lv_obj_t *bg) {
    /* 2026-09-17:PDQ 模式动画数来自 mesh.ppdq(不依赖大体积 anims.json) */
    int n_anims_avail = 0;
    if (s_pd_model) {
        if (s_pdq_mode && s_pd_model->pdq) {
            n_anims_avail = pdq_anim_count(s_pd_model->pdq);
        } else {
            n_anims_avail = s_pd_model->n_anims;
        }
    }
    if (n_anims_avail <= 0) {
        ESP_LOGW(TAG, "pdq: 无原生动画（anims.json/mesh）");
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
        pdq_anim_panel_set(lv_obj_has_flag(s_anim_panel, LV_OBJ_FLAG_HIDDEN));
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
        pdq_anim_panel_set(false);   // 收起面板（反馈明确）+ 恢复形态按钮
    }, LV_EVENT_CLICKED, NULL);

    // 动画列表(2026-09-17:PDQ 模式读 mesh.ppdq 动画名——anims.json 体积大
    // (正面 3.9MB)PSRAM 不足时静默无动画;竖屏仍读 anims.json)
    int n_anims_ui = 0;
    {
        if (s_pdq_mode && s_pd_model && s_pd_model->pdq) {
            n_anims_ui = pdq_anim_count(s_pd_model->pdq);
        } else if (s_pd_model) {
            n_anims_ui = s_pd_model->n_anims;
        }
    }
    for (int i = 0; i < n_anims_ui && i < 14; i++) {
        char name[32];
        if (s_pdq_mode && s_pd_model && s_pd_model->pdq) {
            const char *nm = pdq_anim_name(s_pd_model->pdq, i);
            if (!nm) continue;
            snprintf(name, sizeof(name), "%s", nm);
        } else {
            snprintf(name, sizeof(name), "%s", s_pd_model->anims[i].name);
        }

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
            pdq_anim_panel_set(false);   // 播放时收起面板，完整看清动作 + 恢复形态按钮
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
    // 同 pd_interaction_stop_internal:等 anim task 真正退出再释放资源(防竞态崩溃)
    s_pd_running = false;
    int g2 = 0;
    while (!s_pd_task_exited && g2++ < 200) vTaskDelay(pdMS_TO_TICKS(50));   // 最长 10s
    if (!s_pd_task_exited) ESP_LOGW("PD", "anim task 退出超时(10s),继续停止流程");
    s_pd_task = NULL;
    s_pd_task_exited = false;
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
        ESP_LOGW("PD", "无角色：请在 SD 卡 /sdcard/Arknights/main/operator/<职业>/<星级>/<干员>/PPD/ 放入 scene.json + *.raw");
        return;
    }
    video_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    ppa_release_playback_caches();   //
    mjpeg_free_buffer();   // 2026-10-07 9.3MB 帧缓冲一并让路(曾致 PSRAM 仅 6.6MB 分配失败) cover/emoji/mask 等播放缓存让路（退出时自动重载）
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
    xTaskCreatePinnedToCore(pd_anim_task, "pd_anim", 49152, NULL, 3, &s_pd_task, 1);  /* 2026-10-03 32K->48K:栈顶溢出写 TCB(s11 槽被随机数据覆盖),CANARY 只在栈底不报 */  /* 2026-10-03 栈 16K->32K:解锁背景/检查时仍栈穿(RA==MEPC、寄存器 0xDE 污染) */  /* 渲染专用核 CPU 1；CPU 0 留给 LVGL+AFE（AFE 已改 core 0）；2026-10-03 栈 8K→16K:背景下载/重扫时渲染深调用链栈溢出(Load access fault@6650) */
}

// ══════════════════════════════════════════════════════
// 用户角色资源保障:唤醒 _users 角色时,后台任务检查本地资源,
// 缺失 → 弹窗询问下载(带进度);齐全/完成 → 回主循环切表情画面
// ══════════════════════════════════════════════════════
static lv_obj_t *s_ures_box = nullptr;      // 弹窗容器
static lv_obj_t *s_ures_lbl = nullptr;      // 提示/进度文字
static int s_ures_choice = 0;               // 0=未选择 1=下载 2=取消

static void ures_prompt_show(const char *text, bool with_buttons) {
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    s_ures_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ures_box, 400, 240);
    lv_obj_set_pos(s_ures_box, 40, 280);
    lv_obj_set_style_bg_color(s_ures_box, lv_color_hex(0x1a2230), 0);
    lv_obj_set_style_border_width(s_ures_box, 1, 0);
    lv_obj_set_style_border_color(s_ures_box, lv_color_hex(0x4d9fff), 0);
    lv_obj_set_style_radius(s_ures_box, 10, 0);
    lv_obj_set_style_pad_all(s_ures_box, 14, 0);
    lv_obj_remove_flag(s_ures_box, LV_OBJ_FLAG_SCROLLABLE);

    s_ures_lbl = lv_label_create(s_ures_box);
    lv_label_set_text(s_ures_lbl, text);
    lv_obj_set_style_text_color(s_ures_lbl, lv_color_white(), 0);
    lv_obj_set_width(s_ures_lbl, 370);
    lv_obj_set_pos(s_ures_lbl, 10, 10);
    const lv_font_t *f = Board::GetInstance().GetDisplay()->GetTextFont();
    if (f) lv_obj_set_style_text_font(s_ures_lbl, f, 0);

    if (with_buttons) {
        lv_obj_t *btn_ok = lv_btn_create(s_ures_box);
        lv_obj_set_size(btn_ok, 150, 50);
        lv_obj_set_pos(btn_ok, 30, 160);
        lv_obj_t *l1 = lv_label_create(btn_ok);
        lv_label_set_text(l1, "下载");
        if (f) lv_obj_set_style_text_font(l1, f, 0);
        lv_obj_center(l1);
        lv_obj_add_event_cb(btn_ok, [](lv_event_t *) { s_ures_choice = 1; },
                            LV_EVENT_CLICKED, nullptr);

        lv_obj_t *btn_no = lv_btn_create(s_ures_box);
        lv_obj_set_size(btn_no, 150, 50);
        lv_obj_set_pos(btn_no, 220, 160);
        lv_obj_t *l2 = lv_label_create(btn_no);
        lv_label_set_text(l2, "取消");
        if (f) lv_obj_set_style_text_font(l2, f, 0);
        lv_obj_center(l2);
        lv_obj_add_event_cb(btn_no, [](lv_event_t *) { s_ures_choice = 2; },
                            LV_EVENT_CLICKED, nullptr);
    }
    lvgl_port_unlock();
}

static void ures_prompt_update(const char *text) {
    if (s_ures_lbl && lvgl_port_lock(pdMS_TO_TICKS(200))) {
        lv_label_set_text(s_ures_lbl, text);
        lvgl_port_unlock();
    }
}

static void ures_prompt_hide(void) {
    if (!s_ures_box) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        if (lv_obj_is_valid(s_ures_box)) lv_obj_del(s_ures_box);
        s_ures_box = nullptr;
        s_ures_lbl = nullptr;
        lvgl_port_unlock();
    }
}

struct UresTaskArg {
    std::string sd_path;
    int uid;
    std::string rel;   // 用户根下相对路径(Arknights/main/operator/... 或 other/...)
};

static void ures_task(void *arg) {
    auto *a = (UresTaskArg *)arg;
    std::string path = a->sd_path;
    int uid = a->uid;
    std::string rel = a->rel;
    delete a;

    int64_t total = 0;
    ESP_LOGI(TAG, "用户资源检查: u%d %s", uid, rel.c_str());
    int r = role_download_check_user(uid, rel.c_str(), &total);
    if (r == 1 || r == -2) {
        // 齐全(或云端无资源):直接切画面
        Application::GetInstance().Schedule([path]() {
            expression_display_start(path.c_str(), "neutral");
        });
        return;
    }
    if (r == -1) {
        Application::GetInstance().Schedule([]() {
            if (auto d = Board::GetInstance().GetDisplay()) {
                d->SetChatMessage("system", "资源检查网络失败,画面未切换");
            }
        });
        return;
    }
    // 缺失:弹窗询问
    s_ures_choice = 0;
    char msg[160];
    snprintf(msg, sizeof(msg), "你的角色资源未下载(约 %d 分钟)。\n是否现在下载?",
             role_download_estimate_minutes(total));
    ures_prompt_show(msg, true);
    for (int i = 0; i < 300 && s_ures_choice == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_ures_choice != 1) { ures_prompt_hide(); return; }

    ures_prompt_update("下载中 0%…");
    int res = role_download_fetch_user(uid, rel.c_str(),
        [](int pct, const char *file, void *) -> bool {
            static char buf[160];
            if (file && file[0]) {
                snprintf(buf, sizeof(buf), "下载中 %d%%\n%s", pct, file);
            } else {
                snprintf(buf, sizeof(buf), "下载中 %d%%…", pct < 0 ? 0 : pct);
            }
            ures_prompt_update(buf);
            return true;
        }, nullptr);
    ures_prompt_hide();
    if (res == 0 || res == 2) {
        Application::GetInstance().Schedule([path]() {
            expression_display_start(path.c_str(), "neutral");
        });
    } else {
        Application::GetInstance().Schedule([]() {
            if (auto d = Board::GetInstance().GetDisplay()) {
                d->SetChatMessage("system", "资源下载失败,请稍后重试");
            }
        });
    }
}

/* 唤醒用户角色入口:解析 _users 路径 → 后台任务检查/下载 → 主循环切画面 */
void expression_start_user_res(const char *sd_path) {
    // 格式: /sdcard/_users/u<uid>/<rel...>
    //   rel 如 Arknights/main/operator/SUPPORTER/6STAR/Civilight_Eterna(明日方舟克隆)
    //   或 other/我的OC(用户自定义,任意段数)
    std::string p = sd_path;
    std::vector<std::string> t;
    size_t pos = 0;
    while (pos < p.size()) {
        size_t nx = p.find('/', pos);
        if (nx == std::string::npos) { t.push_back(p.substr(pos)); break; }
        t.push_back(p.substr(pos, nx - pos));
        pos = nx + 1;
    }
    std::string canonical = p;
    int uid = 0;
    std::string rel;
    if (t.size() >= 5 && t[2] == "_users" && t[3].size() > 1 && t[3][0] == 'u') {
        uid = atoi(t[3].c_str() + 1);   // "u3" → 3
        for (size_t i = 4; i < t.size(); i++) {
            rel += (i > 4 ? "/" : "") + t[i];
        }
    } else if (t.size() >= 5 && t[2] == "_users") {
        // 旧格式无 u 前缀:归一化
        uid = atoi(t[3].c_str());
        for (size_t i = 4; i < t.size(); i++) {
            rel += (i > 4 ? "/" : "") + t[i];
        }
        canonical = "/sdcard/_users/u" + std::to_string(uid) + "/" + rel;
        ESP_LOGW(TAG, "用户资源路径旧格式,归一化: %s → %s", sd_path, canonical.c_str());
    } else {
        ESP_LOGW(TAG, "用户资源路径格式异常: %s", sd_path);
        expression_display_start(sd_path, "neutral");
        return;
    }
    if (uid <= 0 || s_ures_box) {
        if (uid <= 0) expression_display_start(canonical.c_str(), "neutral");
        else ESP_LOGW(TAG, "已有用户资源下载进行中,跳过");
        return;
    }
    auto *arg = new UresTaskArg{canonical, uid, rel};
    xTaskCreate([](void *a) {
        ures_task(a);
        vTaskDelete(NULL);
    }, "user_res", 10240, arg, 2, nullptr);
}
