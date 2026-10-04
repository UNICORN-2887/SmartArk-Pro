#ifndef CUSTOM_WAKE_WORD_H
#define CUSTOM_WAKE_WORD_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_afe_sr_models.h>
#include <esp_afe_sr_iface.h>
#include <esp_nsn_models.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>

#include <list>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>

#include "audio_codec.h"
#include "wake_word.h"

class CustomWakeWord : public WakeWord {
public:
    // 一条唤醒词命令:拼音(pinyin)+ 唤醒后回调的显示名(角色名)
    struct WakeWordCommand {
        std::string pinyin;
        std::string display;
    };

    CustomWakeWord();
    ~CustomWakeWord();

    bool Initialize(AudioCodec* codec);
    void Feed(const std::vector<int16_t>& data);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    void Start();
    void Stop();
    size_t GetFeedSize();
    void EncodeWakeWordData();
    bool GetWakeWordOpus(std::vector<uint8_t>& opus);
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }

    // 动态替换命令词表(服务器下发的用户自定义唤醒词)。
    // 应用时机:未初始化→Initialize 时;检测中→下一次 Start() 前(避免并发改 multinet)
    void SetWakeWordCommands(const std::vector<WakeWordCommand>& commands);

private:
    esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    srmodel_list_t *models = nullptr;
    
    // multinet 相关成员变量
    esp_mn_iface_t* multinet_ = nullptr;
    model_iface_data_t* multinet_model_data_ = nullptr;
    char* mn_name_ = nullptr;
 
    char* wakenet_model_ = NULL;
    std::vector<std::string> wake_words_;
    EventGroupHandle_t event_group_;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;
    int64_t last_detect_time_ = 0;  // 冷却时间戳（微秒）

    std::vector<std::string> command_names_;                 // command_id(1-based)→显示名
    std::vector<WakeWordCommand> pending_commands_;          // 待应用的动态命令词表
    bool has_pending_commands_ = false;
    void ApplyCommands(const std::vector<WakeWordCommand>& commands);

    TaskHandle_t wake_word_encode_task_ = nullptr;
    std::list<std::vector<int16_t>> wake_word_pcm_;
    std::list<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    void StoreWakeWordData(const int16_t* data, size_t size);
    void AudioDetectionTask();
};

#endif
