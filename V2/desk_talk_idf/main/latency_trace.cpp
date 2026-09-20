// 2026-09-19: Port bounded V1 latency tracing to ESP_LOG and native FreeRTOS critical sections.
#include "latency_trace.h"

#include <cstddef>
#include <cstdint>
// 2026-09-20: Sort cross-core timestamps and format missing/invalid intervals without dynamic allocation.
#include <algorithm>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace desk_talk::diagnostics {
namespace {

constexpr char kLogTag[] = "latency";
// 2026-09-20: Retain the former fixed capacity; derive the new capacity from the enum so added milestones cannot silently disappear.
// constexpr size_t kEventCount = 10;
constexpr size_t kEventCount = static_cast<size_t>(LatencyEvent::kCount);

struct LatencyEntry {
  LatencyEvent event;
  uint64_t timestamp_us;
};

portMUX_TYPE trace_mutex = portMUX_INITIALIZER_UNLOCKED;
LatencyEntry entries[kEventCount] = {};
size_t entry_count = 0;
uint32_t session = 0;
bool active = false;
// 2026-09-20: Keep first/follow-up context and fallback counts with the trace until the entire conversation turn finishes.
bool followup = false;
uint32_t asr_retries = 0;
uint32_t llm_retries = 0;
uint32_t tts_retries = 0;

const char *event_name(LatencyEvent event) {
  switch (event) {
    case LatencyEvent::kVadStart:
      return "VAD_START";
    case LatencyEvent::kVadEnd:
      return "VAD_END";
    case LatencyEvent::kAudioUploadDone:
      return "AUDIO_UPLOAD_DONE";
    case LatencyEvent::kAsrFinal:
      return "ASR_FINAL";
    case LatencyEvent::kLlmFirstByte:
      return "LLM_FIRST_BYTE";
    case LatencyEvent::kLlmDone:
      return "LLM_DONE";
    case LatencyEvent::kTtsConnected:
      return "TTS_CONNECTED";
    case LatencyEvent::kTtsRequestSent:
      return "TTS_REQUEST_SENT";
    case LatencyEvent::kTtsFirstAudio:
      return "TTS_FIRST_AUDIO";
    case LatencyEvent::kPlaybackStart:
      return "PLAYBACK_START";
    // 2026-09-20: Name the new milestones explicitly; LAST_VOICE_FRAME is a block-read estimate, not an acoustic timestamp.
    case LatencyEvent::kTurnStart:
      return "TURN_START";
    case LatencyEvent::kLastVoiceFrame:
      return "LAST_VOICE_FRAME";
    case LatencyEvent::kLlmRequestStart:
      return "LLM_REQUEST_START";
    case LatencyEvent::kFirstSentenceReady:
      return "FIRST_SENTENCE_READY";
    case LatencyEvent::kI2sWriteDone:
      return "I2S_WRITE_DONE";
    case LatencyEvent::kCount:
      break;
  }
  return "UNKNOWN";
}

// 2026-09-20: Display NA for missing or reversed milestones instead of fabricating zero latency or unsigned underflow.
void format_interval(const LatencyEntry *snapshot, size_t count,
                     LatencyEvent start, LatencyEvent end, bool valid,
                     char (&text)[24]) {
  uint64_t start_us = 0;
  uint64_t end_us = 0;
  bool found_start = false;
  bool found_end = false;
  for (size_t index = 0; index < count; ++index) {
    if (snapshot[index].event == start) {
      start_us = snapshot[index].timestamp_us;
      found_start = true;
    }
    if (snapshot[index].event == end) {
      end_us = snapshot[index].timestamp_us;
      found_end = true;
    }
  }
  if (!valid || !found_start || !found_end || end_us < start_us) {
    std::snprintf(text, sizeof(text), "NA");
    return;
  }
  const uint64_t elapsed_us = end_us - start_us;
  std::snprintf(text, sizeof(text), "%llu.%03llu",
                static_cast<unsigned long long>(elapsed_us / 1000ULL),
                static_cast<unsigned long long>(elapsed_us % 1000ULL));
}

// 2026-09-20: These consecutive intervals explain the software path from the last loud block to the first successful I2S write.
struct StageInterval {
  const char *name;
  LatencyEvent start;
  LatencyEvent end;
};

constexpr StageInterval kStages[] = {
    {"endpoint_wait", LatencyEvent::kLastVoiceFrame, LatencyEvent::kVadEnd},
    {"asr", LatencyEvent::kVadEnd, LatencyEvent::kAsrFinal},
    {"handoff", LatencyEvent::kAsrFinal, LatencyEvent::kLlmRequestStart},
    {"llm_first_content", LatencyEvent::kLlmRequestStart,
     LatencyEvent::kLlmFirstByte},
    {"first_sentence", LatencyEvent::kLlmFirstByte,
     LatencyEvent::kFirstSentenceReady},
    {"tts_prepare", LatencyEvent::kFirstSentenceReady,
     LatencyEvent::kTtsRequestSent},
    {"tts_first_audio", LatencyEvent::kTtsRequestSent,
     LatencyEvent::kTtsFirstAudio},
    {"playback_queue", LatencyEvent::kTtsFirstAudio,
     LatencyEvent::kPlaybackStart},
    {"i2s_write", LatencyEvent::kPlaybackStart, LatencyEvent::kI2sWriteDone},
};

}  // namespace

// 2026-09-20: Preserve the original signature; callers now distinguish the first turn from an active-session follow-up.
// void latency_trace_begin() {
void latency_trace_begin(bool followup_turn) {
  portENTER_CRITICAL(&trace_mutex);
  ++session;
  entry_count = 0;
  active = true;
  // 2026-09-20: Reset once per conversation turn so ASR/LLM/TTS retries retain the same session number.
  followup = followup_turn;
  asr_retries = 0;
  llm_retries = 0;
  tts_retries = 0;
  entries[entry_count++] = {
      LatencyEvent::kTurnStart, static_cast<uint64_t>(esp_timer_get_time())};
  portEXIT_CRITICAL(&trace_mutex);
}

#if 0
// 2026-09-20: Retain the insertion-only implementation; the last voice block needs updating and cross-core handoffs need captured timestamps.
void latency_trace_mark(LatencyEvent event) {
  const uint64_t timestamp = esp_timer_get_time();
  portENTER_CRITICAL(&trace_mutex);
  if (!active || entry_count >= kEventCount) {
    portEXIT_CRITICAL(&trace_mutex);
    return;
  }
  for (size_t index = 0; index < entry_count; ++index) {
    if (entries[index].event == event) {
      portEXIT_CRITICAL(&trace_mutex);
      return;
    }
  }
  entries[entry_count++] = {event, timestamp};
  portEXIT_CRITICAL(&trace_mutex);
}
#endif

// 2026-09-20: Keep hot paths limited to timestamp recording; formatting and serial output remain at the end of the turn.
void latency_trace_mark(LatencyEvent event) {
  latency_trace_mark_at(event, static_cast<uint64_t>(esp_timer_get_time()));
}

// 2026-09-20: Keep the latest loud block but the earliest occurrence of every other milestone, including delayed cross-core recording.
void latency_trace_mark_at(LatencyEvent event, uint64_t timestamp_us) {
  if (static_cast<size_t>(event) >= kEventCount) return;
  portENTER_CRITICAL(&trace_mutex);
  if (!active) {
    portEXIT_CRITICAL(&trace_mutex);
    return;
  }
  for (size_t index = 0; index < entry_count; ++index) {
    if (entries[index].event == event) {
      entries[index].timestamp_us = event == LatencyEvent::kLastVoiceFrame
          ? std::max(entries[index].timestamp_us, timestamp_us)
          : std::min(entries[index].timestamp_us, timestamp_us);
      portEXIT_CRITICAL(&trace_mutex);
      return;
    }
  }
  if (entry_count < kEventCount) entries[entry_count++] = {event, timestamp_us};
  portEXIT_CRITICAL(&trace_mutex);
}

// 2026-09-20: Label serialized fallbacks so timings from separate attempts cannot be mistaken for a single successful pipeline.
void latency_trace_note_retry(LatencyRetry stage) {
  portENTER_CRITICAL(&trace_mutex);
  if (active) {
    switch (stage) {
      case LatencyRetry::kAsr: ++asr_retries; break;
      case LatencyRetry::kLlm: ++llm_retries; break;
      case LatencyRetry::kTts: ++tts_retries; break;
    }
  }
  portEXIT_CRITICAL(&trace_mutex);
}

// 2026-09-20: Preserve the original signature; report whether each completed trace produced a successful conversation.
// void latency_trace_dump() {
void latency_trace_dump(const char *outcome) {
  LatencyEntry snapshot[kEventCount] = {};
  size_t snapshot_count = 0;
  uint32_t snapshot_session = 0;
  // 2026-09-20: Copy context with timestamps under the same lock before doing any logging.
  bool snapshot_followup = false;
  uint32_t snapshot_asr_retries = 0;
  uint32_t snapshot_llm_retries = 0;
  uint32_t snapshot_tts_retries = 0;
  portENTER_CRITICAL(&trace_mutex);
  snapshot_count = entry_count;
  snapshot_session = session;
  // 2026-09-20: Preserve retry metadata even when the successful attempt follows an earlier allocation/network failure.
  snapshot_followup = followup;
  snapshot_asr_retries = asr_retries;
  snapshot_llm_retries = llm_retries;
  snapshot_tts_retries = tts_retries;
  for (size_t index = 0; index < snapshot_count; ++index) {
    snapshot[index] = entries[index];
  }
  active = false;
  portEXIT_CRITICAL(&trace_mutex);

  // 2026-09-20: Capture time may precede insertion time across cores; sort before calculating unsigned adjacent-event differences.
  std::sort(snapshot, snapshot + snapshot_count,
            [](const LatencyEntry &left, const LatencyEntry &right) {
              return left.timestamp_us < right.timestamp_us;
            });
  if (snapshot_count == 0) {
    ESP_LOGI(kLogTag, "LAT,session=%u,event=NO_EVENTS",
             static_cast<unsigned>(snapshot_session));
    return;
  }
  const uint64_t baseline = snapshot[0].timestamp_us;
  uint64_t previous = baseline;
  for (size_t index = 0; index < snapshot_count; ++index) {
    const uint64_t step = snapshot[index].timestamp_us - previous;
    const uint64_t total = snapshot[index].timestamp_us - baseline;
    ESP_LOGI(kLogTag,
             "LAT,session=%u,event=%s,time_us=%llu,step_ms=%llu.%03llu,"
             "total_ms=%llu.%03llu",
             static_cast<unsigned>(snapshot_session),
             event_name(snapshot[index].event),
             static_cast<unsigned long long>(snapshot[index].timestamp_us),
             static_cast<unsigned long long>(step / 1000ULL),
             static_cast<unsigned long long>(step % 1000ULL),
             static_cast<unsigned long long>(total / 1000ULL),
             static_cast<unsigned long long>(total % 1000ULL));
    previous = snapshot[index].timestamp_us;
  }

  // 2026-09-20: Summarize the estimate explicitly as block-read to I2S-write timing; actual acoustic onset still requires an external recording.
  char speech_to_i2s[24];
  char endpoint_to_i2s[24];
  char turn_to_i2s[24];
  const bool single_attempt = snapshot_asr_retries == 0 &&
                              snapshot_llm_retries == 0 &&
                              snapshot_tts_retries == 0;
  format_interval(snapshot, snapshot_count, LatencyEvent::kLastVoiceFrame,
                  LatencyEvent::kI2sWriteDone, snapshot_asr_retries == 0,
                  speech_to_i2s);
  format_interval(snapshot, snapshot_count, LatencyEvent::kVadEnd,
                  LatencyEvent::kI2sWriteDone, snapshot_asr_retries == 0,
                  endpoint_to_i2s);
  format_interval(snapshot, snapshot_count, LatencyEvent::kTurnStart,
                  LatencyEvent::kI2sWriteDone, true, turn_to_i2s);
  // 2026-09-20: A turn with missing/reversed milestones is not a complete stage baseline, even when it did not retry.
  bool stages_valid = single_attempt;
  for (const StageInterval &stage : kStages) {
    char elapsed[24];
    format_interval(snapshot, snapshot_count, stage.start, stage.end,
                    single_attempt, elapsed);
    if (elapsed[0] == 'N') stages_valid = false;
  }
  ESP_LOGI(kLogTag,
           "LAT_SUMMARY,session=%u,turn=%s,outcome=%s,"
           "speech_to_i2s_est_ms=%s,endpoint_to_i2s_ms=%s,turn_to_i2s_ms=%s,"
           "asr_retries=%u,llm_retries=%u,tts_retries=%u,stages_valid=%u",
           static_cast<unsigned>(snapshot_session),
           snapshot_followup ? "followup" : "first",
           outcome != nullptr ? outcome : "unknown", speech_to_i2s,
           endpoint_to_i2s, turn_to_i2s,
           static_cast<unsigned>(snapshot_asr_retries),
           static_cast<unsigned>(snapshot_llm_retries),
           // 2026-09-20: Include completeness in the validity flag rather than checking retry counts alone.
           // static_cast<unsigned>(snapshot_tts_retries), single_attempt ? 1U : 0U);
           static_cast<unsigned>(snapshot_tts_retries), stages_valid ? 1U : 0U);
  // 2026-09-20: Emit one named interval per stage; retries invalidate stage attribution while the raw milestones remain available for diagnosis.
  for (const StageInterval &stage : kStages) {
    char elapsed[24];
    format_interval(snapshot, snapshot_count, stage.start, stage.end,
                    single_attempt, elapsed);
    ESP_LOGI(kLogTag, "LAT_STAGE,session=%u,stage=%s,ms=%s",
             static_cast<unsigned>(snapshot_session), stage.name, elapsed);
  }
}

}  // namespace desk_talk::diagnostics
