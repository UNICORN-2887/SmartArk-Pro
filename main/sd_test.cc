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
#include "driver/gpio.h"

static const char *TAG = "SD_TEST";

#define SD_CLK_GPIO  GPIO_NUM_43
#define SD_CMD_GPIO  GPIO_NUM_44
#define SD_D0_GPIO   GPIO_NUM_39
#define SD_D1_GPIO   GPIO_NUM_40
#define SD_D2_GPIO   GPIO_NUM_41
#define SD_D3_GPIO   GPIO_NUM_42

static esp_err_t sdmmc_host_init_dummy(void) { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }

// 核心挂载逻辑
static bool do_sd_mount(void)
{
    ESP_LOGI(TAG, "Initializing SD card via SDMMC...");

    // 手动复位SD卡GPIO，排除ESP-Hosted对管脚状态的干扰
    gpio_reset_pin(SD_CLK_GPIO);
    gpio_reset_pin(SD_CMD_GPIO);
    gpio_reset_pin(SD_D0_GPIO);
    gpio_reset_pin(SD_D1_GPIO);
    gpio_reset_pin(SD_D2_GPIO);
    gpio_reset_pin(SD_D3_GPIO);

    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LDO failed: %s", esp_err_to_name(ret));
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.pwr_ctrl_handle = pwr_ctrl_handle;
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
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

    sdmmc_card_t *card = NULL;
    for (int attempt = 1; attempt <= 5; attempt++) {
        ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "Mount %d/5: %s", attempt, esp_err_to_name(ret));
        if (attempt < 5) vTaskDelay(pdMS_TO_TICKS(attempt * 300));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted!");
    sdmmc_card_print_info(stdout, card);

    DIR *d = opendir("/sdcard");
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            ESP_LOGI(TAG, "  %s", dir->d_name);
        }
        closedir(d);
    }
    return true;
}

// 同步挂载（阻塞，板子构造函数中调用）
bool sdcard_mount_sync(void)
{
    return do_sd_mount();
}

// 异步挂载（FreeRTOS任务中调用）
static void sd_mount_task(void *pvParameters)
{
    do_sd_mount();
    vTaskDelete(NULL);
}

void sdcard_init(void)
{
    xTaskCreate(sd_mount_task, "sd_mount", 4096, NULL, 5, NULL);
}
