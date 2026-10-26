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
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include "afsk_demod.h"

static const char *TAG = "WifiBoard";

namespace {
/* 2026-10-21 esp-hosted 下 IP_EVENT_STA_GOT_IP 从不 post(WifiStation 的
   IpEventHandler 永不触发),而 esp_wifi_sta_get_ap_info 即使关联中也返回
   失败——两条常规判定都不可用。可靠信号只有两个:
   ① 连接成功 = STA netif 拿到非零 IP(lwip 回调日志 14785 可证)
   ② 断开 = WIFI_EVENT_STA_DISCONNECTED 事件(RPC 日志可证,可靠派发)
   因此:纯 IP 判定 + 断开事件里主动清零 netif IP,根治 stale IP 误判 */
bool wifi_sta_has_ip() {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

void wifi_on_sta_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data) {
    /* 断开即清零 netif IP:防 stale(断网后旧 IP 残留导致"已连接"误判);
       重连成功 esp_wifi_remote 会重新下发新 IP */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t zero = {};
        esp_netif_set_ip_info(sta, &zero);
    }
}
}  // namespace

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

    auto& ssid_manager = SsidManager::GetInstance();

    /* 注册断开事件清零 handler(一次性;断网后 netif IP 归零,判定不再误判) */
    static bool s_disc_registered = false;
    if (!s_disc_registered) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                   wifi_on_sta_disconnected, nullptr);
        s_disc_registered = true;
    }

    /* netif IP 轮询判定(见文件级 wifi_sta_has_ip 注释) */
    auto wait_ip = [&](int timeout_ms) -> bool {
        int waited = 0;
        while (waited < timeout_ms) {
            if (wifi_sta_has_ip()) {
                /* 2026-10-21 连上即重关省电:重连/切换热点后 C6 侧省电
                   恢复(MIN_MODEM)会掉到 ~1Mbps(手机热点实测 150KB/s vs
                   royal7 实测 1.3MB/s)。esp_wifi_set_ps 幂等,多调无害 */
                extern void wifi_power_save_off(void);
                wifi_power_save_off();
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            waited += 200;
        }
        return wifi_sta_has_ip();
    };

    /* 2026-09-20 新配网主路径:屏幕 WiFi 列表(手机式)。
       ① 有已存 SSID → 先自动尝试连接;
       ② 无已存/连接失败 → 弹屏幕 WiFi 列表(选网+密码键盘);
       ③ 用户在列表页跳过 → 旧 AP 热点配网兜底(保留)。 */
    /* 2026-09-29 开机仪式(用户拍板):加载条 → 开始唤醒按钮 → 点击进下一阶段 */
    extern void boot_ceremony_loading(const lv_font_t *font);
    extern void boot_ceremony_ready(void);
    extern int boot_ceremony_wait(int timeout_ms);
    extern void boot_ceremony_hide(void);
    extern void boot_cover_stop(void);
    boot_ceremony_loading(display->GetTextFont());   // 中文字体(对话框同款,LV_FONT_DEFAULT 无中文)

    bool auto_connected = false;
    if (!ssid_manager.GetSsidList().empty()) {
        auto& ws_try = WifiStation::GetInstance();
        ws_try.Start();
        /* 2026-10-21 窗口 8s→12s→netif 轮询:CP 固件 3.0.9 下 C6 WiFi 栈
           ~20s 才就绪,12s 内连不上很正常,点击后还有复查 */
        auto_connected = wait_ip(12 * 1000);
        /* 拿到 IP 再验证服务器可达:认证门户网络有 IP 无外网,不能算连上 */
        if (auto_connected) {
            extern int role_download_network_ok(int timeout_ms);
            auto_connected = (role_download_network_ok(3000) == 0);
            if (!auto_connected)
                ESP_LOGW(TAG, "Got IP but server unreachable (captive portal?)");
        }
    }

    /* 2026-09-29 加载完成:显示开始唤醒按钮(背景继续轮播),等用户点击 */
    boot_ceremony_ready();
    boot_ceremony_wait(300000);   /* 5 分钟兜底(用户不点也继续) */
    boot_ceremony_hide();
    boot_cover_stop();   /* 用户点击进入下一阶段:才停轮播(定格当前图垫底) */

    if (!auto_connected) {
        /* 2026-10-21 C6 WiFi 栈就绪晚:用户点击时(几秒~几十秒后)连接常已完成,
           点击后复查 netif IP(3s),连上就不弹列表页 */
        auto_connected = wait_ip(3 * 1000);
        if (auto_connected) {
            extern int role_download_network_ok(int timeout_ms);
            auto_connected = (role_download_network_ok(3000) == 0);
        }
    }

    if (!auto_connected) {
        /* 2026-09-21:不 Stop!P4 LP 核 Stop→Start 会永久卡死(扫描恒 12289
           且 LP 自己的周期扫描也停)——保持活站点进列表页,其 LP 周期扫描
           还在跑,ScanOnce 被动读取即可;跳过时由 wifi_ui_show 内部 Stop */
        extern bool wifi_ui_show(void);
        if (wifi_ui_show()) {
            return;   /* 屏幕列表选好并连接成功 */
        }
        /* 2026-09-26 配网页「离线模式」:设标志+跳过,不进 AP 兜底直接继续 */
        extern bool application_offline_mode(void);
        if (application_offline_mode()) return;

        /* 列表页跳过 → 旧 AP 热点配网兜底 */
        wifi_config_mode_ = true;
        EnterWifiConfigMode();   // 等待配网完成，返回后 wifi_config_mode_ 已清
    } else {
        /* 2026-10-21 已自动连接:直接进主流程。原代码无条件执行
           EnterWifiConfigMode,连上也被拉进 AP 并把刚连上的网踢断 */
        ESP_LOGI(TAG, "WiFi already connected, skip config mode");
        return;
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
    /* 2026-10-21 崩溃修复:配网 AP 退出后 Start 会重复创建同 key STA netif
       → IDF 5.5 断言崩溃。Start 前统一销毁旧 STA netif(Stop 不销毁)。 */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr) {
        esp_netif_destroy(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
    }
    wifi_station.Start();

    // 等60秒连接，失败则重新配网(netif IP 轮询判定)
    if (!wait_ip(60 * 1000)) {
        ESP_LOGW(TAG, "WiFi connection failed, entering config mode...");
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        // 配网后又回到这里重试。Stop() 不销毁 STA netif，重复 Start 会在
        // esp_netif_create_default_wifi_sta 里重复创建同 key netif 直接断言（IDF 5.5）
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr) {
            esp_netif_destroy(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
        }
        wifi_station.Start();
        if (!wait_ip(60 * 1000)) {
            wifi_station.Stop();
            /* 2026-10-21 用户拍板:没连上 WiFi 不得静默进主界面。
               最后再弹一次屏幕列表,选网成功正常进;再跳过则置离线模式
               (状态栏明确显示离线,不加载唤醒词模型) */
            extern bool wifi_ui_show(void);
            if (wifi_ui_show()) return;
            extern void application_set_offline_mode(void);
            application_set_offline_mode();
            ESP_LOGW(TAG, "No network after retries, entering offline mode");
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
    if (!wifi_sta_has_ip()) {
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = WifiStation::GetInstance().GetRssi();
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
    /* 2026-10-24 下载期间禁开省电:省电 MIN_MODEM 会把 WiFi 下行压到
       ~1Mbps(实测 100KB/s);音频事件在下载中途重开省电会再次掉速 */
    if (enabled) {
        extern bool role_download_busy(void);
        if (role_download_busy()) return;
    }
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
