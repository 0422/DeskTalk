#ifndef GatewayClient_h
#define GatewayClient_h

// 2026-09-18: Keep the experimental cloud pipeline opt-in so the existing direct ASR/LLM/TTS path remains usable.
#include <Arduino.h>
#include <WebSocketsClient.h>

#include "llm.h"
#include "tts.h"

#ifndef GATEWAY_HOST
#define GATEWAY_HOST ""
#endif
#ifndef GATEWAY_PORT
#define GATEWAY_PORT 8765
#endif
#ifndef GATEWAY_PATH
#define GATEWAY_PATH "/chat"
#endif
#ifndef GATEWAY_TOKEN
#define GATEWAY_TOKEN ""
#endif
#ifndef GATEWAY_TLS
#define GATEWAY_TLS false
#endif

enum class GatewayResult : uint8_t {
  SUCCESS,
  FAILED_BEFORE_AUDIO,
  FAILED_AFTER_AUDIO,
};

class GatewayClient {
 public:
  bool enabled() const;
  GatewayResult chat(const String &question, LLM &llm, TtsClient &tts);

 private:
  void onEvent(WStype_t type, uint8_t *payload, size_t length);

  WebSocketsClient socket_;
  LLM *llm_ = nullptr;
  TtsClient *tts_ = nullptr;
  String question_;
  String request_;
  bool connected_ = false;
  bool audio_started_ = false;
  bool reply_received_ = false;
  bool remote_done_ = false;
  bool failed_ = false;
};

#endif
