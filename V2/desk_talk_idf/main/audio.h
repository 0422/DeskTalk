// 2026-09-19: Define the native ESP-IDF audio boundary shared by future ESP-SR, ASR, TTS, and playback tasks.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace desk_talk::audio {

esp_err_t initialize();
bool microphone_ready();
bool speaker_ready();

esp_err_t read_pcm16(void *data, size_t byte_count, size_t *bytes_read,
                     uint32_t timeout_ms);
esp_err_t record(int16_t *samples, size_t sample_count);
void enhance_voice(int16_t *samples, size_t sample_count);

bool play(const int16_t *samples, size_t sample_count,
          float volume_ratio = 1.0F);
bool wait_for_playback_complete();
void stop_playback();
bool test_speaker();

size_t calculate_mean_amplitude(const int16_t *samples,
                                size_t sample_count);

}  // namespace desk_talk::audio
