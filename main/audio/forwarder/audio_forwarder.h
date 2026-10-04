#ifndef AUDIO_FORWARDER_H
#define AUDIO_FORWARDER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>

#include "opus_resampler.h"

// LLM 回复 PCM 旁路转发器：AudioOutputTask 每帧 Feed，UDP 发给局域网手机 App。
// 手机 App 周期广播 XZFA announce，P4 自动锁定目标（无需手动输 IP）。
// 本地静音通过 Feed 返回 true 由调用方短路扬声器实现（不污染音量 NVS）。
class AudioForwarder {
public:
    AudioForwarder();
    ~AudioForwarder();

    // 惰性创建 socket/任务：WiFi 未连时跳过（ESP-Hosted 下 lwIP 就绪前调用 socket() 会 assert），
    // Feed 与 SetEnabled(true) 时重试，LLM 回复必然发生在联网之后
    void EnsureStarted();
    void Stop();

    // AudioOutputTask 每帧调用；返回 true = 应抑制本地扬声器输出
    bool Feed(const std::vector<int16_t>& pcm, uint32_t timestamp_ms, int sample_rate);

    // 以下为设置页 UI（LVGL 任务）调用，线程安全
    void SetEnabled(bool enabled);
    bool enabled() const;
    void SetMuteLocal(bool mute);
    bool mute_local() const;
    void SetMulticast(bool multicast);  // 组播模式：音频发 239.255.1.1:50001（热点实验用）
    bool multicast() const;
    void ResetTarget();
    bool has_target() const;   // 收到 announce 且未超时
    std::string target_ip() const;
    uint32_t packets_sent() const;
    uint32_t packets_dropped() const;

private:
    struct Frame {
        std::vector<int16_t> pcm;  // 已重采样 16k mono
        uint32_t ts_ms;
    };

    void AnnounceTask();
    void SendTask();
    void BuildAudioPacketsLocked(Frame& frame, std::vector<std::vector<uint8_t>>& out);
    void BuildKeepalivePacketLocked(std::vector<uint8_t>& out);
    struct sockaddr_in GetMulticastTargetLocked() const;

    static constexpr uint16_t kAnnouncePort = 50000;
    static constexpr uint16_t kDefaultAudioPort = 50001;
    static constexpr uint32_t kTargetTimeoutMs = 8000;      // 4 次 announce 未到 = 失联
    static constexpr uint32_t kKeepaliveIntervalMs = 1000;  // XZAL 心跳
    static constexpr size_t kMaxQueuedFrames = 100;         // 6s @60ms
    static constexpr int kForwardSampleRate = 16000;
    static constexpr int kForwardChannels = 1;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Frame> queue_;
    std::atomic<int> running_tasks_{0};

    bool enabled_ = false;
    bool mute_local_ = false;
    bool multicast_ = false;
    bool quit_ = false;
    uint32_t seq_ = 0;
    uint32_t packets_sent_ = 0;
    uint32_t packets_dropped_ = 0;

    bool has_target_ = false;
    struct sockaddr_in target_{};
    std::string target_ip_str_;
    int64_t last_announce_ms_ = 0;
    int64_t last_packet_ms_ = 0;

    int recv_fd_ = -1;  // bind 0.0.0.0:50000 收 announce
    int send_fd_ = -1;  // 单播音频帧

    OpusResampler resampler_;
    int resample_from_ = 0;
};

#endif
