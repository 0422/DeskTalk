// 2026-09-19: Add a minimal native ESP-IDF entry point before migrating hardware and cloud modules.
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

// 2026-09-19: Initialize the migrated audio layer and reusable memory diagnostics from the native IDF entry point.
#include "audio.h"
#include "command_console.h"
#include "conversation.h"
#include "device_ui.h"
#include "hardware_config.h"
#include "memory_diagnostics.h"
#include "storage.h"
#include "tts_client.h"
#include "wake_word.h"
#include "wifi_manager.h"

namespace {

constexpr char kLogTag[] = "desk_talk";

}  // namespace

extern "C" void app_main(void) {
  ESP_LOGI(kLogTag, "Desk Talk ESP-IDF migration baseline started");
#if 0
  // 2026-09-19: Retain the former inline heap log while the shared diagnostic now reports internal, DMA, and PSRAM pools.
  ESP_LOGI(kLogTag,
           "Internal heap: free=%u, largest=%u",
           static_cast<unsigned>(
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif

  desk_talk::diagnostics::log_memory_snapshot("startup");
  const esp_err_t storage_result = desk_talk::storage::initialize();
  if (storage_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Storage initialization failed: %s",
             esp_err_to_name(storage_result));
  }

  const esp_err_t audio_result = desk_talk::audio::initialize();
  if (audio_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Audio initialization failed: %s",
             esp_err_to_name(audio_result));
  }
  desk_talk::diagnostics::log_memory_snapshot("audio-ready");

  // 2026-09-19: Initialize migrated OLED, WS2812, and servo drivers before cloud tasks can publish state.
  const esp_err_t ui_result = desk_talk::ui::initialize();
  if (ui_result != ESP_OK) {
    ESP_LOGW(kLogTag, "UI initialization was partial: %s",
             esp_err_to_name(ui_result));
  }
  desk_talk::ui::camera_initialize_disabled();
  desk_talk::ui::gesture_initialize_disabled();

  // 2026-09-19: Bring up Wi-Fi after local audio so later cloud clients share a known memory baseline.
  if (storage_result == ESP_OK) {
    const esp_err_t wifi_result =
        desk_talk::network::wifi_manager().initialize();
    if (wifi_result != ESP_OK) {
      ESP_LOGE(kLogTag, "Wi-Fi initialization failed: %s",
               esp_err_to_name(wifi_result));
    }
  }
  desk_talk::diagnostics::log_memory_snapshot("wifi-ready");

  // 2026-09-19: Keep the audible self-test opt-in so normal boots remain silent.
  if (desk_talk::hardware::kRunSpeakerSelfTestOnBoot &&
      !desk_talk::audio::test_speaker()) {
    ESP_LOGE(kLogTag, "Speaker self-test failed");
  }

  // 2026-09-19: Load WakeNet after I2S is ready; failure is non-fatal so ASR/TLS diagnostics remain reachable.
  const esp_err_t wake_result = desk_talk::wake_word::initialize();
  if (wake_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Wake word unavailable: %s", esp_err_to_name(wake_result));
  }

#if 0
  // 2026-09-20: Retain the former boot-time TTS warmup; it competed with the idle WakeNet/AFE instance for internal RAM.
  if (desk_talk::network::wifi_manager().connected() &&
      !desk_talk::cloud::tts_client().warmup_persistent_connection()) {
    ESP_LOGW(kLogTag, "TTS warmup failed; the first sentence will retry");
  }
#endif
  // 2026-09-20: Defer TTS warmup until WakeNet detects speech and releases ESP-SR, then preserve that socket across follow-up turns.
  const esp_err_t conversation_result = desk_talk::conversation::start();
  if (conversation_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Conversation task failed: %s",
             esp_err_to_name(conversation_result));
  }
  // 2026-09-19: Restore V1 newline-delimited JSON actions and factory commands on the configured IDF console transport.
  const esp_err_t console_result = desk_talk::console::start();
  if (console_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Command console task failed: %s",
             esp_err_to_name(console_result));
  }
}
