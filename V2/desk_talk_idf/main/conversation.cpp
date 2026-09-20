// 2026-09-19: Run the migrated voice loop and preserve a serialized fallback when simultaneous LLM/TTS TLS allocation fails.
#include "conversation.h"

#include <string>

#include "asr_client.h"
#include "device_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "latency_trace.h"
#include "llm_client.h"
#include "memory_diagnostics.h"
#include "tts_client.h"
#include "wake_word.h"
#include "wifi_manager.h"

namespace desk_talk::conversation {
namespace {

constexpr char kLogTag[] = "conversation";

// 2026-09-19: Keep V1-style idle motion irregular while ensuring it never runs during an active voice turn.
int64_t next_idle_action_time_us() {
  constexpr int64_t kMinimumDelayUs = 6000000LL;
  constexpr uint32_t kDelayRangeUs = 4000001U;
  return esp_timer_get_time() + kMinimumDelayUs +
         static_cast<int64_t>(esp_random() % kDelayRangeUs);
}

bool enqueue_sentence(const std::string &sentence, void *context) {
  if (context == nullptr) return false;
  return static_cast<cloud::TtsClient *>(context)->enqueue_sentence(sentence);
}

// 2026-09-20: End the cloud session before restoring ESP-SR so the mutually exclusive internal-memory owners never overlap while idle.
void restore_wake_word(cloud::TtsClient &tts) {
  // 2026-09-20: Clear the voice status light when leaving a cloud session and returning to local wake-word detection.
  ui::set_status_color(0, 0);
  tts.release_persistent_connection();
  const esp_err_t result = wake_word::initialize();
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Unable to restore wake word: %s",
             esp_err_to_name(result));
  }
}

void conversation_task(void *) {
  cloud::AsrClient asr;
  cloud::LlmClient llm;
  cloud::TtsClient &tts = cloud::tts_client();
  tts.initialize();
  bool session_active = false;
  int64_t session_deadline_us = 0;
  int64_t last_broadcast_us = 0;
  int64_t next_idle_action_us = next_idle_action_time_us();

  while (true) {
    network::wifi_manager().maintain();
    const int64_t now = esp_timer_get_time();
    if (now - last_broadcast_us >= 1000000) {
      network::wifi_manager().send_ip_broadcast();
      last_broadcast_us = now;
    }
    if (!network::wifi_manager().connected()) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (session_active && now >= session_deadline_us) {
      session_active = false;
#if 0
      // 2026-09-20: Retain the former resume-only transition; ESP-SR is now fully released during a cloud session.
      wake_word::resume();
#endif
      // 2026-09-20: Close the session-scoped TTS socket and reconstruct WakeNet when the follow-up window expires.
      restore_wake_word(tts);
      next_idle_action_us = next_idle_action_time_us();
    }
    if (!session_active && wake_word::ready() &&
        !wake_word::wait_for_detection(250)) {
      // 2026-09-19: Run idle animation only between conversations; ESP-SR continues feeding in its own task while this short action plays.
      if (esp_timer_get_time() >= next_idle_action_us) {
        ui::play_random_idle_action();
        next_idle_action_us = next_idle_action_time_us();
      }
      continue;
    }
    if (!session_active && !wake_word::ready()) {
      // 2026-09-19: Without an ESP-SR model, remain operational by starting one turn after boot rather than blocking all cloud diagnostics.
      static bool fallback_turn_started = false;
      if (fallback_turn_started) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      fallback_turn_started = true;
    }

    // 2026-09-20: Give every accepted first/follow-up turn one trace, including cloud preparation and any serialized fallback attempts.
    diagnostics::latency_trace_begin(session_active);
    if (!session_active) {
      // 2026-09-20: Acknowledge wake detection immediately with dim red while ESP-SR is released and cloud connections are prepared.
      ui::set_status_color(0xFF0000, 10);
#if 0
      // 2026-09-20: Retain the former pause-only transition; pausing leaves AFE and model allocations resident.
      wake_word::pause();
#endif
      // 2026-09-20: Release ESP-SR before opening the session-scoped persistent TTS connection used by ASR/LLM follow-up turns.
      wake_word::shutdown();
      if (!tts.warmup_persistent_connection()) {
        ESP_LOGW(kLogTag,
                 "Session TTS warmup failed; synthesis will retry after ASR");
      }
      session_active = true;
      session_deadline_us = esp_timer_get_time() + 600000000LL;
    }
#if 0
    // 2026-09-19: Retain the earlier trace reset for reference; ASR now begins it at the exact recording boundary.
    diagnostics::latency_trace_begin();
#endif
    ui::set_face(ui::FaceState::kListening);
    bool recognized = asr.recognize();
    if (!recognized && asr.result().empty() &&
        tts.persistent_connection_ready()) {
      // 2026-09-19: Retry only a no-result ASR failure after releasing TTS, so no recognized speech can be duplicated.
      ESP_LOGW(kLogTag,
               "ASR failed beside persistent TTS; retrying serialized");
      // 2026-09-20: Keep this failed attempt visible in the same turn instead of letting ASR reset the trace.
      diagnostics::latency_trace_note_retry(diagnostics::LatencyRetry::kAsr);
      tts.release_persistent_connection();
      asr.disconnect();
      recognized = asr.recognize();
    }
    if (!recognized || asr.result().empty()) {
      ESP_LOGW(kLogTag, "No speech recognized; returning to wake word");
      asr.disconnect();
      // 2026-09-20: Preserve the unlabelled dump; failed/no-speech turns now carry an explicit outcome for baseline filtering.
      // diagnostics::latency_trace_dump();
      diagnostics::latency_trace_dump("asr_no_result");
      // 2026-09-20: Restore the original three red flashes for an empty/failed recognition before returning to idle.
      ui::blink_status(0xFF0000, 3);
      ui::set_face(ui::FaceState::kNeutral);
      session_active = false;
#if 0
      // 2026-09-20: Retain the former resume-only recovery; the released ESP-SR instance must now be recreated.
      wake_word::resume();
#endif
      // 2026-09-20: A silent or failed ASR turn ends the session, closes TTS, and restores local wake-word detection.
      restore_wake_word(tts);
      next_idle_action_us = next_idle_action_time_us();
      continue;
    }

    const std::string question = asr.result();
    asr.disconnect();
    // 2026-09-20: Show yellow while DeepSeek prepares its answer; the playback task switches to green when PCM starts.
    ui::set_status_color(0xFFFF00, 10);
    ui::set_face(ui::FaceState::kThinking);
    bool stream_ready = tts.begin_turn();
    cloud::LlmResult answer = llm.chat(
        question, stream_ready ? enqueue_sentence : nullptr,
        stream_ready ? &tts : nullptr);

    // 2026-09-19: If dual TLS cannot coexist, release the optional persistent socket and retry once in serialized mode.
    if (!answer.success && answer.transport_failure &&
        tts.persistent_connection_ready()) {
      ESP_LOGW(kLogTag, "LLM TLS failed beside persistent TTS; retrying serialized");
      diagnostics::log_memory_snapshot("dual-TLS fallback");
      // 2026-09-19: Fully drain the current turn before reuse; a short timeout can leave stale queue markers that complete the next turn incorrectly.
      bool partial_audio_played = false;
      if (stream_ready) {
        tts.finish_turn();
        partial_audio_played = tts.last_turn_had_audio();
        stream_ready = false;
      }
      tts.release_persistent_connection();
      if (!partial_audio_played) {
        // 2026-09-20: Count only a retry that will actually issue another LLM request, not an abandoned partial-audio fallback.
        diagnostics::latency_trace_note_retry(diagnostics::LatencyRetry::kLlm);
        // 2026-09-20: Restore thinking yellow after draining a failed TTS turn before retrying the LLM request.
        ui::set_status_color(0xFFFF00, 10);
        stream_ready = tts.begin_turn();
        answer = llm.chat(question, stream_ready ? enqueue_sentence : nullptr,
                          stream_ready ? &tts : nullptr);
      } else {
        ESP_LOGW(kLogTag,
                 "LLM failed after partial speech; skipping replay to avoid duplicate audio");
      }
    }

    // 2026-09-20: Retry a clean blank LLM reply once per turn, after any TLS fallback; no sentence was submitted, so the existing empty TTS turn is reusable.
    if (!answer.success && answer.empty_reply) {
      ESP_LOGW(kLogTag, "LLM returned blank reply; retrying once before TTS");
      diagnostics::latency_trace_note_retry(diagnostics::LatencyRetry::kLlm);
      answer = llm.chat(question, stream_ready ? enqueue_sentence : nullptr,
                        stream_ready ? &tts : nullptr);
    }

    if (answer.success && !stream_ready) {
      stream_ready = tts.begin_turn();
      if (stream_ready) tts.enqueue_sentence(answer.answer);
    }
    bool spoken = stream_ready && tts.finish_turn();
    if (answer.success && !spoken && !tts.last_turn_had_audio()) {
      // 2026-09-19: Retry a zero-audio concurrent failure only after DeepSeek TLS has closed; partial speech is never replayed.
      ESP_LOGW(kLogTag,
               "Concurrent TTS produced no PCM; retrying serialized");
      // 2026-09-20: Mark the second synthesis attempt so first-attempt milestones are not misread as normal stage timings.
      diagnostics::latency_trace_note_retry(diagnostics::LatencyRetry::kTts);
      tts.release_persistent_connection();
      if (tts.begin_turn() && tts.enqueue_sentence(answer.answer)) {
        spoken = tts.finish_turn();
      }
    }
    if (answer.success) llm.commit_history(question, answer.answer);
    if (answer.success && !answer.actions.empty()) {
      size_t offset = 0;
      while (offset < answer.actions.size()) {
        const size_t comma = answer.actions.find(',', offset);
        ui::execute_action(answer.actions.substr(
            offset, comma == std::string::npos ? std::string::npos
                                                : comma - offset));
        if (comma == std::string::npos) break;
        offset = comma + 1;
      }
    }
    if (!answer.success || !spoken) {
      ESP_LOGE(kLogTag, "Conversation failed: llm=%u tts=%u",
               answer.success ? 1U : 0U, spoken ? 1U : 0U);
      ui::set_face(ui::FaceState::kError);
      // 2026-09-20: Make cloud conversation failures visible on the board LED after playback has finished.
      ui::blink_status(0xFF0000, 3);
    } else {
      ui::set_face(ui::FaceState::kHappy);
    }
    // 2026-09-20: Preserve the unlabelled dump; report cloud/playback outcome alongside per-turn timing and retry counts.
    // diagnostics::latency_trace_dump();
    diagnostics::latency_trace_dump(
        !answer.success ? "llm_failed" : (!spoken ? "tts_failed" : "ok"));
    // 2026-09-20: Clear the completed turn's color; the next ASR turn will set listening red explicitly.
    ui::set_status_color(0, 0);
    ui::set_face(ui::FaceState::kNeutral);
#if 0
    // 2026-09-19: Keep WakeNet paused during V1-compatible follow-up turns; empty ASR or the ten-minute deadline resumes it.
    wake_word::resume();
#endif
  }
}

}  // namespace

esp_err_t start() {
#if 0
  // 2026-09-20: Retain the former internal-stack conversation task creation for migration reference.
  return xTaskCreate(conversation_task, "conversation", 10240, nullptr, 3,
                     nullptr) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
#endif
  // 2026-09-20: Place the long-lived conversation stack in PSRAM to preserve internal RAM for Wi-Fi and simultaneous TLS sessions.
  return xTaskCreateWithCaps(
             conversation_task, "conversation", 10240, nullptr, 3, nullptr,
             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

}  // namespace desk_talk::conversation
