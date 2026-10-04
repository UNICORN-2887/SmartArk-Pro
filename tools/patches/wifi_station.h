#ifndef _WIFI_STATION_H_
#define _WIFI_STATION_H_

#include <string>
#include <vector>
#include <functional>

#include <esp_event.h>
#include <esp_timer.h>
#include <esp_wifi_types_generic.h>

struct WifiApRecord {
    std::string ssid;
    std::string password;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
    int8_t rssi;    // 2026-09-20:扫描信号强度(屏幕列表用;连接队列项为 0)
};

class WifiStation {
public:
    static WifiStation& GetInstance();
    void AddAuth(const std::string &&ssid, const std::string &&password);
    void Start();
    void Stop();
    bool IsConnected();
    bool WaitForConnected(int timeout_ms = 10000);
    int8_t GetRssi();
    std::string GetSsid() const { return ssid_; }
    std::string GetIpAddress() const { return ip_address_; }
    uint8_t GetChannel();
    void SetPowerSaveMode(bool enabled);

    void OnConnect(std::function<void(const std::string& ssid)> on_connect);
    void OnConnected(std::function<void(const std::string& ssid)> on_connected);
    void OnScanBegin(std::function<void()> on_scan_begin);

    /* 2026-09-20 屏幕 WiFi 列表页接口:
       ScanOnce = 扫描一次,返回 AP 记录(信号降序,同 SSID 去重);
       ConnectTo = 主动连接(存 NVS 并立即发起连接) */
    bool ScanOnce(std::vector<WifiApRecord>& out);
    void ConnectTo(const std::string& ssid, const std::string& password);
    /* 2026-09-20:暂停/恢复组件定时扫描(屏幕列表页 ScanOnce 与定时扫描互斥,
       并发扫描会 ESP_ERR_WIFI_STATE) */
    void PauseScanTimer(bool pause);
    /* 2026-09-21:P4 LP 核 Stop→Start 会永久卡死(恒 12289)——调用方须先查
       IsStarted 避免重启活站点 */
    bool IsStarted() const { return started_; }
    /* 2026-09-21:列表页浏览期间置 true,HandleScanResult 不自动连已存 SSID
       (避免抢用户正在进行的选网操作) */
    void SetManualConfig(bool manual) { manual_config_ = manual; }

private:
    WifiStation();
    ~WifiStation();
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    EventGroupHandle_t event_group_;
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_event_handler_instance_t instance_any_id_ = nullptr;
    esp_event_handler_instance_t instance_got_ip_ = nullptr;
    std::string ssid_;
    std::string password_;
    std::string ip_address_;
    int8_t max_tx_power_;
    uint8_t remember_bssid_;
    int reconnect_count_ = 0;
    bool started_ = false;
    bool manual_config_ = false;
    bool scan_timer_paused_ = false;
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void()> on_scan_begin_;
    std::vector<WifiApRecord> connect_queue_;

    void HandleScanResult();
    void StartConnect();
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
};

#endif // _WIFI_STATION_H_
