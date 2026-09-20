// 2026-09-19: Replace esp32-hal-sr with native ESP-SR AFE, WakeNet, and MultiNet processing over the existing I2S microphone.
#include "wake_word.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "audio.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#if 0
// 2026-09-19: Retain the pre-2.5 ESP-SR model-path include for migration reference; ESP-SR 2.5.4 exports model_path.h.
#include "esp_srmodel.h"
#endif
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "hardware_config.h"
#include "memory_diagnostics.h"

namespace desk_talk::wake_word {
namespace {

constexpr char kLogTag[] = "wake_word";
constexpr EventBits_t kDetectedBit = BIT0;
constexpr EventBits_t kFeedPausedBit = BIT1;
constexpr EventBits_t kDetectPausedBit = BIT2;
const esp_afe_sr_iface_t *afe = nullptr;
esp_afe_sr_data_t *afe_data = nullptr;
esp_mn_iface_t *multinet = nullptr;
model_iface_data_t *multinet_data = nullptr;
int16_t *multinet_samples = nullptr;
size_t multinet_chunk_samples = 0;
size_t multinet_buffered_samples = 0;
srmodel_list_t *models = nullptr;
EventGroupHandle_t events = nullptr;
TaskHandle_t feed_task = nullptr;
TaskHandle_t detect_task = nullptr;
volatile bool paused = false;
volatile bool initialized = false;

// 2026-09-19: Reset command recognition at every microphone ownership transition so stale pre-ASR audio cannot trigger a later turn.
void reset_detection_state() {
  if (multinet != nullptr && multinet_data != nullptr) {
    multinet->clean(multinet_data);
  }
  multinet_buffered_samples = 0;
  if (afe != nullptr && afe_data != nullptr) afe->reset_buffer(afe_data);
}

// 2026-09-19: Accumulate AFE frames because its fetch size is not required to equal the selected MultiNet model's input chunk size.
void detect_multinet_commands(const int16_t *samples, size_t sample_count) {
  if (multinet == nullptr || multinet_data == nullptr ||
      multinet_samples == nullptr || multinet_chunk_samples == 0) {
    return;
  }
  while (sample_count > 0) {
    const size_t copied = std::min(
        sample_count, multinet_chunk_samples - multinet_buffered_samples);
    std::memcpy(multinet_samples + multinet_buffered_samples, samples,
                copied * sizeof(int16_t));
    multinet_buffered_samples += copied;
    samples += copied;
    sample_count -= copied;
    if (multinet_buffered_samples != multinet_chunk_samples) continue;

    multinet_buffered_samples = 0;
    const esp_mn_state_t state =
        multinet->detect(multinet_data, multinet_samples);
    if (state == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t *result = multinet->get_results(multinet_data);
      if (result != nullptr && result->num > 0) {
        ESP_LOGI(kLogTag, "MultiNet wake phrase detected: id=%d text=%s",
                 result->command_id[0], result->string);
        xEventGroupSetBits(events, kDetectedBit);
      }
      multinet->clean(multinet_data);
    } else if (state == ESP_MN_STATE_TIMEOUT) {
      multinet->clean(multinet_data);
    }
  }
}

void feed_loop(void *) {
  const int feed_samples = afe->get_feed_chunksize(afe_data);
  int16_t *samples = static_cast<int16_t *>(heap_caps_malloc(
      feed_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (samples == nullptr) {
    ESP_LOGE(kLogTag, "AFE feed buffer allocation failed");
#if 0
    // 2026-09-20: Retain the ordinary deletion used before the feed task stack moved to capability-aware PSRAM allocation.
    vTaskDelete(nullptr);
#endif
    // 2026-09-20: A task created with xTaskCreatePinnedToCoreWithCaps must use the matching capability-aware deletion API.
    vTaskDeleteWithCaps(nullptr);
    return;
  }
  // 2026-09-19: Report a low-rate microphone level while diagnosing migrated INMP441 capture without flooding the serial console.
  unsigned diagnostic_frame_count = 0;
  // 2026-09-20: Accumulate the full diagnostic window so short speech is not missed between sparse log samples.
  uint64_t diagnostic_amplitude_sum = 0;
  size_t diagnostic_peak_amplitude = 0;
  while (true) {
    if (paused) {
      xEventGroupSetBits(events, kFeedPausedBit);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    xEventGroupClearBits(events, kFeedPausedBit);
    size_t bytes_read = 0;
    const esp_err_t result = audio::read_pcm16(
        samples, feed_samples * sizeof(int16_t), &bytes_read, 1000);
    if (result == ESP_OK &&
        bytes_read == static_cast<size_t>(feed_samples) * sizeof(int16_t)) {
      afe->feed(afe_data, samples);
#if 0
      // 2026-09-19: Emit roughly one level sample per two seconds so silent/incorrect I2S input is visible before WakeNet detection.
      if (++diagnostic_frame_count >= 64) {
        diagnostic_frame_count = 0;
        ESP_LOGI(kLogTag, "Microphone mean amplitude: %u",
                 static_cast<unsigned>(audio::calculate_mean_amplitude(
                     samples, static_cast<size_t>(feed_samples))));
      }
#endif
      // 2026-09-20: Report both the two-second average and highest frame mean so a short wake phrase remains visible in diagnostics.
      const size_t frame_amplitude = audio::calculate_mean_amplitude(
          samples, static_cast<size_t>(feed_samples));
      diagnostic_amplitude_sum += frame_amplitude;
      diagnostic_peak_amplitude =
          std::max(diagnostic_peak_amplitude, frame_amplitude);
      if (++diagnostic_frame_count >= 64) {
        ESP_LOGI(kLogTag, "Microphone amplitude: average=%u peak=%u",
                 static_cast<unsigned>(diagnostic_amplitude_sum /
                                       diagnostic_frame_count),
                 static_cast<unsigned>(diagnostic_peak_amplitude));
        diagnostic_frame_count = 0;
        diagnostic_amplitude_sum = 0;
        diagnostic_peak_amplitude = 0;
      }
    } else {
      vTaskDelay(1);
    }
  }
}

void detect_loop(void *) {
  while (true) {
    if (paused) {
      // 2026-09-19: Wait for the producer to stop before clearing AFE/MultiNet state, then acknowledge that ASR may safely claim I2S.
      if ((xEventGroupGetBits(events) & kFeedPausedBit) != 0) {
        reset_detection_state();
        xEventGroupSetBits(events, kDetectPausedBit);
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    xEventGroupClearBits(events, kDetectPausedBit);
    afe_fetch_result_t *result = afe->fetch(afe_data);
    if (result == nullptr || result->ret_value != ESP_OK) {
      vTaskDelay(1);
      continue;
    }
    if (result->wakeup_state == WAKENET_DETECTED) {
      ESP_LOGI(kLogTag, "Wake word detected");
      xEventGroupSetBits(events, kDetectedBit);
    }
    if (result->data != nullptr && result->data_size > 0) {
      detect_multinet_commands(
          result->data, static_cast<size_t>(result->data_size) / sizeof(int16_t));
    }
  }
}

}  // namespace

esp_err_t initialize() {
  if (!hardware::kEnableWakeWord) return ESP_ERR_NOT_SUPPORTED;
  if (initialized) return ESP_OK;
  if (!audio::microphone_ready()) return ESP_ERR_INVALID_STATE;
  events = xEventGroupCreate();
  if (events == nullptr) return ESP_ERR_NO_MEM;
#if 0
  // 2026-09-20: Retain the former unconditional model mapping for reference; repeated session teardown must not unmap Flash from a PSRAM-backed task stack.
  models = esp_srmodel_init("model");
#endif
  // 2026-09-20: Keep the model partition mapped across conversations and reuse it when WakeNet is reconstructed.
  if (models == nullptr) models = esp_srmodel_init("model");
  if (models == nullptr) {
    ESP_LOGE(kLogTag, "No ESP-SR model partition was loaded");
    return ESP_ERR_NOT_FOUND;
  }
  char *wake_model = esp_srmodel_filter(models, ESP_WN_PREFIX, nullptr);
  if (wake_model == nullptr) {
    ESP_LOGE(kLogTag, "No WakeNet model is enabled in the model partition");
    return ESP_ERR_NOT_FOUND;
  }
  // 2026-09-19: Add both V1 English wake phrases to MultiNet so either "Hi ESP" or "Hi Della" starts a conversation.
  char *multinet_model = esp_srmodel_filter(models, ESP_MN_PREFIX, nullptr);
  if (multinet_model == nullptr) {
    ESP_LOGE(kLogTag, "No MultiNet model is enabled in the model partition");
    return ESP_ERR_NOT_FOUND;
  }
  multinet = esp_mn_handle_from_name(multinet_model);
  if (multinet == nullptr) return ESP_ERR_NOT_SUPPORTED;
  multinet_data = multinet->create(multinet_model, 6000);
  if (multinet_data == nullptr) return ESP_ERR_NO_MEM;
  if (esp_mn_commands_alloc(multinet, multinet_data) != ESP_OK ||
      esp_mn_commands_phoneme_add(1, "HI ESP", "hi m fS Pm") != ESP_OK ||
      esp_mn_commands_phoneme_add(2, "HI DELLA", "hi DfLc") != ESP_OK ||
      esp_mn_commands_update() != nullptr) {
    ESP_LOGE(kLogTag, "Failed to configure MultiNet wake phrases");
    return ESP_ERR_INVALID_STATE;
  }
  multinet_chunk_samples = static_cast<size_t>(
      multinet->get_samp_chunksize(multinet_data));
  multinet_samples = static_cast<int16_t *>(heap_caps_malloc(
      multinet_chunk_samples * sizeof(int16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (multinet_samples == nullptr) return ESP_ERR_NO_MEM;

  // 2026-09-19: Use ESP-SR 2.5.4's explicit configuration factory; the former AFE_CONFIG_DEFAULT/ESP_AFE_SR_HANDLE API was removed.
  afe_config_t *configuration =
      afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (configuration == nullptr) return ESP_ERR_NO_MEM;
  configuration->wakenet_model_name = wake_model;
  configuration->wakenet_init = true;
  configuration->aec_init = false;
  configuration->se_init = false;
  configuration->vad_init = true;
  configuration->afe_perferred_core = 0;
  configuration->afe_perferred_priority = 5;
  configuration->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  configuration->pcm_config.total_ch_num = 1;
  configuration->pcm_config.mic_num = 1;
  configuration->pcm_config.ref_num = 0;
  afe = esp_afe_handle_from_config(configuration);
  if (afe == nullptr) {
    afe_config_free(configuration);
    return ESP_ERR_INVALID_STATE;
  }
  afe_data = afe->create_from_config(configuration);
  afe_config_free(configuration);
#if 0
// 2026-09-19: Retain the pre-ESP-SR-2.5 initialization sequence for migration reference; it is incompatible with the current component API.
  afe_config_t legacy_configuration = AFE_CONFIG_DEFAULT();
  legacy_configuration.wakenet_model_name = wake_model;
  legacy_configuration.wakenet_init = true;
  legacy_configuration.aec_init = false;
  legacy_configuration.se_init = true;
  legacy_configuration.vad_init = true;
  legacy_configuration.voice_communication_init = false;
  legacy_configuration.afe_perferred_core = 0;
  legacy_configuration.afe_perferred_priority = 5;
  legacy_configuration.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  legacy_configuration.pcm_config.total_ch_num = 1;
  legacy_configuration.pcm_config.mic_num = 1;
  legacy_configuration.pcm_config.ref_num = 0;
  afe = &ESP_AFE_SR_HANDLE;
  afe_data = afe->create_from_config(&legacy_configuration);
#endif
  if (afe_data == nullptr) return ESP_ERR_NO_MEM;
#if 0
  // 2026-09-20: Retain the former internal-stack task creation path for migration reference.
  if (xTaskCreatePinnedToCore(feed_loop, "sr_feed", 4096, nullptr, 5,
                              &feed_task, 0) != pdPASS ||
      xTaskCreatePinnedToCore(detect_loop, "sr_detect", 6144, nullptr, 5,
                              &detect_task, 1) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
#endif
  // 2026-09-20: Place the long-lived ESP-SR feed and detect stacks in PSRAM so AFE and TLS retain contiguous internal RAM.
  if (xTaskCreatePinnedToCoreWithCaps(
          feed_loop, "sr_feed", 4096, nullptr, 5, &feed_task, 0,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
      xTaskCreatePinnedToCoreWithCaps(
          detect_loop, "sr_detect", 6144, nullptr, 5, &detect_task, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  initialized = true;
  ESP_LOGI(kLogTag, "WakeNet/MultiNet ready: %s, %s (chunk=%u)",
           wake_model, multinet_model,
           static_cast<unsigned>(multinet_chunk_samples));
  return ESP_OK;
}

bool wait_for_detection(unsigned timeout_ms) {
  if (!initialized) return false;
  const TickType_t timeout = timeout_ms == UINT32_MAX
                                 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(timeout_ms);
  return (xEventGroupWaitBits(events, kDetectedBit, pdTRUE, pdFALSE, timeout) &
          kDetectedBit) != 0;
}

void pause() {
#if 0
  // 2026-09-20: Retain the former unguarded pause path for migration reference.
  paused = true;
  xEventGroupClearBits(events, kDetectedBit);
#endif
  // 2026-09-20: Ignore pause requests after ESP-SR has been released for an active cloud conversation.
  if (!initialized || events == nullptr) return;
  paused = true;
  xEventGroupClearBits(events, kDetectedBit);
  // 2026-09-19: Wait for both ESP-SR tasks and their reset before cloud ASR claims the microphone.
  const EventBits_t bits = xEventGroupWaitBits(
      events, kFeedPausedBit | kDetectPausedBit, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(1200));
  if ((bits & (kFeedPausedBit | kDetectPausedBit)) !=
      (kFeedPausedBit | kDetectPausedBit)) {
    ESP_LOGW(kLogTag, "Timed out while pausing ESP-SR tasks");
  }
}

void resume() {
#if 0
  // 2026-09-20: Retain the former unguarded resume path for migration reference.
  // 2026-09-19: The detect task reset AFE and MultiNet while paused; clear acknowledgements before restarting both workers.
  xEventGroupClearBits(events,
                       kDetectedBit | kFeedPausedBit | kDetectPausedBit);
  paused = false;
#endif
  // 2026-09-20: Resume only a resident ESP-SR instance; released instances must be restored through initialize().
  if (!initialized || events == nullptr) return;
  xEventGroupClearBits(events,
                       kDetectedBit | kFeedPausedBit | kDetectPausedBit);
  paused = false;
}

// 2026-09-20: Tear down all wake-word inference resources after detection so a persistent TTS socket can coexist with ASR and LLM TLS.
void shutdown() {
  if (!initialized) return;
  pause();

  if (feed_task != nullptr) {
    // 2026-09-20: Freeze the acknowledged paused task before freeing its PSRAM stack from another core.
    vTaskSuspend(feed_task);
    vTaskDeleteWithCaps(feed_task);
    feed_task = nullptr;
  }
  if (detect_task != nullptr) {
    // 2026-09-20: Freeze the acknowledged paused task before freeing its PSRAM stack from another core.
    vTaskSuspend(detect_task);
    vTaskDeleteWithCaps(detect_task);
    detect_task = nullptr;
  }
  if (afe != nullptr && afe_data != nullptr) {
    afe->destroy(afe_data);
    afe_data = nullptr;
  }
  if (multinet != nullptr && multinet_data != nullptr) {
#if 0
    // 2026-09-20: Retain the former explicit command cleanup for reference; the current ESP-SR command helper reports uninitialized state here and alloc cleans stale state on reconstruction.
    esp_mn_commands_free();
#endif
    multinet->destroy(multinet_data);
    multinet_data = nullptr;
  }
  if (multinet_samples != nullptr) {
    heap_caps_free(multinet_samples);
    multinet_samples = nullptr;
  }
  multinet_chunk_samples = 0;
  multinet_buffered_samples = 0;
  multinet = nullptr;
  afe = nullptr;
#if 0
  // 2026-09-20: Retain the former model unmap path for reference; esp_srmodel_deinit freezes Flash/PSRAM cache and cannot run on this PSRAM-backed conversation task stack.
  if (models != nullptr) {
    esp_srmodel_deinit(models);
    models = nullptr;
  }
#endif
  // 2026-09-20: Leave models mapped so shutdown avoids a cache-freeze assertion and initialize can reuse the partition mapping.
  if (events != nullptr) {
    vEventGroupDelete(events);
    events = nullptr;
  }
  paused = false;
  initialized = false;
  diagnostics::log_memory_snapshot("WakeNet released");
}

bool ready() { return initialized; }

}  // namespace desk_talk::wake_word
