/* 交友雷达第四层:ESP-NOW 连续测距(2026-10-23)
 * host shim(esp_now_hosted.cpp)把 esp_now.h 桥到 C6 的 esphome overlay;
 * 本层:每 200ms 广播 "SZFDN:<uid>",接收侧拿 src_addr + rx_ctrl->rssi
 * 连续采样(比 BLE 的 7.5s 一轮快 50 倍),供服务器三边解算/平滑。 */
#include "radar_ble.h"

#include "sdkconfig.h"
#include "esp_log.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA)

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define TAG "RadarEsnow"
#define ESNOW_PREFIX "SZFDN:"
/* 2026-10-24 心跳 500ms→1000ms:C6 射频/SDIO 负载减半(与 BLE 扫描、
   WiFi 数据、LBS HTTP 共存),每轮 ping(3.1s)仍有 ~3 个样本,EMA 足够 */
#define ESNOW_BCAST_MS 1000
/* 邻居超时:心跳 1s,4s(4 跳)未收到视为离线。
   2026-10-23 修复:旧缓存只增不减 → 关机设备残留 forever,服务器
   每轮都收到旧 RSSI → 其他机器永远显示已关闭的设备 */
#define ESNOW_PEER_TTL_MS 4000

/* 测向模式:心跳临时加速到 200ms(3s 采样窗口内 ~15 样本,中位数抗噪;
   2026-10-24 四点测向三向采样用) */
static volatile int s_fast_ms = 0;

void radar_esnow_set_fast(int on) {
    s_fast_ms = on ? 200 : 0;
}

/* 最近一次收到的邻居 uid(去重:同 uid 按 EMA 平滑 rssi) */
#define ESNOW_MAX_PEERS 8
static uint32_t s_peer_uids[ESNOW_MAX_PEERS];
static int8_t s_peer_rssi[ESNOW_MAX_PEERS];
static uint32_t s_peer_last_ms[ESNOW_MAX_PEERS];   /* 最后收到时间(esp_timer) */
static int s_peer_n = 0;

static void esnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (data == nullptr || len < (int)strlen(ESNOW_PREFIX) + 1) return;
    if (memcmp(data, ESNOW_PREFIX, strlen(ESNOW_PREFIX)) != 0) return;
    char ubuf[16];
    size_t ulen = (size_t)len - strlen(ESNOW_PREFIX);
    if (ulen >= sizeof(ubuf)) ulen = sizeof(ubuf) - 1;
    memcpy(ubuf, data + strlen(ESNOW_PREFIX), ulen);
    ubuf[ulen] = 0;
    uint32_t uid = (uint32_t)strtoul(ubuf, nullptr, 10);
    if (uid == 0) return;
    int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : -100;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < s_peer_n; i++) {
        if (s_peer_uids[i] == uid) {
            /* 2026-10-23 EMA 平滑:旧逻辑只升不降(取最强),设备走远后
               RSSI 仍保持历史最强 → 距离偏近且不随移动更新 */
            s_peer_rssi[i] = (int8_t)((s_peer_rssi[i] * 2 + rssi) / 3);
            s_peer_last_ms[i] = now;
            return;
        }
    }
    if (s_peer_n < ESNOW_MAX_PEERS) {
        s_peer_uids[s_peer_n] = uid;
        s_peer_rssi[s_peer_n] = rssi;
        s_peer_last_ms[s_peer_n] = now;
        s_peer_n++;
        ESP_LOGI(TAG, "发现 ESP-NOW 邻居 uid=%lu rssi=%d", (unsigned long)uid, (int)rssi);
    }
}

/* 添加广播 peer 并显式绑定当前信道(init 后 / 周期重置后调用) */
static void radar_esnow_repair_peer(void) {
    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);   /* 广播地址 */
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) { ESP_LOGW(TAG, "repair add_peer 失败: %s", esp_err_to_name(err)); return; }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        esp_now_peer_info_t p2 = {};
        memset(p2.peer_addr, 0xFF, 6);
        p2.channel = ap_info.primary;
        p2.ifidx = WIFI_IF_STA;
        p2.encrypt = false;
        esp_now_mod_peer(&p2);
    }
}

static void esnow_task(void *arg) {
    (void)arg;
    char payload[24];
    uint32_t tick = 0;
    int fail_streak = 0;
    /* 2026-10-23 显式广播 MAC:esp_now_send(NULL) 的广播路径在某些版本
       TX 完成回调丢失(PR #17712 开放 bug);显式 FF:FF:FF:FF:FF:FF 单播
       路径不同,且广播 peer 已添加 */
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (;;) {
        /* 2026-10-25 下载/CP OTA 期间静默:SD 卡与 C6 SDIO 共用 P4 SDMMC,
           心跳 RPC 与卡写并发可能跨槽互锁(下载永久卡死根因之一) */
        if (radar_ble_paused()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int n = snprintf(payload, sizeof(payload), ESNOW_PREFIX "%lu",
                         (unsigned long)radar_bound_uid());
        esp_err_t serr = esp_now_send(bcast, (const uint8_t *)payload, (size_t)n);
        if (serr != ESP_OK) {
            /* 2026-10-23 崩溃根因修复:旧版每 100 轮(20s)无条件 deinit/init,
               其 RPC 序列与 CP 侧 WiFi 断连/重连(esphome 3.0.10 固件不稳)
               交织后 SDIO 传输失败 → eh_host 强制 abort 整机复位(无 panic
               打印的"莫名其妙崩")。改为仅 send 连续失败(真 TX stall)才
               重置一次:正常运行时对 WiFi/SDIO 零扰动。 */
            if (++fail_streak >= 10) {
                ESP_LOGW(TAG, "send 连续失败 %d 次,重置 ESP-NOW(deinit/init)", fail_streak);
                fail_streak = 0;
                esp_now_deinit();
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_now_init();
                vTaskDelay(pdMS_TO_TICKS(50));
                radar_esnow_repair_peer();
            }
        } else {
            fail_streak = 0;
        }
        if (++tick % 10 == 0) {   /* 每 5 秒一次诊断(500ms 心跳;不刷屏) */
            ESP_LOGI(TAG, "ESNOW 心跳: send 返回 %s, 已收邻居 %d",
                     esp_err_to_name(serr), s_peer_n);
        }
        vTaskDelay(pdMS_TO_TICKS(s_fast_ms ? s_fast_ms : ESNOW_BCAST_MS));
    }
}

bool radar_esnow_start(void) {
    uint32_t uid = radar_bound_uid();
    if (uid == 0) { ESP_LOGW(TAG, "未绑定,ESP-NOW 停用"); return false; }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_init 失败: %s(C6 固件无 ESP-NOW overlay?)", esp_err_to_name(err));
        return false;
    }
    err = esp_now_register_recv_cb(esnow_recv_cb);
    if (err != ESP_OK) { ESP_LOGW(TAG, "recv_cb 注册失败: %s", esp_err_to_name(err)); return false; }

    /* 添加广播 peer + 显式绑定当前信道(esphome espnow 组件在 WiFi 连接后
       rebind_peers;channel=0"跟随当前信道"在 hosted 组合下可能不生效) */
    radar_esnow_repair_peer();
    uint32_t ver = 0;
    err = esp_now_get_version(&ver);
    ESP_LOGI(TAG, "esp_now_get_version: v%u 返回 %s", (unsigned)ver, esp_err_to_name(err));
    if (err != ESP_OK) { ESP_LOGW(TAG, "RPC 往返异常,ESP-NOW 停用"); return false; }

    xTaskCreate(esnow_task, "radar_esnow", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "ESP-NOW 雷达已启动(广播 %s%lu)", ESNOW_PREFIX, (unsigned long)uid);
    return true;
}

/* ── 对外:最近收到的 ESP-NOW 邻居列表(供 radar_report 并入 ping) ── */
int radar_esnow_peers(uint32_t *uids, int8_t *rssis, int max) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int n = 0;
    for (int i = 0; i < s_peer_n; i++) {
        if (now - s_peer_last_ms[i] > ESNOW_PEER_TTL_MS) {
            /* 超时:与末元素交换后删除(2026-10-23 关机设备快速消失) */
            s_peer_n--;
            if (i != s_peer_n) {
                s_peer_uids[i] = s_peer_uids[s_peer_n];
                s_peer_rssi[i] = s_peer_rssi[s_peer_n];
                s_peer_last_ms[i] = s_peer_last_ms[s_peer_n];
            }
            i--;
            continue;
        }
        if (n < max) {
            uids[n] = s_peer_uids[i];
            rssis[n] = s_peer_rssi[i];
            n++;
        }
    }
    return n;
}

#else /* PEER_DATA 未开:占位桩,ESP-NOW 停用 */

bool radar_esnow_start(void) {
    ESP_LOGW("RadarEsnow", "PEER_DATA 未开启,ESP-NOW 停用");
    return false;
}

#endif
