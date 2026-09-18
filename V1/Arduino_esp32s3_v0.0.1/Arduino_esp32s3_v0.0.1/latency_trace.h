#ifndef LatencyTrace_h
#define LatencyTrace_h

#include <Arduino.h>

// 2026-09-17: Define the six serial-only baseline events used to compare conversation latency without a logic analyzer.
enum class LatencyEvent : uint8_t {
  VAD_START,
  AUDIO_UPLOAD_DONE,
  ASR_FINAL,
  LLM_FIRST_BYTE,
  // 2026-09-18: Separate remaining LLM generation, TTS connection, and request-to-audio time in serial baselines.
  LLM_DONE,
  TTS_CONNECTED,
  TTS_REQUEST_SENT,
  TTS_FIRST_AUDIO,
  PLAYBACK_START,
};

// 2026-09-17: Buffer timestamps during a conversation and print them only after the measured path has finished.
void latency_trace_begin();
void latency_trace_mark(LatencyEvent event);
void latency_trace_dump();

#endif
