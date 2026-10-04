#include "audio_forwarder.h"

#include <algorithm>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "wifi_station.h"

#define TAG "AudioForwarder"

namespace {
constexpr char kAnnounceMagic[4] = {'X', 'Z', 'F', 'A'};
constexpr char kAudioMagic[4] = {'X', 'Z', 'A', 'D'};
constexpr char kKeepaliveMagic[4] = {'X', 'Z', 'A', 'L'};
constexpr int kHeaderSize = 20;
// 单包样本上限：20 + 500*2 = 1020B < MTU 1500，避免 IP 分片
// （手机 WiFi 芯片/路由器常丢分片 UDP，电脑收分片正常——拆包根治）
constexpr int kMaxFragmentSamples = 500;
// 组播模式（热点场景实验）：组播地址 + 端口（与单播收口一致）
constexpr char kMulticastIp[] = "239.255.1.1";
}  // namespace

AudioForwarder::AudioForwarder() = default;

AudioForwarder::~AudioForwarder() {
    Stop();
}

void AudioForwarder::EnsureStarted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recv_fd_ >= 0) return;  // 已启动
    }
    // ESP-Hosted 下 lwIP 在 WiFi 连接后才就绪，未联网时调用 socket() 会 assert panic
    if (!WifiStation::GetInstance().IsConnected()) {
        return;  // 等下次 Feed/SetEnabled 重试
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (recv_fd_ >= 0) return;  // 双重检查（并发 EnsureStarted）

    recv_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create recv socket: %d", errno);
        return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(recv_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kAnnouncePort);
    if (bind(recv_fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "Failed to bind announce port %u: %d (another instance?)", kAnnouncePort, errno);
    }

    send_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create send socket: %d", errno);
        close(recv_fd_);
        recv_fd_ = -1;
        return;
    }
    // 非阻塞发送：UDP 丢帧可接受，sendto 卡死会拖垮音频任务与 keepalive
    int flags = fcntl(send_fd_, F_GETFL, 0);
    fcntl(send_fd_, F_SETFL, flags | O_NONBLOCK);
    // 组播 TTL（默认 1 = 仅本网段，正合适）
    uint8_t mttl = 1;
    setsockopt(send_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &mttl, sizeof(mttl));

    quit_ = false;
    xTaskCreate([](void* arg) {
        auto* self = static_cast<AudioForwarder*>(arg);
        self->AnnounceTask();
        vTaskDelete(NULL);
    }, "fwd_announce", 4096, this, 2, nullptr);
    xTaskCreate([](void* arg) {
        auto* self = static_cast<AudioForwarder*>(arg);
        self->SendTask();
        vTaskDelete(NULL);
    }, "fwd_send", 4096, this, 2, nullptr);
    ESP_LOGI(TAG, "Started (announce port %u)", kAnnouncePort);
}

void AudioForwarder::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recv_fd_ < 0) return;
        quit_ = true;
    }
    cv_.notify_all();
    for (int i = 0; i < 200 && running_tasks_ != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (recv_fd_ >= 0) {
        close(recv_fd_);
        recv_fd_ = -1;
    }
    if (send_fd_ >= 0) {
        close(send_fd_);
        send_fd_ = -1;
    }
    ESP_LOGI(TAG, "Stopped");
}

bool AudioForwarder::Feed(const std::vector<int16_t>& pcm, uint32_t timestamp_ms, int sample_rate) {
    EnsureStarted();  // 惰性创建（未联网时无操作；能收到 LLM 回复必然已联网）
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return false;

    Frame frame;
    frame.ts_ms = timestamp_ms;
    if (sample_rate != kForwardSampleRate) {
        if (resample_from_ != sample_rate) {
            resampler_.Configure(sample_rate, kForwardSampleRate);
            resample_from_ = sample_rate;
        }
        int out_samples = resampler_.GetOutputSamples(pcm.size());
        frame.pcm.resize(out_samples);
        resampler_.Process(pcm.data(), pcm.size(), frame.pcm.data());
    } else {
        frame.pcm = pcm;
    }

    if (queue_.size() >= kMaxQueuedFrames) {
        queue_.pop_front();  // 直播音频要新不要等
        packets_dropped_++;
    }
    queue_.push_back(std::move(frame));
    cv_.notify_all();
    return mute_local_;
}

void AudioForwarder::AnnounceTask() {
    running_tasks_++;
    uint8_t buf[64];
    ESP_LOGI(TAG, "Announce task started");
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (quit_) break;
        }
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(recv_fd_, buf, sizeof(buf), 0, (struct sockaddr*)&from, &from_len);
        if (n >= 12 && memcmp(buf, kAnnounceMagic, 4) == 0 && buf[4] == 0x01) {
            uint16_t audio_port = (uint16_t)(buf[6] | (buf[7] << 8));
            if (audio_port == 0) audio_port = kDefaultAudioPort;
            std::lock_guard<std::mutex> lock(mutex_);
            if (!has_target_ || from.sin_addr.s_addr == target_.sin_addr.s_addr) {
                // 锁定首个来源；同源刷新；异源忽略（防多 App 抖动）
                if (!has_target_) {
                    target_.sin_family = AF_INET;
                    target_.sin_addr = from.sin_addr;
                    target_.sin_port = htons(audio_port);
                    has_target_ = true;
                    char ip[16];
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    target_ip_str_ = ip;
                    ESP_LOGI(TAG, "Target locked: %s:%u", ip, audio_port);
                }
                last_announce_ms_ = esp_timer_get_time() / 1000;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recvfrom error: %d", errno);
        }

        // 失联判定（recv 超时后走到这里，约每 500ms 一次）
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_target_ && esp_timer_get_time() / 1000 - last_announce_ms_ > kTargetTimeoutMs) {
            ESP_LOGW(TAG, "Target %s offline (announce timeout)", target_ip_str_.c_str());
            has_target_ = false;  // 保留 IP 字符串供 UI 显示"离线"
        }
    }
    ESP_LOGW(TAG, "Announce task stopped");
    running_tasks_--;
}

void AudioForwarder::SendTask() {
    running_tasks_++;
    ESP_LOGI(TAG, "Send task started");
    while (true) {
        int fd = -1;
        struct sockaddr_in target = {};
        std::vector<std::vector<uint8_t>> pkts;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() { return quit_ || !queue_.empty(); });
            if (quit_) break;
            int64_t now = esp_timer_get_time() / 1000;
            if (!queue_.empty()) {
                Frame frame = std::move(queue_.front());
                queue_.pop_front();
                if (enabled_ && has_target_ && send_fd_ >= 0) {
                    fd = send_fd_;
                    target = multicast_ ? GetMulticastTargetLocked() : target_;
                    BuildAudioPacketsLocked(frame, pkts);
                }
                // 未启用/无目标：帧直接丢弃（不计数，属预期）
            } else if (enabled_ && has_target_ && send_fd_ >= 0 &&
                       now - last_packet_ms_ >= kKeepaliveIntervalMs) {
                fd = send_fd_;
                target = multicast_ ? GetMulticastTargetLocked() : target_;
                std::vector<uint8_t> pkt;
                BuildKeepalivePacketLocked(pkt);
                pkts.push_back(std::move(pkt));
            }
        }
        if (fd >= 0) {
            // 锁外发送：UDP sendto 可能瞬时阻塞，绝不能传染互斥锁（否则 keepalive/Feed 全卡）
            uint32_t sent_count = 0;
            uint32_t drop_count = 0;
            for (auto& pkt : pkts) {
                ssize_t sent = sendto(fd, pkt.data(), pkt.size(), 0, (struct sockaddr*)&target, sizeof(target));
                if (sent < 0) drop_count++;
                else sent_count++;
            }
            if (sent_count || drop_count) {
                std::lock_guard<std::mutex> lock(mutex_);
                packets_sent_ += sent_count;
                packets_dropped_ += drop_count;
                last_packet_ms_ = esp_timer_get_time() / 1000;
            }
        }
    }
    ESP_LOGW(TAG, "Send task stopped");
    running_tasks_--;
}

void AudioForwarder::BuildAudioPacketsLocked(Frame& frame, std::vector<std::vector<uint8_t>>& out) {
    uint16_t samples = (uint16_t)frame.pcm.size();
    uint32_t s = seq_++;  // 帧序号：同一帧的所有 fragment 共用
    uint8_t frag_count = (uint8_t)((samples + kMaxFragmentSamples - 1) / kMaxFragmentSamples);
    if (frag_count == 0) frag_count = 1;
    uint32_t ts = frame.ts_ms;
    uint16_t sr = (uint16_t)kForwardSampleRate;
    uint16_t ch = (uint16_t)kForwardChannels;

    out.clear();
    out.resize(frag_count);
    for (uint8_t fi = 0; fi < frag_count; fi++) {
        size_t off = (size_t)fi * kMaxFragmentSamples;
        size_t n = std::min<size_t>((size_t)kMaxFragmentSamples, (size_t)samples - off);
        auto& pkt = out[fi];
        pkt.resize(kHeaderSize + n * 2);
        memcpy(pkt.data(), kAudioMagic, 4);
        pkt[4] = (uint8_t)(s & 0xFF);
        pkt[5] = (uint8_t)((s >> 8) & 0xFF);
        pkt[6] = (uint8_t)((s >> 16) & 0xFF);
        pkt[7] = (uint8_t)((s >> 24) & 0xFF);
        pkt[8] = (uint8_t)(ts & 0xFF);
        pkt[9] = (uint8_t)((ts >> 8) & 0xFF);
        pkt[10] = (uint8_t)((ts >> 16) & 0xFF);
        pkt[11] = (uint8_t)((ts >> 24) & 0xFF);
        pkt[12] = (uint8_t)(sr & 0xFF);
        pkt[13] = (uint8_t)((sr >> 8) & 0xFF);
        pkt[14] = (uint8_t)(ch & 0xFF);
        pkt[15] = (uint8_t)((ch >> 8) & 0xFF);
        pkt[16] = (uint8_t)(samples & 0xFF);
        pkt[17] = (uint8_t)((samples >> 8) & 0xFF);  // 整帧样本数
        pkt[18] = fi;         // fragment 序号
        pkt[19] = frag_count; // fragment 总数（1 = 未拆包）
        memcpy(pkt.data() + kHeaderSize, frame.pcm.data() + off, n * 2);
    }
}

void AudioForwarder::BuildKeepalivePacketLocked(std::vector<uint8_t>& out) {
    out.resize(8);
    memcpy(out.data(), kKeepaliveMagic, 4);
    uint32_t s = seq_++;
    out[4] = (uint8_t)(s & 0xFF);
    out[5] = (uint8_t)((s >> 8) & 0xFF);
    out[6] = (uint8_t)((s >> 16) & 0xFF);
    out[7] = (uint8_t)((s >> 24) & 0xFF);
}

void AudioForwarder::SetEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
    }
    cv_.notify_all();  // 立即发一次 keepalive
    if (enabled) EnsureStarted();  // 开启转发时若已联网立即建立 socket
}

bool AudioForwarder::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void AudioForwarder::SetMuteLocal(bool mute) {
    std::lock_guard<std::mutex> lock(mutex_);
    mute_local_ = mute;
}

bool AudioForwarder::mute_local() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mute_local_;
}

void AudioForwarder::SetMulticast(bool multicast) {
    std::lock_guard<std::mutex> lock(mutex_);
    multicast_ = multicast;
}

bool AudioForwarder::multicast() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return multicast_;
}

struct sockaddr_in AudioForwarder::GetMulticastTargetLocked() const {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDefaultAudioPort);
    inet_pton(AF_INET, kMulticastIp, &addr.sin_addr);
    return addr;
}

void AudioForwarder::ResetTarget() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_target_ = false;
    target_ip_str_.clear();
    memset(&target_, 0, sizeof(target_));
    last_announce_ms_ = 0;
    ESP_LOGI(TAG, "Target reset, waiting for next announce");
}

bool AudioForwarder::has_target() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_target_) return false;
    return esp_timer_get_time() / 1000 - last_announce_ms_ <= kTargetTimeoutMs;
}

std::string AudioForwarder::target_ip() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_ip_str_;
}

uint32_t AudioForwarder::packets_sent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_sent_;
}

uint32_t AudioForwarder::packets_dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_dropped_;
}
