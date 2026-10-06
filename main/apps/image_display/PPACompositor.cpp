/*
 * PPA Hardware Compositor - Color-key compositing for video playback
 * Uses ESP32-P4 PPA BLEND engine to key out red background and composite character over static bg
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "driver/jpeg_decode.h"
#include "MjpegPlayer.h"
#include "PPACompositor.h"

#define TAG "PPACompositor"

#define DISPLAY_W 480
#define DISPLAY_H 800
#define DECODE_MAX_H 900
#define FRAME_SIZE_RGB565 (DISPLAY_W * DECODE_MAX_H * 2)
#define FRAME_SIZE_ARGB (DISPLAY_W * DECODE_MAX_H * 4)  // ARGB8888 for alpha blend

static ppa_client_handle_t s_ppa_client = NULL;
static uint8_t *s_bg_buf = NULL;       // Background (static)
static uint8_t *s_fg_buf = NULL;       // Foreground (JPEG decoded, RGB565)
static uint8_t *s_alpha_buf = NULL;    // Foreground (ARGB8888 for alpha blend)
static bool s_alpha_oom = false;       // 分配失败置位：不再每帧重试，释放缓存时复位
static uint8_t *s_comp_buf = NULL;     // Composited output (RGB565)
static int s_frame_idx = 0;            // 双缓冲槽位（0/1 翻转）
static uint8_t *s_tx_buf = NULL;       // JPEG 输入缓冲（一次分配复用，防 PSRAM 碎片化）
static size_t s_tx_cap = 0;

// ── 加载进度回调（加载动画用）──
static PPALoadProgressCb s_ppa_prog_cb = NULL;
void ppa_set_load_progress_cb(PPALoadProgressCb cb) { s_ppa_prog_cb = cb; }
static inline void ppa_prog(const char* s, int p) { if (s_ppa_prog_cb) s_ppa_prog_cb(s, p); }

// JPEG decoder config
static jpeg_decoder_handle_t s_jpg_handle = NULL;
static uint32_t s_last_decoded_size = 0;
static jpeg_decode_engine_cfg_t s_jpg_eng_cfg = { .timeout_ms = 40 };
static int s_decode_cool_down = 0;  // 解码失败后的提交冷却帧数（防 DMA2D 悬挂累积）

// 2026-10-04 2D-DMA 通道互斥：所有 jpeg 引擎创建/解码/删除必须持锁。
// 多引擎并发（封面 + 缩略图 A/B + 背景 + 长背景 + 立牌）会让 2D-DMA 通道悬挂，
// 后续 dma2d_connect 忙等复位 → IWDT 重启（第二台设备封面第 70 帧实测）。
static SemaphoreHandle_t s_jpg_mtx = NULL;
/* 2026-10-06 加超时:解码悬挂(DMA 通道竞争)时持锁任务可能永不返回,
   portMAX_DELAY 等待让后续所有解码调用(菜单 profile/封面/背景)连锁卡死。
   超时拿不到锁返回 false,调用方走失败路径,不再死等 */
bool ppa_jpg_lock(uint32_t timeout_ms) {
    if (!s_jpg_mtx) s_jpg_mtx = xSemaphoreCreateMutex();
    if (!s_jpg_mtx) return false;
    return xSemaphoreTake(s_jpg_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void ppa_jpg_unlock(void) {
    if (s_jpg_mtx) xSemaphoreGive(s_jpg_mtx);
}
static jpeg_decode_cfg_t s_jpg_cfg_rgb = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};

// ─── Background ───────────────────────────────────────────────

bool ppa_load_background(const char *path) {
    ppa_prog("加载背景", -1);  // 单文件读+解码，无内循环，不可量化
    // Read JPEG file
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "Cannot open %s", path); return false; }
    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!jpg_data) {
        fclose(fp);
        ESP_LOGE(TAG, "bg jpg alloc failed (%u B, PSRAM free %u KB)", (unsigned)size,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }
    if (fread(jpg_data, 1, size, fp) != size) { fclose(fp); free(jpg_data); ESP_LOGE(TAG, "bg jpg short read"); return false; }
    fclose(fp);

    // Decode background to RGB565
    jpeg_decode_memory_alloc_cfg_t rx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };

    size_t out_size;
    size_t tx_size;
    uint8_t *tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(size, &tx_cfg, &tx_size);
    if (!tx_buf) {
        free(jpg_data);
        ESP_LOGE(TAG, "bg tx_buf alloc failed (PSRAM free %u KB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }
    memcpy(tx_buf, jpg_data, size);
    free(jpg_data);

    // 先释放旧背景（防重复加载泄漏）；失败时置 NULL 保持 has_background 语义一致
    if (s_bg_buf) { free(s_bg_buf); s_bg_buf = NULL; }
    s_bg_buf = (uint8_t*)jpeg_alloc_decoder_mem(FRAME_SIZE_RGB565, &rx_cfg, &out_size);
    if (!s_bg_buf) {
        free(tx_buf);
        ESP_LOGE(TAG, "bg rx_buf alloc failed (PSRAM free %u KB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }

    if (!ppa_jpg_lock()) {   // 2026-10-06 超时不等待(悬挂保护)
        free(tx_buf); free(s_bg_buf); s_bg_buf = NULL;
        ESP_LOGE(TAG, "bg decode lock timeout (PSRAM free %u KB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }
    jpeg_decoder_handle_t tmp_handle = NULL;
    if (jpeg_new_decoder_engine(&s_jpg_eng_cfg, &tmp_handle) != ESP_OK) {
        ppa_jpg_unlock();
        free(tx_buf); free(s_bg_buf); s_bg_buf = NULL;
        ESP_LOGE(TAG, "bg engine create failed (PSRAM free %u KB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }

    uint32_t decoded_size;
    esp_err_t ret = jpeg_decoder_process(tmp_handle, &s_jpg_cfg_rgb, tx_buf, tx_size, s_bg_buf, out_size, &decoded_size);
    jpeg_del_decoder_engine(tmp_handle);
    ppa_jpg_unlock();
    free(tx_buf);
    if (ret != ESP_OK) {
        free(s_bg_buf); s_bg_buf = NULL;
        ESP_LOGE(TAG, "bg decode failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "Background loaded: %s (%ux%u)", path, DISPLAY_W, DISPLAY_H);
    /* 2026-10-03 移除 bg.raw 同步写:SDMMC DMA 大块写疑破坏 .bss(解锁背景崩溃)。
       背景一致性暂由 bg_switch 的 PPA 缓冲同步保证;bg.raw 持久化方案待
       DMA 问题定位后用小缓冲分块写重做 */
    ppa_prog("加载背景", 100);
    return true;
}

bool ppa_background_set_from_rgb565(const uint8_t* buf, int width, int stride, int offset_x) {
    /* 2026-10-03 选背景全局持久化:从长背景槽裁剪 480×800 更新 PPA 背景缓冲,
       使对话模式/拍照恢复的背景与用户选择一致(不再固定 background.jpg) */
    if (!s_bg_buf || !buf) return false;
    if (offset_x < 0) offset_x = 0;
    if (offset_x + 480 > width) offset_x = width - 480;
    if (offset_x < 0) offset_x = 0;
    for (int y = 0; y < 800; y++) {
        memcpy(s_bg_buf + y * 480 * 2, buf + ((size_t)y * stride + offset_x) * 2, 480 * 2);
    }
    ESP_LOGI(TAG, "Background updated from long bg slot (off=%d)", offset_x);
    return true;
}

void ppa_unload_background(void) {    if (s_bg_buf) { free(s_bg_buf); s_bg_buf = NULL; }
    ESP_LOGI(TAG, "Background unloaded");
}
bool ppa_has_background(void) { return s_bg_buf != NULL; }
const uint8_t* ppa_get_background_buffer(void) { return s_bg_buf; }

int ppa_get_last_decoded_height(void) {
    // RGB565: height = total_bytes / (width * 2)
    // width 固定 480 (DISPLAY_W)
    return (int)(s_last_decoded_size / (DISPLAY_W * 2));
}

// ─── PPA Client ───────────────────────────────────────────────

// 双缓冲每份 stride：全屏 768KB + 64KB 冗余（cover 直通裁切跳过顶部 ≤64 行时
// canvas 指针后移仍在本份内，不越界到下一份）
#define BUF_STRIDE (FRAME_SIZE_RGB565 + 65536)

bool ppa_init(void) {
    // 双缓冲：fg/comp 各两份交替使用——canvas 显示上一帧 buffer 的同时，
    // 本帧解码/blend 写另一份，消除单缓冲读写撕裂（cover 黑屏闪烁根因）
    // 注意：用 heap_caps_aligned_alloc 保证 64B 对齐（手动 +63&~63 后 free 会
    // 指向非块首 → TLSF block_next 断言崩溃）
    s_fg_buf = (uint8_t*)heap_caps_aligned_alloc(64, 2 * BUF_STRIDE, MALLOC_CAP_SPIRAM);
    s_alpha_buf = (uint8_t*)heap_caps_aligned_alloc(64, FRAME_SIZE_ARGB, MALLOC_CAP_SPIRAM);
    s_comp_buf = (uint8_t*)heap_caps_aligned_alloc(64, 2 * BUF_STRIDE, MALLOC_CAP_SPIRAM);
    if (!s_fg_buf || !s_alpha_buf || !s_comp_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        return false;
    }
    memset(s_fg_buf, 0, 2 * BUF_STRIDE);
    memset(s_comp_buf, 0, 2 * BUF_STRIDE);

    // Register PPA BLEND client
    ppa_client_config_t client_cfg = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ret = ppa_register_client(&client_cfg, &s_ppa_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA client register failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "PPA BLEND client registered");
    return true;
}

// ─── Frame Cache + MJPEG ──────────────────────────────────────

#define MAX_CACHE 260
static uint8_t *s_jpg_cache[MAX_CACHE];
static size_t s_jpg_cache_size[MAX_CACHE];
static int s_cache_count = 0;
static bool s_use_mjpeg = false;

// ─── 第四槽：Profile 缓存（独立，不争抢其他槽）───
static uint8_t *s_profile_cache[MAX_CACHE];
static size_t s_profile_sizes[MAX_CACHE];
static int s_profile_count = 0;
static bool s_profile_loaded = false;
static bool s_use_profile = false;  // 播放源：false=active, true=profile

// Alpha 遮罩缓存（RLE 压缩, ~10KB/帧）
static uint8_t *s_mask_cache[MAX_CACHE];
static size_t  s_mask_cache_size[MAX_CACHE];
static bool    s_use_alpha = false;

// 双缓冲：后台异步预加载下一个表情
static volatile bool s_preload_cancel = false;  // 取消正在跑的预加载（防释放竞态）
static uint8_t *s_pending_cache[MAX_CACHE];
static size_t  s_pending_sizes[MAX_CACHE];
static uint8_t *s_pending_mask[MAX_CACHE];
static size_t  s_pending_mask_sizes[MAX_CACHE];
static bool    s_pending_has_alpha = false;
static int     s_pending_count = 0;
static bool    s_pending_ready = false;
static TaskHandle_t s_preload_task = NULL;

// ─── 三槽缓存：Cover 槽（独立，不被 emotion 换出）───
static uint8_t *s_cover_cache[MAX_CACHE];
static uint8_t *s_cover_mask[MAX_CACHE];
static size_t  s_cover_sizes[MAX_CACHE];
static size_t  s_cover_mask_sizes[MAX_CACHE];
static int     s_cover_count = 0;
static int     s_cover_location = 0;  // 0=无, 1=在cover槽, 2=在active
static char    s_cover_agent[256] = {0};  // cover 所属角色路径

void ppa_preload_frames(const char *paths[], int count) {
    if (count > MAX_CACHE) count = MAX_CACHE;
    int loaded = 0;
    for (int i = 0; i < count; i++) {
        FILE *fp = fopen(paths[i], "rb");
        if (!fp) { s_jpg_cache[i] = NULL; continue; }
        fseek(fp, 0, SEEK_END);
        size_t sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (sz == 0 || sz > 512 * 1024) { fclose(fp); s_jpg_cache[i] = NULL; continue; }

        uint8_t *buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!buf) { fclose(fp); s_jpg_cache[i] = NULL; continue; }

        // Read with retry (SDMMC 0x107 from C6 SDIO polling)
        bool ok = false;
        for (int r = 0; r < 20; r++) {
            clearerr(fp);            // Clear stdio error
            fseek(fp, 0, SEEK_SET);  // Reset to start of file
            if (fread(buf, 1, sz, fp) == sz) { ok = true; break; }
            if (r == 0) ESP_LOGW(TAG, "Frame %d fread retry (0x107)...", i);
            vTaskDelay(pdMS_TO_TICKS(50));  // 50ms gap — let C6 polling finish
        }
        clearerr(fp);  // 确保 FATFS 正确释放文件描述符
        fclose(fp);

        if (ok) {
            s_jpg_cache[loaded] = buf;
            s_jpg_cache_size[loaded] = sz;
            loaded++;
        } else {
            free(buf);
            ESP_LOGE(TAG, "Frame %d failed after retries", i);
        }
    }
    s_cache_count = loaded;
    ESP_LOGI(TAG, "Preloaded %d/%d frames to PSRAM", loaded, count);
}

int ppa_get_cache_count(void) { return s_cache_count; }

// 前向声明（逐帧fread，支持大文件）
static int load_mjpeg_into(const char *path, uint8_t *cache[], size_t sizes[], int max_count, const char *stage);
static bool load_mask_file(const char *path, uint8_t *mask_cache[], size_t mask_sizes[], int expected_fc);

int ppa_preload_mjpeg(const char *path) {
    // 保护：如果 cover 在 active 中，标记丢失
    if (s_cover_location == 2) s_cover_location = 0;

    // 先释放旧帧缓存，避免 PSRAM 碎片化
    for (int i = 0; i < s_cache_count; i++) {
        if (s_jpg_cache[i]) { free(s_jpg_cache[i]); s_jpg_cache[i] = NULL; }
        if (s_mask_cache[i]) { free(s_mask_cache[i]); s_mask_cache[i] = NULL; }
    }
    s_cache_count = 0;
    s_use_alpha = false;

    // 复用逐帧加载逻辑（无整文件缓冲，支持大文件）
    s_cache_count = load_mjpeg_into(path, s_jpg_cache, s_jpg_cache_size, MAX_CACHE, "表情");
    if (s_cache_count == 0) return 0;

    // 尝试加载配套 .mask 文件
    char mask_path[320];
    snprintf(mask_path, sizeof(mask_path), "%s", path);
    char *dot = strrchr(mask_path, '.');
    if (dot) strcpy(dot, ".mask");
    if (load_mask_file(mask_path, s_mask_cache, s_mask_cache_size, s_cache_count)) {
        s_use_alpha = true;
    }

    return s_cache_count;
}

// ── 内部：加载MJPEG到指定缓冲区（顺序流式读取，零fseek）──
static int load_mjpeg_into(const char *path,
                           uint8_t *cache[], size_t sizes[], int max_count,
                           const char *stage) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "Cannot open %s", path); return 0; }

    // Step 1: 读取头部（帧数 + 偏移表）
    uint32_t frame_count;
    if (fread(&frame_count, 4, 1, fp) != 1 || frame_count == 0 || frame_count > (uint32_t)max_count) {
        ESP_LOGE(TAG, "Bad header: count=%lu", (unsigned long)frame_count);
        fclose(fp); return 0;
    }

    uint32_t offsets[MAX_CACHE];
    for (int i = 0; i < (int)frame_count; i++) {
        if (fread(&offsets[i], 4, 1, fp) != 1) { fclose(fp); return 0; }
    }
    ppa_prog(stage, 0);

    // Step 2: 跳到第一帧位置，帧在文件中连续存储 — 顺序读取，零 seek！
    fseek(fp, offsets[0], SEEK_SET);

    // 用较大 chunk 顺序读，提升 SD 吞吐
    #define CHUNK_SIZE 524288  // 512KB — 文件小，大chunk提速
    uint8_t *chunk = (uint8_t*)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk) { fclose(fp); return 0; }

    uint32_t total_kb = 0;
    int loaded = 0;
    size_t chunk_pos = 0, chunk_filled = 0;

    for (int i = 0; i < (int)frame_count && loaded < max_count; i++) {
        if (s_preload_cancel) break;  // 预加载被取消（释放方等待退出，防 free 竞态）
        size_t start = offsets[i];
        size_t end = (i < (int)frame_count - 1) ? offsets[i + 1] : (size_t)-1;
        if (end == (size_t)-1) { fseek(fp, 0, SEEK_END); end = ftell(fp); }
        if (end <= start || end - start > 512 * 1024) continue;
        size_t sz = end - start;

        uint8_t *buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!buf) break;

        // 从 chunk 缓冲区拷贝（不足时先补充）
        size_t copied = 0;
        while (copied < sz) {
            if (chunk_pos >= chunk_filled) {
                size_t to_read = CHUNK_SIZE;
                chunk_filled = fread(chunk, 1, to_read, fp);
                chunk_pos = 0;
                if (chunk_filled == 0) break;
            }
            size_t avail = chunk_filled - chunk_pos;
            size_t need = sz - copied;
            size_t n = avail < need ? avail : need;
            memcpy(buf + copied, chunk + chunk_pos, n);
            chunk_pos += n;
            copied += n;
        }

        if (copied < sz) { free(buf); break; }

        cache[loaded] = buf;
        sizes[loaded] = sz;
        loaded++;
        total_kb += (uint32_t)(sz / 1024);
        ppa_prog(stage, (int)(loaded * 100 / frame_count));  // 帧级百分比
    }
    free(chunk);
    fclose(fp);

    ESP_LOGI(TAG, "%s: %d/%lu frames, %u KB PSRAM (streaming, 0 seek)",
             path, loaded, (unsigned long)frame_count, total_kb);
    ppa_prog(stage, 100);
    return loaded;
}

// ── 异步预加载到后备缓冲区 ──
static void preload_task(void *arg) {
    const char *path = (const char*)arg;
    // 注意：s_preload_cancel 在这里绝不能清零！
    // 任务创建与首次运行之间存在窗口：释放方可能已设 cancel 并等待退出，
    // 任务一跑就清零会把取消信号吃掉 → 释放方超时后 free 槽，任务继续写入 → PSRAM 幽灵占用
    // 清零点已移到 ppa_preload_mjpeg_async（任务创建之前）
    // 先清空后备缓冲区
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending_cache[i]) free(s_pending_cache[i]);
        if (s_pending_mask[i]) { free(s_pending_mask[i]); s_pending_mask[i] = NULL; }
    }
    s_pending_count = 0;
    s_pending_ready = false;
    s_pending_has_alpha = false;

    int count = load_mjpeg_into(path, s_pending_cache, s_pending_sizes, MAX_CACHE, "表情");
    if (count > 0 && !s_preload_cancel) {
        // 尝试加载配套 .mask
        char mask_path[320];
        snprintf(mask_path, sizeof(mask_path), "%s", path);
        char *dot = strrchr(mask_path, '.');
        if (dot) strcpy(dot, ".mask");
        if (load_mask_file(mask_path, s_pending_mask, s_pending_mask_sizes, count))
            s_pending_has_alpha = true;

        s_pending_count = count;
        s_pending_ready = true;
        ESP_LOGI(TAG, "Pending emotion ready: %d frames (%s)", count, path);
    }
    if (s_preload_cancel) {
        // 取消：清掉本任务已加载的部分帧（释放方随后也会清）
        for (int i = 0; i < count; i++) {
            if (s_pending_cache[i]) { free(s_pending_cache[i]); s_pending_cache[i] = NULL; }
        }
        s_pending_count = 0;
        s_pending_ready = false;
        ESP_LOGI(TAG, "Preload cancelled (%d frames discarded)", count);
    }
    s_preload_cancel = false;  // 任务退出时复位——否则残留 true 会让后续
                               // cover/表情同步加载首帧即 break（0 帧 → 立绘切换卡死）
    s_preload_task = NULL;
    vTaskDelete(NULL);
}

void ppa_preload_mjpeg_async(const char *path) {
    if (s_preload_task) {
        // 上一个预加载还在跑，先等它完成
        return;
    }
    // 复制路径字符串（任务可能在函数返回后才用）
    static char s_path_buf[256];
    strncpy(s_path_buf, path, sizeof(s_path_buf) - 1);
    s_preload_cancel = false;  // 创建前清零：任务首次运行时不会吃掉释放方设的 cancel
    xTaskCreate(preload_task, "mjpeg_preload", 8192, (void*)s_path_buf, 2, &s_preload_task);
}

int ppa_swap_emotion(void) {
    if (!s_pending_ready) return 0;

    // 交换活跃 ↔ 后备（JPEG 缓存 + 遮罩）
    for (int i = 0; i < MAX_CACHE; i++) {
        uint8_t *tmp = s_jpg_cache[i];
        s_jpg_cache[i] = s_pending_cache[i];
        s_pending_cache[i] = tmp;
        size_t stmp = s_jpg_cache_size[i];
        s_jpg_cache_size[i] = s_pending_sizes[i];
        s_pending_sizes[i] = stmp;

        tmp = s_mask_cache[i];
        s_mask_cache[i] = s_pending_mask[i];
        s_pending_mask[i] = tmp;
        stmp = s_mask_cache_size[i];
        s_mask_cache_size[i] = s_pending_mask_sizes[i];
        s_pending_mask_sizes[i] = stmp;
    }
    int new_count = s_pending_count;
    s_pending_count = s_cache_count;
    s_cache_count = new_count;
    s_use_alpha = s_pending_has_alpha;
    s_pending_has_alpha = false;
    s_pending_ready = true;  // 旧活跃已变后备，数据有效，可直接 swap

    ESP_LOGI(TAG, "Swapped emotion: %d frames active, %d pending",
             s_cache_count, s_pending_count);
    return s_cache_count;
}

// ─── 三槽缓存：Cover 槽操作 ────────────────────────

int ppa_preload_cover(const char *path) {
    // 释放旧 cover
    for (int i = 0; i < s_cover_count; i++) {
        if (s_cover_cache[i]) { free(s_cover_cache[i]); s_cover_cache[i] = NULL; }
        if (s_cover_mask[i]) { free(s_cover_mask[i]); s_cover_mask[i] = NULL; }
    }
    s_cover_count = 0;
    s_cover_location = 0;

    s_cover_count = load_mjpeg_into(path, s_cover_cache, s_cover_sizes, MAX_CACHE, "封面");
    if (s_cover_count == 0) return 0;

    char mask_path[320];
    snprintf(mask_path, sizeof(mask_path), "%s", path);
    char *dot = strrchr(mask_path, '.');
    if (dot) strcpy(dot, ".mask");
    load_mask_file(mask_path, s_cover_mask, s_cover_mask_sizes, s_cover_count);

    s_cover_location = 1;  // cover 在槽里
    // 记录 cover 所属角色（提取 agent 根路径）
    strncpy(s_cover_agent, path, sizeof(s_cover_agent) - 1);
    char *cover_dir = strstr(s_cover_agent, "/cover/");
    if (cover_dir) *cover_dir = '\0';  // 截断到 /operator/.../Name
    ESP_LOGI(TAG, "Cover cached: %d frames (%s)", s_cover_count, s_cover_agent);
    return s_cover_count;
}

// ── 异步预加载 cover ──
static TaskHandle_t s_cover_preload_task = NULL;

static void cover_preload_task(void *arg) {
    const char *path = (const char*)arg;
    ppa_preload_cover(path);
    s_cover_preload_task = NULL;
    vTaskDelete(NULL);
}

void ppa_preload_cover_async(const char *path) {
    // 等旧任务完成
    if (s_cover_preload_task) {
        while (s_cover_preload_task) vTaskDelay(pdMS_TO_TICKS(20));
    }
    static char s_cover_path_buf[320];
    strncpy(s_cover_path_buf, path, sizeof(s_cover_path_buf) - 1);
    xTaskCreate(cover_preload_task, "cover_preload", 8192, (void*)s_cover_path_buf, 2, &s_cover_preload_task);
}

// 等待 cover 异步加载完成
void ppa_wait_cover_preload(void) {
    while (s_cover_preload_task) vTaskDelay(pdMS_TO_TICKS(20));
}

// 等待 pending 异步加载完成
void ppa_wait_pending_preload(void) {
    while (s_preload_task) vTaskDelay(pdMS_TO_TICKS(20));
}

void ppa_release_jpeg_engine(void) {
    if (!ppa_jpg_lock()) return;   // 2026-10-06 拿不到锁(解码悬挂)不等待,跳过删除
    if (s_jpg_handle) {
        jpeg_del_decoder_engine(s_jpg_handle);
        s_jpg_handle = NULL;
    }
    ppa_jpg_unlock();
}

void ppa_restore_jpeg_engine(void) {
    // PPA 会在下次 composite_frame 时 lazy 重建
}

// 用 PPA 的 JPEG 引擎解码单个文件到 RGB565
uint8_t* ppa_decode_jpeg_to_rgb565(const char *path, int *out_w, int *out_h) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size == 0 || size > 512 * 1024) { fclose(fp); return NULL; }

    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!jpg_data) { fclose(fp); return NULL; }
    fread(jpg_data, 1, size, fp);
    fclose(fp);

    jpeg_decode_picture_info_t pic_info;
    if (jpeg_decoder_get_info(jpg_data, size, &pic_info) != ESP_OK) { free(jpg_data); return NULL; }

    size_t tx_size = (size + 63) & ~63;
    uint8_t *tx_buf = (uint8_t*)heap_caps_malloc(tx_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!tx_buf) { free(jpg_data); return NULL; }
    memcpy(tx_buf, jpg_data, size);
    free(jpg_data);

    uint32_t aw = (pic_info.width + 15) & ~15;
    uint32_t ah = (pic_info.height + 15) & ~15;
    size_t out_size = (aw * ah * 2 + 63) & ~63;
    uint8_t *rgb_buf = (uint8_t*)heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!rgb_buf) { free(tx_buf); return NULL; }

    // 复用 PPA JPEG 引擎（需空闲，调用者保证视频已停）
    if (!ppa_jpg_lock()) { free(tx_buf); free(rgb_buf); return NULL; }   // 2026-10-06 超时不等待
    jpeg_decoder_handle_t handle = NULL;
    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 1000 };
    if (jpeg_new_decoder_engine(&eng_cfg, &handle) != ESP_OK) {
        ppa_jpg_unlock();
        free(tx_buf); free(rgb_buf); return NULL;
    }
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t decoded;
    esp_err_t ret = jpeg_decoder_process(handle, &cfg, tx_buf, tx_size, rgb_buf, out_size, &decoded);
    jpeg_del_decoder_engine(handle);
    ppa_jpg_unlock();
    free(tx_buf);

    if (ret != ESP_OK) { free(rgb_buf); return NULL; }
    *out_w = (int)aw;
    *out_h = (int)ah;
    return rgb_buf;
}

int ppa_swap_to_cover(void) {
    if (s_cover_location == 0) return 0;  // 没有 cover

    // 双向交换 active ↔ cover slot
    for (int i = 0; i < MAX_CACHE; i++) {
        uint8_t *tmp = s_jpg_cache[i];
        s_jpg_cache[i] = s_cover_cache[i];
        s_cover_cache[i] = tmp;
        size_t stmp = s_jpg_cache_size[i];
        s_jpg_cache_size[i] = s_cover_sizes[i];
        s_cover_sizes[i] = stmp;

        tmp = s_mask_cache[i];
        s_mask_cache[i] = s_cover_mask[i];
        s_cover_mask[i] = tmp;
        stmp = s_mask_cache_size[i];
        s_mask_cache_size[i] = s_cover_mask_sizes[i];
        s_cover_mask_sizes[i] = stmp;
    }
    int new_count = s_cover_count;
    s_cover_count = s_cache_count;
    s_cache_count = new_count;
    // 翻转位置：slot(1)↔active(2)
    s_cover_location = (s_cover_location == 1) ? 2 : 1;

    ESP_LOGI(TAG, "Swapped cover (loc=%d): %d frames active, %d in slot",
             s_cover_location, s_cache_count, s_cover_count);
    return s_cache_count;
}

// 把 active 帧整体搬进 cover 槽（槽必须为空），active 清空。
// 场景：profile 打开时 release_expendable 释放了 cover 槽，退出 profile 后
// cover 帧仍在 active 播放；此时切 expression，若没有槽可用，
// mode_switch_task 的"复用"逻辑会误把 cover 帧当表情复用 → 搬走后 active=0 → 强制加载表情
int ppa_save_active_to_cover(const char *agent_path) {
    if (s_cover_location != 0) return 0;  // 槽非空：调用方应走 swap_to_cover
    for (int i = 0; i < s_cache_count; i++) {
        s_cover_cache[i] = s_jpg_cache[i];
        s_jpg_cache[i] = NULL;
        s_cover_sizes[i] = s_jpg_cache_size[i];
        s_cover_mask[i] = s_mask_cache[i];
        s_mask_cache[i] = NULL;
        s_cover_mask_sizes[i] = s_mask_cache_size[i];
    }
    s_cover_count = s_cache_count;
    s_cache_count = 0;
    s_use_alpha = false;  // active 已清空，alpha 标志一并复位（composite 靠 mask 数组判空兜底）
    s_cover_location = 1;  // cover 在槽里
    if (agent_path && agent_path[0]) {
        strncpy(s_cover_agent, agent_path, sizeof(s_cover_agent) - 1);
        s_cover_agent[sizeof(s_cover_agent) - 1] = '\0';
    }
    ESP_LOGI(TAG, "Active→cover slot saved: %d frames (%s)", s_cover_count, s_cover_agent);
    return s_cover_count;
}

// 释放 cover 槽中的旧数据（swap 后 slot 被污染，清掉避免下次 save-cover 误复用）
void ppa_free_cover_slot(void) {
    for (int i = 0; i < s_cover_count; i++) {
        if (s_cover_cache[i]) { free(s_cover_cache[i]); s_cover_cache[i] = NULL; }
        if (s_cover_mask[i])  { free(s_cover_mask[i]);  s_cover_mask[i]  = NULL; }
    }
    s_cover_count = 0;
}

bool ppa_has_cover(void) { return s_cover_location != 0; }
const char* ppa_get_cover_agent(void) { return s_cover_agent; }

void ppa_unload_cover(void) {
    int loc = s_cover_location;
    uint8_t **cache = (loc == 1) ? s_cover_cache : s_jpg_cache;
    uint8_t **mask  = (loc == 1) ? s_cover_mask  : s_mask_cache;
    int count = (loc == 1) ? s_cover_count : s_cache_count;
    for (int i = 0; i < count; i++) {
        if (cache[i]) { free(cache[i]); cache[i] = NULL; }
        if (mask[i])  { free(mask[i]);  mask[i]  = NULL; }
    }
    if (loc == 2) s_cache_count = 0;
    s_cover_count = 0;
    s_cover_location = 0;
    s_cover_agent[0] = '\0';
    ESP_LOGI(TAG, "Cover cache unloaded");
}

// ─── 第四槽：Profile 操作 ──────────────────────────

int ppa_preload_profile(const char *path) {
    for (int i = 0; i < s_profile_count; i++) {
        if (s_profile_cache[i]) { free(s_profile_cache[i]); s_profile_cache[i] = NULL; }
    }
    s_profile_count = load_mjpeg_into(path, s_profile_cache, s_profile_sizes, MAX_CACHE, "资料");
    s_profile_loaded = (s_profile_count > 0);
    if (s_profile_loaded) ESP_LOGI(TAG, "Profile cached: %d frames (%s)", s_profile_count, path);
    return s_profile_count;
}

void ppa_use_profile_cache(bool use) { s_use_profile = use; }

void ppa_free_profile_slot(void) {
    for (int i = 0; i < s_profile_count; i++) {
        if (s_profile_cache[i]) { free(s_profile_cache[i]); s_profile_cache[i] = NULL; }
    }
    s_profile_count = 0;
    s_profile_loaded = false;
}

// 等待正在跑的预加载任务退出（取消 + 轮询，防与 pending 释放竞态 → TLSF 破坏）
static void preload_wait_cancel(void) {
    if (s_preload_task) {
        s_preload_cancel = true;
        // 最多 10s：任务每帧检查 cancel（单帧 SD 读 ~30ms），正常应在百 ms 内退出
        for (int i = 0; i < 1000 && s_preload_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_preload_task) {
            ESP_LOGW(TAG, "preload task still running after 10s wait! (SD stuck?)");
        } else {
            s_preload_cancel = false;  // 双保险：任务退出后复位（残留 true 会毒死后续同步加载）
        }
    }
}

// 释放可牺牲缓存但保留 active 帧 + mask（profile 打开/退出可秒切）
void ppa_release_expendable_caches(void) {
    preload_wait_cancel();
    // pending 槽
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending_cache[i]) { free(s_pending_cache[i]); s_pending_cache[i] = NULL; }
    }
    s_pending_count = 0;
    s_pending_ready = false;      // 防 swap_emotion 把已释放的 NULL 数组换进 active
    s_pending_has_alpha = false;
    // cover 槽
    ppa_free_cover_slot();
    s_cover_location = 0;
    // profile 槽
    ppa_free_profile_slot();
    // streaming 模式文件
    mjpeg_close();
    s_use_mjpeg = false;
    // alpha 混合缓冲（1.7MB，惰性重建：expression 模式 mask 合成时按需分配）
    if (s_alpha_buf) { free(s_alpha_buf); s_alpha_buf = NULL; }
    s_alpha_oom = false;
    ESP_LOGI(TAG, "Expendable caches released (active frames kept)");
}

void ppa_release_playback_caches(void) {
    preload_wait_cancel();
    // active 槽（含 mask）
    for (int i = 0; i < s_cache_count; i++) {
        if (s_jpg_cache[i]) { free(s_jpg_cache[i]); s_jpg_cache[i] = NULL; }
        if (s_mask_cache[i]) { free(s_mask_cache[i]); s_mask_cache[i] = NULL; }
    }
    s_cache_count = 0;
    s_use_alpha = false;
    // cover 槽
    ppa_free_cover_slot();
    s_cover_location = 0;
    // profile 槽
    ppa_free_profile_slot();
    // pending 槽
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending_cache[i]) { free(s_pending_cache[i]); s_pending_cache[i] = NULL; }
    }
    s_pending_count = 0;
    s_pending_ready = false;
    s_pending_has_alpha = false;
    // streaming 模式文件（固定帧缓冲保留，后续重新 open 复用）
    mjpeg_close();
    s_use_mjpeg = false;
    ESP_LOGI(TAG, "Playback caches released (PSRAM reclaimed)");
}

bool ppa_open_mjpeg(const char *path, int *out_frame_count) {
    if (!mjpeg_open(path)) return false;
    s_use_mjpeg = true;
    *out_frame_count = mjpeg_get_frame_count();
    ESP_LOGI(TAG, "MJPEG mode: %s (%d frames)", path, *out_frame_count);
    return true;
}

void ppa_close_mjpeg(void) {
    mjpeg_close();
    s_use_mjpeg = false;
}

// ─── Alpha Mask ─────────────────────────────────────────────────
// .mask 文件格式: [4B fc][N×4B offsets][RLE data: 2B count, 1B value ...]

static bool load_mask_file(const char *path, uint8_t *mask_cache[], size_t mask_sizes[], int expected_fc) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    uint32_t fc;
    if (fread(&fc, 4, 1, fp) != 1 || (int)fc != expected_fc) { fclose(fp); return false; }

    uint32_t offsets[MAX_CACHE];
    for (int i = 0; i < (int)fc; i++)
        if (fread(&offsets[i], 4, 1, fp) != 1) { fclose(fp); return false; }

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);

    int loaded = 0;
    for (int i = 0; i < (int)fc; i++) {
        size_t start = offsets[i];
        size_t end = (i < (int)fc - 1) ? offsets[i + 1] : file_size;
        if (end <= start || end - start > 64 * 1024) continue;
        size_t sz = end - start;
        mask_cache[i] = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!mask_cache[i]) break;
        fseek(fp, start, SEEK_SET);
        fread(mask_cache[i], 1, sz, fp);
        mask_sizes[i] = sz;
        loaded++;
    }
    fclose(fp);
    ESP_LOGI(TAG, "Mask loaded: %s (%d/%lu frames)", path, loaded, (unsigned long)fc);
    return loaded > 0;
}

// RLE 解码 + RGB565→ARGB8888（输出到 s_alpha_buf）
static void apply_rle_mask(uint8_t *mask_data, size_t mask_size, uint16_t *fg_rgb565, int total_px) {
    uint32_t *argb = (uint32_t*)s_alpha_buf;
    size_t pos = 0;
    int px = 0;
    while (pos + 3 <= mask_size && px < total_px) {
        uint16_t cnt = *(uint16_t*)(mask_data + pos); pos += 2;
        uint8_t  val = mask_data[pos]; pos += 1;
        if (cnt == 0) continue;
        if (val == 0) {
            // 透明 → ARGB8888 alpha=0x00
            memset(argb + px, 0, cnt * 4);
        } else {
            // 不透明 → RGB565 转 ARGB8888 (alpha=0xFF)
            for (int i = 0; i < (int)cnt && px + i < total_px; i++) {
                uint16_t rgb = fg_rgb565[px + i];
                uint8_t r = ((rgb >> 11) & 0x1F) << 3;
                uint8_t g = ((rgb >> 5) & 0x3F) << 2;
                uint8_t b = (rgb & 0x1F) << 3;
                argb[px + i] = ((uint32_t)0xFF << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
        px += cnt;
    }
    // 剩余像素→透明
    if (px < total_px) memset(argb + px, 0, (total_px - px) * 4);
}

// ─── Composite & Return Output Buffer ─────────────────────────

uint8_t* ppa_composite_frame(int frame_index) {
    if (!s_fg_buf || !s_comp_buf) return NULL;

    // 解码失败冷却期：跳过提交，等 PPA/DMA2D 硬件复位（见 decode 失败路径注释）
    if (s_decode_cool_down > 0) {
        s_decode_cool_down--;
        return NULL;
    }

    // 每 30 帧做一次堆完整性检查（定位内存损坏源，损坏时打印告警）
    static int s_integrity_ticks = 0;
    if (++s_integrity_ticks >= 30) {
        s_integrity_ticks = 0;
        if (!heap_caps_check_integrity_all(false)) {
            ESP_LOGE(TAG, "HEAP CORRUPTED! (detected at frame %d)", frame_index);
        }
    }

    // 双缓冲：本帧写 idx 槽位，canvas 仍显示上一帧的另一个槽位
    s_frame_idx ^= 1;
    uint8_t *fg = s_fg_buf + s_frame_idx * BUF_STRIDE;
    uint8_t *comp = s_comp_buf + s_frame_idx * BUF_STRIDE;

    // ── Step 1: Get JPEG data ──
    size_t jpg_size;
    uint8_t *jpg_data = NULL;
    bool need_free = false;

    if (s_use_mjpeg) {
        // 固定缓冲（MjpegPlayer 内部复用），不再 free
        if (!mjpeg_get_frame(frame_index, &jpg_data, &jpg_size)) return NULL;
    } else if (s_use_profile) {
        if (frame_index < 0 || frame_index >= s_profile_count || !s_profile_cache[frame_index]) return NULL;
        jpg_size = s_profile_sizes[frame_index];
        jpg_data = s_profile_cache[frame_index];
    } else {
        if (frame_index < 0 || frame_index >= s_cache_count || !s_jpg_cache[frame_index]) return NULL;
        jpg_size = s_jpg_cache_size[frame_index];
        jpg_data = s_jpg_cache[frame_index];
    }

    // 2026-10-04 解码前校验 JPEG：坏帧/截断帧进硬件会让 2D-DMA 通道悬挂，
    // 下一帧 dma2d_connect 忙等复位 → IWDT 重启（第二台设备封面第 70 帧实测崩溃）。
    jpeg_decode_picture_info_t jinfo;
    if (jpeg_decoder_get_info(jpg_data, jpg_size, &jinfo) != ESP_OK) {
        ESP_LOGW(TAG, "corrupt jpeg frame %d (%u bytes), cooldown 30", frame_index, (unsigned)jpg_size);
        s_decode_cool_down = 30;
        return NULL;
    }

    jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t tx_size;
    // 输入缓冲一次分配后复用（每帧 alloc/free 会造成 PSRAM 碎片化，
    // 索引页缩略图加载后大块分配失败返回 NULL → memcpy(NULL) 崩溃）
    if (!s_tx_buf || s_tx_cap < jpg_size) {
        if (s_tx_buf) { free(s_tx_buf); s_tx_buf = NULL; s_tx_cap = 0; }
        s_tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpg_size, &tx_cfg, &tx_size);
        if (!s_tx_buf) {
            ESP_LOGW(TAG, "tx_buf alloc failed (%u bytes), drop frame", (unsigned)jpg_size);
            if (need_free) free(jpg_data);
            return NULL;  // 丢帧不崩，下一帧重试
        }
        s_tx_cap = jpg_size;
    } else {
        tx_size = s_tx_cap;
    }
    uint8_t *tx_buf = s_tx_buf;
    memcpy(tx_buf, jpg_data, jpg_size);
    if (need_free) free(jpg_data);

    if (!ppa_jpg_lock()) {   // 2026-10-06 超时不等待(悬挂保护)
        ESP_LOGE(TAG, "cover decode lock timeout");
        return NULL;
    }
    if (!s_jpg_handle) {
        jpeg_new_decoder_engine(&s_jpg_eng_cfg, &s_jpg_handle);
    }

    uint32_t decoded_size;
    // 解码前清零：帧小于全屏（480×800）时，未覆盖区域防上一帧残留（白边）
    memset(fg, 0, FRAME_SIZE_RGB565);
    esp_err_t ret = jpeg_decoder_process(s_jpg_handle, &s_jpg_cfg_rgb,
                                          tx_buf, tx_size,
                                          fg, FRAME_SIZE_RGB565,
                                          &decoded_size);
    ppa_jpg_unlock();
    // tx_buf 为复用缓冲，不释放
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        // 数据流损坏会让 PPA 硬件通道悬挂（实测表情坏帧连败后 DMA2D assert：
        // _dma2d_default_rx_isr FSM not idle）。对策：冷却 30 帧（约 1-2 秒）
        // 期间不提交任何 PPA 任务，等硬件自然复位（PPA 驱动内部有超时恢复）。
        // 注意：不要 jpeg_del_decoder_engine 重建——重建时 dma2d_connect 在
        // 硬件未完全释放时会死等复位（实测 Interrupt wdt timeout panic）。
        s_decode_cool_down = 30;
        return NULL;
    }
    s_last_decoded_size = decoded_size;

    // ── Step 2: Apply alpha mask → ARGB8888（alpha 缓冲惰性分配：cover 播放期不占用）──
    uint8_t *fg_for_blend = fg;
    ppa_blend_color_mode_t fg_cm = PPA_BLEND_COLOR_MODE_RGB565;

    if (!s_use_profile && s_use_alpha && s_mask_cache[frame_index]) {
        if (!s_alpha_buf && !s_alpha_oom) {
            // 惰性分配（同样用 aligned_alloc，free 指针即块首）
            s_alpha_buf = (uint8_t*)heap_caps_aligned_alloc(64, FRAME_SIZE_ARGB, MALLOC_CAP_SPIRAM);
            if (!s_alpha_buf) {
                // 第一次失败：释放 cover 槽（~3.5MB，播放期间闲置）再试一次。
                // mask 混合正确性 > cover 秒切（切回 cover 时重新从 SD 加载即可）
                ppa_free_cover_slot();
                s_cover_location = 0;
                s_alpha_buf = (uint8_t*)heap_caps_aligned_alloc(64, FRAME_SIZE_ARGB, MALLOC_CAP_SPIRAM);
                if (s_alpha_buf) {
                    ESP_LOGI(TAG, "alpha buf alloc retried after cover slot release (PSRAM free %u KB)",
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
                }
            }
            if (!s_alpha_buf) {
                s_alpha_oom = true;  // 不再每帧重试（日志刷屏+无效开销），释放缓存时复位
                ESP_LOGW(TAG, "alpha buf alloc failed (mask blending degraded, PSRAM free %u KB)",
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            }
        }
        if (s_alpha_buf) {
            apply_rle_mask(s_mask_cache[frame_index], s_mask_cache_size[frame_index],
                           (uint16_t*)fg, DISPLAY_W * DISPLAY_H);
            fg_for_blend = s_alpha_buf;
            fg_cm = PPA_BLEND_COLOR_MODE_ARGB8888;
        }
    }

    // ── Step 3: PPA BLEND (or pass-through if no background) ──
    if (!s_ppa_client || !s_bg_buf) {
        return fg_for_blend;
    }
    ppa_in_pic_blk_config_t bg_cfg = {};
    bg_cfg.buffer = s_bg_buf;
    bg_cfg.pic_w = DISPLAY_W; bg_cfg.pic_h = DISPLAY_H;
    bg_cfg.block_w = DISPLAY_W; bg_cfg.block_h = DISPLAY_H;
    bg_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    ppa_in_pic_blk_config_t fg_cfg = {};
    fg_cfg.buffer = fg_for_blend;
    fg_cfg.pic_w = DISPLAY_W; fg_cfg.pic_h = DISPLAY_H;
    fg_cfg.block_w = DISPLAY_W; fg_cfg.block_h = DISPLAY_H;
    fg_cfg.blend_cm = fg_cm;

    ppa_out_pic_blk_config_t out_cfg = {};
    out_cfg.buffer = comp;
    out_cfg.buffer_size = FRAME_SIZE_RGB565;
    out_cfg.pic_w = DISPLAY_W; out_cfg.pic_h = DISPLAY_H;
    out_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    ppa_blend_oper_config_t blend_cfg = {};
    blend_cfg.in_bg = bg_cfg;
    blend_cfg.in_fg = fg_cfg;
    blend_cfg.out = out_cfg;
    blend_cfg.mode = PPA_TRANS_MODE_BLOCKING;

    // 注意：必须同时检查 s_alpha_buf——OOM 降级时 fg 仍是 RGB565（无 alpha 通道），
    // 若仍走 alpha 配置（色键全关），红底不会被抠掉，整块矩形盖住背景（"背景变红"）
    if (s_use_alpha && s_alpha_buf) {
        // Alpha 混合：ARGB8888 逐像素 A 通道
        blend_cfg.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        blend_cfg.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        blend_cfg.bg_ck_en = false;
        blend_cfg.fg_ck_en = false;
    } else {
        // 色键模式：抠红底 R=[200,255]
        color_pixel_rgb888_data_t ck_low  = { .b = 0,   .g = 0,   .r = 200 };
        color_pixel_rgb888_data_t ck_high = { .b = 80,  .g = 80,  .r = 255 };
        color_pixel_rgb888_data_t ck_default = { .b = 0, .g = 0, .r = 0 };
        blend_cfg.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        blend_cfg.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        blend_cfg.bg_ck_en = false;
        blend_cfg.fg_ck_en = true;
        blend_cfg.fg_ck_rgb_low_thres = ck_low;
        blend_cfg.fg_ck_rgb_high_thres = ck_high;
        blend_cfg.ck_rgb_default_val = ck_default;
    }

    ret = ppa_do_blend(s_ppa_client, &blend_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA blend failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    return comp;
}

// ─── 横屏立牌：PC 导出端已转好 90°，设备端 480×800 竖帧直通（无旋转）───
// 帧数据来自独立预加载槽（PSRAM），播放零 SD 读（SD/SDIO 共享总线只有 ~620KB/s）
#define STANDEE_SIZE_RGB565 (DISPLAY_W * DISPLAY_H * 2)   // 480×800×2

static uint8_t *s_standee_cache[MAX_CACHE];
static size_t  s_standee_sizes[MAX_CACHE];
static int     s_standee_count = 0;
static char    s_standee_agent[256] = {0};   // standee 所属文件路径（重进秒切判定）

int ppa_preload_standee(const char *path) {
    // 同路径已缓存：秒返回（退出/重进零加载）
    if (s_standee_count > 0 && s_standee_agent[0] && strcmp(s_standee_agent, path) == 0)
        return s_standee_count;
    ppa_free_standee_slot();
    s_standee_count = load_mjpeg_into(path, s_standee_cache, s_standee_sizes, MAX_CACHE, "横屏立牌");
    if (s_standee_count > 0) {
        strncpy(s_standee_agent, path, sizeof(s_standee_agent) - 1);
        s_standee_agent[sizeof(s_standee_agent) - 1] = 0;
    }
    ESP_LOGI(TAG, "Standee cached: %d frames (%s)", s_standee_count, path);
    return s_standee_count;
}

void ppa_free_standee_slot(void) {
    for (int i = 0; i < s_standee_count; i++) {
        if (s_standee_cache[i]) { free(s_standee_cache[i]); s_standee_cache[i] = NULL; }
    }
    s_standee_count = 0;
    s_standee_agent[0] = 0;
}

uint8_t* ppa_composite_standee_frame(int frame_index) {
    if (!s_comp_buf) return NULL;
    if (s_decode_cool_down > 0) { s_decode_cool_down--; return NULL; }   // 2026-10-04 与主路径同款冷却
    if (frame_index < 0 || frame_index >= s_standee_count || !s_standee_cache[frame_index]) return NULL;

    // 双槽交替：本帧写 idx 槽，canvas 仍显示上一帧的另一个槽（与主路径同机制）
    s_frame_idx ^= 1;
    uint8_t *comp = s_comp_buf + s_frame_idx * BUF_STRIDE;

    // Step 1: 帧数据直接来自 standee 槽（零 SD 读）
    size_t jpg_size = s_standee_sizes[frame_index];
    uint8_t *jpg_data = s_standee_cache[frame_index];

    // Step 2: 输入缓冲（复用主管线 s_tx_buf，互斥使用）
    if (!s_tx_buf || s_tx_cap < jpg_size) {
        if (s_tx_buf) { free(s_tx_buf); s_tx_buf = NULL; s_tx_cap = 0; }
        jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
        size_t tx_size;
        s_tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpg_size, &tx_cfg, &tx_size);
        if (!s_tx_buf) { ESP_LOGW(TAG, "standee tx_buf alloc fail, drop frame"); return NULL; }
        s_tx_cap = jpg_size;
    }
    memcpy(s_tx_buf, jpg_data, jpg_size);

    // Step 3: JPEG 解码直通到 comp 槽（480×800 竖帧，与竖屏 cover 同构 → 18fps 同款性能）
    if (!ppa_jpg_lock()) {   // 2026-10-06 超时不等待(悬挂保护)
        ESP_LOGE(TAG, "standee decode lock timeout");
        s_decode_cool_down = 30;
        return NULL;
    }
    if (!s_jpg_handle) jpeg_new_decoder_engine(&s_jpg_eng_cfg, &s_jpg_handle);
    int64_t t_decode0 = esp_timer_get_time();
    uint32_t decoded_size = 0;
    esp_err_t ret = jpeg_decoder_process(s_jpg_handle, &s_jpg_cfg_rgb,
                                         s_tx_buf, jpg_size,
                                         comp, STANDEE_SIZE_RGB565,
                                         &decoded_size);
    ppa_jpg_unlock();
    int64_t t_decode1 = esp_timer_get_time();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "standee decode fail: %s", esp_err_to_name(ret)); s_decode_cool_down = 30; return NULL; }
    if (decoded_size != STANDEE_SIZE_RGB565) {
        ESP_LOGW(TAG, "standee frame size %u != %u（期望 PC 端转好 90° 的 480x800 竖帧导出）",
                 (unsigned)decoded_size, (unsigned)STANDEE_SIZE_RGB565);
        return NULL;   // 尺寸不符丢帧
    }
    // 计时日志（每 30 帧一次；%d：nano printf 不支持 %lld，之前打印出 "ld" 字面量）
    static int s_standee_tick = 0;
    if (++s_standee_tick >= 30) {
        s_standee_tick = 0;
        ESP_LOGI(TAG, "standee timing: decode %d ms",
                 (int)((t_decode1 - t_decode0) / 1000));
    }
    return comp;
}

// ─── 长背景（横屏长图竖屏切片，PPD 交互页用）───

#define LONG_BG_MAX_W 1440
#define LONG_BG_MAX_H 800

typedef struct {
    uint8_t *buf;   // NULL=空槽
    int w, h;       // 可见宽高（JPEG 真实尺寸）
    int stride;     // 行 stride（MCU pad 后宽度，像素）
} LongBgSlot;

static LongBgSlot s_long_bg[2];
static uint8_t *s_long_bg_tx = NULL;
static size_t s_long_bg_tx_cap = 0;

int ppa_long_bg_alloc(void) {
    if (s_long_bg[0].buf) return s_long_bg[1].buf ? 2 : 1;
    jpeg_decode_memory_alloc_cfg_t rx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t out_size;
    int slots = 0;
    for (int i = 0; i < 2; i++) {
        s_long_bg[i].buf = (uint8_t*)jpeg_alloc_decoder_mem(
            LONG_BG_MAX_W * LONG_BG_MAX_H * 2, &rx_cfg, &out_size);
        if (s_long_bg[i].buf) {
            s_long_bg[i].w = s_long_bg[i].h = s_long_bg[i].stride = 0;
            slots++;
        } else {
            break;
        }
    }
    ESP_LOGI(TAG, "long bg alloc: %d slots (PSRAM free %u KB)", slots,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return slots;
}

int ppa_long_bg_decode(const char *path, int slot) {
    if (slot < 0 || slot > 1 || !s_long_bg[slot].buf) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "long bg open fail: %s", path); return -1; }
    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size == 0 || size > 512 * 1024) {
        fclose(fp);
        ESP_LOGE(TAG, "long bg bad size: %s (%u B)", path, (unsigned)size);
        return -1;
    }
    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!jpg_data) { fclose(fp); return -1; }
    if (fread(jpg_data, 1, size, fp) != size) { fclose(fp); free(jpg_data); return -1; }
    fclose(fp);

    jpeg_decode_picture_info_t pic_info;
    if (jpeg_decoder_get_info(jpg_data, size, &pic_info) != ESP_OK) {
        free(jpg_data);
        ESP_LOGE(TAG, "long bg info fail: %s", path);
        return -1;
    }
    if (pic_info.width > LONG_BG_MAX_W || pic_info.height > LONG_BG_MAX_H) {
        free(jpg_data);
        ESP_LOGE(TAG, "long bg too big: %ux%u", (unsigned)pic_info.width, (unsigned)pic_info.height);
        return -1;
    }

    // tx 缓冲一次分配复用（防 PSRAM 碎片化；最大 512KB 封顶）
    size_t tx_need = (size + 63) & ~63;
    if (!s_long_bg_tx || tx_need > s_long_bg_tx_cap) {
        if (s_long_bg_tx) { free(s_long_bg_tx); s_long_bg_tx = NULL; s_long_bg_tx_cap = 0; }
        s_long_bg_tx = (uint8_t*)heap_caps_malloc(tx_need, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        s_long_bg_tx_cap = tx_need;
        if (!s_long_bg_tx) { free(jpg_data); return -1; }
    }
    memcpy(s_long_bg_tx, jpg_data, size);
    free(jpg_data);

    // 独立引擎 timeout 1000ms（480×800 需 30-55ms，1422×800 约 3 倍量，40ms 必超时）
    if (!ppa_jpg_lock()) {   // 2026-10-06 超时不等待(悬挂保护)
        ESP_LOGE(TAG, "long bg decode lock timeout");
        return -1;
    }
    jpeg_decoder_handle_t handle = NULL;
    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 1000 };
    if (jpeg_new_decoder_engine(&eng_cfg, &handle) != ESP_OK) {
        ppa_jpg_unlock();
        ESP_LOGE(TAG, "long bg engine create fail");
        return -1;
    }
    uint32_t decoded;
    esp_err_t ret = jpeg_decoder_process(handle, &s_jpg_cfg_rgb, s_long_bg_tx, tx_need,
                                         s_long_bg[slot].buf,
                                         LONG_BG_MAX_W * LONG_BG_MAX_H * 2, &decoded);
    jpeg_del_decoder_engine(handle);
    ppa_jpg_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "long bg decode fail: %s (%s)", esp_err_to_name(ret), path);
        return -1;
    }

    // 行 stride = MCU pad 后宽度（4:2:0 mcux=16；填充列在可见宽之外，渲染只看可见宽）
    s_long_bg[slot].w = (int)pic_info.width;
    s_long_bg[slot].h = (int)pic_info.height;
    s_long_bg[slot].stride = (int)((pic_info.width + 15) & ~15);
    ESP_LOGI(TAG, "long bg decoded: %s (%dx%d stride %d)", path,
             s_long_bg[slot].w, s_long_bg[slot].h, s_long_bg[slot].stride);
    return (int)pic_info.width;
}

uint8_t* ppa_long_bg_buffer(int slot) { return (slot >= 0 && slot <= 1) ? s_long_bg[slot].buf : NULL; }
int ppa_long_bg_width(int slot) { return (slot >= 0 && slot <= 1) ? s_long_bg[slot].w : 0; }
int ppa_long_bg_height(int slot) { return (slot >= 0 && slot <= 1) ? s_long_bg[slot].h : 0; }
int ppa_long_bg_stride(int slot) { return (slot >= 0 && slot <= 1) ? s_long_bg[slot].stride : 0; }

void ppa_long_bg_free(void) {
    for (int i = 0; i < 2; i++) {
        if (s_long_bg[i].buf) { free(s_long_bg[i].buf); s_long_bg[i].buf = NULL; }
        s_long_bg[i].w = s_long_bg[i].h = s_long_bg[i].stride = 0;
    }
    if (s_long_bg_tx) { free(s_long_bg_tx); s_long_bg_tx = NULL; s_long_bg_tx_cap = 0; }
    ESP_LOGI(TAG, "long bg freed");
}

void ppa_deinit(void) {
    ppa_long_bg_free();
    ppa_free_standee_slot();
    mjpeg_close();
    s_use_mjpeg = false;
    for (int i = 0; i < s_cache_count; i++) {
        if (s_jpg_cache[i]) free(s_jpg_cache[i]);
        if (s_mask_cache[i]) free(s_mask_cache[i]);
    }
    s_cache_count = 0;
    s_use_alpha = false;
    /* 2026-10-06 超时不等待(悬挂保护):拿不到锁跳过引擎删除,其余清理照常 */
    if (ppa_jpg_lock()) {
        if (s_jpg_handle) { jpeg_del_decoder_engine(s_jpg_handle); s_jpg_handle = NULL; }
        ppa_jpg_unlock();
    }
    if (s_ppa_client) { ppa_unregister_client(s_ppa_client); s_ppa_client = NULL; }
    if (s_bg_buf) { free(s_bg_buf); s_bg_buf = NULL; }
    if (s_fg_buf) { free(s_fg_buf); s_fg_buf = NULL; }
    if (s_alpha_buf) { free(s_alpha_buf); s_alpha_buf = NULL; }
    s_alpha_oom = false;
    if (s_comp_buf) { free(s_comp_buf); s_comp_buf = NULL; }
}
