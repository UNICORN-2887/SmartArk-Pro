#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "wifi"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 5

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
    }
    err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
    }
    err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
    if (err != ESP_OK) {
        remember_bssid_ = 0;
    }
    nvs_close(nvs);
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    started_ = false;
    scan_timer_paused_ = false;
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }
    
    // 取消注册事件处理程序
    if (instance_any_id_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_));
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_));
        instance_got_ip_ = nullptr;
    }

    // Reset the WiFi stack
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    // 2026-09-17:销毁 STA netif 前必须先摘默认 driver/handlers——否则晚到的
    // STA_DISCONNECTED 事件(如 reason=201)会触发默认 handler 对已销毁 netif
    // 执行 down,DHCP 结构野指针崩溃(dhcp_release_and_stop Load access fault)。
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != nullptr) {
        esp_wifi_clear_default_wifi_driver_and_handlers(sta);
        esp_netif_destroy(sta);
    }
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

bool WifiStation::ScanOnce(std::vector<WifiApRecord>& out) {
    /* 2026-09-21 v3:本板(P4 + LP 核 WiFi)固件不接受主机主动扫描(block/async 恒
       ESP_ERR_WIFI_STATE 12289),但 LP 核自己在 STA 断开期间以 ~2.4s 周期持续
       扫描并推送 SCAN_DONE——主机只需被动等下一轮 SCAN_DONE 后读记录。
       因此:先试一次主动(其它板正常路径),失败即转入被动等待,最多等 3 轮。 */
    out.clear();
    static EventGroupHandle_t scan_evt = NULL;
    if (!scan_evt) scan_evt = xEventGroupCreate();
    esp_event_handler_instance_t inst = nullptr;
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
        [](void *arg, esp_event_base_t base, int32_t id, void *data) {
            /* 2026-09-21 诊断:C6 协处理器在 SCAN_DONE 事件里自报 status/number——
                status≠0=CP 扫描失败;number=CP 侧存到的 AP 数(与 get_ap_num RPC
               返回对比即可定位"扫描没结果"还是"结果取不回") */
            wifi_event_sta_scan_done_t *evt = (wifi_event_sta_scan_done_t *)data;
            if (evt) {
                ESP_LOGI("WifiStation", "SCAN_DONE status=%u number=%u scan_id=%u",
                         (unsigned)evt->status, (unsigned)evt->number, (unsigned)evt->scan_id);
            }
            xEventGroupSetBits((EventGroupHandle_t)arg, 1);
        }, scan_evt, &inst);

    xEventGroupClearBits(scan_evt, 1);
    esp_err_t err = esp_wifi_scan_start(nullptr, false);
    bool passive = (err != ESP_OK);
    if (passive) {
        ESP_LOGW("WifiStation", "active scan start fail %d, waiting for LP periodic scan", (int)err);
    }

    for (int round = 0; round < 3; round++) {
        xEventGroupClearBits(scan_evt, 1);
        /* 2026-09-26:等待窗口统一 6s——LP 核 ~2.4s 周期扫描必然在 6s 内推
           SCAN_DONE(无论主动扫描是否被接受);原主动模式 15s 窗口导致首次
           扫描 3 轮耗时 45s 全空,用户须手动刷新第二次才有结果 */
        if (!(xEventGroupWaitBits(scan_evt, 1, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(6000)) & 1)) {
            break;   /* 6s 内无 LP 扫描(站点卡死?)→ 失败 */
        }
        uint16_t n = 0;
        esp_err_t e1 = esp_wifi_scan_get_ap_num(&n);
        if (e1 != ESP_OK) {
            ESP_LOGW("WifiStation", "get_ap_num err 0x%x (round %d)", (int)e1, round);
        } else {
            ESP_LOGI("WifiStation", "get_ap_num ok n=%u (round %d)", n, round);
        }
        if (n == 0) {
            /* 2026-09-21 兼容尝试:num 报 0(或 num RPC 坏)时仍直接取一次记录,
               CP 侧可能实际有记录只是 num 通道有问题 */
            n = 32;
        }
        wifi_ap_record_t* recs = (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t));
        if (!recs) break;
        uint16_t got = n;
        esp_err_t e2 = esp_wifi_scan_get_ap_records(&got, recs);
        if (e2 != ESP_OK || got == 0) {
            ESP_LOGW("WifiStation", "get_ap_records err 0x%x got=%u (round %d)", (int)e2, got, round);
            free(recs);
            continue;   /* 被动模式等下一轮 LP 扫描 */
        }
        std::sort(recs, recs + got, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
            return a.rssi > b.rssi;
        });
        for (int i = 0; i < (int)got; i++) {
            if (!recs[i].ssid[0]) continue;   // 隐藏 SSID 不显示
            WifiApRecord r;
            r.ssid = (char*)recs[i].ssid;
            r.channel = recs[i].primary;
            r.authmode = recs[i].authmode;
            r.rssi = recs[i].rssi;
            memcpy(r.bssid, recs[i].bssid, 6);
            bool dup = false;
            for (auto& x : out)
                if (x.ssid == r.ssid) { dup = true; break; }
            if (!dup) out.push_back(std::move(r));
        }
        free(recs);
        ESP_LOGI("WifiStation", "ScanOnce got %d APs (passive=%d, round=%d)", (int)got, passive, round);
        if (inst) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, inst);
        return !out.empty();
    }
    if (inst) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, inst);
    ESP_LOGW("WifiStation", "ScanOnce failed (passive=%d)", passive);
    return false;
}

void WifiStation::ConnectTo(const std::string& ssid, const std::string& password) {
    /* 2026-09-20 屏幕列表主动连接:存 NVS + 立即发起 */
    auto& sm = SsidManager::GetInstance();
    sm.AddSsid(ssid, password);
    WifiApRecord rec;
    rec.ssid = ssid;
    rec.password = password;
    connect_queue_.push_back(rec);
    /* 2026-09-29 手动切换网络:已连旧网络时 CP 2.3.2 对 set_config+connect
       不生效(选 royal7 却还停在 sdu_guest)——先断开再连接 */
    if (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));   // 等断开事件处理
        xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    }
    StartConnect();
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::Start() {
    started_ = true;
    manual_config_ = false;
    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));

    // Create the default event loop
    esp_netif_create_default_wifi_sta();

    // Initialize the WiFi stack in station mode
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            esp_wifi_scan_start(nullptr, false);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

void WifiStation::PauseScanTimer(bool pause) {
    /* 2026-09-20:屏幕列表页扫描期间暂停定时器,恢复时重新安排 10s 后扫描 */
    scan_timer_paused_ = pause;
    if (!timer_handle_) return;
    if (pause) {
        esp_timer_stop(timer_handle_);
    } else {
        esp_timer_start_once(timer_handle_, 10 * 1000);
    }
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    /* 2026-09-21:P4 LP 核即使已连接也会持续推送 SCAN_DONE,已连接时不要重连;
       列表页浏览期间(manual_config_)也不要自动连已存 SSID 抢用户操作 */
    if (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED) return;
    if (manual_config_) {
        ESP_LOGI(TAG, "manual config mode, skip autoconnect");
        return;
    }

    uint16_t ap_num = 0;
    esp_err_t num_err = esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        ESP_LOGI(TAG, "Wait for next scan (ap_num=0 err=0x%x)", (int)num_err);
        if (!scan_timer_paused_ && timer_handle_) esp_timer_start_once(timer_handle_, 10 * 1000);
        return;
    }
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    // sort by rssi descending
    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "Wait for next scan (ap_num=%u no saved match)", ap_num);
        if (!scan_timer_paused_ && timer_handle_) esp_timer_start_once(timer_handle_, 10 * 1000);
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    /* 2026-09-29 清旧网络残留 CONNECTED 位:WaitForConnected 会误判旧网络"已连上",
       配网页放行进索引页(实际新网络没连) */
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Get station info
    // 2026-10-03 容错:进配网页时 WiFi 可能未连接(ESP_ERR_WIFI_NOT_CONNECT),
    // 原 ESP_ERROR_CHECK 直接 abort;未连接返回 0(用户批准修改)
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return 0;
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Get station info
    // 2026-10-03 容错(同上:未连接返回 0)
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return 0;
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    ESP_ERROR_CHECK(esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_scan_start(nullptr, false);
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, wait for next scan");
        if (!this_->scan_timer_paused_ && this_->timer_handle_)
            esp_timer_start_once(this_->timer_handle_, 10 * 1000);
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->manual_config_ = false;
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
}
