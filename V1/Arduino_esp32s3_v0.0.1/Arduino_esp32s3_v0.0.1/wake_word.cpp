// 2026-09-11: Implement local "Hi ESP" and "Hi Della" detection on the existing mono INMP441 I2S input.
#include "wake_word.h"

// 2026-09-11: Keep the legacy I2S include for reference but exclude it so ESP-SR and audio use only the new driver.
#if 0
#include <driver/i2s.h>
#endif
#include <esp32-hal-sr.h>
// 2026-09-17: Do not subscribe ESP-SR inference tasks to the 5-second task watchdog; a valid AFE pass or an intentional pause can exceed it.
// #include <esp_task_wdt.h>

#include "audio.h"
#include "common.h"

// 2026-09-11: Share wake-word state safely between ESP-SR callback tasks and the Arduino loop task.
static volatile bool wake_word_detected = false;
static volatile int wake_word_phrase_id = -1;
static bool wake_word_ready = false;
static volatile bool wake_word_paused = false;
// 2026-09-11: Reject late callbacks and buffered audio briefly after ASR releases the microphone.
static volatile unsigned long wake_word_resumed_at = 0;
static volatile bool wake_word_resume_cooldown = false;

// 2026-09-11: Register both phrases as aliases of one MultiNet trigger so either phrase starts a conversation.
static constexpr int WAKE_COMMAND_ID = 0;
static const sr_cmd_t wake_word_commands[] = {
  {WAKE_COMMAND_ID, "Hi ESP", "hi m fS Pm"},
  {WAKE_COMMAND_ID, "Hi Della", "hi DfLc"},
};

// 2026-09-17: Arduino-ESP32 3.0.0 creates these ESP-SR tasks without subscribing them, while the underlying speech model calls esp_task_wdt_reset().
#if 0
static bool register_sr_watchdog_task(const char *task_name) {
  TaskHandle_t task = xTaskGetHandle(task_name);
  if (task == nullptr) {
    log_error("ESP-SR watchdog task was not found: %s", task_name);
    return false;
  }

  const esp_err_t status = esp_task_wdt_status(task);
  if (status == ESP_OK) {
    return true;
  }
  if (status != ESP_ERR_NOT_FOUND) {
    log_error("ESP-SR watchdog status failed for %s: %s", task_name,
              esp_err_to_name(status));
    return false;
  }

  const esp_err_t result = esp_task_wdt_add(task);
  if (result != ESP_OK) {
    log_error("ESP-SR watchdog registration failed for %s: %s", task_name,
              esp_err_to_name(result));
    return false;
  }
  return true;
}
#endif

// 2026-09-11: Feed ESP-SR from the already configured I2S_NUM_0 driver so no second driver claims the microphone pins.
static esp_err_t fill_wake_word_audio(void *arg, void *out, size_t len,
                                      size_t *bytes_read, uint32_t timeout_ms) {
  (void)arg;
  // 2026-09-11: Preserve the former legacy-driver read path for reference while feeding WakeNet from the shared new-driver microphone channel.
#if 0
  TickType_t timeout_ticks = timeout_ms == portMAX_DELAY
                                 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(timeout_ms);
  return i2s_read(I2S_NUM_0, out, len, bytes_read, timeout_ticks);
#endif
  // return read_audio_bytes(out, len, bytes_read, timeout_ms);
  // 2026-09-17: Yield after each shared-I2S read so Core 0's idle task can run while ESP-SR inference occupies Core 1.
  const esp_err_t result = read_audio_bytes(out, len, bytes_read, timeout_ms);
  vTaskDelay(1);
  return result;
}

// 2026-09-11: Defer the conversation state change to loop() after WakeNet detects the phrase on mono input or verifies a stereo channel.
static void on_wake_word_event(void *arg, sr_event_t event, int command_id,
                               int phrase_id) {
  (void)arg;

  // 2026-09-11: Keep the former fixed WakeNet event path for reference.
#if 0
  // 2026-09-11: Mono INMP441 input emits SR_EVENT_WAKEWORD without requiring the channel-verification event used by stereo examples.
  if (wake_word_resume_cooldown && millis() - wake_word_resumed_at >= 1000) {
    wake_word_resume_cooldown = false;
  }
  if ((event == SR_EVENT_WAKEWORD || event == SR_EVENT_WAKEWORD_CHANNEL) &&
      !wake_word_paused && !wake_word_resume_cooldown) {
    wake_word_detected = true;
  }
#endif

  // 2026-09-11: Clear the post-ASR cooldown using wrap-safe elapsed-time arithmetic.
  if (wake_word_resume_cooldown && millis() - wake_word_resumed_at >= 1000) {
    wake_word_resume_cooldown = false;
  }

  if (event == SR_EVENT_COMMAND && command_id == WAKE_COMMAND_ID &&
      !wake_word_paused && !wake_word_resume_cooldown) {
    wake_word_phrase_id = phrase_id;
    wake_word_detected = true;
    return;
  }

  // 2026-09-11: A rejected command leaves MultiNet off, so reopen listening after residual audio during cooldown.
  if (event == SR_EVENT_COMMAND && !wake_word_paused) {
    esp_err_t result = sr_set_mode(SR_MODE_COMMAND);
    if (result != ESP_OK) {
      log_error("Wake phrase command recovery failed: %s", esp_err_to_name(result));
    }
    return;
  }

  // 2026-09-11: MultiNet stops after its command window expires, so immediately open a fresh listening window.
  if (event == SR_EVENT_TIMEOUT && !wake_word_paused) {
    esp_err_t result = sr_set_mode(SR_MODE_COMMAND);
    if (result != ESP_OK) {
      log_error("Wake phrase timeout recovery failed: %s", esp_err_to_name(result));
    }
  }
}

// 2026-09-11: Start the bundled English MultiNet model in mono mode for both supported trigger phrases.
bool setup_wake_word() {
  // 2026-09-11: Skip ESP-SR tasks and model loading when wake-word support is disabled.
#if !DESK_EMOJI_ENABLE_WAKE_WORD
  log_info("Wake word disabled; skipping initialization.");
  return false;
#endif
  // 2026-09-11: Fail open when microphone initialization failed instead of starting a feed task with no data source.
  if (!microphone_is_ready()) {
    log_error("Wake phrases unavailable because the microphone is not ready");
    return false;
  }
  // 2026-09-11: Keep the former fixed "Hi ESP" startup call for reference.
#if 0
  esp_err_t result = sr_start(fill_wake_word_audio, nullptr, SR_CHANNELS_MONO,
                              SR_MODE_WAKEWORD, nullptr, 0,
                              on_wake_word_event, nullptr);
#endif
  esp_err_t result = sr_start(
      fill_wake_word_audio, nullptr, SR_CHANNELS_MONO, SR_MODE_COMMAND,
      wake_word_commands,
      sizeof(wake_word_commands) / sizeof(wake_word_commands[0]),
      on_wake_word_event, nullptr);
  if (result != ESP_OK) {
    log_error("Wake phrase initialization failed: %s", esp_err_to_name(result));
    return false;
  }

  // 2026-09-17: Stop ESP-IDF from emitting "task not found" for every watchdog reset performed by the ESP-SR model.
  // 2026-09-17: Keep the former registration disabled because AFE inference and sr_pause() can legitimately prevent this task from feeding within five seconds.
  // const bool feed_watchdog_ready =
  //     register_sr_watchdog_task("SR Feed Task");
  // 2026-09-17: Do not register SR Detect Task; its neural-network inference can run longer than the 5-second watchdog window.
  // const bool detect_watchdog_ready =
  //     register_sr_watchdog_task("SR Detect Task");
  // if (!feed_watchdog_ready) {
  //   log_warn("ESP-SR started without complete watchdog registration");
  // }

  wake_word_ready = true;
  log_info("Wake phrases are Ready. Say: Hi ESP or Hi Della");
  return true;
}

// 2026-09-11: Open the existing chat flow only from the normal Arduino loop context.
void handle_wake_word() {
  if (!wake_word_detected) {
    return;
  }

  wake_word_detected = false;
  int phrase_id = wake_word_phrase_id;
  wake_word_phrase_id = -1;
  if (phrase_id >= 0 &&
      phrase_id < static_cast<int>(sizeof(wake_word_commands) /
                                   sizeof(wake_word_commands[0]))) {
    log_info("Wake phrase detected: %s", wake_word_commands[phrase_id].str);
  } else {
    log_info("Wake phrase detected");
  }
  start_chat = true;
}

// 2026-09-11: Stop MultiNet from consuming microphone samples while cloud ASR is recording.
void pause_wake_word() {
  if (!wake_word_ready || wake_word_paused) {
    return;
  }

  // 2026-09-11: Block callbacks before pausing the worker so an in-flight event cannot reopen chat later.
  wake_word_paused = true;
  wake_word_detected = false;
  wake_word_phrase_id = -1;
  esp_err_t result = sr_pause();
  if (result != ESP_OK) {
    wake_word_paused = false;
    log_error("Wake phrase pause failed: %s", esp_err_to_name(result));
    return;
  }

  delay(50);
  wake_word_detected = false;
  wake_word_phrase_id = -1;
}

// 2026-09-11: Re-enable the trigger-phrase model after cloud conversation recording releases the microphone.
void resume_wake_word() {
  if (!wake_word_ready || !wake_word_paused) {
    return;
  }

  // 2026-09-11: Clear the previous turn and ignore one second of residual microphone/model state.
  wake_word_detected = false;
  wake_word_phrase_id = -1;
  wake_word_resumed_at = millis();
  wake_word_resume_cooldown = true;
  // esp_err_t mode_result = sr_set_mode(SR_MODE_WAKEWORD);
  // 2026-09-11: Resume the custom MultiNet trigger instead of the fixed "Hi ESP" WakeNet model.
  esp_err_t mode_result = sr_set_mode(SR_MODE_COMMAND);
  esp_err_t resume_result = sr_resume();
  if (mode_result != ESP_OK || resume_result != ESP_OK) {
    log_error("Wake phrase resume failed: mode=%s, resume=%s",
              esp_err_to_name(mode_result), esp_err_to_name(resume_result));
    return;
  }

  wake_word_paused = false;
  log_info("Wake phrase listening resumed. Say: Hi ESP or Hi Della");
}
