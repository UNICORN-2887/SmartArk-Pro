#include "wifi_board.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "application.h"
#include "display/lcd_display.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "p4_camera.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#define TAG "jc4880p443"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

static const st7701_lcd_init_cmd_t lcd_cmd[] = {
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x13},5,0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x10},5,0},
    {0xC0, (uint8_t []){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},

    {0xB0, (uint8_t []){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t []){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

    {0xB0, (uint8_t []){0x5D}, 1, 0},
    {0xB1, (uint8_t []){0x58}, 1, 0},
    {0xB2, (uint8_t []){0x87}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4E}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0},
    {0xB9, (uint8_t []){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t []){0x03}, 1,0},
    {0xBC, (uint8_t []){0x00}, 1,0},
    
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},

    {0xE0, (uint8_t []){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x04, 0xA0, 0x00, 0xA0, 0x05,0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t []){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

    {0xEB, (uint8_t []){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t []){0x08, 0x01}, 2, 0},

    {0xED, (uint8_t []){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    // {0x3A, (uint8_t []){0x66}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 20},

};

class jc4880p443 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    Button int_button_;
    LcdDisplay *display__;

    /* 2026-09-25 软开关机:电源键只接 CHIP_PU 的 RC 电路(长按 20s 硬件断电,
       固件检测不到该键),故用外壳可达的 BOOT 键(GPIO35)做软开关:
       短按 → 关屏+挂断+停唤醒词(等效关机);再短按 → 恢复。 */
    bool soft_power_off_ = false;
    uint8_t saved_brightness_ = 0;

    void ToggleSoftPower() {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        if (!soft_power_off_) {
            soft_power_off_ = true;
            if (app.GetDeviceState() != kDeviceStateIdle) {
                app.SetDeviceState(kDeviceStateIdle);   // 对话中挂断,停语音处理
            }
            audio.EnableWakeWordDetection(false);   // 软关机期间不再听唤醒词(省电+免误触发)
            saved_brightness_ = GetBacklight()->brightness();
            GetBacklight()->SetBrightness(0, true);
            ESP_LOGI(TAG, "软关机(短按 BOOT 键恢复)");
        } else {
            soft_power_off_ = false;
            audio.EnableWakeWordDetection(true);    // 恢复唤醒词检测
            GetBacklight()->SetBrightness(saved_brightness_ ? saved_brightness_ : 60, true);
            ESP_LOGI(TAG, "软开机");
        }
    }

    void InitializeCodecI2c() {
        // 板子自建 I2C 总线(音频 codec / 触摸 GT911 / 摄像头 SCCB 共享;
        // 2026-10-01 弃 BSP 的 bsp_i2c:引入 BSP 组件导致堆损坏唤醒崩溃)
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeGT911()
    {
        // 先手动复位 GT911（确保芯片上电就绪）
        gpio_config_t rst_conf = {
            .pin_bit_mask = 1ULL << LCD_TOUCH_RST,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_conf);
        gpio_set_level(LCD_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(LCD_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = LCD_H_RES,
            .y_max = LCD_V_RES,
            .rst_gpio_num = GPIO_NUM_NC,      // 手动复位，不交给驱动
            .int_gpio_num = GPIO_NUM_NC,       // 不用中断
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        };

        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        // 2026-09-30 IDF 5.5.1 结构体字段顺序变化(scl_speed_hz 移到最后),
        // gt911 组件 1.2.1 的宏顺序不匹配(C++ designator order 报错)→ 手动初始化
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,
            .flags = { .disable_control_phase = 1 },
            .scl_speed_hz = 400 * 1000,  // 400kHz 匹配 Waveshare
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &tp_io_handle));

        esp_err_t ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "GT911 init failed (0x%X), touch disabled", ret);
            return;
        }

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "GT911 touch registered");
    }

static esp_err_t bsp_enable_dsi_phy_power(void)
{
    #if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
        // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
        ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
    #endif // BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0

        return ESP_OK;
}

    void InitializeLCD()
    {
        bsp_enable_dsi_phy_power();
        esp_lcd_panel_io_handle_t io = NULL;
        esp_lcd_panel_handle_t disp_panel = NULL;

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 500,
        };
        esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        // we use DBI interface to send LCD commands and parameters
        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,
            .lcd_cmd_bits = 8,   // according to the LCD spec
            .lcd_param_bits = 8, // according to the LCD spec
        };
        esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io);

        // esp_lcd_dpi_panel_config_t dpi_config = JD9165_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);

        esp_lcd_dpi_panel_config_t dpi_config ={
            .virtual_channel = 0, 
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,  
            .dpi_clock_freq_mhz = 34,                                             
            .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,                    
            .num_fbs = 2,                                 
            .video_timing = {                             
                .h_size = 480,                            
                .v_size = 800, 
                .hsync_pulse_width = 12,                            
                .hsync_back_porch = 42,                              
                .hsync_front_porch = 42,   
                .vsync_pulse_width = 2,                
                .vsync_back_porch = 8,                                          
                .vsync_front_porch = 166,                  
            },                                            
            .flags={
                .use_dma2d = true,
            }                      
        };

        st7701_vendor_config_t vendor_config = {
            .init_cmds = lcd_cmd,
            .init_cmds_size = sizeof(lcd_cmd) / sizeof(st7701_lcd_init_cmd_t),
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
            .flags = {
                .use_mipi_interface = 1,
            }
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_config,
        };
        esp_lcd_new_panel_st7701(io, &lcd_dev_config, &disp_panel);
        esp_lcd_panel_reset(disp_panel);
        esp_lcd_panel_init(disp_panel);

        display__ = new MipiLcdDisplay(io,disp_panel,LCD_H_RES,LCD_V_RES,
                 DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                 {
                    .text_font = &font_puhui_20_4,
                    .icon_font = &font_awesome_20_4,
                    .emoji_font = font_emoji_64_init(),
                });
        return;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
                return;
            }
            /* 2026-09-25 运行态:短按 BOOT 键 = 软开关机
               (启动配网期仍是"重配网",两种功能互不冲突) */
            ToggleSoftPower();
        });

        // 触屏已通过 LVGL 驱动正常工作，GPIO 21 中断不再触发监听
        // int_button_.OnPressDown/OnPressUp 已移除
    }

    // 物联网初始化，添加对 AI 可见设备
    // void InitializeIot() {
    //     auto& thing_manager = iot::ThingManager::GetInstance();
    //     thing_manager.AddThing(iot::CreateThing("Speaker"));
    // }

public:
    jc4880p443() : boot_button_(BOOT_BUTTON_GPIO),int_button_(LCD_TOUCH_INT){

        InitializeCodecI2c();
        // InitializeIot();
        InitializeLCD();
        InitializeGT911();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_1, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }
    
    virtual Display* GetDisplay() override {
        
        return display__;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(PIN_NUM_BK_LIGHT, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        // 2026-10-01 P4 MIPI-CSI 摄像头(OV02C10):SCCB 与音频 codec 共享 I2C 总线
        static P4Camera camera(codec_i2c_bus_);
        return &camera;
    }

    // ── 电池电量(2026-10-02,Guition adc_test 标定) ──
    // 板子电池经分压接 GPIO53 = ADC2_CHANNEL_4;满电检测值 2450mV、
    // 空电 2250mV,线性映射百分比(与 phone 出厂固件同源)。
    // 充电状态:IP5306 无状态引脚,用 30 秒电压趋势判断(升=充电,降=放电)。
    adc_oneshot_unit_handle_t bat_adc_ = nullptr;
    adc_cali_handle_t bat_cali_ = nullptr;
    SemaphoreHandle_t bat_mutex_ = nullptr;   /* 2026-10-02 防两个显示层并发读 ADC(0% 跳变) */
    int last_percent_ = -1;
    float last_bat_mv_ = 0;
    int64_t last_trend_ts_ = 0;
    bool trend_charging_ = false;

    void InitBatteryAdc() {
        adc_oneshot_unit_init_cfg_t init = {
            .unit_id = ADC_UNIT_2,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        if (adc_oneshot_new_unit(&init, &bat_adc_) != ESP_OK) {
            ESP_LOGW(TAG, "battery ADC unit init failed");
            return;
        }
        adc_oneshot_chan_cfg_t chan = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_config_channel(bat_adc_, ADC_CHANNEL_4, &chan) != ESP_OK) {
            ESP_LOGW(TAG, "battery ADC channel config failed");
            return;
        }
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_2,
            .chan = ADC_CHANNEL_4,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &bat_cali_);
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        if (bat_adc_ == nullptr) InitBatteryAdc();
        if (bat_adc_ == nullptr) return false;
        if (bat_mutex_ == nullptr) bat_mutex_ = xSemaphoreCreateMutex();
        if (bat_mutex_ == nullptr ||
            xSemaphoreTake(bat_mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return false;
        bool ok = false;
        int raw = 0, mv = 0, pct = 0;   /* goto out 前声明(C++ 跨初始化限制) */
        int64_t now = 0;
        // 2026-10-02 采样 64 次取中位数:充电时 IP5306 开关噪声大,
        // 均值会被尖峰拉偏(曾出现 99%↔12% 跳变)
        int samples[64];
        for (int i = 0; i < 64; i++) {
            int r = 0;
            if (adc_oneshot_read(bat_adc_, ADC_CHANNEL_4, &r) != ESP_OK) goto out;
            samples[i] = r;
        }
        // 插入排序取中位(64 个值,简单排序)
        for (int i = 1; i < 64; i++) {
            int v = samples[i], j = i - 1;
            while (j >= 0 && samples[j] > v) { samples[j + 1] = samples[j]; j--; }
            samples[j + 1] = v;
        }
        raw = (samples[31] + samples[32]) / 2;
        mv = 0;
        if (bat_cali_ != nullptr) {
            if (adc_cali_raw_to_voltage(bat_cali_, raw, &mv) != ESP_OK) goto out;
        } else {
            goto out;   // 无校准拿不到电压,不显示(与 Guition 行为一致)
        }
        // 线性映射:2450mV=100%,2250mV=0%
        pct = (mv - 2250) * 100 / (2450 - 2250);
        // 2026-10-02 诊断:每 30 秒打印原始电压(排查充电 0% 读数异常)
        {
            static int64_t dbg_ts = 0;
            if (esp_timer_get_time() - dbg_ts > 30 * 1000 * 1000LL) {
                dbg_ts = esp_timer_get_time();
                ESP_LOGI(TAG, "batt: raw=%d mv=%d pct=%d trend_chg=%d",
                         raw, mv, pct, (int)trend_charging_);
            }
        }
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        // 强平滑:上次 70% + 本次 30%,单次变化钳制 ±8%(防 UI 跳变)
        if (last_percent_ >= 0) {
            int delta = pct - last_percent_;
            if (delta > 8) delta = 8;
            if (delta < -8) delta = -8;
            pct = last_percent_ + delta;
        }
        last_percent_ = pct;
        // 60 秒电压趋势:>5mV 升=充电,< -5mV 降=放电
        // (2026-10-02 阈值 25→5mV:充电后期电压爬升仅 4-7mV/分钟,25mV 判不出)
        now = esp_timer_get_time();
        if (last_trend_ts_ == 0) {
            last_trend_ts_ = now;
            last_bat_mv_ = mv;
        } else if (now - last_trend_ts_ > 60 * 1000 * 1000LL) {
            if (mv - last_bat_mv_ > 5) trend_charging_ = true;
            else if (mv - last_bat_mv_ < -5) trend_charging_ = false;
            last_trend_ts_ = now;
            last_bat_mv_ = mv;
        }
        charging = trend_charging_;
        discharging = !trend_charging_;
        level = pct;
        ok = true;
    out:
        xSemaphoreGive(bat_mutex_);
        return ok;
    }

};

DECLARE_BOARD(jc4880p443);
