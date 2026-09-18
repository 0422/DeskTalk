#include "audio.h"

#include <math.h>

// 2026-09-11: Use ESP-IDF's new standard-mode I2S API so audio can coexist with the new driver linked by ESP-SR.
#include <driver/i2s_std.h>
// 2026-09-17: Mark the first I2S write separately from TTS audio arrival for latency measurement.
#include "latency_trace.h"

// 2026-09-11: Keep the original legacy I2S configuration for reference, but exclude all legacy symbols from the firmware.
#if 0
i2s_config_t i2sIn_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = i2s_bits_per_sample_t(16),
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = DMA_BUF_LEN
};

const i2s_pin_config_t i2sIn_pin_config = {
    .bck_io_num = INMP441_SCK,
    .ws_io_num = INMP441_WS,
    .data_out_num = -1,
    .data_in_num = INMP441_SD
};

i2s_config_t i2sOut_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = i2s_bits_per_sample_t(16),
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = DMA_BUF_LEN
};

const i2s_pin_config_t i2sOut_pin_config = {
    .bck_io_num = MAX98357_BCLK,
    .ws_io_num = MAX98357_LRC,
    .data_out_num = MAX98357_DIN,
    .data_in_num = -1
};
#endif

// 2026-09-11: Keep microphone RX on I2S0 and speaker TX on I2S1, matching the former hardware-controller assignment.
static i2s_chan_handle_t i2s_in_handle = nullptr;
static i2s_chan_handle_t i2s_out_handle = nullptr;
static bool microphone_ready = false;
static bool speaker_ready = false;

// 2026-09-11: Capture the INMP441's native 24-bit samples in 32-bit mono-left I2S slots at 16 kHz.
static esp_err_t setup_i2s_input() {
  i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel_config.dma_desc_num = DMA_BUF_COUNT;
  channel_config.dma_frame_num = DMA_BUF_LEN;

  esp_err_t result = i2s_new_channel(&channel_config, nullptr, &i2s_in_handle);
  if (result != ESP_OK) {
    return result;
  }

  i2s_std_config_t standard_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = INMP441_SCK,
          .ws = INMP441_WS,
          .dout = I2S_GPIO_UNUSED,
          .din = INMP441_SD,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  result = i2s_channel_init_std_mode(i2s_in_handle, &standard_config);
  if (result == ESP_OK) {
    result = i2s_channel_enable(i2s_in_handle);
  }
  if (result != ESP_OK) {
    i2s_del_channel(i2s_in_handle);
    i2s_in_handle = nullptr;
  }
  return result;
}

// 2026-09-11: Allocate the MAX98357 output explicitly on I2S1 so it cannot share clocks with the microphone channel.
static esp_err_t setup_i2s_output() {
  i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  channel_config.dma_desc_num = DMA_BUF_COUNT;
  channel_config.dma_frame_num = DMA_BUF_LEN;
  channel_config.auto_clear = true;

  esp_err_t result = i2s_new_channel(&channel_config, &i2s_out_handle, nullptr);
  if (result != ESP_OK) {
    return result;
  }

  i2s_std_config_t standard_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = MAX98357_BCLK,
          .ws = MAX98357_LRC,
          .dout = MAX98357_DIN,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  // Send the same mono signal in both slots. This works with MAX98357 boards
  // configured for left, right, or mono output through SD_MODE.
  standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

  result = i2s_channel_init_std_mode(i2s_out_handle, &standard_config);
  if (result == ESP_OK) {
    result = i2s_channel_enable(i2s_out_handle);
  }
  if (result != ESP_OK) {
    i2s_del_channel(i2s_out_handle);
    i2s_out_handle = nullptr;
  }
  return result;
}

void setup_audio() {
    // 2026-09-11: Allow non-audio hardware profiles to skip both I2S controllers cleanly.
#if !DESK_EMOJI_ENABLE_AUDIO
    log_info("Audio disabled; skipping initialization.");
    return;
#endif
    pinMode(MAX98357_GAIN, INPUT);
    pinMode(MAX98357_SD, OUTPUT);
    digitalWrite(MAX98357_SD, HIGH);

    // 2026-09-11: Preserve the former initialization calls for reference while preventing the legacy driver from being linked.
#if 0
    i2s_driver_install(I2S_NUM_0, &i2sIn_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &i2sIn_pin_config);

    i2s_driver_install(I2S_NUM_1, &i2sOut_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &i2sOut_pin_config);
#endif

    // 2026-09-11: Initialize both audio directions with the new driver and report each hardware failure independently.
    esp_err_t input_result = setup_i2s_input();
    microphone_ready = input_result == ESP_OK;
    if (!microphone_ready) {
      log_error("Microphone I2S initialization failed: %s", esp_err_to_name(input_result));
    }

    esp_err_t output_result = setup_i2s_output();
    speaker_ready = output_result == ESP_OK;
    if (!speaker_ready) {
      log_error("Speaker I2S initialization failed: %s", esp_err_to_name(output_result));
      digitalWrite(MAX98357_SD, LOW);
    }

    if (microphone_ready && speaker_ready) {
      log_info("Audio is Ready.");
    }
}

// 2026-09-11: Expose input readiness so WakeNet cannot start without a working I2S microphone channel.
bool microphone_is_ready() {
  return microphone_ready;
}

// 2026-09-11: Expose output readiness for diagnostics without sharing the I2S channel handle.
bool speaker_is_ready() {
  return speaker_ready;
}

// 2026-09-11: Convert native 32-bit INMP441 slots to the 16-bit mono PCM expected by WakeNet and cloud ASR.
esp_err_t read_audio_bytes(void *data, size_t length, size_t *bytes_read, uint32_t timeout_ms) {
  if (bytes_read != nullptr) {
    *bytes_read = 0;
  }
  if (!microphone_ready || i2s_in_handle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if ((data == nullptr && length != 0) || length % sizeof(int16_t) != 0) {
    return ESP_ERR_INVALID_ARG;
  }

  // 2026-09-11: Use a bounded stack buffer and the same high-word conversion as ESP_I2S's 32-to-16 RX transform.
  static constexpr size_t native_chunk_samples = 128;
  uint32_t native_samples[native_chunk_samples];
  uint16_t *output_samples = static_cast<uint16_t *>(data);
  const size_t requested_samples = length / sizeof(int16_t);
  size_t converted_samples = 0;

  while (converted_samples < requested_samples) {
    const size_t chunk_samples = min(
        native_chunk_samples, requested_samples - converted_samples);
    size_t native_bytes_read = 0;
    esp_err_t result = i2s_channel_read(
        i2s_in_handle, native_samples,
        chunk_samples * sizeof(native_samples[0]),
        &native_bytes_read, timeout_ms);
    const size_t samples_read = native_bytes_read / sizeof(native_samples[0]);

    for (size_t i = 0; i < samples_read; i++) {
      output_samples[converted_samples + i] = native_samples[i] >> 16;
    }
    converted_samples += samples_read;
    if (bytes_read != nullptr) {
      *bytes_read = converted_samples * sizeof(int16_t);
    }

    if (result != ESP_OK) {
      return result;
    }
    // 2026-09-11: Reject an empty successful read so callers cannot wait forever in this conversion loop.
    if (samples_read == 0) {
      return ESP_ERR_TIMEOUT;
    }
  }

  return ESP_OK;
}

void record(int16_t *data, size_t length) {
  // 2026-09-11: Keep the original legacy read for reference while recording through the shared new-driver channel.
#if 0
  size_t bytes_read;
  i2s_read(I2S_NUM_0, data, length * sizeof(int16_t), &bytes_read, portMAX_DELAY);
#endif
  // 2026-09-11: Zero any unread tail so ASR never processes stale samples after an I2S error or short read.
  size_t bytes_read = 0;
  size_t bytes_requested = length * sizeof(int16_t);
  esp_err_t result = read_audio_bytes(data, bytes_requested, &bytes_read, portMAX_DELAY);
  if (bytes_read < bytes_requested) {
    memset(reinterpret_cast<uint8_t *>(data) + bytes_read, 0, bytes_requested - bytes_read);
  }
  if (result != ESP_OK) {
    log_error("Microphone I2S read failed: %s", esp_err_to_name(result));
  }
}

void enhanceVoice(int16_t *data, size_t length) {
  const float a0 = 0.2;
  const float a1 = 0.8;
  static int16_t prev_sample = 0;
  
  for (int i = 0; i < length; i++) {
    int16_t filtered = a0 * data[i] + a1 * prev_sample;
    prev_sample = filtered;
    data[i] = constrain(filtered * 5, -32768, 32767);
  }
}

void play(const int16_t *data, size_t length, float volume_ratio) {
  // 2026-09-11: Keep the original sample-by-sample legacy write for reference while using the new TX channel below.
#if 0
  size_t bytes_written;
  for(size_t i = 0; i < length; i++) {
    int16_t scaled_sample = data[i] * volume_ratio;
    i2s_write(I2S_NUM_1, &scaled_sample, sizeof(int16_t), &bytes_written, portMAX_DELAY);
  }
#endif
  // 2026-09-11: Write scaled audio in bounded chunks to reduce driver overhead and re-enable the amplifier after stop_play().
  if (!speaker_ready || i2s_out_handle == nullptr) {
    return;
  }

  digitalWrite(MAX98357_SD, HIGH);
  static const size_t playback_chunk_samples = 128;
  int16_t stereo_samples[playback_chunk_samples * 2];
  static constexpr size_t stereo_frame_bytes = sizeof(int16_t) * 2;
  size_t offset = 0;
  while (offset < length) {
    size_t chunk_samples = min(playback_chunk_samples, length - offset);
    for (size_t i = 0; i < chunk_samples; i++) {
      const int16_t scaled_sample = data[offset + i] * volume_ratio;
      stereo_samples[i * 2] = scaled_sample;
      stereo_samples[i * 2 + 1] = scaled_sample;
    }

    size_t bytes_written = 0;
    // 2026-09-17: Timestamp immediately before the first PCM chunk is submitted to the speaker's I2S DMA path.
    if (offset == 0) {
      latency_trace_mark(LatencyEvent::PLAYBACK_START);
    }
    esp_err_t result = i2s_channel_write(
        i2s_out_handle, stereo_samples, chunk_samples * stereo_frame_bytes,
        &bytes_written, portMAX_DELAY);
    if (result != ESP_OK) {
      log_error("Speaker I2S write failed: %s", esp_err_to_name(result));
      break;
    }
    // 2026-09-11: Prevent an unexpected zero-length successful write from leaving playback in an endless loop.
    if (bytes_written == 0) {
      log_error("Speaker I2S write returned zero bytes");
      break;
    }
    if (bytes_written % stereo_frame_bytes != 0) {
      log_error("Speaker I2S write returned a partial stereo frame");
      break;
    }
    offset += bytes_written / stereo_frame_bytes;
  }
}

void wait_for_playback_complete() {
  if (!speaker_ready || i2s_out_handle == nullptr) {
    return;
  }

  // i2s_channel_write() returns after copying data into DMA. At most all TX
  // descriptors can still be pending, so wait for that bounded duration plus
  // a small scheduling margin before MAX98357_SD is pulled low.
  constexpr uint32_t dma_drain_ms =
      (DMA_BUF_COUNT * DMA_BUF_LEN * 1000UL + SAMPLE_RATE - 1) /
          SAMPLE_RATE +
      50;
  delay(dma_drain_ms);
}

void test_speaker() {
  if (!speaker_ready || i2s_out_handle == nullptr) {
    log_error("Speaker test unavailable: I2S output is not ready");
    return;
  }

  // A one-second 1 kHz tone makes wiring and the I2S format easy to verify.
  constexpr float kTwoPi = 6.28318530718f;
  constexpr float kToneFrequency = 1000.0f;
  constexpr int16_t kToneAmplitude = 10000;
  constexpr size_t kToneSamples = SAMPLE_RATE;
  int16_t tone[256];
  log_info("Speaker test: 1 kHz tone for 1 second");
  digitalWrite(MAX98357_SD, HIGH);
  for (size_t offset = 0; offset < kToneSamples; offset += sizeof(tone) / sizeof(tone[0])) {
    const size_t count = min(sizeof(tone) / sizeof(tone[0]), kToneSamples - offset);
    for (size_t i = 0; i < count; i++) {
      const float phase = kTwoPi * kToneFrequency * (offset + i) / SAMPLE_RATE;
      tone[i] = static_cast<int16_t>(kToneAmplitude * sinf(phase));
    }
    play(tone, count, 1.0f);
  }
  wait_for_playback_complete();
  stop_play();
}

void stop_play() {
  // 2026-09-11: Do not touch the amplifier shutdown pin unless speaker I2S was initialized.
  if (!speaker_ready) {
    return;
  }
  // 2026-09-11: Keep the legacy DMA clear call for reference; the new TX channel auto-clears and the amplifier shutdown pin mutes immediately.
#if 0
  i2s_zero_dma_buffer(I2S_NUM_1);
#endif
  digitalWrite(MAX98357_SD, LOW);
}

size_t calculate_mean(const int16_t *data, size_t length) {
  // size_t mean = 0;
  // size_t count = 0;
  // for (size_t i = 0; i < length; i += 1000) {
  //   mean += abs(data[i]);
  //   count++;
  // }
  // return mean / count;
  // 2026-09-11: Average the full audio block so speech and silence detection is stable instead of relying on a few sparse samples.
  if (length == 0) {
    return 0;
  }

  uint64_t amplitude_sum = 0;
  for (size_t i = 0; i < length; i++) {
    amplitude_sum += abs((int32_t)data[i]);
  }
  return amplitude_sum / length;
}
