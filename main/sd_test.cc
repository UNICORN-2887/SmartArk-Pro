#include "esp_log.h"
#include "esp_err.h"
#include "dirent.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "apps/image_display/ImageDisplay.hpp"

static const char *TAG = "SD_TEST";

// SD卡引脚（Slot 0）
#define SD_CLK_GPIO  GPIO_NUM_43
#define SD_CMD_GPIO  GPIO_NUM_44
#define SD_D0_GPIO   GPIO_NUM_39
#define SD_D1_GPIO   GPIO_NUM_40
#define SD_D2_GPIO   GPIO_NUM_41
#define SD_D3_GPIO   GPIO_NUM_42

// ESP-Hosted 会先初始化 SDMMC host，需要 workaround
static esp_err_t sdmmc_host_init_dummy(void)
{
    return ESP_OK;
}

static esp_err_t sdmmc_host_deinit_dummy(void)
{
    return ESP_OK;
}

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
    
    // ESP-Hosted 已经初始化了 SDMMC host，跳过初始化
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
        .max_files = 5,
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
    
    // 读取文件
    ESP_LOGI(TAG, "Listing /sdcard directory:");
    DIR *d = opendir("/sdcard");
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            ESP_LOGI(TAG, "  %s", dir->d_name);
        }
        closedir(d);
    }

    // 自动显示第一张JPG图片，然后开始视频循环播放
    if (image_display_init()) {
        video_playback_start(30);  // 30 FPS 循环播放
    }

    vTaskDelete(NULL);
}

void sdcard_init(void)
{
    xTaskCreate(sd_mount_task, "sd_mount", 4096, NULL, 5, NULL);
}
