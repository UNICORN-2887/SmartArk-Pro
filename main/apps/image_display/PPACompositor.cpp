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

// ─── Composite & Return Output Buffer ─────────────────────────

uint8_t* ppa_composite_frame(const char *frame_path) {
    if (!s_fg_buf || !s_comp_buf) {
        ESP_LOGE(TAG, "PPA buffers not initialized");
        return NULL;
    }

    // ── Step 1: JPEG decode foreground ──
    FILE *fp = fopen(frame_path, "rb");
    if (!fp) { ESP_LOGE(TAG, "Cannot open %s", frame_path); return NULL; }
    fseek(fp, 0, SEEK_END);
    size_t jpg_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(jpg_size, MALLOC_CAP_SPIRAM);
    fread(jpg_data, 1, jpg_size, fp);
    fclose(fp);

    jpeg_decode_memory_alloc_cfg_t tx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t tx_size;
    uint8_t *tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpg_size, &tx_cfg, &tx_size);
    memcpy(tx_buf, jpg_data, jpg_size);
    free(jpg_data);

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
    if (s_jpg_handle) { jpeg_del_decoder_engine(s_jpg_handle); s_jpg_handle = NULL; }
    if (s_ppa_client) { ppa_unregister_client(s_ppa_client); s_ppa_client = NULL; }
    if (s_bg_buf) { free(s_bg_buf); s_bg_buf = NULL; }
    if (s_fg_buf) { free(s_fg_buf); s_fg_buf = NULL; }
    if (s_comp_buf) { free(s_comp_buf); s_comp_buf = NULL; }
}
