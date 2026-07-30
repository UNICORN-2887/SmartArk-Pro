/*
 * PPA Hardware Compositor - Color-key compositing for video playback
 * Uses ESP32-P4 PPA BLEND engine to key out red background and composite character over static bg
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/ppa.h"
#include "driver/jpeg_decode.h"
#include "MjpegPlayer.h"

#define TAG "PPACompositor"

#define DISPLAY_W 480
#define DISPLAY_H 800
#define FRAME_SIZE (DISPLAY_W * DISPLAY_H * 2)  // RGB565

static ppa_client_handle_t s_ppa_client = NULL;
static uint8_t *s_bg_buf = NULL;       // Background (static)
static uint8_t *s_fg_buf = NULL;       // Foreground (JPEG decoded)
static uint8_t *s_comp_buf = NULL;      // Composited output

// JPEG decoder config
static jpeg_decoder_handle_t s_jpg_handle = NULL;
static jpeg_decode_engine_cfg_t s_jpg_eng_cfg = { .timeout_ms = 40 };
static jpeg_decode_cfg_t s_jpg_cfg_rgb = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};

// ─── Background ───────────────────────────────────────────────

bool ppa_load_background(const char *path) {
    // Read JPEG file
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "Cannot open %s", path); return false; }
    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    fread(jpg_data, 1, size, fp);
    fclose(fp);

    // Decode background to RGB565
    jpeg_decode_memory_alloc_cfg_t rx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };

    size_t out_size;
    size_t tx_size;
    uint8_t *tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(size, &tx_cfg, &tx_size);
    memcpy(tx_buf, jpg_data, size);
    free(jpg_data);

    s_bg_buf = (uint8_t*)jpeg_alloc_decoder_mem(FRAME_SIZE, &rx_cfg, &out_size);

    jpeg_decoder_handle_t tmp_handle;
    jpeg_new_decoder_engine(&s_jpg_eng_cfg, &tmp_handle);

    uint32_t decoded_size;
    jpeg_decoder_process(tmp_handle, &s_jpg_cfg_rgb, tx_buf, tx_size, s_bg_buf, out_size, &decoded_size);
    jpeg_del_decoder_engine(tmp_handle);
    free(tx_buf);

    ESP_LOGI(TAG, "Background loaded: %s (%ux%u)", path, DISPLAY_W, DISPLAY_H);
    return true;
}

// ─── PPA Client ───────────────────────────────────────────────

bool ppa_init(void) {
    // Allocate buffers in PSRAM (cache-line aligned)
    s_fg_buf = (uint8_t*)heap_caps_calloc(1, FRAME_SIZE + 64, MALLOC_CAP_SPIRAM);
    s_comp_buf = (uint8_t*)heap_caps_calloc(1, FRAME_SIZE + 64, MALLOC_CAP_SPIRAM);
    if (!s_fg_buf || !s_comp_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        return false;
    }
    s_fg_buf = (uint8_t*)(((uintptr_t)s_fg_buf + 63) & ~63);
    s_comp_buf = (uint8_t*)(((uintptr_t)s_comp_buf + 63) & ~63);

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

#define MAX_CACHE 200
static uint8_t *s_jpg_cache[MAX_CACHE];
static size_t s_jpg_cache_size[MAX_CACHE];
static int s_cache_count = 0;
static bool s_use_mjpeg = false;

// 双缓冲：后台异步预加载下一个表情
static uint8_t *s_pending_cache[MAX_CACHE];
static size_t  s_pending_sizes[MAX_CACHE];
static int     s_pending_count = 0;
static bool    s_pending_ready = false;
static TaskHandle_t s_preload_task = NULL;

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

int ppa_preload_mjpeg(const char *path) {
    // 先释放旧帧缓存，避免 PSRAM 碎片化导致 alloc 失败
    for (int i = 0; i < s_cache_count; i++) {
        if (s_jpg_cache[i]) { free(s_jpg_cache[i]); s_jpg_cache[i] = NULL; }
    }
    s_cache_count = 0;

    // ── Step 1: ONE sequential fread of ENTIRE file into PSRAM ──
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "Cannot open %s", path); return 0; }

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    if (file_size == 0 || file_size > 16 * 1024 * 1024) { fclose(fp); return 0; }
    rewind(fp);

    uint8_t *file_buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buf) { ESP_LOGE(TAG, "PSRAM alloc %u KB failed", (unsigned)(file_size/1024)); fclose(fp); return 0; }

    // Read in small chunks (16KB) — prevents SD card internal GC timeout
    size_t total = 0;
    int fails = 0;
    while (total < file_size && fails < 100) {
        size_t chunk = file_size - total;
        if (chunk > 16384) chunk = 16384;  // 16KB per read

        size_t got = fread(file_buf + total, 1, chunk, fp);
        if (got > 0) { total += got; fails = 0; continue; }

        fails++;
        clearerr(fp);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (fails >= 100) {
        ESP_LOGW(TAG, "Giving up at %u/%u KB", (unsigned)(total/1024), (unsigned)(file_size/1024));
    }
    clearerr(fp);
    fclose(fp);

    if (total == 0) { free(file_buf); return 0; }
    ESP_LOGI(TAG, "Read %u/%u KB in one pass", (unsigned)(total/1024), (unsigned)(file_size/1024));

    // ── Step 2: Parse frames from the PSRAM buffer (zero SD access) ──
    uint32_t frame_count;
    memcpy(&frame_count, file_buf, 4);
    if (frame_count == 0 || frame_count > MAX_CACHE) { free(file_buf); return 0; }

    uint32_t offsets[MAX_CACHE];
    for (int i = 0; i < (int)frame_count; i++) {
        memcpy(&offsets[i], file_buf + 4 + i * 4, 4);
    }

    int loaded = 0;
    for (int i = 0; i < (int)frame_count && loaded < MAX_CACHE; i++) {
        size_t start = offsets[i];
        size_t end = (i < (int)frame_count - 1) ? offsets[i + 1] : total;
        if (end <= start || end - start > 512 * 1024 || start >= total) continue;
        size_t sz = end - start;

        uint8_t *buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!buf) break;
        memcpy(buf, file_buf + start, sz);

        s_jpg_cache[loaded] = buf;
        s_jpg_cache_size[loaded] = sz;
        loaded++;
    }
    free(file_buf);  // Free the raw file buffer

    s_cache_count = loaded;
    ESP_LOGI(TAG, "MJPEG parsed: %d/%lu frames, %u KB PSRAM file buf freed",
             loaded, (unsigned long)frame_count, (unsigned)(total/1024));
    return loaded;
}

// ── 内部：加载MJPEG到指定缓冲区（不碰活跃缓存）──
static int load_mjpeg_into(const char *path,
                           uint8_t *cache[], size_t sizes[], int max_count) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "Cannot open %s", path); return 0; }

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    if (file_size == 0 || file_size > 16*1024*1024) { fclose(fp); return 0; }
    rewind(fp);

    uint8_t *file_buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buf) {
        ESP_LOGE(TAG, "PSRAM alloc %u KB failed", (unsigned)(file_size/1024));
        fclose(fp); return 0;
    }

    size_t total = 0;
    int zero_streak = 0;
    while (total < file_size) {
        size_t got = fread(file_buf + total, 1, file_size - total, fp);
        if (got > 0) { total += got; zero_streak = 0; continue; }
        if (++zero_streak > 30) break;
        clearerr(fp);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    clearerr(fp); fclose(fp);
    if (total == 0) { free(file_buf); return 0; }
    ESP_LOGI(TAG, "Read %u/%u KB in one pass", (unsigned)(total/1024), (unsigned)(file_size/1024));

    uint32_t frame_count;
    memcpy(&frame_count, file_buf, 4);
    if (frame_count == 0 || frame_count > (uint32_t)max_count) { free(file_buf); return 0; }

    uint32_t offsets[MAX_CACHE];
    for (int i = 0; i < (int)frame_count; i++)
        memcpy(&offsets[i], file_buf + 4 + i * 4, 4);

    int loaded = 0;
    for (int i = 0; i < (int)frame_count && loaded < max_count; i++) {
        size_t start = offsets[i];
        size_t end = (i < (int)frame_count - 1) ? offsets[i + 1] : total;
        if (end <= start || end - start > 512*1024 || start >= total) continue;
        size_t sz = end - start;
        uint8_t *buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!buf) break;
        memcpy(buf, file_buf + start, sz);
        cache[loaded] = buf;
        sizes[loaded] = sz;
        loaded++;
    }
    free(file_buf);
    ESP_LOGI(TAG, "Async MJPEG: %d/%lu frames, %u KB buf freed",
             loaded, (unsigned long)frame_count, (unsigned)(total/1024));
    return loaded;
}

// ── 异步预加载到后备缓冲区 ──
static void preload_task(void *arg) {
    const char *path = (const char*)arg;
    // 先清空后备缓冲区
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending_cache[i]) free(s_pending_cache[i]);
    }
    s_pending_count = 0;
    s_pending_ready = false;

    int count = load_mjpeg_into(path, s_pending_cache, s_pending_sizes, MAX_CACHE);
    if (count > 0) {
        s_pending_count = count;
        s_pending_ready = true;
        ESP_LOGI(TAG, "Pending emotion ready: %d frames (%s)", count, path);
    }
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
    xTaskCreate(preload_task, "mjpeg_preload", 8192, (void*)s_path_buf, 2, &s_preload_task);
}

int ppa_swap_emotion(void) {
    if (!s_pending_ready) return 0;

    // 交换活跃 ↔ 后备
    for (int i = 0; i < MAX_CACHE; i++) {
        uint8_t *tmp = s_jpg_cache[i];
        s_jpg_cache[i] = s_pending_cache[i];
        s_pending_cache[i] = tmp;

        size_t stmp = s_jpg_cache_size[i];
        s_jpg_cache_size[i] = s_pending_sizes[i];
        s_pending_sizes[i] = stmp;
    }
    int new_count = s_pending_count;
    s_pending_count = s_cache_count;  // 旧活跃变成后备（等下被异步任务清掉）
    s_cache_count = new_count;
    s_pending_ready = false;

    ESP_LOGI(TAG, "Swapped emotion: %d frames active, %d pending",
             s_cache_count, s_pending_count);
    return s_cache_count;
}

bool ppa_open_mjpeg(const char *path, int *out_frame_count) {
    if (!mjpeg_open(path)) return false;
    s_use_mjpeg = true;
    *out_frame_count = mjpeg_get_frame_count();
    ESP_LOGI(TAG, "MJPEG mode: %s (%d frames)", path, *out_frame_count);
    return true;
}

// ─── Composite & Return Output Buffer ─────────────────────────

uint8_t* ppa_composite_frame(int frame_index) {
    if (!s_fg_buf || !s_comp_buf) return NULL;

    // ── Step 1: Get JPEG data ──
    size_t jpg_size;
    uint8_t *jpg_data = NULL;
    bool need_free = false;

    if (s_use_mjpeg) {
        if (!mjpeg_get_frame(frame_index, &jpg_data, &jpg_size)) return NULL;
        need_free = true;
    } else {
        if (frame_index < 0 || frame_index >= s_cache_count || !s_jpg_cache[frame_index]) return NULL;
        jpg_size = s_jpg_cache_size[frame_index];
        jpg_data = s_jpg_cache[frame_index];
    }

    jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t tx_size;
    uint8_t *tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpg_size, &tx_cfg, &tx_size);
    memcpy(tx_buf, jpg_data, jpg_size);
    if (need_free) free(jpg_data);

    if (!s_jpg_handle) {
        jpeg_new_decoder_engine(&s_jpg_eng_cfg, &s_jpg_handle);
    }

    uint32_t decoded_size;
    esp_err_t ret = jpeg_decoder_process(s_jpg_handle, &s_jpg_cfg_rgb,
                                          tx_buf, tx_size,
                                          s_fg_buf, FRAME_SIZE,
                                          &decoded_size);
    free(tx_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    // ── Step 2: PPA BLEND (or pass-through if no background) ──
    if (!s_ppa_client || !s_bg_buf) {
        // No background or PPA not ready — return decoded foreground directly
        return s_fg_buf;
    }
    ppa_in_pic_blk_config_t bg_cfg = {};
    bg_cfg.buffer = s_bg_buf;
    bg_cfg.pic_w = DISPLAY_W; bg_cfg.pic_h = DISPLAY_H;
    bg_cfg.block_w = DISPLAY_W; bg_cfg.block_h = DISPLAY_H;
    bg_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    ppa_in_pic_blk_config_t fg_cfg = {};
    fg_cfg.buffer = s_fg_buf;
    fg_cfg.pic_w = DISPLAY_W; fg_cfg.pic_h = DISPLAY_H;
    fg_cfg.block_w = DISPLAY_W; fg_cfg.block_h = DISPLAY_H;
    fg_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    ppa_out_pic_blk_config_t out_cfg = {};
    out_cfg.buffer = s_comp_buf;
    out_cfg.buffer_size = FRAME_SIZE;
    out_cfg.pic_w = DISPLAY_W; out_cfg.pic_h = DISPLAY_H;
    out_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    color_pixel_rgb888_data_t red_low  = { .b = 0,   .g = 0,  .r = 200 };
    color_pixel_rgb888_data_t red_high = { .b = 80,  .g = 80, .r = 255 };
    color_pixel_rgb888_data_t ck_default = { .b = 0, .g = 0, .r = 0 };

    ppa_blend_oper_config_t blend_cfg = {};
    blend_cfg.in_bg = bg_cfg;
    blend_cfg.in_fg = fg_cfg;
    blend_cfg.out = out_cfg;
    blend_cfg.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    blend_cfg.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    blend_cfg.bg_ck_en = false;
    // Color-key: remove red pixels from foreground, R=[200,255], G=[0,80], B=[0,80]
    blend_cfg.fg_ck_en = true;
    blend_cfg.fg_ck_rgb_low_thres = red_low;
    blend_cfg.fg_ck_rgb_high_thres = red_high;
    blend_cfg.ck_rgb_default_val = ck_default;
    blend_cfg.mode = PPA_TRANS_MODE_BLOCKING;

    ret = ppa_do_blend(s_ppa_client, &blend_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA blend failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    return s_comp_buf;
}

void ppa_deinit(void) {
    mjpeg_close();
    s_use_mjpeg = false;
    for (int i = 0; i < s_cache_count; i++) { if (s_jpg_cache[i]) free(s_jpg_cache[i]); }
    s_cache_count = 0;
    if (s_jpg_handle) { jpeg_del_decoder_engine(s_jpg_handle); s_jpg_handle = NULL; }
    if (s_ppa_client) { ppa_unregister_client(s_ppa_client); s_ppa_client = NULL; }
    if (s_bg_buf) { free(s_bg_buf); s_bg_buf = NULL; }
    if (s_fg_buf) { free(s_fg_buf); s_fg_buf = NULL; }
    if (s_comp_buf) { free(s_comp_buf); s_comp_buf = NULL; }
}
