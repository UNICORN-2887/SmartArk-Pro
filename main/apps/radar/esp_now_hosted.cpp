/*
 * esp_now_hosted — host-side shim implementing <esp_now.h> over esp-hosted
 * CustomRpc, so a radio-less host (ESP32-P4) can use ESP-NOW whose radio lives
 * on an esp-hosted co-processor (C6 running esphome firmware with the
 * ESP-NOW overlay).
 *
 * Ported from esphome/components/esp32_hosted/esp_now_hosted.cpp (PR #17712),
 * adapted to native IDF (ESP_LOGx instead of ESPHome logging; no other
 * behavioural changes). The matching coprocessor side ships in esphome's
 * esp-hosted-firmware (slave-overlay/esp_now_hosted_rpc.h).
 *
 * esp_wifi_remote INJECTS the full esp_now.h header (declarations) but ships
 * NO implementation; this translation unit provides those definitions.
 * Requires CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA=y.
 */

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA)

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <esp_now.h>         // injected declarations we are now DEFINING
#include <esp_wifi_types.h>  // wifi_pkt_rx_ctrl_t, wifi_tx_info_t

extern "C" {
#include "esp_hosted_misc.h"  // esp_hosted_{send_custom_data,register_custom_callback}
}

#include "esp_now_hosted_rpc.h"

static const char *TAG = "esp_now_hosted";

namespace {

// One outstanding request at a time, serialized by g_req_mutex; responses and
// async RECV/SEND events arrive on the esp-hosted RPC RX thread.
SemaphoreHandle_t g_req_mutex = nullptr;
SemaphoreHandle_t g_resp_sem = nullptr;  // given when the matching RESP lands
bool g_setup_done = false;
uint8_t g_seq = 0;
volatile uint8_t g_expect_seq = 0;
volatile int32_t g_resp_status = 0;
uint8_t g_resp_ret[16];
volatile uint16_t g_resp_ret_len = 0;

volatile esp_now_recv_cb_t g_recv_cb = nullptr;
volatile esp_now_send_cb_t g_send_cb = nullptr;

// Local mirror of the co-processor's peer table (avoids RPC round-trip per
// is_peer_exist query). ESP_NOW_MAX_TOTAL_PEER_NUM is 20.
constexpr size_t ESP_NOW_HOSTED_MAX_PEERS = 20;
uint8_t g_peer_cache[ESP_NOW_HOSTED_MAX_PEERS][6];
size_t g_peer_count = 0;
portMUX_TYPE g_peer_lock = portMUX_INITIALIZER_UNLOCKED;

int peer_cache_find_locked(const uint8_t *mac) {
  for (size_t i = 0; i < g_peer_count; i++) {
    if (memcmp(g_peer_cache[i], mac, 6) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

bool peer_cache_contains(const uint8_t *mac) {
  portENTER_CRITICAL(&g_peer_lock);
  const bool found = peer_cache_find_locked(mac) >= 0;
  portEXIT_CRITICAL(&g_peer_lock);
  return found;
}

void peer_cache_add(const uint8_t *mac) {
  portENTER_CRITICAL(&g_peer_lock);
  if (peer_cache_find_locked(mac) < 0 && g_peer_count < ESP_NOW_HOSTED_MAX_PEERS)
    memcpy(g_peer_cache[g_peer_count++], mac, 6);
  portEXIT_CRITICAL(&g_peer_lock);
}

void peer_cache_remove(const uint8_t *mac) {
  portENTER_CRITICAL(&g_peer_lock);
  const int idx = peer_cache_find_locked(mac);
  if (idx >= 0) {
    g_peer_count--;
    if (static_cast<size_t>(idx) != g_peer_count)
      memcpy(g_peer_cache[idx], g_peer_cache[g_peer_count], 6);
  }
  portEXIT_CRITICAL(&g_peer_lock);
}

void peer_cache_clear() {
  portENTER_CRITICAL(&g_peer_lock);
  g_peer_count = 0;
  portEXIT_CRITICAL(&g_peer_lock);
}

// ── CustomRpc event handlers (run on the esp-hosted RPC RX thread) ──────────

void on_resp(uint32_t msg_id, const uint8_t *data, size_t len, void * /*ctx*/) {
  // 诊断日志 2026-10-23 降为 DEBUG:500ms 心跳 + 同步 RPC 下每轮多条,
  // INFO 级刷屏本身是压力源(串口+日志任务),且淹没真正的错误
  ESP_LOGD(TAG, "peer_data RESP: msg_id=0x%08lx len=%u", (unsigned long)msg_id, (unsigned)len);
  if (len < sizeof(esp_now_hosted_resp_t)) {
    ESP_LOGW(TAG, "RESP too short: %u bytes", static_cast<unsigned>(len));
    return;
  }  const auto *r = reinterpret_cast<const esp_now_hosted_resp_t *>(data);
  if (r->seq != g_expect_seq) {  // late response from a timed-out request
    return;
  }
  g_resp_status = r->status;
  uint16_t rl = r->ret_len;
  if (rl > sizeof(g_resp_ret)) {
    ESP_LOGW(TAG, "RESP ret_len %u exceeds buffer, clamping", rl);
    rl = sizeof(g_resp_ret);
  }
  if (len >= sizeof(esp_now_hosted_resp_t) + rl) {
    memcpy(g_resp_ret, r->ret, rl);
  } else {
    ESP_LOGW(TAG, "RESP truncated");
    rl = 0;
    g_resp_status = ESP_ERR_INVALID_RESPONSE;
  }
  g_resp_ret_len = rl;
  xSemaphoreGive(g_resp_sem);
}

void on_recv(uint32_t msg_id, const uint8_t *data, size_t len, void * /*ctx*/) {
  // 诊断日志 2026-10-23 降为 DEBUG(见 on_resp 注释)
  ESP_LOGD(TAG, "peer_data 事件: msg_id=0x%08lx len=%u", (unsigned long)msg_id, (unsigned)len);
  const esp_now_recv_cb_t cb = g_recv_cb;
  if (cb == nullptr)
    return;
  if (len < sizeof(esp_now_hosted_recv_evt_t)) {
    ESP_LOGW(TAG, "RECV too short: %u bytes", static_cast<unsigned>(len));
    return;
  }
  const auto *e = reinterpret_cast<const esp_now_hosted_recv_evt_t *>(data);
  if (len < sizeof(esp_now_hosted_recv_evt_t) + e->data_len) {
    ESP_LOGW(TAG, "RECV data_len %u exceeds frame", e->data_len);
    return;
  }

  wifi_pkt_rx_ctrl_t rx_ctrl;
  memset(&rx_ctrl, 0, sizeof(rx_ctrl));
  rx_ctrl.rssi = e->rssi;
  rx_ctrl.channel = e->channel;
  rx_ctrl.timestamp = static_cast<uint32_t>(esp_timer_get_time());

  esp_now_recv_info_t info;
  info.src_addr = const_cast<uint8_t *>(e->src_addr);
  info.des_addr = const_cast<uint8_t *>(e->des_addr);
  info.rx_ctrl = &rx_ctrl;
  cb(&info, e->data, static_cast<int>(e->data_len));
}

void on_send(uint32_t msg_id, const uint8_t *data, size_t len, void * /*ctx*/) {
  // 诊断日志 2026-10-23 降为 DEBUG(见 on_resp 注释)
  ESP_LOGD(TAG, "peer_data 事件: msg_id=0x%08lx len=%u", (unsigned long)msg_id, (unsigned)len);
  const esp_now_send_cb_t cb = g_send_cb;
  if (cb == nullptr)
    return;
  if (len < sizeof(esp_now_hosted_send_evt_t)) {
    ESP_LOGW(TAG, "SEND evt too short: %u bytes", static_cast<unsigned>(len));
    return;
  }
  const auto *e = reinterpret_cast<const esp_now_hosted_send_evt_t *>(data);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  esp_now_send_info_t si;
  memset(&si, 0, sizeof(si));
  si.des_addr = const_cast<uint8_t *>(e->des_addr);
  cb(&si, static_cast<esp_now_send_status_t>(e->status));
#else
  cb(e->des_addr, static_cast<esp_now_send_status_t>(e->status));
#endif
}

esp_err_t ensure_setup() {
  if (g_setup_done)
    return ESP_OK;
  if (g_req_mutex == nullptr)
    g_req_mutex = xSemaphoreCreateMutex();
  if (g_resp_sem == nullptr)
    g_resp_sem = xSemaphoreCreateBinary();
  if (g_req_mutex == nullptr || g_resp_sem == nullptr)
    return ESP_ERR_NO_MEM;
  esp_err_t err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_RESP, on_resp, nullptr);
  if (err != ESP_OK)
    return err;
  err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_RECV, on_recv, nullptr);
  if (err != ESP_OK)
    return err;
  err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_SEND, on_send, nullptr);
  if (err != ESP_OK)
    return err;
  g_setup_done = true;
  return ESP_OK;
}

esp_err_t request(uint8_t opcode, const void *payload, uint16_t plen, void *ret, uint16_t ret_cap, uint16_t *ret_len,
                  bool wait = true, const void *tail = nullptr, uint16_t tail_len = 0) {
  esp_err_t err = ensure_setup();
  if (err != ESP_OK)
    return err;
  if (plen > ESP_NOW_HOSTED_MAX_PAYLOAD || tail_len > ESP_NOW_HOSTED_MAX_PAYLOAD - plen)
    return ESP_ERR_INVALID_SIZE;
  const uint16_t total_len = static_cast<uint16_t>(plen + tail_len);

  if (xSemaphoreTake(g_req_mutex, portMAX_DELAY) != pdTRUE)
    return ESP_FAIL;

  static uint8_t buf[sizeof(esp_now_hosted_req_t) + ESP_NOW_HOSTED_MAX_PAYLOAD];  // guarded by g_req_mutex
  auto *req = reinterpret_cast<esp_now_hosted_req_t *>(buf);
  req->opcode = opcode;
  req->seq = ++g_seq;
  req->payload_len = total_len;
  if (plen != 0)
    memcpy(req->payload, payload, plen);
  if (tail_len != 0)
    memcpy(req->payload + plen, tail, tail_len);
  g_expect_seq = req->seq;

  xSemaphoreTake(g_resp_sem, 0);  // drain any stale signal before sending
  err = esp_hosted_send_custom_data(ESP_NOW_HOSTED_MSG_REQ, buf, sizeof(esp_now_hosted_req_t) + total_len);
  if (err != ESP_OK) {
    xSemaphoreGive(g_req_mutex);
    return err;
  }
  if (!wait) {
    xSemaphoreGive(g_req_mutex);
    return ESP_OK;
  }
  if (xSemaphoreTake(g_resp_sem, pdMS_TO_TICKS(ESP_NOW_HOSTED_TIMEOUT_MS)) != pdTRUE) {
    ESP_LOGW(TAG, "opcode %u timed out", opcode);
    xSemaphoreGive(g_req_mutex);
    return ESP_ERR_TIMEOUT;
  }

  const int32_t status = g_resp_status;
  if (ret != nullptr && ret_cap != 0) {
    uint16_t n = g_resp_ret_len < ret_cap ? g_resp_ret_len : ret_cap;
    memcpy(ret, const_cast<const uint8_t *>(g_resp_ret), n);
    if (ret_len != nullptr)
      *ret_len = n;
  }
  xSemaphoreGive(g_req_mutex);
  return static_cast<esp_err_t>(status);
}

}  // namespace

// ── The <esp_now.h> surface, defined for the radio-less host ────────────────
extern "C" {

esp_err_t esp_now_init(void) { return request(ESP_NOW_HOSTED_OP_INIT, nullptr, 0, nullptr, 0, nullptr); }

esp_err_t esp_now_deinit(void) {
  g_recv_cb = nullptr;
  g_send_cb = nullptr;
  peer_cache_clear();
  return request(ESP_NOW_HOSTED_OP_DEINIT, nullptr, 0, nullptr, 0, nullptr);
}

esp_err_t esp_now_get_version(uint32_t *version) {
  uint32_t v = 0;
  uint16_t rl = 0;
  esp_err_t err = request(ESP_NOW_HOSTED_OP_GET_VERSION, nullptr, 0, &v, sizeof(v), &rl);
  if (version != nullptr)
    *version = v;
  return err;
}

esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) {
  esp_err_t err = ensure_setup();
  if (err != ESP_OK)
    return err;
  g_recv_cb = cb;
  return ESP_OK;
}
esp_err_t esp_now_unregister_recv_cb(void) {
  g_recv_cb = nullptr;
  return ESP_OK;
}
esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb) {
  esp_err_t err = ensure_setup();
  if (err != ESP_OK)
    return err;
  g_send_cb = cb;
  return ESP_OK;
}
esp_err_t esp_now_unregister_send_cb(void) {
  g_send_cb = nullptr;
  return ESP_OK;
}

static esp_err_t add_or_mod_peer(uint8_t opcode, const esp_now_peer_info_t *peer, bool wait) {
  if (peer == nullptr)
    return ESP_ERR_ESPNOW_ARG;
  esp_now_hosted_peer_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, peer->peer_addr, 6);
  memcpy(p.lmk, peer->lmk, 16);
  p.channel = peer->channel;
  p.ifidx = static_cast<uint8_t>(peer->ifidx);
  p.encrypt = peer->encrypt ? 1 : 0;
  return request(opcode, &p, sizeof(p), nullptr, 0, nullptr, wait);
}
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer) {
  // 2026-10-23 雷达用途:初始化期加广播 peer,改同步等待——fire-and-forget
  // 会把 CP 侧 ADD_PEER 失败吞掉(peer 表空 → 广播空发,两台互收不到)
  esp_err_t err = add_or_mod_peer(ESP_NOW_HOSTED_OP_ADD_PEER, peer, /*wait=*/true);
  if (err == ESP_OK)
    peer_cache_add(peer->peer_addr);
  return err;
}
esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *peer) {
  return add_or_mod_peer(ESP_NOW_HOSTED_OP_MOD_PEER, peer, /*wait=*/true);
}

esp_err_t esp_now_del_peer(const uint8_t *peer_addr) {
  if (peer_addr == nullptr)
    return ESP_ERR_ESPNOW_ARG;
  esp_err_t err = request(ESP_NOW_HOSTED_OP_DEL_PEER, peer_addr, 6, nullptr, 0, nullptr, /*wait=*/false);
  if (err == ESP_OK)
    peer_cache_remove(peer_addr);
  return err;
}

bool esp_now_is_peer_exist(const uint8_t *peer_addr) {
  if (peer_addr == nullptr)
    return false;
  return peer_cache_contains(peer_addr);
}

esp_err_t esp_now_send(const uint8_t *peer_addr, const uint8_t *data, size_t len) {
  if (len > ESP_NOW_HOSTED_MAX_FRAME)
    return ESP_ERR_ESPNOW_ARG;
  if (data == nullptr && len != 0)
    return ESP_ERR_ESPNOW_ARG;
  uint8_t hdr[sizeof(esp_now_hosted_send_req_t)] = {};
  auto *s = reinterpret_cast<esp_now_hosted_send_req_t *>(hdr);
  s->has_addr = peer_addr != nullptr ? 1 : 0;
  if (peer_addr != nullptr) {
    memcpy(s->peer_addr, peer_addr, 6);
  } else {
    memset(s->peer_addr, 0, 6);
  }
  s->data_len = static_cast<uint16_t>(len);
  return request(ESP_NOW_HOSTED_OP_SEND, hdr, sizeof(hdr), nullptr, 0, nullptr, /*wait=*/false, data,
                 static_cast<uint16_t>(len));
}

esp_err_t esp_now_set_pmk(const uint8_t *pmk) {
  if (pmk == nullptr)
    return ESP_ERR_ESPNOW_ARG;
  return request(ESP_NOW_HOSTED_OP_SET_PMK, pmk, 16, nullptr, 0, nullptr);
}

esp_err_t esp_now_get_peer(const uint8_t * /*peer_addr*/, esp_now_peer_info_t * /*peer*/) {
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_now_fetch_peer(bool /*from_head*/, esp_now_peer_info_t * /*peer*/) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_now_get_peer_num(esp_now_peer_num_t * /*num*/) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_now_set_wake_window(uint16_t /*window*/) {
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_now_set_peer_rate_config(const uint8_t * /*peer_addr*/, esp_now_rate_config_t * /*cfg*/) {
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_wifi_config_espnow_rate(wifi_interface_t /*ifx*/, wifi_phy_rate_t /*rate*/) {
  return ESP_ERR_NOT_SUPPORTED;
}

}  // extern "C"

#endif  // CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA
