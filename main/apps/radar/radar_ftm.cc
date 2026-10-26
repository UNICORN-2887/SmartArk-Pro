/* 交友雷达第三层:WiFi FTM 精确测距(2026-10-23)
 * C6 固件 3.0.10 开启 FTM(initiator + responder);host 侧经 esp_wifi_remote
 * 代理标准 esp_wifi_ftm_* API。设备 STA 连路由器 + SoftAP 做 responder,
 * 对扫到的邻居 SoftAP(SSID "SZFDAP-<uid>")发起 FTM session(不关联,
 * 不同信道走单 burst ASAP),事件回调拿 dist_est(厘米)。 */
#include "radar_ftm.h"
#include "radar_ble.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "wifi_station.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define TAG "RadarFtm"
#define FTM_AP_PREFIX "SZFDAP"
#define FTM_AP_PASS "szfd2026"
#define FTM_MAX_PEERS 8
#define FTM_PROBE_TIMEOUT_MS 2500

static EventGroupHandle_t s_ftm_evt = NULL;
#define FTM_DONE_BIT BIT0
static volatile int s_ftm_status = -1;
static volatile uint32_t s_ftm_dist = 0;

static uint32_t s_peer_uids[FTM_MAX_PEERS];
static uint32_t s_peer_dists[FTM_MAX_PEERS];
static int s_peer_n = 0;

static void ftm_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_FTM_REPORT) {
        wifi_event_ftm_report_t *ev = (wifi_event_ftm_report_t *)data;
        s_ftm_status = ev->status;
        s_ftm_dist = ev->dist_est;
        xEventGroupSetBits(s_ftm_evt, FTM_DONE_BIT);
    }
}

static void ftm_store(uint32_t uid, uint32_t dist) {
    for (int i = 0; i < s_peer_n; i++) {
        if (s_peer_uids[i] == uid) { s_peer_dists[i] = dist; return; }
    }
    if (s_peer_n < FTM_MAX_PEERS) {
        s_peer_uids[s_peer_n] = uid;
        s_peer_dists[s_peer_n] = dist;
        s_peer_n++;
    }
}

/* 对单个 AP 发起 FTM 测距,返回距离(厘米);失败返回 false */
static bool ftm_probe(const uint8_t bssid[6], uint8_t channel, uint32_t *dist_cm) {
    wifi_ftm_initiator_cfg_t cfg = {};
    memcpy(cfg.resp_mac, bssid, 6);
    cfg.channel = channel;
    cfg.frm_count = 8;        /* 8 bursts */
    cfg.burst_period = 2;     /* 200ms */
    cfg.use_get_report_api = false;
    s_ftm_status = -1;
    s_ftm_dist = 0;
    xEventGroupClearBits(s_ftm_evt, FTM_DONE_BIT);
    if (esp_wifi_ftm_initiate_session(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "FTM 发起失败(芯片不支持 initiator?)");
        return false;
    }
    xEventGroupWaitBits(s_ftm_evt, FTM_DONE_BIT, pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(FTM_PROBE_TIMEOUT_MS));
    esp_wifi_ftm_end_session();
    if (s_ftm_status == FTM_STATUS_SUCCESS) {
        *dist_cm = s_ftm_dist;
        return true;
    }
    return false;
}

/* ── 周期测距任务:每 10s 扫一遍邻居 SoftAP 并逐个 FTM ── */
static void radar_ftm_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));   /* 等 STA(set_mode APSTA 后)重连稳定 */
    for (;;) {
        std::vector<WifiApRecord> aps;
        if (WifiStation::GetInstance().ScanOnce(aps)) {
            for (auto &a : aps) {
                if (a.ssid.rfind(FTM_AP_PREFIX, 0) != 0) continue;
                uint32_t uid = (uint32_t)atoi(a.ssid.c_str() + strlen(FTM_AP_PREFIX) + 1);
                if (uid == 0 || uid == radar_bound_uid()) continue;
                uint32_t dist = 0;
                if (ftm_probe(a.bssid, (uint8_t)a.channel, &dist)) {
                    ESP_LOGI(TAG, "FTM uid=%lu 距离=%lu cm", (unsigned long)uid, (unsigned long)dist);
                    ftm_store(uid, dist);
                } else {
                    ESP_LOGD(TAG, "FTM uid=%lu 失败 status=%d", (unsigned long)uid, s_ftm_status);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

bool radar_ftm_start(void) {
    uint32_t uid = radar_bound_uid();
    if (uid == 0) { ESP_LOGW(TAG, "未绑定,FTM 停用"); return false; }

    s_ftm_evt = xEventGroupCreate();
    if (!s_ftm_evt) return false;
    if (esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, &ftm_event_cb, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "FTM 事件注册失败");
        return false;
    }

    /* APSTA:STA 保持连路由器 + SoftAP 做 FTM responder。
       set_mode 会短暂断网,设备自动重连(数秒),一次性代价 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) { ESP_LOGW(TAG, "set_mode APSTA 失败: %s", esp_err_to_name(err)); return false; }
    wifi_config_t ap_cfg = {};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s-%lu", FTM_AP_PREFIX, (unsigned long)uid);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", FTM_AP_PASS);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.ftm_responder = true;
    ap_cfg.ap.channel = 1;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) { ESP_LOGW(TAG, "SoftAP 配置失败: %s", esp_err_to_name(err)); return false; }
    ESP_LOGI(TAG, "FTM SoftAP 已启动: %s(responder)", ap_cfg.ap.ssid);

    xTaskCreate(radar_ftm_task, "radar_ftm", 8192, NULL, 3, NULL);
    return true;
}

uint32_t radar_ftm_dist_cm(uint32_t peer_uid) {
    for (int i = 0; i < s_peer_n; i++)
        if (s_peer_uids[i] == peer_uid) return s_peer_dists[i];
    return 0;
}
