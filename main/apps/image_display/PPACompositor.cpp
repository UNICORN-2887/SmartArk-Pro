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
#define DECODE_MAX_H 900
#define FRAME_SIZE_RGB565 (DISPLAY_W * DECODE_MAX_H * 2)
#define FRAME_SIZE_ARGB (DISPLAY_W * DECODE_MAX_H * 4)  // ARGB8888 for alpha blend

static ppa_client_handle_t s_ppa_client = NULL;
static uint8_t *s_bg_buf = NULL;       // Background (static)
static uint8_t *s_fg_buf = NULL;       // Foreground (JPEG decoded, RGB565)
static uint8_t *s_alpha_buf = NULL;    // Foreground (ARGB8888 for alpha blend)
static uint8_t *s_comp_buf = NULL;     // Composited output (RGB565)

// JPEG decoder config
static jpeg_decoder_handle_t s_jpg_handle = NULL;
static uint32_t s_last_decoded_size = 0;
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

    s_bg_buf = (uint8_t*)jpeg_alloc_decoder_mem(FRAME_SIZE_RGB565, &rx_cfg, &out_size);

    jpeg_decoder_handle_t tmp_handle;
    jpeg_new_decoder_engine(&s_jpg_eng_cfg, &tmp_handle);

    uint32_t decoded_size;
    jpeg_decoder_process(tmp_handle, &s_jpg_cfg_rgb, tx_buf, tx_size, s_bg_buf, out_size, &decoded_size);
    jpeg_del_decoder_engine(tmp_handle);
    free(tx_buf);

    ESP_LOGI(TAG, "Background loaded: %s (%ux%u)", path, DISPLAY_W, DISPLAY_H);
    return true;
}

void ppa_unload_background(void) {
    if (s_bg_buf) { free(s_bg_buf); s_bg_buf = NULL; }
    ESP_LOGI(TAG, "Background unloaded");
}
bool ppa_has_background(void) { return s_bg_buf != NULL; }

int ppa_get_last_decoded_height(void) {
    // RGB565: height = total_bytes / (width * 2)
    // width 固定 480 (DISPLAY_W)
    return (int)(s_last_decoded_size / (DISPLAY_W * 2));
}

// ─── PPA Client ───────────────────────────────────────────────

bool ppa_init(void) {
    // Allocate buffers in PSRAM (cache-line aligned)
    s_fg_buf = (uint8_t*)heap_caps_calloc(1, FRAME_SIZE_RGB565 + 64, MALLOC_CAP_SPIRAM);
    s_alpha_buf = (uint8_t*)heap_caps_calloc(1, FRAME_SIZE_ARGB + 64, MALLOC_CAP_SPIRAM);
    s_comp_buf = (uint8_t*)heap_caps_calloc(1, FRAME_SIZE_RGB565 + 64, MALLOC_CAP_SPIRAM);
    if (!s_fg_buf || !s_alpha_buf || !s_comp_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        return false;
    }
    s_fg_buf = (uint8_t*)(((uintptr_t)s_fg_buf + 63) & ~63);
    s_alpha_buf = (uint8_t*)(((uintptr_t)s_alpha_buf + 63) & ~63);
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

#define MAX_CACHE 260
static uint8_t *s_jpg_cache[MAX_CACHE];
static size_t s_jpg_cache_size[MAX_CACHE];
static int s_cache_count = 0;
static bool s_use_mjpeg = false;

// ─── 第四槽：Profile 缓存（独立，不争抢 cover/pending 槽）───
static uint8_t *s_profile_cache[MAX_CACHE];
static size_t s_profile_sizes[MAX_CACHE];
static int s_profile_count = 0;
static bool s_profile_loaded = false;

// Alpha 遮罩缓存（RLE 压缩, ~10KB/帧）
static uint8_t *s_mask_cache[MAX_CACHE];
static size_t  s_mask_cache_size[MAX_CACHE];
static bool    s_use_alpha = false;

// 双缓冲：后台异步预加载下一个表情
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
static int load_mjpeg_into(const char *path, uint8_t *cache[], size_t sizes[], int max_count);
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
    s_cache_count = load_mjpeg_into(path, s_jpg_cache, s_jpg_cache_size, MAX_CACHE);
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
                           uint8_t *cache[], size_t sizes[], int max_count) {
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
    }
    free(chunk);
    fclose(fp);

    ESP_LOGI(TAG, "%s: %d/%lu frames, %u KB PSRAM (streaming, 0 seek)",
             path, loaded, (unsigned long)frame_count, total_kb);
    return loaded;
}

// ── 异步预加载到后备缓冲区 ──
static void preload_task(void *arg) {
    const char *path = (const char*)arg;
    // 先清空后备缓冲区
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending_cache[i]) free(s_pending_cache[i]);
        if (s_pending_mask[i]) { free(s_pending_mask[i]); s_pending_mask[i] = NULL; }
    }
    s_pending_count = 0;
    s_pending_ready = false;
    s_pending_has_alpha = false;

    int count = load_mjpeg_into(path, s_pending_cache, s_pending_sizes, MAX_CACHE);
    if (count > 0) {
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

    s_cover_count = load_mjpeg_into(path, s_cover_cache, s_cover_sizes, MAX_CACHE);
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
    if (s_jpg_handle) {
        jpeg_del_decoder_engine(s_jpg_handle);
        s_jpg_handle = NULL;
    }
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
    jpeg_decoder_handle_t handle = NULL;
    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 1000 };
    if (jpeg_new_decoder_engine(&eng_cfg, &handle) != ESP_OK) {
        free(tx_buf); free(rgb_buf); return NULL;
    }
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t decoded;
    esp_err_t ret = jpeg_decoder_process(handle, &cfg, tx_buf, tx_size, rgb_buf, out_size, &decoded);
    jpeg_del_decoder_engine(handle);
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

// ─── 第四槽：Profile 缓存操作 ───────────────────────────

int ppa_preload_profile(const char *path) {
    // 释放旧数据
    for (int i = 0; i < s_profile_count; i++) {
        if (s_profile_cache[i]) { free(s_profile_cache[i]); s_profile_cache[i] = NULL; }
    }
    s_profile_count = load_mjpeg_into(path, s_profile_cache, s_profile_sizes, MAX_CACHE);
    s_profile_loaded = (s_profile_count > 0);
    if (s_profile_loaded)
        ESP_LOGI(TAG, "Profile cached: %d frames (%s)", s_profile_count, path);
    return s_profile_count;
}

int ppa_swap_profile_to_active(void) {
    if (!s_profile_loaded) return 0;
    for (int i = 0; i < MAX_CACHE; i++) {
        uint8_t *tmp = s_jpg_cache[i];
        s_jpg_cache[i] = s_profile_cache[i];
        s_profile_cache[i] = tmp;
        size_t stmp = s_jpg_cache_size[i];
        s_jpg_cache_size[i] = s_profile_sizes[i];
        s_profile_sizes[i] = stmp;
    }
    int new_count = s_profile_count;
    s_profile_count = s_cache_count;
    s_cache_count = new_count;
    s_use_alpha = false;  // profile 直通无 alpha 合成
    ESP_LOGI(TAG, "Swapped profile: %d frames active, %d in profile",
             s_cache_count, s_profile_count);
    return s_cache_count;
}

void ppa_free_profile_slot(void) {
    // swap 回去后再调用——此时 active 已恢复，profile 槽里是 profile 帧
    for (int i = 0; i < s_profile_count; i++) {
        if (s_profile_cache[i]) { free(s_profile_cache[i]); s_profile_cache[i] = NULL; }
    }
    s_profile_count = 0;
    s_profile_loaded = false;
    ESP_LOGI(TAG, "Profile slot freed");
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
                                          s_fg_buf, FRAME_SIZE_RGB565,
                                          &decoded_size);
    free(tx_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        return NULL;
    }
    s_last_decoded_size = decoded_size;

    // ── Step 2: Apply alpha mask → ARGB8888 ──
    uint8_t *fg_for_blend = s_fg_buf;
    ppa_blend_color_mode_t fg_cm = PPA_BLEND_COLOR_MODE_RGB565;

    if (s_use_alpha && s_mask_cache[frame_index]) {
        apply_rle_mask(s_mask_cache[frame_index], s_mask_cache_size[frame_index],
                       (uint16_t*)s_fg_buf, DISPLAY_W * DISPLAY_H);
        fg_for_blend = s_alpha_buf;
        fg_cm = PPA_BLEND_COLOR_MODE_ARGB8888;
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
    out_cfg.buffer = s_comp_buf;
    out_cfg.buffer_size = FRAME_SIZE_RGB565;
    out_cfg.pic_w = DISPLAY_W; out_cfg.pic_h = DISPLAY_H;
    out_cfg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    ppa_blend_oper_config_t blend_cfg = {};
    blend_cfg.in_bg = bg_cfg;
    blend_cfg.in_fg = fg_cfg;
    blend_cfg.out = out_cfg;
    blend_cfg.mode = PPA_TRANS_MODE_BLOCKING;

    if (s_use_alpha) {
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

    return s_comp_buf;
}

void ppa_deinit(void) {
    mjpeg_close();
    s_use_mjpeg = false;
    for (int i = 0; i < s_cache_count; i++) {
        if (s_jpg_cache[i]) free(s_jpg_cache[i]);
        if (s_mask_cache[i]) free(s_mask_cache[i]);
    }
    s_cache_count = 0;
    s_use_alpha = false;
    if (s_jpg_handle) { jpeg_del_decoder_engine(s_jpg_handle); s_jpg_handle = NULL; }
    if (s_ppa_client) { ppa_unregister_client(s_ppa_client); s_ppa_client = NULL; }
    if (s_bg_buf) { free(s_bg_buf); s_bg_buf = NULL; }
    if (s_fg_buf) { free(s_fg_buf); s_fg_buf = NULL; }
    if (s_alpha_buf) { free(s_alpha_buf); s_alpha_buf = NULL; }
    if (s_comp_buf) { free(s_comp_buf); s_comp_buf = NULL; }
}
