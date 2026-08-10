#include "tts_engine.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_tts.h>
#include <esp_tts_voice_template.h>
#include <esp_partition.h>
#include "application.h"

#define TAG "TTS"

static esp_tts_handle_t s_tts = nullptr;
static bool s_tts_busy = false;
static const void *s_voice_mmap = nullptr;
static esp_partition_mmap_handle_t s_mmap_handle = 0;

bool tts_init(void) {
    ESP_LOGI(TAG, "Initializing ESP-TTS from flash partition...");

    // Load voice data from "voice" partition (avoids P4 PSRAM XIP crash)
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "voice");
    if (!part) {
        ESP_LOGE(TAG, "Voice partition not found! Flash voice_data_xiaole.dat to 0xD00000");
        return false;
    }
    ESP_LOGI(TAG, "Voice partition: offset=0x%lx size=0x%lx", (long)part->address, (long)part->size);

    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                        ESP_PARTITION_MMAP_DATA, &s_voice_mmap, &s_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap voice partition: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Voice data mmap'd at %p", s_voice_mmap);

    // Init voice set from partition data
    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_template, (void*)s_voice_mmap);
    if (!voice) {
        ESP_LOGE(TAG, "Failed to init voice set");
        return false;
    }

    s_tts = esp_tts_create(voice);
    if (!s_tts) {
        ESP_LOGE(TAG, "Failed to create TTS handle");
        return false;
    }

    ESP_LOGI(TAG, "ESP-TTS ready (partition-based)");
    return true;
}

void tts_speak(const char *text) {
    if (!s_tts) {
        ESP_LOGW(TAG, "TTS not initialized — cannot speak: %s", text);
        return;
    }
    if (s_tts_busy) {
        ESP_LOGW(TAG, "TTS busy, skipping: %s", text);
        return;
    }
    s_tts_busy = true;

    ESP_LOGI(TAG, "Speaking: %s", text);

    int ret = esp_tts_parse_chinese(s_tts, text);
    if (!ret) {
        ESP_LOGE(TAG, "Parse failed");
        esp_tts_stream_reset(s_tts);
        s_tts_busy = false;
        return;
    }

    auto &audio_service = Application::GetInstance().GetAudioService();
    audio_service.ClearSendQueue();
    vTaskDelay(pdMS_TO_TICKS(200));  // let encoder drain before pushing TTS

    int total_samples = 0;
    int len = 0;
    std::vector<int16_t> buffer;
    buffer.reserve(960);

    while (true) {
        short *pcm = esp_tts_stream_play(s_tts, &len, 3);
        if (len == 0 || !pcm) break;

        buffer.insert(buffer.end(), pcm, pcm + len);
        total_samples += len;

        // Flush 960-sample frames (Opus frame size)
        while (buffer.size() >= 960) {
            std::vector<int16_t> frame(buffer.begin(), buffer.begin() + 960);
            audio_service.PushAudioForSend(std::move(frame));
            buffer.erase(buffer.begin(), buffer.begin() + 960);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    // Flush remaining samples
    if (!buffer.empty()) {
        buffer.resize(960, 0);  // pad with silence
        audio_service.PushAudioForSend(std::move(buffer));
    }

    esp_tts_stream_reset(s_tts);
    s_tts_busy = false;
    ESP_LOGI(TAG, "Done: %d samples (%d ms)",
             total_samples, total_samples * 1000 / 16000);
}

void tts_deinit(void) {
    if (s_tts) {
        esp_tts_destroy(s_tts);
        s_tts = nullptr;
    }
    if (s_voice_mmap) {
        esp_partition_munmap(s_mmap_handle);
        s_voice_mmap = nullptr;
    }
    ESP_LOGI(TAG, "TTS deinitialized");
}
