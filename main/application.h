#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <vector>
#include <memory>
#include <map>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    DeviceState GetDeviceState() const { return device_state_; }
    int bound_user_uid() const { return bound_user_uid_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "", const char* sd_key = nullptr);
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    void SwitchAgent(const std::string& agent_id);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    Protocol* GetProtocol() { return protocol_.get(); }   /* 2026-10-02 键盘文本直发 agent */
    void SendUserText(const std::string& text);   /* 2026-10-02 键盘文本直发(复用唤醒对话链路) */
    void CloseAudioChannel();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    int stop_switch_pending_ = 0;   // 正常播完（is_aborted=false）延迟切 listening 的剩余秒数
    int64_t last_wake_word_detect_time_ = 0;  // 唤醒词检测冷却时间戳（ms）
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    // 唤醒词-用户隔离:0=未知(词表拉取失败,用硬编码保底) 1=未绑定(禁止对话) 2=已绑定
    int wake_words_state_ = 0;
    std::map<std::string, std::string> wake_word_agents_;   // 唤醒词显示名 → agent_id(服务器下发)
    std::map<std::string, std::string> wake_word_paths_;    // 唤醒词显示名 → 画面资源路径(相对 /sdcard/,服务器下发)
    std::map<std::string, std::string> wake_word_names_cn_; // 唤醒词显示名 → 干员中文名(2026-09-28 聊天框表头用)
    std::string bound_username_;                            // 绑定用户账号名(2026-09-28 聊天框输入表头用;NVS 持久化)
    int bound_user_uid_ = 0;                                // 当前绑定用户 uid(2026-09-10:解析 wakewords 响应 uid 字段;换绑随词表刷新)
    bool pending_clear_operator_ = false;                   // 换绑待清空 operator(主循环安全执行,成功后才写 NVS)
    std::string wake_words_sig_;                            // 词表签名(变化才热更新,避免 5 分钟轮询反复 Stop/Start)
    TaskHandle_t wakeword_poll_task_ = nullptr;             // 轮询任务:未绑定 10s / 已绑定 5min(网页改动自动生效)

    void StartWakeWordBindPoll();
    void WakeWordBindPollTask();
    void ClearOperatorAfterRebind(int new_uid);   // 换绑清空 operator(主循环上下文,2026-09-10)

    void MainEventLoop();
    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void FetchWakeWords(bool quiet = false);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode);
};

#endif // _APPLICATION_H_
