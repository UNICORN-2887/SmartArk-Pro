#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include "afsk_demod.h"

static const char *TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage(Lang::CODE);
    wifi_ap.SetSsidPrefix("Xiaozhi");
    wifi_ap.Start();

    // 记录配网前的SSID数量
    auto& ssid_manager = SsidManager::GetInstance();
    size_t ssid_before = ssid_manager.GetSsidList().size();

    // 显示 WiFi 配置 AP 的 SSID 和 Web 服务器 URL
    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += wifi_ap.GetSsid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += wifi_ap.GetWebServerUrl();
    hint += "\n\n";

    // 播报配置 WiFi 的提示
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "", Lang::Sounds::P3_WIFICONFIG);

    #if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    audio_wifi_config::ReceiveWifiCredentialsFromAudio(&application, &wifi_ap);
    #endif

    // Phase 1: 轮询检测新SSID（/submit 或 /save 触发SSID保存）
    ESP_LOGI(TAG, "Waiting for WiFi configuration...");
    bool ssid_saved = false;
    for (int i = 0; i < 240; i++) { // 最多等2分钟
        vTaskDelay(pdMS_TO_TICKS(500));
        if (ssid_manager.GetSsidList().size() > ssid_before) {
            ESP_LOGI(TAG, "New SSID detected, waiting for exit signal...");
            ssid_saved = true;
            break;
        }
    }

    // Phase 2: 等待 /exit 或 /reboot（保持 AP 运行直到客户端确认）
    if (ssid_saved) {
        for (int i = 0; i < 120; i++) { // 最多等60秒
            if (wifi_ap.IsExitRequested()) {
                ESP_LOGI(TAG, "Exit signal received, switching to station mode...");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    // 停AP，切换回Station模式（不重启！）
    ESP_LOGI(TAG, "Switching to station mode...");
    wifi_ap.Stop();
    wifi_config_mode_ = false;
}

void WifiBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();

    // 如果没有保存的SSID，进入配网模式
    auto& ssid_manager = SsidManager::GetInstance();
    if (ssid_manager.GetSsidList().empty() && !wifi_config_mode_) {
        wifi_config_mode_ = true;
    }

    // 配网模式（首次使用 或 用户长按进入）
    if (wifi_config_mode_) {
        EnterWifiConfigMode();  // 等待配网完成，返回后 wifi_config_mode_ 已清
        // 配网完成后继续往下走，尝试连接
    }

    // 尝试连接已保存的WiFi
    if (ssid_manager.GetSsidList().empty()) {
        ESP_LOGW(TAG, "Still no WiFi configured, re-entering config mode...");
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([this, display]() {
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this, display](const std::string& ssid) {
        std::string notification = Lang::Strings::CONNECT_TO + ssid + "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this, display](const std::string& ssid) {
        std::string notification = Lang::Strings::CONNECTED_TO + ssid;
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.Start();

    // 等60秒连接，失败则重新配网
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        ESP_LOGW(TAG, "WiFi connection failed, entering config mode...");
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        // 配网后又回到这里重试
        wifi_station.Start();
        if (!wifi_station.WaitForConnected(60 * 1000)) {
            wifi_station.Stop();
            ESP_LOGW(TAG, "Still failed, continuing without WiFi");
        }
    }
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    if (!wifi_config_mode_) {
        board_json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        cJSON_AddStringToObject(screen, "theme", display->GetTheme().c_str());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
