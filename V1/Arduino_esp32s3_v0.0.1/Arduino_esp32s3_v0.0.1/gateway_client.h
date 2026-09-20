#if 0
// 2026-09-18: Keep the gateway API out of the board-direct firmware build;
// the complete historical implementation remains below for reference.
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
  // 2026-09-18: Start and optionally warm the LAN WebSocket before the robot
  // accepts speech, then pump it from the ordinary firmware loop.
  void begin();
  bool warmup(unsigned long timeout_ms = 3000UL);
  void loop();
  GatewayResult chat(const String &question, LLM &llm, TtsClient &tts);

 private:
  // 2026-09-18: Open the physical socket once; WebSocketsClient handles idle
  // reconnects without rebuilding the endpoint for every conversation.
  void startSocket();
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
  // 2026-09-18: Separate socket lifetime from one active chat request so an
  // expected idle disconnect does not mark a future request as failed.
  bool event_handler_ready_ = false;
  bool socket_started_ = false;
  bool request_active_ = false;
};

#endif
#endif  // 2026-09-18: gateway experiment disabled for board-direct mode.
