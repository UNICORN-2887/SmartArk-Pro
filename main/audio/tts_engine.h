#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initialize ESP-TTS (call once at startup)
bool tts_init(void);

// Speak text: converts Chinese to PCM, pushes to audio send queue
// Runs synchronously (call from a task to avoid blocking)
void tts_speak(const char *text);

// Clean up TTS resources
void tts_deinit(void);

#ifdef __cplusplus
}
#endif
