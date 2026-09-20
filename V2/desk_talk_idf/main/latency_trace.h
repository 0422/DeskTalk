// 2026-09-19: Preserve the V1 conversation milestones without logging on latency-critical callback paths.
#pragma once

#include <cstdint>

namespace desk_talk::diagnostics {

enum class LatencyEvent : uint8_t {
  kVadStart,
  kVadEnd,
  kAudioUploadDone,
  kAsrFinal,
  kLlmFirstByte,
  kLlmDone,
  kTtsConnected,
  kTtsRequestSent,
  kTtsFirstAudio,
  kPlaybackStart,
  // 2026-09-20: Separate the speech-end estimate, LLM preparation, first sentence and successful I2S submission for per-turn latency attribution.
  kTurnStart,
  kLastVoiceFrame,
  kLlmRequestStart,
  kFirstSentenceReady,
  kI2sWriteDone,
  kCount,
};

// 2026-09-20: Preserve the original declarations while adding turn context and an explicit result to the summaries.
// void latency_trace_begin();
void latency_trace_begin(bool followup_turn = false);
void latency_trace_mark(LatencyEvent event);
// void latency_trace_dump();
void latency_trace_dump(const char *outcome = "unknown");

// 2026-09-20: Retain timestamps captured before cross-core queue handoffs, and label retries instead of treating mixed attempts as normal stage timings.
void latency_trace_mark_at(LatencyEvent event, uint64_t timestamp_us);
enum class LatencyRetry : uint8_t { kAsr, kLlm, kTts };
void latency_trace_note_retry(LatencyRetry stage);

}  // namespace desk_talk::diagnostics
