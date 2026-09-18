#include "latency_trace.h"

#include <esp_timer.h>

namespace {

// 2026-09-17: Keep the trace bounded so timestamp collection never allocates memory or writes to Serial on the critical path.
// constexpr size_t kLatencyEventCount = 6;
// 2026-09-18: Include detailed LLM completion and TTS connection/request milestones.
constexpr size_t kLatencyEventCount = 9;

struct LatencyEntry {
  LatencyEvent event;
  uint64_t timestamp_us;
};

portMUX_TYPE trace_mux = portMUX_INITIALIZER_UNLOCKED;
LatencyEntry trace_entries[kLatencyEventCount];
size_t trace_entry_count = 0;
uint32_t trace_session = 0;
bool trace_active = false;

const char *latencyEventName(LatencyEvent event) {
  switch (event) {
    case LatencyEvent::VAD_START:
      return "VAD_START";
    case LatencyEvent::AUDIO_UPLOAD_DONE:
      return "AUDIO_UPLOAD_DONE";
    case LatencyEvent::ASR_FINAL:
      return "ASR_FINAL";
    case LatencyEvent::LLM_FIRST_BYTE:
      return "LLM_FIRST_BYTE";
    case LatencyEvent::LLM_DONE:
      return "LLM_DONE";
    case LatencyEvent::TTS_CONNECTED:
      return "TTS_CONNECTED";
    case LatencyEvent::TTS_REQUEST_SENT:
      return "TTS_REQUEST_SENT";
    case LatencyEvent::TTS_FIRST_AUDIO:
      return "TTS_FIRST_AUDIO";
    case LatencyEvent::PLAYBACK_START:
      return "PLAYBACK_START";
  }
  return "UNKNOWN";
}

}  // namespace

void latency_trace_begin() {
  portENTER_CRITICAL(&trace_mux);
  ++trace_session;
  trace_entry_count = 0;
  trace_active = true;
  portEXIT_CRITICAL(&trace_mux);
}

void latency_trace_mark(LatencyEvent event) {
  const uint64_t timestamp_us = esp_timer_get_time();

  portENTER_CRITICAL(&trace_mux);
  if (!trace_active || trace_entry_count >= kLatencyEventCount) {
    portEXIT_CRITICAL(&trace_mux);
    return;
  }

  // 2026-09-17: Ignore repeated callbacks and audio chunks so each baseline event appears at most once per session.
  for (size_t i = 0; i < trace_entry_count; ++i) {
    if (trace_entries[i].event == event) {
      portEXIT_CRITICAL(&trace_mux);
      return;
    }
  }

  trace_entries[trace_entry_count++] = {event, timestamp_us};
  portEXIT_CRITICAL(&trace_mux);
}

void latency_trace_dump() {
  LatencyEntry entries[kLatencyEventCount];
  size_t entry_count = 0;
  uint32_t session = 0;

  portENTER_CRITICAL(&trace_mux);
  entry_count = trace_entry_count;
  session = trace_session;
  for (size_t i = 0; i < entry_count; ++i) {
    entries[i] = trace_entries[i];
  }
  trace_active = false;
  portEXIT_CRITICAL(&trace_mux);

  if (entry_count == 0) {
    Serial.printf("LAT,session=%lu,event=NO_EVENTS\n",
                  static_cast<unsigned long>(session));
    return;
  }

  // 2026-09-17: Emit raw microseconds plus ready-to-read stage and total milliseconds after measurement is complete.
  const uint64_t baseline_us = entries[0].timestamp_us;
  uint64_t previous_us = baseline_us;
  for (size_t i = 0; i < entry_count; ++i) {
    const uint64_t step_us = entries[i].timestamp_us - previous_us;
    const uint64_t total_us = entries[i].timestamp_us - baseline_us;
    Serial.printf(
        "LAT,session=%lu,event=%s,time_us=%llu,step_ms=%llu.%03llu,total_ms=%llu.%03llu\n",
        static_cast<unsigned long>(session), latencyEventName(entries[i].event),
        static_cast<unsigned long long>(entries[i].timestamp_us),
        static_cast<unsigned long long>(step_us / 1000ULL),
        static_cast<unsigned long long>(step_us % 1000ULL),
        static_cast<unsigned long long>(total_us / 1000ULL),
        static_cast<unsigned long long>(total_us % 1000ULL));
    previous_us = entries[i].timestamp_us;
  }
}
