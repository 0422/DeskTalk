// 2026-09-19: Port the validated V1 INMP441 and MAX98357 paths from Arduino helpers to native ESP-IDF drivers.
#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
// 2026-09-20: Time the first successful I2S write separately from the attempted submission.
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hardware_config.h"
#include "latency_trace.h"

namespace desk_talk::audio {
namespace {

constexpr char kLogTag[] = "audio";
constexpr size_t kNativeReadChunkSamples = 128;
constexpr size_t kPlaybackChunkSamples = 128;
constexpr size_t kStereoSlots = 2;
constexpr size_t kStereoFrameBytes = sizeof(int16_t) * kStereoSlots;

i2s_chan_handle_t microphone_channel = nullptr;
i2s_chan_handle_t speaker_channel = nullptr;
bool is_microphone_ready = false;
bool is_speaker_ready = false;

// 2026-09-19: Configure INMP441 as 32-bit mono-left Philips I2S on controller 0, matching V1's verified format.
esp_err_t initialize_microphone() {
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel_config.dma_desc_num = hardware::kAudioDmaDescriptorCount;
  channel_config.dma_frame_num = hardware::kAudioDmaFrameCount;

  esp_err_t result =
      i2s_new_channel(&channel_config, nullptr, &microphone_channel);
  if (result != ESP_OK) {
    return result;
  }

  i2s_std_config_t standard_config = {};
  standard_config.clk_cfg =
      I2S_STD_CLK_DEFAULT_CONFIG(hardware::kAudioSampleRate);
  standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
  standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  standard_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  standard_config.gpio_cfg.bclk = hardware::kMicrophoneBitClock;
  standard_config.gpio_cfg.ws = hardware::kMicrophoneWordSelect;
  standard_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  standard_config.gpio_cfg.din = hardware::kMicrophoneData;
  standard_config.gpio_cfg.invert_flags.mclk_inv = false;
  standard_config.gpio_cfg.invert_flags.bclk_inv = false;
  standard_config.gpio_cfg.invert_flags.ws_inv = false;

  result = i2s_channel_init_std_mode(microphone_channel, &standard_config);
  if (result == ESP_OK) {
    result = i2s_channel_enable(microphone_channel);
  }
  if (result != ESP_OK) {
    i2s_del_channel(microphone_channel);
    microphone_channel = nullptr;
  }
  return result;
}

// 2026-09-19: Configure MAX98357 as 16-bit stereo Philips I2S on controller 1 and duplicate mono PCM into both slots.
esp_err_t initialize_speaker() {
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  channel_config.dma_desc_num = hardware::kAudioDmaDescriptorCount;
  channel_config.dma_frame_num = hardware::kAudioDmaFrameCount;
  channel_config.auto_clear = true;

  esp_err_t result =
      i2s_new_channel(&channel_config, &speaker_channel, nullptr);
  if (result != ESP_OK) {
    return result;
  }

  i2s_std_config_t standard_config = {};
  standard_config.clk_cfg =
      I2S_STD_CLK_DEFAULT_CONFIG(hardware::kAudioSampleRate);
  standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  standard_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  standard_config.gpio_cfg.bclk = hardware::kSpeakerBitClock;
  standard_config.gpio_cfg.ws = hardware::kSpeakerWordSelect;
  standard_config.gpio_cfg.dout = hardware::kSpeakerData;
  standard_config.gpio_cfg.din = I2S_GPIO_UNUSED;
  standard_config.gpio_cfg.invert_flags.mclk_inv = false;
  standard_config.gpio_cfg.invert_flags.bclk_inv = false;
  standard_config.gpio_cfg.invert_flags.ws_inv = false;

  result = i2s_channel_init_std_mode(speaker_channel, &standard_config);
  if (result == ESP_OK) {
    result = i2s_channel_enable(speaker_channel);
  }
  if (result != ESP_OK) {
    i2s_del_channel(speaker_channel);
    speaker_channel = nullptr;
  }
  return result;
}

int16_t scale_sample(int16_t sample, float volume_ratio) {
  const float scaled = static_cast<float>(sample) * volume_ratio;
  const float bounded = std::clamp(
      scaled, static_cast<float>(std::numeric_limits<int16_t>::min()),
      static_cast<float>(std::numeric_limits<int16_t>::max()));
  return static_cast<int16_t>(bounded);
}

}  // namespace

esp_err_t initialize() {
  if (!hardware::kEnableAudio) {
    ESP_LOGI(kLogTag, "Audio disabled; skipping initialization");
    return ESP_OK;
  }
  if (is_microphone_ready || is_speaker_ready) {
    return ESP_ERR_INVALID_STATE;
  }

  // 2026-09-19: Drive only MAX98357 SD; leave GPIO20 at its reset input state to avoid disturbing native USB/JTAG use of that pin.
  gpio_config_t shutdown_config = {};
  shutdown_config.pin_bit_mask = 1ULL << hardware::kSpeakerShutdown;
  shutdown_config.mode = GPIO_MODE_OUTPUT;
  shutdown_config.pull_up_en = GPIO_PULLUP_DISABLE;
  shutdown_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  shutdown_config.intr_type = GPIO_INTR_DISABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&shutdown_config), kLogTag,
                      "Unable to configure MAX98357 shutdown GPIO");
  ESP_RETURN_ON_ERROR(gpio_set_level(hardware::kSpeakerShutdown, 0), kLogTag,
                      "Unable to mute MAX98357 during initialization");

  const esp_err_t input_result = initialize_microphone();
  is_microphone_ready = input_result == ESP_OK;
  if (!is_microphone_ready) {
    ESP_LOGE(kLogTag, "Microphone I2S initialization failed: %s",
             esp_err_to_name(input_result));
  }

  const esp_err_t output_result = initialize_speaker();
  is_speaker_ready = output_result == ESP_OK;
  if (!is_speaker_ready) {
    ESP_LOGE(kLogTag, "Speaker I2S initialization failed: %s",
             esp_err_to_name(output_result));
  }

  if (is_microphone_ready && is_speaker_ready) {
    ESP_LOGI(kLogTag, "Audio is ready");
    return ESP_OK;
  }
  return input_result != ESP_OK ? input_result : output_result;
}

bool microphone_ready() { return is_microphone_ready; }

bool speaker_ready() { return is_speaker_ready; }

esp_err_t read_pcm16(void *data, size_t byte_count, size_t *bytes_read,
                     uint32_t timeout_ms) {
  if (bytes_read != nullptr) {
    *bytes_read = 0;
  }
  if (!is_microphone_ready || microphone_channel == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if ((data == nullptr && byte_count != 0) ||
      byte_count % sizeof(int16_t) != 0) {
    return ESP_ERR_INVALID_ARG;
  }

  // 2026-09-19: Convert the INMP441 native 32-bit slots to signed 16-bit mono PCM in bounded stack chunks.
  int32_t native_samples[kNativeReadChunkSamples];
  auto *output_samples = static_cast<int16_t *>(data);
  const size_t requested_samples = byte_count / sizeof(int16_t);
  size_t converted_samples = 0;

  while (converted_samples < requested_samples) {
    const size_t chunk_samples = std::min(
        kNativeReadChunkSamples, requested_samples - converted_samples);
    size_t native_bytes_read = 0;
    const esp_err_t result = i2s_channel_read(
        microphone_channel, native_samples,
        chunk_samples * sizeof(native_samples[0]), &native_bytes_read,
        timeout_ms);
    const size_t samples_read = native_bytes_read / sizeof(native_samples[0]);

    for (size_t index = 0; index < samples_read; ++index) {
      output_samples[converted_samples + index] =
          static_cast<int16_t>(native_samples[index] >> 16);
    }
    converted_samples += samples_read;
    if (bytes_read != nullptr) {
      *bytes_read = converted_samples * sizeof(int16_t);
    }
    if (result != ESP_OK) {
      return result;
    }
    if (samples_read == 0) {
      return ESP_ERR_TIMEOUT;
    }
  }
  return ESP_OK;
}

esp_err_t record(int16_t *samples, size_t sample_count) {
  if (samples == nullptr && sample_count != 0) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t bytes_read = 0;
  const size_t requested_bytes = sample_count * sizeof(int16_t);
  const esp_err_t result = read_pcm16(samples, requested_bytes, &bytes_read,
                                      portMAX_DELAY);
  if (bytes_read < requested_bytes) {
    std::memset(reinterpret_cast<uint8_t *>(samples) + bytes_read, 0,
                requested_bytes - bytes_read);
  }
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Microphone I2S read failed: %s",
             esp_err_to_name(result));
  }
  return result;
}

void enhance_voice(int16_t *samples, size_t sample_count) {
  if (samples == nullptr) {
    return;
  }

  // 2026-09-19: Preserve the V1 low-pass and gain behavior while adding explicit saturation.
  constexpr float kCurrentWeight = 0.2F;
  constexpr float kPreviousWeight = 0.8F;
  constexpr float kGain = 5.0F;
  static int16_t previous_sample = 0;
  for (size_t index = 0; index < sample_count; ++index) {
    const float filtered = kCurrentWeight * samples[index] +
                           kPreviousWeight * previous_sample;
    previous_sample = scale_sample(static_cast<int16_t>(filtered), 1.0F);
    samples[index] = scale_sample(previous_sample, kGain);
  }
}

bool play(const int16_t *samples, size_t sample_count, float volume_ratio) {
  if (!is_speaker_ready || speaker_channel == nullptr ||
      (samples == nullptr && sample_count != 0) || volume_ratio < 0.0F) {
    return false;
  }
  if (gpio_set_level(hardware::kSpeakerShutdown, 1) != ESP_OK) {
    ESP_LOGE(kLogTag, "Unable to enable MAX98357");
    return false;
  }

  // 2026-09-19: Duplicate each scaled mono sample into both MAX98357 I2S slots using bounded writes.
  int16_t stereo_samples[kPlaybackChunkSamples * kStereoSlots];
  size_t offset = 0;
  while (offset < sample_count) {
    const size_t chunk_samples =
        std::min(kPlaybackChunkSamples, sample_count - offset);
    for (size_t index = 0; index < chunk_samples; ++index) {
      const int16_t scaled_sample =
          scale_sample(samples[offset + index], volume_ratio);
      stereo_samples[index * kStereoSlots] = scaled_sample;
      stereo_samples[index * kStereoSlots + 1] = scaled_sample;
    }

    size_t bytes_written = 0;
#if 0
    // 2026-09-20: Retain the pre-write marker; a failed write must not be reported as successful playback.
    // 2026-09-19: Preserve the first I2S submission milestone used by the V1 end-to-end latency trace.
    if (offset == 0) {
      diagnostics::latency_trace_mark(
          diagnostics::LatencyEvent::kPlaybackStart);
    }
#endif
    // 2026-09-20: Keep the submission timestamp for the first chunk, publishing it only after the write returns valid PCM bytes.
    const uint64_t write_start_us = offset == 0
        ? static_cast<uint64_t>(esp_timer_get_time()) : 0;
    const esp_err_t result = i2s_channel_write(
        speaker_channel, stereo_samples,
        chunk_samples * kStereoFrameBytes, &bytes_written, portMAX_DELAY);
    if (result != ESP_OK) {
      ESP_LOGE(kLogTag, "Speaker I2S write failed: %s",
               esp_err_to_name(result));
      return false;
    }
    if (bytes_written == 0 || bytes_written % kStereoFrameBytes != 0) {
      ESP_LOGE(kLogTag, "Speaker I2S returned invalid byte count: %u",
               static_cast<unsigned>(bytes_written));
      return false;
    }
    // 2026-09-20: Distinguish queue submission from a completed I2S write; neither timestamp claims the speaker's acoustic onset.
    if (offset == 0) {
      diagnostics::latency_trace_mark_at(
          diagnostics::LatencyEvent::kPlaybackStart, write_start_us);
      diagnostics::latency_trace_mark(
          diagnostics::LatencyEvent::kI2sWriteDone);
    }
    offset += bytes_written / kStereoFrameBytes;
  }
  return true;
}

bool wait_for_playback_complete() {
  if (!is_speaker_ready || speaker_channel == nullptr) {
    return false;
  }

  // 2026-09-19: Push one full DMA ring of checked silence because IDF 5.1 has no i2s_channel_wait_tx_done API.
  int16_t silence[kPlaybackChunkSamples * kStereoSlots] = {};
  size_t frames_remaining = hardware::kAudioDmaDescriptorCount *
                            hardware::kAudioDmaFrameCount;
  while (frames_remaining > 0) {
    const size_t frame_count =
        std::min(kPlaybackChunkSamples, frames_remaining);
    size_t bytes_written = 0;
    const size_t expected_bytes = frame_count * kStereoFrameBytes;
    const esp_err_t result = i2s_channel_write(
        speaker_channel, silence, expected_bytes, &bytes_written,
        portMAX_DELAY);
    if (result != ESP_OK || bytes_written != expected_bytes) {
      ESP_LOGE(kLogTag, "Speaker DMA drain failed: result=%s bytes=%u/%u",
               esp_err_to_name(result), static_cast<unsigned>(bytes_written),
               static_cast<unsigned>(expected_bytes));
      return false;
    }
    frames_remaining -= frame_count;
  }
  return true;
}

void stop_playback() {
  if (is_speaker_ready &&
      gpio_set_level(hardware::kSpeakerShutdown, 0) != ESP_OK) {
    ESP_LOGE(kLogTag, "Unable to mute MAX98357");
  }
}

bool test_speaker() {
  if (!is_speaker_ready || speaker_channel == nullptr) {
    ESP_LOGE(kLogTag, "Speaker test unavailable: I2S output is not ready");
    return false;
  }

  // 2026-09-19: Generate the V1 one-second 1 kHz diagnostic tone without allocating a full second of PCM.
  constexpr float kTwoPi = 6.28318530718F;
  constexpr float kToneFrequency = 1000.0F;
  constexpr int16_t kToneAmplitude = 10000;
  constexpr size_t kToneSamples = hardware::kAudioSampleRate;
  int16_t tone[256];
  ESP_LOGI(kLogTag, "Speaker test: 1 kHz tone for 1 second");

  bool succeeded = true;
  for (size_t offset = 0; offset < kToneSamples;
       offset += sizeof(tone) / sizeof(tone[0])) {
    const size_t count = std::min(sizeof(tone) / sizeof(tone[0]),
                                  kToneSamples - offset);
    for (size_t index = 0; index < count; ++index) {
      const float phase = kTwoPi * kToneFrequency * (offset + index) /
                          hardware::kAudioSampleRate;
      tone[index] =
          static_cast<int16_t>(kToneAmplitude * std::sin(phase));
    }
    if (!play(tone, count)) {
      succeeded = false;
      break;
    }
  }
  if (succeeded) {
    succeeded = wait_for_playback_complete();
  }
  stop_playback();
  return succeeded;
}

size_t calculate_mean_amplitude(const int16_t *samples,
                                size_t sample_count) {
  if (samples == nullptr || sample_count == 0) {
    return 0;
  }

  uint64_t amplitude_sum = 0;
  for (size_t index = 0; index < sample_count; ++index) {
    const int32_t sample = samples[index];
    amplitude_sum += sample < 0 ? static_cast<uint32_t>(-sample)
                                : static_cast<uint32_t>(sample);
  }
  return static_cast<size_t>(amplitude_sum / sample_count);
}

}  // namespace desk_talk::audio
