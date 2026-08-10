#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/event_groups.h"
#include "apps/image_display/PPACompositor.h"
#include "apps/live2d/moc3_parser.h"
#include "apps/live2d/lv2_render.h"

static const char *TAG = "SD_TEST";

// Global Live2D model for animation
static Lv2RenderModel* g_lv2_model = NULL;

// Event group for signaling SD card + preload completion
static EventGroupHandle_t s_sd_event_group = NULL;
#define SD_READY_BIT BIT0

// SD卡引脚（Slot 0）
#define SD_CLK_GPIO  GPIO_NUM_43
#define SD_CMD_GPIO  GPIO_NUM_44
#define SD_D0_GPIO   GPIO_NUM_39
#define SD_D1_GPIO   GPIO_NUM_40
#define SD_D2_GPIO   GPIO_NUM_41
#define SD_D3_GPIO   GPIO_NUM_42
#define SD_MOUNT_POINT "/sdcard"

static esp_err_t sdmmc_host_init_dummy(void) { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }

static void sd_mount_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Initializing SD card via SDMMC...");
    ESP_LOGI(TAG, "Pins: CLK=%d, CMD=%d, D0=%d, D1=%d, D2=%d, D3=%d",
             SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO, SD_D1_GPIO, SD_D2_GPIO, SD_D3_GPIO);

    // ESP32-P4 SDMMC Slot 0 需要使用内部LDO供电
    // LDO通道4用于SDMMC Slot 0
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,  // ESP32-P4 SDMMC Slot 0 专用LDO通道
    };
    
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create LDO power control: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without LDO power control...");
    } else {
        ESP_LOGI(TAG, "LDO power control initialized successfully");
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.pwr_ctrl_handle = pwr_ctrl_handle;
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_SDR50;
    host.init = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_CLK_GPIO;
    slot_config.cmd = SD_CMD_GPIO;
    slot_config.d0 = SD_D0_GPIO;
    slot_config.d1 = SD_D1_GPIO;
    slot_config.d2 = SD_D2_GPIO;
    slot_config.d3 = SD_D3_GPIO;
    slot_config.width = 4;
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 30,
        .allocation_unit_size = 16 * 1024,
    };
    
    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, 
                                   &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Please check:");
        ESP_LOGW(TAG, "  1. SD card is inserted");
        ESP_LOGW(TAG, "  2. SD card is formatted as FAT32");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "SD card mounted successfully!");
    sdmmc_card_print_info(stdout, card);

    // 提前预加载默认 cover 到 cover 专用槽（WiFi 前，SD 独占，速度快）
    ESP_LOGI(TAG, "Early preload: Kaltsit cover...");
    int loaded = ppa_preload_cover("/sdcard/main/operator/MEDIC/6STAR/Kaltsit/cover/kaltsit_cover.mjpeg");
    ESP_LOGI(TAG, "Early preload done: %d frames", loaded);

    // ── Live2D MOC3 parser integration (early preload for fast expression switching) ──
    ESP_LOGI(TAG, "Preloading MOC3 model: /sdcard/Amiya.moc3");
    FILE* fp = fopen("/sdcard/Amiya.moc3", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (buf) {
            fread(buf, 1, sz, fp);
            Lv2Model* m = lv2_parse_moc3(buf, sz);
            if (m) {
                ESP_LOGI(TAG, "Live2D model loaded: %lu params, %lu drawables, %.0fx%.0f",
                    (unsigned long)m->counts.parameters, (unsigned long)m->counts.art_meshes,
                    (double)m->canvas_w, (double)m->canvas_h);
                lv2_free_model(m);
            }
            heap_caps_free(buf);
        }
        fclose(fp);
    }

    // ── Live2D render ──
    ESP_LOGI(TAG, "Loading Live2D model...");
    g_lv2_model = lv2_load("/sdcard/Amiya/model.l2d", "/sdcard/Amiya/amiya_tex.raw");
    if (g_lv2_model) {
        uint16_t* fb = (uint16_t*)heap_caps_malloc(480*800*2, MALLOC_CAP_SPIRAM);
        if (fb) {
            lv2_render_frame(g_lv2_model, fb, 480, 800);
            ESP_LOGI(TAG, "Live2D render OK! %dx%d, %d drawables, tex=%dx%d",
                480, 800, g_lv2_model->drawable_count, g_lv2_model->tex_w, g_lv2_model->tex_h);
            extern uint16_t* g_lv2_fb;
            extern int g_lv2_fb_w, g_lv2_fb_h;
            if (g_lv2_fb) heap_caps_free(g_lv2_fb);
            g_lv2_fb = fb; g_lv2_fb_w = 480; g_lv2_fb_h = 800;
        }
    } else ESP_LOGW(TAG, "Live2D render test skipped — no model.l2d or amiya_tex.raw on SD");

    // 通知主任务：SD卡就绪
    if (s_sd_event_group) xEventGroupSetBits(s_sd_event_group, SD_READY_BIT);

    vTaskDelete(NULL);
}

void sdcard_init(void)
{
    if (!s_sd_event_group) s_sd_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_sd_event_group, SD_READY_BIT);
    xTaskCreate(sd_mount_task, "sd_mount", 8192, NULL, 5, NULL);
}

bool sdcard_wait_ready(int timeout_ms)
{
    if (!s_sd_event_group) return false;
    EventBits_t bits = xEventGroupWaitBits(s_sd_event_group, SD_READY_BIT,
                                            pdTRUE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & SD_READY_BIT) != 0;
}

// Called periodically for Live2D animation (loads keyforms on first call)
void lv2_update_animation(float time_sec) {
    if (!g_lv2_model) return;
    static bool kf_loaded = false;
    if (!kf_loaded) {
        kf_loaded = true;
        lv2_load_keyforms(g_lv2_model, "/sdcard/Amiya/keyforms.bin");
    }
    extern uint16_t* g_lv2_fb;
    if (!g_lv2_fb) return;
    lv2_render_animated(g_lv2_model, g_lv2_fb, 480, 800, time_sec);
}


