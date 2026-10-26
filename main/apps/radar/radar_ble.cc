/* 交友雷达第一层:BLE 信标(2026-10-22)
 * 设备周期广播 mfg data "SZFD:<uid>"(只有真设备会发,普通手机不会);
 * 同时扫描附近同类广播,RSSI 测距,上报服务器匹配"附近的人"。
 * 参考 esp-hosted bleprph_minimal 示例:esp_hosted_bt_host_stack_setup
 * 是唯一 esp-hosted 特有调用,其余为标准 NimBLE。 */
#include "radar_ble.h"
#include "radar_ftm.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <vector>

#include "esp_log.h"
#include "nvs_flash.h"
#include "settings.h"
#include "application.h"
#include <dirent.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_hosted_bt_host_stack.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "system_info.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "wifi_station.h"

#define TAG "Radar"
#ifndef RADAR_DISABLED
#define RADAR_DISABLED 0
#endif

#define RADAR_MFG_PREFIX "SZFD:"
#define RADAR_MAX_PEERS 32

typedef struct {
    uint32_t uid;
    int8_t rssi;
    uint32_t last_ms;   /* 2026-10-23 最后收到时间:TTL 清理,防旧值残留 */
} radar_peer_t;

static radar_peer_t s_peers[RADAR_MAX_PEERS];
static int s_peer_n = 0;
static uint8_t s_own_addr_type;
static volatile bool s_ble_ready = false;
static uint32_t s_adv_uid = 0;   /* 当前广播身份(2026-10-22:词表晚于广播,动态更新用) */
static volatile bool s_radar_paused = false;   /* 2026-10-23 OTA 期间暂停 */
static char s_nearby_json[2048] = {0};
static radar_user_t s_nearby_users[16];
static int s_nearby_n = 0;
static uint32_t s_nearby_ts = 0;
static char s_me_name[32] = {0};
static float s_me_heading = -1;   /* 行走方向角(度,-1=未知;2026-10-24 车头朝上) */
/* WiFi 定位:周期扫描周围 AP,上报服务器换坐标(2026-10-22) */
#define RADAR_AP_MAX 10
static struct { uint8_t bssid[6]; int8_t rssi; } s_aps[RADAR_AP_MAX];
static int s_ap_n = 0;
/* 每 peer 最近 3 次 RSSI(趋势:靠近/远离) */
static int8_t s_rssi_hist[16][3];
static int s_rssi_hist_n[16] = {0};

/* ── GAP 事件:扫描结果收集 ── */
static int radar_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        if (rc != 0) return 0;
        /* 只认本项目的广播: mfg data = "SZFD:<uid>" */
        if (fields.mfg_data && fields.mfg_data_len >= (int)strlen(RADAR_MFG_PREFIX) + 1 &&
            memcmp(fields.mfg_data, RADAR_MFG_PREFIX, strlen(RADAR_MFG_PREFIX)) == 0) {
            char ubuf[16];
            size_t ulen = fields.mfg_data_len - strlen(RADAR_MFG_PREFIX);
            if (ulen >= sizeof(ubuf)) ulen = sizeof(ubuf) - 1;
            memcpy(ubuf, fields.mfg_data + strlen(RADAR_MFG_PREFIX), ulen);
            ubuf[ulen] = 0;
            uint32_t uid = (uint32_t)strtoul(ubuf, NULL, 10);
            if (uid == 0) return 0;
            /* 去重:同 uid 按 EMA 平滑(2026-10-23:旧"只升不降"取最强,
               设备走远后 BLE 值仍偏强,距离不敏感) */
            for (int i = 0; i < s_peer_n; i++) {
                if (s_peers[i].uid == uid) {
                    s_peers[i].rssi = (int8_t)((s_peers[i].rssi * 2 + event->disc.rssi) / 3);
                    s_peers[i].last_ms = (uint32_t)(esp_timer_get_time() / 1000);
                    return 0;
                }
            }
            if (s_peer_n < RADAR_MAX_PEERS) {
                s_peers[s_peer_n].uid = uid;
                s_peers[s_peer_n].rssi = event->disc.rssi;
                s_peers[s_peer_n].last_ms = (uint32_t)(esp_timer_get_time() / 1000);
                s_peer_n++;
                ESP_LOGI(TAG, "发现信标 uid=%lu rssi=%d", (unsigned long)uid, (int)event->disc.rssi);
            }
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        /* 扫描窗口结束:通知上报任务 */
    }
    return 0;
}

/* ── 广播自己身份 ── */
static void radar_advertise(void) {
    uint32_t uid = radar_bound_uid();
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    char mfg[32];
    int mfg_len;
    int rc;

    mfg_len = snprintf(mfg, sizeof(mfg), RADAR_MFG_PREFIX "%lu", (unsigned long)uid);
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = (uint8_t *)mfg;
    fields.mfg_data_len = mfg_len;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv set fields failed: %d", rc); return; }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 快广播 100-200ms(0.625ms 单位):被动扫描 5s 窗口内必然可见 */
    adv_params.itvl_min = 160;
    adv_params.itvl_max = 320;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, radar_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else if (s_adv_uid != uid) {
        s_adv_uid = uid;
        ESP_LOGI(TAG, "雷达广播中: %s", mfg);
    }
}

/* ── 绑定 uid 晚于广播到达时更新广播身份(2026-10-22) ── */
static void radar_update_adv_uid(void) {
    uint32_t uid = radar_bound_uid();
    if (uid == 0 || uid == s_adv_uid) return;
    char mfg[32];
    int mfg_len = snprintf(mfg, sizeof(mfg), RADAR_MFG_PREFIX "%lu", (unsigned long)uid);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = (uint8_t *)mfg;
    fields.mfg_data_len = mfg_len;
    if (ble_gap_adv_set_fields(&fields) == 0) {
        s_adv_uid = uid;
        ESP_LOGI(TAG, "雷达广播更新: %s", mfg);
    }
}

/* ── 扫描(600ms 短窗口:快速轮换,广播占比高) ── */
static void radar_scan_once(void) {
    struct ble_gap_disc_params dp;
    int rc;
    memset(&dp, 0, sizeof(dp));
    dp.filter_duplicates = 0;
    dp.passive = 1;   /* 被动扫描:广播已够,不主动请求 */
    dp.itvl = 0;
    dp.window = 0;
    /* 2026-10-22 快速轮换:600ms 扫描 / ~1.9s 广播(周期 2.5s)。
       5s 大窗口方案下两台设备扫描窗口重叠即整轮互盲,且 16s 才重掷一次;
       亚秒轮换让重叠状态最多持续一轮(2.5s 后重掷),广播占比 76% 也更高。
       s_peers 跨轮累积不清零,上报前才清(见 radar_report) */
    rc = ble_gap_disc(s_own_addr_type, 600, &dp, radar_gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "扫描启动失败: %d", rc);
}

/* ── 上报 + 拉取附近(2026-10-24 合并为一个 ping 请求) ── */
static void radar_report(void) {
    /* 2026-10-23 TTL 清理(6s ≈ 2 轮扫描):每轮上报时 beacons 带最近
       2 轮并集,数据量与旧"3 轮累积"相当;走远/关机的信标快速消失 */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int w = 0;
    for (int i = 0; i < s_peer_n; i++) {
        if (now_ms - s_peers[i].last_ms <= 6000) {
            if (w != i) s_peers[w] = s_peers[i];
            w++;
        }
    }
    s_peer_n = w;

    /* 组装 beacons JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON *arr = cJSON_AddArrayToObject(root, "beacons");
    for (int i = 0; i < s_peer_n; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "u", s_peers[i].uid);
        cJSON_AddNumberToObject(b, "r", s_peers[i].rssi);
        cJSON_AddItemToArray(arr, b);
    }
    /* WiFi 定位:周围 AP 列表(2026-10-22,服务器换坐标) */
    cJSON *aarr = cJSON_AddArrayToObject(root, "aps");
    for (int i = 0; i < s_ap_n; i++) {
        cJSON *ap = cJSON_CreateObject();
        char macbuf[18];
        snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 s_aps[i].bssid[0], s_aps[i].bssid[1], s_aps[i].bssid[2],
                 s_aps[i].bssid[3], s_aps[i].bssid[4], s_aps[i].bssid[5]);
        cJSON_AddStringToObject(ap, "m", macbuf);
        cJSON_AddNumberToObject(ap, "r", s_aps[i].rssi);
        cJSON_AddItemToArray(aarr, ap);
    }
    /* ESP-NOW 邻居(2026-10-23 连续采样,200ms/样本,并入三边解算) */
    {
        extern int radar_esnow_peers(uint32_t *uids, int8_t *rssis, int max);
        uint32_t euids[8];
        int8_t erssi[8];
        int en = radar_esnow_peers(euids, erssi, 8);
        if (en > 0) {
            cJSON *earr = cJSON_AddArrayToObject(root, "en");
            for (int i = 0; i < en; i++) {
                cJSON *eb = cJSON_CreateObject();
                cJSON_AddNumberToObject(eb, "u", euids[i]);
                cJSON_AddNumberToObject(eb, "r", erssi[i]);
                cJSON_AddItemToArray(earr, eb);
            }
        }
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return;

    esp_http_client_config_t cfg = {};
    std::string url = std::string("http://124.221.186.33/api/radar/ping");
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.method = HTTP_METHOD_POST;
    /* 2026-10-24 ping/nearby 合并:ping 响应即 nearby 数据,收集响应体;
       一轮一个请求,更新频率 = ping 频率(根治 nearby 间歇黑洞) */
    struct Nc { char b[2048]; int n; } ctx = {{0}, 0};
    cfg.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        Nc *c2 = (Nc *)evt->user_data;
        if (evt->event_id == HTTP_EVENT_ON_DATA && c2->n < (int)sizeof(c2->b) - 1) {
            int n2 = evt->data_len;
            if (n2 > (int)sizeof(c2->b) - 1 - c2->n) n2 = (int)sizeof(c2->b) - 1 - c2->n;
            memcpy(c2->b + c2->n, evt->data, n2);
            c2->n += n2;
        }
        return ESP_OK;
    };
    cfg.user_data = &ctx;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, strlen(body));
        esp_http_client_perform(c);
        esp_http_client_cleanup(c);
    }
    free(body);

    /* 2026-10-24 ping/nearby 合并:解析 ping 响应(即 nearby 数据),
       不再有独立的 nearby 请求 */
    if (ctx.n > 0) {
        char *buf = ctx.b;
        int total = ctx.n;
        buf[total] = 0;
        snprintf(s_nearby_json, sizeof(s_nearby_json), "%s", buf);
        ESP_LOGI(TAG, "nearby: %.200s", buf);
            /* 解析到用户数组(供雷达页) */
            cJSON *nj = cJSON_Parse(buf);
            if (nj) {
                cJSON *jme = cJSON_GetObjectItem(nj, "me");
                if (jme) {
                    cJSON *jmn = cJSON_GetObjectItem(jme, "name");
                    snprintf(s_me_name, sizeof(s_me_name), "%s",
                             cJSON_IsString(jmn) ? jmn->valuestring : "");
                }
                /* 行走朝向(2026-10-24):盘面"车头朝上"旋转角 */
                cJSON *jhd = cJSON_GetObjectItem(nj, "heading");
                if (cJSON_IsNumber(jhd)) s_me_heading = (float)jhd->valuedouble;
                else s_me_heading = -1;
                cJSON *arr = cJSON_GetObjectItem(nj, "nearby");
                int k = 0;
                if (cJSON_IsArray(arr)) {
                    cJSON *it;
                    cJSON_ArrayForEach(it, arr) {
                        if (k >= 16) break;
                        cJSON *ju = cJSON_GetObjectItem(it, "uid");
                        cJSON *jn = cJSON_GetObjectItem(it, "name");
                        cJSON *jd = cJSON_GetObjectItem(it, "dist");
                        cJSON *jr = cJSON_GetObjectItem(it, "rssi");
                        if (!cJSON_IsNumber(ju)) continue;
                        s_nearby_users[k].uid = (uint32_t)ju->valuedouble;
                        snprintf(s_nearby_users[k].name, sizeof(s_nearby_users[k].name), "%s",
                                 cJSON_IsString(jn) ? jn->valuestring : "?");
                        snprintf(s_nearby_users[k].dist, sizeof(s_nearby_users[k].dist), "%s",
                                 cJSON_IsString(jd) ? jd->valuestring : "远");
                        int8_t rssi = cJSON_IsNumber(jr) ? (int8_t)jr->valuedouble : -100;
                        /* 趋势:与历史均值比(差值 >=3dB 判靠近/远离) */
                        int h = s_rssi_hist_n[k];
                        if (h >= 3) {
                            memmove(s_rssi_hist[k], s_rssi_hist[k] + 1, 2);
                            h = 2;
                        }
                        s_rssi_hist[k][h++] = rssi;
                        s_rssi_hist_n[k] = h;
                        int32_t avg = 0;
                        for (int hi = 0; hi < h; hi++) avg += s_rssi_hist[k][hi];
                        avg /= h;
                        s_nearby_users[k].trend = (rssi - avg >= 3) ? 1 : ((avg - rssi >= 3) ? -1 : 0);
                        cJSON *js2 = cJSON_GetObjectItem(it, "side");
                        s_nearby_users[k].side = cJSON_IsNumber(js2) ? (int8_t)js2->valuedouble : 0;
                        cJSON *jdir = cJSON_GetObjectItem(it, "dir");
                        s_nearby_users[k].dir = cJSON_IsNumber(jdir) ? (int16_t)jdir->valuedouble : -1;
                        cJSON *jdm = cJSON_GetObjectItem(it, "dist_m");
                        s_nearby_users[k].dist_m = cJSON_IsNumber(jdm) ? (uint16_t)jdm->valuedouble : 0;
                        cJSON *jrx = cJSON_GetObjectItem(it, "rel_x");
                        cJSON *jry = cJSON_GetObjectItem(it, "rel_y");
                        if (cJSON_IsNumber(jrx) && cJSON_IsNumber(jry)) {
                            s_nearby_users[k].rel_x = (float)jrx->valuedouble;
                            s_nearby_users[k].rel_y = (float)jry->valuedouble;
                            s_nearby_users[k].has_rel = 1;
                        } else {
                            s_nearby_users[k].has_rel = 0;
                        }
                        s_nearby_users[k].rssi = rssi;
                        s_nearby_users[k].ts = (uint32_t)(esp_timer_get_time() / 1000000);
                        k++;
                    }
                }
                s_nearby_n = k;
                s_nearby_ts = (uint32_t)(esp_timer_get_time() / 1000000);
                cJSON_Delete(nj);
        } else {
            ESP_LOGW(TAG, "nearby JSON 解析失败: %.120s", buf);
        }
    }
    /* 上报完成:不再全清(2026-10-23 起每轮上报,信标靠 TTL 6s 过期;
       全清会让 beacons 只剩本轮 600ms 扫描,邻居数骤降) */
}

/* ── NimBLE 同步回调 ── */
static void radar_on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr: %d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto: %d", rc); return; }
    radar_advertise();
    s_ble_ready = true;
}

static int64_t s_ble_disable_until = 0;   /* 2026-10-26 连续 reset 熔断时间点(us) */

static void radar_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE reset(%d)", reason);
    /* 2026-10-26 HCI 连续错误(SDIO 与下载/probe 争抢时)触发 NimBLE reset
       循环,多次 reset 后 NimBLE 内部内存损坏 → Load access fault 崩溃
       (用户实测:点角色缩略图即崩)。30s 内 reset ≥3 次 → 熔断 BLE 60s,
       避开 SDIO 拥塞窗口,从根上杜绝崩溃循环;期间 ESP-NOW 雷达照常 */
    static int reset_streak = 0;
    static int64_t last_reset_ms = 0;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_reset_ms < 30000) reset_streak++;
    else reset_streak = 1;
    last_reset_ms = now_ms;
    if (reset_streak >= 3) {
        ESP_LOGE(TAG, "NimBLE 连续 reset %d 次,熔断 BLE 60s 防崩溃", reset_streak);
        s_ble_disable_until = esp_timer_get_time() + 60000000;
        reset_streak = 0;
        s_ble_ready = false;
    }
}

static void radar_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── 周期任务:快速轮换 600ms 扫描 / ~1.9s 广播,每 3 轮(约 7.5s)上报一次 ── */
static void radar_wifi_scan(void);   /* 前向声明(定义见后) */

static void radar_task(void *arg) {
    (void)arg;
    /* 等网络与 BLE 就绪 */
    for (int i = 0; i < 300 && !s_ble_ready; i++) vTaskDelay(pdMS_TO_TICKS(100));
    int round = 0;
    for (;;) {
        /* 2026-10-26 熔断期:连续 reset 后 BLE 停用 60s(防崩溃循环) */
        while (esp_timer_get_time() < s_ble_disable_until) vTaskDelay(pdMS_TO_TICKS(500));
        /* 2026-10-23 CP OTA 期间暂停:扫描事件走 SDIO HCI,与 OTA 传输抢总线 */
        while (s_radar_paused) vTaskDelay(pdMS_TO_TICKS(500));
        radar_update_adv_uid();   /* 词表晚到时更新广播身份(SZFD:0 → SZFD:uid) */
        radar_scan_once();
        vTaskDelay(pdMS_TO_TICKS(1000));   /* 600ms 扫描窗口 + 余量 */
        /* 广播持续运行(ble_gap_adv_start 重复调用会 EALREADY),无需每轮重启 */
        round++;
        /* 2026-10-23 每轮上报(约 3.1s):ESP-NOW 500ms 采样不再囤 9.3s,
           服务器 rel 响应降到秒级;beacons 数据量由 TTL 6s 并集保证 */
        radar_report();
        if (round % 24 == 0) {
            radar_wifi_scan();   /* 约 60s 一次:扫 AP 供服务器 WiFi 定位 */
        }
        vTaskDelay(pdMS_TO_TICKS(1500));   /* 广播期 */
    }
}

/* ── WiFi 定位:周期扫描周围 AP,上报服务器换坐标(2026-10-22) ── */
static void radar_wifi_scan(void) {
    auto &ws = WifiStation::GetInstance();
    if (!ws.IsConnected()) { s_ap_n = 0; return; }
    std::vector<WifiApRecord> aps;
    /* ScanOnce 阻塞扫描数秒;配网页正在扫描时会失败,放弃本轮即可 */
    if (!ws.ScanOnce(aps)) return;
    s_ap_n = 0;
    for (auto &a : aps) {
        if (s_ap_n >= RADAR_AP_MAX) break;
        memcpy(s_aps[s_ap_n].bssid, a.bssid, 6);
        s_aps[s_ap_n].rssi = a.rssi;
        s_ap_n++;
    }
}

/* ── 对外接口 ── */
uint32_t radar_bound_uid(void) {
    /* 与 ImageDisplay::scan_bound_user_uid 一致:应用级绑定 uid + SD 目录兜底
       (此前用 Settings("wakewords") 读 NVS 键名不对,恒得 0) */
    int uid = Application::GetInstance().bound_user_uid();
    if (uid > 0) return (uint32_t)uid;
    /* NVS 持久化绑定(词表到达后写入):开机早期即有,不等词表(2026-10-22,
       否则新绑定设备广播 SZFD:0,其他设备按 uid==0 过滤 → 永久隐形) */
    Settings s("wakewords", false);
    uid = s.GetInt("bound_uid", 0);
    if (uid > 0) return (uint32_t)uid;
    DIR *d = opendir("/sdcard/_users");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == 'u' && e->d_name[1] >= '0' && e->d_name[1] <= '9') {
            uid = atoi(e->d_name + 1);
            if (uid > 0) break;
        }
    }
    closedir(d);
    return (uint32_t)uid;
}

void radar_ble_start(void) {
#if RADAR_DISABLED
    ESP_LOGW(TAG, "雷达已禁用(对照实验开关 RADAR_DISABLED=1)");
    return;
#endif
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_hosted_bt_host_stack_cfg_t bt = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    err = esp_hosted_bt_host_stack_setup(&bt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BT 栈初始化失败(%s),雷达停用", esp_err_to_name(err));
        return;
    }
    err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err)); return; }

    ble_hs_cfg.sync_cb = radar_on_sync;
    ble_hs_cfg.reset_cb = radar_on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    nimble_port_freertos_init(radar_host_task);
    xTaskCreate(radar_task, "radar", 8192, NULL, 3, NULL);
    ESP_LOGI(TAG, "雷达第一层已启动");

    /* 第三层:WiFi FTM 精确测距 —— 2026-10-23 停用:
       esp-hosted 无 FTM RPC(空壳 NOT_SUPPORTED),且 SoftAP(信道1)与
       STA(信道11)的 APSTA 分时可能干扰 ESP-NOW 收发 */
    // radar_ftm_start();

    /* 第四层:ESP-NOW 连续测距(C6 esphome 固件 overlay + host shim;
       失败不致命——C6 无 overlay 时 esp_now_init 报错并停用) */
    extern bool radar_esnow_start(void);
    radar_esnow_start();
}

/* ── nearby 数据接口 ── */
float radar_me_heading(void) {
    /* 2026-10-24 保持最后有效值:停止行走后方向不丢(旧逻辑
       服务器 30s 无位移→None,停下看屏时盘面回全局坐标) */
    static float s_last_valid = -1;
    if (s_me_heading >= 0) s_last_valid = s_me_heading;
    return s_last_valid;
}

void radar_flip_request(void) {
    /* 一键翻转(2026-10-24):镜像歧义纠正,服务器 _G_SIGN 取反 */
    esp_http_client_config_t cfg = {};
    cfg.url = "http://124.221.186.33/api/radar/flip";
    cfg.timeout_ms = 5000;
    cfg.method = HTTP_METHOD_POST;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_perform(c);
        esp_http_client_cleanup(c);
    }
    s_me_heading = -1;   /* 翻转后旧 heading 失效,等下一轮刷新 */
}

float radar_nearby_age(void) {
    /* 距上次 nearby 成功解析的秒数(2026-10-24 雷达页更新率显示) */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    return (float)(now - s_nearby_ts);
}

int radar_nearby_list(radar_user_t *out, int max) {
    int n = 0;
    for (int i = 0; i < s_nearby_n && n < max; i++) {
        out[n++] = s_nearby_users[i];
    }
    return n;
}

const char *radar_me_name(void) {
    return s_me_name;
}

void radar_ble_pause(void) {
    s_radar_paused = true;
}

void radar_ble_resume(void) {
    s_radar_paused = false;
}

bool radar_ble_paused(void) {
    return s_radar_paused;
}
