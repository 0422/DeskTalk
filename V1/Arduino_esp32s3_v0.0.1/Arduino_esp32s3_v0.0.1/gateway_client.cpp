#if 0
// 2026-09-18: Disable the optional gateway implementation while the firmware
// uses direct ASR, DeepSeek, and Volcengine providers. Preserve the experiment
// without allowing Arduino's automatic .cpp discovery to compile it.
#include "gateway_client.h"

// 2026-09-18: Decode gateway control messages separately from raw 8 kHz mono PCM frames.
#include <ArduinoJson.h>
#include "act.h"
#include "latency_trace.h"

bool GatewayClient::enabled() const {
  return GATEWAY_HOST[0] != '\0' && GATEWAY_TOKEN[0] != '\0';
}

// 2026-09-18: Register one event handler and start a reconnecting LAN socket
// during boot instead of paying its handshake cost after every ASR result.
void GatewayClient::begin() {
  if (!enabled()) {
    return;
  }
  if (!event_handler_ready_) {
    socket_.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
      onEvent(type, payload, length);
    });
    event_handler_ready_ = true;
  }
  if (!socket_started_) {
    startSocket();
  }
}

// 2026-09-18: Build the endpoint once and leave automatic reconnect enabled
// so Wi-Fi or gateway restarts recover while the robot is idle.
void GatewayClient::startSocket() {
  if (GATEWAY_TLS) {
    socket_.beginSSL(GATEWAY_HOST, GATEWAY_PORT, GATEWAY_PATH);
  } else {
    socket_.begin(GATEWAY_HOST, GATEWAY_PORT, GATEWAY_PATH);
  }
  const String header = String("Authorization: Bearer ") + GATEWAY_TOKEN;
  socket_.setExtraHeaders(header.c_str());
  socket_.setReconnectInterval(1000);
  socket_started_ = true;
}

// 2026-09-18: Spend the first LAN handshake during initialization, where it
// cannot add to the user's post-speech latency. Failure remains non-fatal.
bool GatewayClient::warmup(unsigned long timeout_ms) {
  begin();
  if (!enabled()) {
    return false;
  }
  const unsigned long started_at = millis();
  while (!connected_ && millis() - started_at < timeout_ms) {
    socket_.loop();
    delay(5);
  }
  if (connected_) {
    log_info("Gateway connection is ready");
  } else {
    log_warn("Gateway warmup timed out; background reconnect remains active");
  }
  return connected_;
}

// 2026-09-18: Keep the persistent gateway socket responsive between turns.
void GatewayClient::loop() {
  if (socket_started_) {
    socket_.loop();
  }
}

// 2026-09-18: Let one board-side socket receive LLM milestones and TTS PCM while the gateway overlaps its provider connections.
GatewayResult GatewayClient::chat(const String &question, LLM &llm,
                                  TtsClient &tts) {
  // 2026-09-18: Building the request also resets stale reply state before playback or a network handshake can fail.
  request_ = llm.gatewayRequest(question);
  if (!enabled() || request_.isEmpty() || !tts.beginExternalStream()) {
    return GatewayResult::FAILED_BEFORE_AUDIO;
  }

  llm_ = &llm;
  tts_ = &tts;
  question_ = question;
  // connected_ = false;
  // 2026-09-18: Preserve the physical socket state established during boot or
  // the preceding turn; disconnect/error callbacks alone own this flag.
  audio_started_ = false;
  reply_received_ = false;
  remote_done_ = false;
  failed_ = false;
  // request_active_ = true;
  // 2026-09-18: Connection-attempt callbacks are idle transport state, not a
  // failed chat. Mark the request active only immediately before sendTXT().
  request_active_ = false;

  // 2026-09-18: Retain the former per-request connection setup for reference;
  // the persistent connection below replaces it on the active path.
#if 0
  if (!failed_) {
    socket_.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
      onEvent(type, payload, length);
    });
    if (GATEWAY_TLS) {
      socket_.beginSSL(GATEWAY_HOST, GATEWAY_PORT, GATEWAY_PATH);
    } else {
      socket_.begin(GATEWAY_HOST, GATEWAY_PORT, GATEWAY_PATH);
    }
    const String header = String("Authorization: Bearer ") + GATEWAY_TOKEN;
    socket_.setExtraHeaders(header.c_str());
    socket_.setReconnectInterval(UINT32_MAX);
  }
#endif
  // 2026-09-18: Reuse the boot-time socket and wait only when background
  // reconnect has not completed before this particular question.
  begin();
  const unsigned long connect_started_at = millis();
  while (!connected_ && !failed_ &&
         millis() - connect_started_at < 3500UL) {
    socket_.loop();
    delay(1);
  }
  if (!connected_) {
    failed_ = true;
    log_error("Gateway connection timed out; using direct providers");
  } else {
    request_active_ = true;
    if (!socket_.sendTXT(request_)) {
      failed_ = true;
      log_error("Gateway request send failed");
    }
  }

  const unsigned long started_at = millis();
  while (!failed_ && !remote_done_ && millis() - started_at < 90000UL) {
    socket_.loop();
    // 2026-09-18: The old timeout lived inside the response loop; connection
    // waiting is now completed before the request is sent.
#if 0
    if (!connected_ && millis() - started_at >= 3500UL) {
      failed_ = true;
      log_error("Gateway connection timed out; using direct providers");
    }
#endif
    delay(1);
  }
  if (!failed_ && !remote_done_) {
    failed_ = true;
    log_error("Gateway response timed out");
  }
  // socket_.disconnect();
  // 2026-09-18: Keep a successful LAN socket open for the next turn. A failed
  // TLS gateway socket is closed before direct cloud fallback to release the
  // ESP32's constrained TLS buffers.
  request_active_ = false;
  if (failed_ && GATEWAY_TLS && socket_.isConnected()) {
    socket_.disconnect();
    connected_ = false;
    socket_started_ = false;
  }

  const bool complete = remote_done_ && reply_received_ &&
                        audio_started_ && !failed_;
  if (!tts.finishExternalStream(complete)) {
    failed_ = true;
  }
  if (reply_received_) {
    llm.finishGatewayReply(question);
  }
  llm_ = nullptr;
  tts_ = nullptr;
  request_ = "";
  question_ = "";
  return !failed_ && complete ? GatewayResult::SUCCESS
                             : audio_started_ ? GatewayResult::FAILED_AFTER_AUDIO
                                              : GatewayResult::FAILED_BEFORE_AUDIO;
}

// 2026-09-18: A failed gateway before PCM may fall back to the direct path, but partial speech must never be repeated.
void GatewayClient::onEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    connected_ = true;
    // if (!socket_.sendTXT(request_)) {
    //   failed_ = true;
    // }
    // 2026-09-18: Requests are sent explicitly by chat(); a reconnect must
    // never replay a question whose audio may already have started.
    return;
  }
  if (type == WStype_BIN) {
    if (request_active_ && tts_ != nullptr && !failed_ && length > 0) {
      if (!tts_->queueExternalPcm(payload, length)) {
        failed_ = true;
        log_error("Gateway PCM playback queue failed");
      } else {
        audio_started_ = true;
      }
    }
    return;
  }
  if (type == WStype_TEXT && request_active_) {
    JsonDocument message;
    if (deserializeJson(message, payload, length)) {
      failed_ = true;
      return;
    }
    const char *kind = message["type"] | "";
    if (strcmp(kind, "llm_first") == 0) {
      latency_trace_mark(LatencyEvent::LLM_FIRST_BYTE);
    } else if (strcmp(kind, "tts_connected") == 0) {
      latency_trace_mark(LatencyEvent::TTS_CONNECTED);
    } else if (strcmp(kind, "tts_request_sent") == 0) {
      latency_trace_mark(LatencyEvent::TTS_REQUEST_SENT);
    } else if (strcmp(kind, "reply") == 0) {
      String response;
      serializeJson(message["response"], response);
      reply_received_ = llm_->acceptGatewayReply(question_, response);
      if (reply_received_) {
        async_sequent_act(llm_->actions());
      } else {
        failed_ = true;
      }
    } else if (strcmp(kind, "done") == 0) {
      remote_done_ = true;
    } else if (strcmp(kind, "error") == 0) {
      failed_ = true;
      const char *detail = message["message"] | "gateway error";
      log_error("Gateway: %.180s", detail);
    }
    return;
  }
  if (type == WStype_DISCONNECTED) {
    connected_ = false;
    if (request_active_ && !remote_done_) {
      failed_ = true;
    }
    return;
  }
  if (type == WStype_ERROR) {
    connected_ = false;
    if (request_active_) {
      failed_ = true;
    }
  }
  // 2026-09-18: Retain the former combined terminal condition; persistent
  // idle disconnects now reconnect without poisoning the next request.
#if 0
  if (type == WStype_ERROR ||
      (type == WStype_DISCONNECTED && !remote_done_)) {
    failed_ = true;
  }
#endif
}
#endif  // 2026-09-18: gateway experiment disabled for board-direct mode.
