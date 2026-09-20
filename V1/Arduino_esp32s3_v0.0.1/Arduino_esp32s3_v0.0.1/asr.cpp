#include "asr.h"

#include <algorithm>
#include <esp_heap_caps.h>
#include <new>

#include "volc_speech_protocol.h"
// 2026-09-17: Record serial-only latency milestones for the V1 conversation baseline.
#include "latency_trace.h"

namespace {

constexpr uint8_t kFlagPositiveSequence = 0x01;
constexpr uint8_t kFlagLast = 0x02;
constexpr uint8_t kFlagWithEvent = 0x04;
constexpr uint8_t kCompressionGzip = 0x01;
constexpr uint8_t kSerializationJson = 0x01;
constexpr uint8_t kSerializationNone = 0x00;
constexpr uint8_t kAsrMaxConnectAttempts = 3;
constexpr unsigned long kAsrConnectTimeoutMs = 7000;

// 2026-09-18: Capture ASR TLS and microphone-buffer pressure in the same
// format as the LLM/TTS diagnostics before deciding on an ESP-IDF migration.
constexpr bool kEnableAsrMemoryDiagnostics = true;

void logAsrMemoryDiagnostics(const char *stage) {
  if (!kEnableAsrMemoryDiagnostics) {
    return;
  }
  const size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_min =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
  const size_t psram_free = psramFound()
                                ? heap_caps_get_free_size(
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                : 0;
  log_info(
      "MEM ASR %s: heap=%u heap_min=%u max=%u internal=%u "
      "internal_min=%u internal_max=%u dma=%u psram=%u",
      stage == nullptr ? "unknown" : stage,
      static_cast<unsigned int>(ESP.getFreeHeap()),
      static_cast<unsigned int>(ESP.getMinFreeHeap()),
      static_cast<unsigned int>(ESP.getMaxAllocHeap()),
      static_cast<unsigned int>(internal_free),
      static_cast<unsigned int>(internal_min),
      static_cast<unsigned int>(internal_largest),
      static_cast<unsigned int>(dma_free),
      static_cast<unsigned int>(psram_free));
}

}  // namespace

AsrClient::AsrClient() = default;

void AsrClient::connect() {
  if (webSocket.isConnected()) {
    return;
  }

  if (!event_handler_ready) {
    webSocket.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
      handleWebSocketEvent(type, payload, length);
    });
    event_handler_ready = true;
  }

  for (uint8_t attempt = 1; attempt <= kAsrMaxConnectAttempts; ++attempt) {
    if (WiFi.status() != WL_CONNECTED) {
      log_error("ASR 2.0 cannot connect because WiFi is not connected (status=%d)",
                WiFi.status());
      break;
    }

    const String request_id = generate_uuid();
    const String headers =
        volc_speech::make_auth_headers(resource_id, request_id, false);
    // 2026-09-18: Measure the actual ASR TLS allocation rather than inferring
    // it from the post-ASR heap visible to the main conversation loop.
    logAsrMemoryDiagnostics("before-beginSSL");
    webSocket.beginSSL(host, 443, asr_url);
    webSocket.setExtraHeaders(headers.c_str());
    logAsrMemoryDiagnostics("after-beginSSL");
    webSocket.setReconnectInterval(0);
    log_info("ASR 2.0 WebSocket connect attempt %u/%u", attempt,
             kAsrMaxConnectAttempts);

    const unsigned long started_at = millis();
    while (!webSocket.isConnected() &&
           millis() - started_at < kAsrConnectTimeoutMs) {
      webSocket.loop();
      delay(10);
    }
    if (webSocket.isConnected()) {
      break;
    }

    log_warn("ASR 2.0 WebSocket connect attempt %u/%u timed out (WiFi status=%d)",
             attempt, kAsrMaxConnectAttempts, WiFi.status());
    if (attempt < kAsrMaxConnectAttempts) {
      delay(static_cast<unsigned long>(attempt) * 500UL);
    }
  }
  webSocket.setReconnectInterval(UINT32_MAX);
  if (!webSocket.isConnected()) {
    log_error("ASR 2.0 WebSocket connection timed out");
    logAsrMemoryDiagnostics("connect-failed");
  }
}

void AsrClient::disconnect() {
  // 2026-09-18: Confirm that closing ASR returns its TLS buffers and restores
  // the largest contiguous internal allocation.
  logAsrMemoryDiagnostics("before-disconnect");
  if (webSocket.isConnected()) {
    webSocket.disconnect();
  }
  logAsrMemoryDiagnostics("after-disconnect");
}

void AsrClient::handleWebSocketEvent(WStype_t type, uint8_t *payload,
                                     size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      log_info("ASR 2.0 WebSocket connected");
      // 2026-09-18: Record the ASR post-handshake low-water mark.
      logAsrMemoryDiagnostics("connected");
      break;
    case WStype_DISCONNECTED:
      if (request_active && !final_response_received) {
        if (payload != nullptr && length > 0) {
          log_error("ASR 2.0 WebSocket disconnected during recognition: %.*s",
                    static_cast<int>(length),
                    reinterpret_cast<const char *>(payload));
        } else {
          log_error("ASR 2.0 WebSocket disconnected during recognition");
        }
        request_failed = true;
      } else {
        log_info("ASR 2.0 WebSocket disconnected");
      }
      break;
    case WStype_TEXT:
      request_failed = true;
      log_error("ASR 2.0 text error: %.*s", static_cast<int>(length),
                reinterpret_cast<const char *>(payload));
      break;
    case WStype_BIN:
      message_received = true;
      if (!parseResponse(payload, length)) {
        request_failed = true;
      }
      break;
    case WStype_ERROR:
      request_failed = true;
      log_error("ASR 2.0 WebSocket error");
      break;
    case WStype_PING:
      log_debug("ASR Ping");
      break;
    case WStype_PONG:
      log_debug("ASR Pong");
      break;
    default:
      break;
  }
}

void AsrClient::loop(int delay_time) {
  webSocket.loop();
  if (delay_time > 0) {
    delay(delay_time);
  }
}

bool AsrClient::waitForMessage(uint32_t timeout_ms) {
  const unsigned long started_at = millis();
  while (!message_received && !request_failed &&
         millis() - started_at < timeout_ms) {
    loop(5);
  }
  if (request_failed) {
    return false;
  }
  if (!message_received) {
    log_error("ASR response wait timeout");
    return false;
  }
  message_received = false;
  return !request_failed;
}

bool AsrClient::waitForFinalResponse(uint32_t timeout_ms) {
  const unsigned long started_at = millis();
  while (!final_response_received && !request_failed &&
         millis() - started_at < timeout_ms) {
    loop(5);
  }
  if (request_failed) {
    return false;
  }
  if (!final_response_received) {
    log_error("ASR final response wait timeout");
    return false;
  }
  return !request_failed;
}

String AsrClient::constructRequest() {
  JsonDocument document;
  document["user"]["uid"] = uid;

  JsonObject audio = document["audio"].to<JsonObject>();
  audio["format"] = "pcm";
  audio["codec"] = "raw";
  audio["rate"] = SAMPLE_RATE;
  audio["bits"] = 16;
  audio["channel"] = 1;

  JsonObject request = document["request"].to<JsonObject>();
  request["model_name"] = "bigmodel";
  request["enable_itn"] = true;
  request["enable_punc"] = true;
  request["enable_ddc"] = true;
  request["show_utterances"] = false;
  request["enable_nonstream"] = false;
  request["result_type"] = "full";

  String json;
  serializeJson(document, json);
  return json;
}

bool AsrClient::sendFullRequest() {
  const String json = constructRequest();
  const size_t frame_length = 8 + json.length();
  uint8_t *frame = new (std::nothrow) uint8_t[frame_length];
  if (frame == nullptr) {
    logAsrMemoryDiagnostics("request-allocation-failed");
    log_error("ASR request allocation failed");
    return false;
  }

  frame[0] = (PROTOCOL_VERSION << 4) | DEFAULT_HEADER_SIZE;
  frame[1] = CLIENT_FULL_REQUEST << 4;
  frame[2] = kSerializationJson << 4;
  frame[3] = 0;
  volc_speech::write_u32_be(frame + 4, json.length());
  memcpy(frame + 8, json.c_str(), json.length());

  const bool sent = webSocket.sendBIN(frame, frame_length);
  delete[] frame;
  if (!sent) {
    log_error("Send ASR 2.0 full request failed");
  }
  return sent;
}

bool AsrClient::sendAudioRequest(const uint8_t *data, size_t length,
                                 bool is_last) {
  const size_t frame_length = 8 + length;
  uint8_t *frame = new (std::nothrow) uint8_t[frame_length];
  if (frame == nullptr) {
    logAsrMemoryDiagnostics("audio-frame-allocation-failed");
    log_error("ASR audio frame allocation failed");
    return false;
  }

  frame[0] = (PROTOCOL_VERSION << 4) | DEFAULT_HEADER_SIZE;
  frame[1] = (CLIENT_AUDIO_ONLY_REQUEST << 4) |
             (is_last ? kFlagLast : NO_SEQUENCE);
  frame[2] = kSerializationNone << 4;
  frame[3] = 0;
  volc_speech::write_u32_be(frame + 4, length);
  memcpy(frame + 8, data, length);

  const bool sent = webSocket.sendBIN(frame, frame_length);
  delete[] frame;
  if (!sent) {
    log_error("Send ASR 2.0 audio request failed");
  }
  return sent;
}

bool AsrClient::ASR() {
  // 2026-09-17: Start one isolated latency session for every cloud ASR turn.
  latency_trace_begin();
  asr_result = "";
  message_received = false;
  final_response_received = false;
  request_failed = false;
  request_active = false;
  voice_detected = false;
  set_led(COLOR_RED, 10);

  if (!webSocket.isConnected()) {
    connect();
  }
  if (!webSocket.isConnected()) {
    log_error("ASR 2.0 is not connected");
    return false;
  }

  request_active = true;
  if (!sendFullRequest() || !waitForMessage()) {
    request_active = false;
    return false;
  }
  log_info("ASR listening; speak now");

  const size_t samples_needed = RECORD_TIME * SAMPLE_RATE;
  size_t samples_recorded = 0;
  int16_t *buffer = new (std::nothrow) int16_t[BUFFER_SIZE];
  if (buffer == nullptr) {
    logAsrMemoryDiagnostics("microphone-buffer-allocation-failed");
    log_error("ASR audio buffer allocation failed");
    request_active = false;
    return false;
  }
  // 2026-09-18: Isolate the fixed microphone buffer from TLS and temporary
  // per-frame request allocations.
  logAsrMemoryDiagnostics("after-microphone-buffer-allocation");

  unsigned long silence_started_at = 0;
  bool is_silent = false;
  bool speech_started = false;
  // 2026-09-17: Track the local amplitude threshold independently from cloud partial-result speech detection.
  bool vad_marked = false;
  size_t max_mean = 0;
  bool sent_last_frame = false;

  while (samples_recorded < samples_needed && !request_failed) {
    const size_t samples_to_read =
        std::min(static_cast<size_t>(BUFFER_SIZE),
                 samples_needed - samples_recorded);
    record(buffer, samples_to_read);
    enhanceVoice(buffer, samples_to_read);

    const size_t mean = calculate_mean(buffer, samples_to_read);
    max_mean = max(max_mean, mean);
    if (voice_detected && !speech_started) {
      speech_started = true;
      is_silent = false;
      silence_started_at = 0;
      log_info("Speech detected by ASR");
    }

    bool is_last = samples_recorded + samples_to_read >= samples_needed;
    if (mean > SOUND_THRESHOLD) {
      // 2026-09-17: Timestamp the first audio block that crosses the firmware's current VAD threshold.
      if (!vad_marked) {
        vad_marked = true;
        latency_trace_mark(LatencyEvent::VAD_START);
      }
      if (!speech_started) {
        speech_started = true;
        log_info("Speech detected by microphone");
      }
      set_led(COLOR_RED, 100);
      is_silent = false;
      silence_started_at = 0;
    } else {
      set_led(COLOR_RED, 10);
      if (!is_silent) {
        is_silent = true;
        silence_started_at = millis();
      // } else if (speech_started &&
      //            millis() - silence_started_at >=
      //                MAX_SILENCE_TIME * 1000UL) {
      // 2026-09-17: Use a millisecond endpoint threshold so the baseline can be tuned below whole seconds.
      } else if (speech_started &&
                 millis() - silence_started_at >= MAX_SILENCE_TIME_MS) {
        is_last = true;
        // log_info("End of speech detected after %d seconds of silence",
        //          MAX_SILENCE_TIME);
        log_info("End of speech detected after %lu ms of silence",
                 static_cast<unsigned long>(MAX_SILENCE_TIME_MS));
      } else if (!speech_started &&
                 samples_recorded + samples_to_read >=
                     SAMPLE_RATE * START_SPEECH_TIMEOUT) {
        is_last = true;
        log_info("No speech detected within %d seconds (max mean=%u, threshold=%u)",
                 START_SPEECH_TIMEOUT, static_cast<unsigned int>(max_mean),
                 static_cast<unsigned int>(SOUND_THRESHOLD));
      }
    }

    // 2026-09-18: Record the local speech-end decision once. The trace layer
    // ignores repeats if a provider result and the microphone endpoint race.
    if (is_last && speech_started) {
      latency_trace_mark(LatencyEvent::VAD_END);
    }
    if (!sendAudioRequest(reinterpret_cast<const uint8_t *>(buffer),
                          samples_to_read * sizeof(int16_t), is_last)) {
      request_failed = true;
      break;
    }
    // 2026-09-17: Treat successful submission of the last WebSocket audio frame as terminal upload completion.
    if (is_last) {
      latency_trace_mark(LatencyEvent::AUDIO_UPLOAD_DONE);
    }
    samples_recorded += samples_to_read;

    const unsigned long pump_started_at = millis();
    while (millis() - pump_started_at < 20) {
      webSocket.loop();
      delay(1);
    }

    if (is_last) {
      sent_last_frame = true;
      break;
    }
  }

  logAsrMemoryDiagnostics("before-microphone-buffer-free");
  delete[] buffer;
  logAsrMemoryDiagnostics("after-microphone-buffer-free");
  if (!sent_last_frame || request_failed) {
    request_active = false;
    return false;
  }
  const bool success = waitForFinalResponse();
  // 2026-09-18: Capture ASR state after the final result but before the main
  // loop explicitly closes its WebSocket.
  logAsrMemoryDiagnostics("after-final-response");
  request_active = false;
  return success;
}

bool AsrClient::parseResponse(const uint8_t *frame, size_t length) {
  if (frame == nullptr || length < 4) {
    log_error("ASR response frame is too short");
    return false;
  }

  const size_t header_length = (frame[0] & 0x0F) * 4;
  const uint8_t message_type = frame[1] >> 4;
  const uint8_t flags = frame[1] & 0x0F;
  const uint8_t serialization = frame[2] >> 4;
  const uint8_t compression = frame[2] & 0x0F;
  if (header_length < 4 || header_length > length) {
    log_error("ASR response has an invalid header size");
    return false;
  }

  size_t offset = header_length;
  int32_t response_sequence = 0;
  if ((flags & kFlagPositiveSequence) != 0 &&
      !volc_speech::read_i32_be(frame, length, offset, response_sequence)) {
    log_error("ASR response is missing its sequence number");
    return false;
  }

  if ((flags & kFlagWithEvent) != 0) {
    int32_t event = 0;
    if (!volc_speech::read_i32_be(frame, length, offset, event)) {
      log_error("ASR response is missing its event number");
      return false;
    }
    log_debug("ASR event: %ld", static_cast<long>(event));
  }

  uint32_t error_code = 0;
  uint32_t payload_length = 0;
  if (message_type == SERVER_ERROR_RESPONSE) {
    if (!volc_speech::read_u32_be(frame, length, offset, error_code) ||
        !volc_speech::read_u32_be(frame, length, offset, payload_length)) {
      log_error("ASR error response is truncated");
      return false;
    }
  } else if (message_type == SERVER_FULL_RESPONSE || message_type == SERVER_ACK) {
    if (!volc_speech::read_u32_be(frame, length, offset, payload_length)) {
      log_error("ASR response is missing its payload size");
      return false;
    }
  } else {
    log_error("ASR response has unsupported message type 0x%X", message_type);
    return false;
  }

  if (payload_length > length - offset) {
    log_error("ASR response payload is truncated");
    return false;
  }

  const uint8_t *json_payload = frame + offset;
  size_t json_length = payload_length;
  uint8_t *decompressed = nullptr;
  if (compression == kCompressionGzip && payload_length > 0) {
    if (!volc_speech::gzip_decompress(json_payload, payload_length,
                                      &decompressed, &json_length)) {
      log_error("ASR response GZIP decompression failed");
      return false;
    }
    json_payload = decompressed;
  }

  if (message_type == SERVER_ERROR_RESPONSE) {
    String error_message;
    if (json_length > 0) {
      error_message = String(reinterpret_cast<const char *>(json_payload),
                             json_length);
    }
    log_error("ASR 2.0 server error %u: %s", error_code,
              error_message.c_str());
    delete[] decompressed;
    final_response_received = true;
    return false;
  }

  if (serialization == kSerializationJson && json_length > 0) {
    parseResultJson(json_payload, json_length);
  }
  delete[] decompressed;

  if ((flags & kFlagLast) != 0 || response_sequence < 0) {
    final_response_received = true;
    // 2026-09-17: Mark the final ASR result after its response payload has been parsed.
    latency_trace_mark(LatencyEvent::ASR_FINAL);
    log_info("ASR 2.0 final result: %s", asr_result.c_str());
  }
  return true;
}

void AsrClient::parseResultJson(const uint8_t *payload, size_t length) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload, length);
  if (error) {
    log_error("ASR response JSON parse failed: %s", error.c_str());
    return;
  }

  String text;
  JsonVariant result = document["result"];
  if (result["text"].is<const char *>()) {
    text = result["text"].as<String>();
  } else if (result.is<JsonArray>() && result.size() > 0 &&
             result[0]["text"].is<const char *>()) {
    text = result[0]["text"].as<String>();
  }

  if (!text.isEmpty()) {
    asr_result = text;
    voice_detected = true;
    log_debug("ASR partial result: %s", text.c_str());
  }
}

String AsrClient::asrResult() { return asr_result; }
