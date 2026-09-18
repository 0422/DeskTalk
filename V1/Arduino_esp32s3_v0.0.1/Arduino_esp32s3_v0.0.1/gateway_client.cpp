#include "gateway_client.h"

// 2026-09-18: Decode gateway control messages separately from raw 8 kHz mono PCM frames.
#include <ArduinoJson.h>
#include "act.h"
#include "latency_trace.h"

bool GatewayClient::enabled() const {
  return GATEWAY_HOST[0] != '\0' && GATEWAY_TOKEN[0] != '\0';
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
  connected_ = false;
  audio_started_ = false;
  reply_received_ = false;
  remote_done_ = false;
  failed_ = false;

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

  const unsigned long started_at = millis();
  while (!failed_ && !remote_done_ && millis() - started_at < 90000UL) {
    socket_.loop();
    if (!connected_ && millis() - started_at >= 3500UL) {
      failed_ = true;
      log_error("Gateway connection timed out; using direct providers");
    }
    delay(1);
  }
  if (!failed_ && !remote_done_) {
    failed_ = true;
    log_error("Gateway response timed out");
  }
  socket_.disconnect();

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
    if (!socket_.sendTXT(request_)) {
      failed_ = true;
    }
    return;
  }
  if (type == WStype_BIN) {
    if (!failed_ && length > 0) {
      if (!tts_->queueExternalPcm(payload, length)) {
        failed_ = true;
        log_error("Gateway PCM playback queue failed");
      } else {
        audio_started_ = true;
      }
    }
    return;
  }
  if (type == WStype_TEXT) {
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
  if (type == WStype_ERROR ||
      (type == WStype_DISCONNECTED && !remote_done_)) {
    failed_ = true;
  }
}
