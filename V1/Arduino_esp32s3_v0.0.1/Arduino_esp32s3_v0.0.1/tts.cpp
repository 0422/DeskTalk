#include "tts.h"

#include <esp_heap_caps.h>
#include <new>

#include "volc_speech_protocol.h"

namespace {

constexpr uint8_t kFullClientRequest = 0x01;
constexpr uint8_t kFullServerResponse = 0x09;
constexpr uint8_t kAudioOnlyServer = 0x0B;
constexpr uint8_t kErrorResponse = 0x0F;
constexpr uint8_t kFlagPositiveSequence = 0x01;
constexpr uint8_t kFlagLastSequence = 0x02;
constexpr uint8_t kFlagNegativeSequence = 0x03;
constexpr uint8_t kFlagWithEvent = 0x04;
constexpr uint8_t kSerializationJson = 0x01;
constexpr uint8_t kCompressionGzip = 0x01;

constexpr int32_t kConnectionStarted = 50;
constexpr int32_t kConnectionFailed = 51;
constexpr int32_t kConnectionFinished = 52;
constexpr int32_t kSessionFinished = 152;
constexpr int32_t kSessionFailed = 153;
constexpr int32_t kTtsSentenceStart = 350;
constexpr int32_t kTtsSentenceEnd = 351;
constexpr int32_t kTtsResponse = 352;

// arduinoWebSockets 2.7.2 rejects frames larger than 15 KiB. Volcengine's
// 16 kHz PCM stream can exceed that limit, while 8 kHz frames stay below it.
// Playback is expanded back to the speaker's 16 kHz I2S rate below.
constexpr uint32_t kTtsSampleRate = 8000;
constexpr UBaseType_t kPlaybackQueueDepthWithPsram = 48;
constexpr UBaseType_t kPlaybackQueueDepthWithoutPsram = 8;
constexpr TickType_t kPlaybackQueueWait = pdMS_TO_TICKS(1500);
constexpr unsigned long kPlaybackFinishTimeoutMs = 60000;
// Short requests avoid the Seed-TTS WebSocket ending after only the first
// part of a long answer. Keep UTF-8 code points and sentence punctuation
// intact so consecutive requests still sound natural.
constexpr size_t kTtsSegmentMaxBytes = 120;

bool eventHasSessionId(int32_t event) {
  return event != kConnectionStarted && event != kConnectionFailed &&
         event != kConnectionFinished;
}

bool eventHasConnectId(int32_t event) {
  return event == kConnectionStarted || event == kConnectionFailed ||
         event == kConnectionFinished;
}

bool hasSequenceNumber(uint8_t flags) {
  return flags == kFlagPositiveSequence || flags == kFlagLastSequence ||
         flags == kFlagNegativeSequence;
}

size_t utf8CodePointLength(uint8_t first_byte) {
  if ((first_byte & 0x80) == 0) {
    return 1;
  }
  if ((first_byte & 0xE0) == 0xC0) {
    return 2;
  }
  if ((first_byte & 0xF0) == 0xE0) {
    return 3;
  }
  if ((first_byte & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

bool isSentenceEnd(const String &text, size_t offset) {
  const char c = text[offset];
  if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n') {
    return true;
  }
  return text.startsWith("。", offset) || text.startsWith("！", offset) ||
         text.startsWith("？", offset) || text.startsWith("；", offset);
}

String nextTtsSegment(const String &text, size_t &offset) {
  const size_t text_length = text.length();
  while (offset < text_length &&
         (text[offset] == ' ' || text[offset] == '\r' ||
          text[offset] == '\n' || text[offset] == '\t')) {
    ++offset;
  }
  if (offset >= text_length) {
    return "";
  }

  const size_t start = offset;
  size_t cursor = start;
  size_t last_sentence_end = start;
  while (cursor < text_length) {
    size_t code_point_length =
        utf8CodePointLength(static_cast<uint8_t>(text[cursor]));
    if (cursor + code_point_length > text_length) {
      code_point_length = 1;
    }

    if (cursor > start &&
        cursor + code_point_length - start > kTtsSegmentMaxBytes) {
      offset = last_sentence_end > start ? last_sentence_end : cursor;
      String segment = text.substring(start, offset);
      segment.trim();
      return segment;
    }

    if (isSentenceEnd(text, cursor)) {
      last_sentence_end = cursor + code_point_length;
    }
    cursor += code_point_length;
  }

  offset = text_length;
  String segment = text.substring(start);
  segment.trim();
  return segment;
}

}  // namespace

TtsClient::TtsClient() = default;

void TtsClient::connect() {
  if (webSocket.isConnected()) {
    return;
  }

  if (!event_handler_ready) {
    webSocket.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
      handleWebSocketEvent(type, payload, length);
    });
    event_handler_ready = true;
  }

  const String request_id = generate_uuid();
  String headers = volc_speech::make_auth_headers(resource_id, request_id, true);
  // The usage-token return header is optional.  Keep only the four required
  // authentication headers so the final header block remains well-formed.
  webSocket.beginSSL(host, 443, tts_url);
  webSocket.setExtraHeaders(headers.c_str());
  webSocket.setReconnectInterval(0);

  const unsigned long started_at = millis();
  while (!webSocket.isConnected() && millis() - started_at < 7000) {
    webSocket.loop();
    delay(10);
  }
  webSocket.setReconnectInterval(UINT32_MAX);
  if (!webSocket.isConnected()) {
    log_error("TTS 2.0 WebSocket connection timed out");
  }
}

void TtsClient::disconnect() {
  if (webSocket.isConnected()) {
    webSocket.disconnect();
  }
}

void TtsClient::handleWebSocketEvent(WStype_t type, uint8_t *payload,
                                     size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      log_info("TTS 2.0 WebSocket connected");
      break;
    case WStype_DISCONNECTED:
      if (request_active && !final_response_received) {
        const unsigned int queued_chunks = playback_queue == nullptr
                                               ? 0
                                               : uxQueueMessagesWaiting(
                                                     playback_queue);
        // This unidirectional endpoint normally closes the WebSocket after
        // sending its last audio frame. Some deployments omit both event 152
        // and a negative sequence marker, so a clean close after PCM is the
        // only end-of-stream signal. Keep draining the already queued audio.
        if (audio_bytes_received > 0 && (payload == nullptr || length == 0)) {
          final_response_received = true;
          log_info(
              "TTS 2.0 stream complete after %u PCM bytes "
              "(%u chunks queued)",
              static_cast<unsigned int>(audio_bytes_received), queued_chunks);
        } else if (payload != nullptr && length > 0) {
          log_error("TTS 2.0 WebSocket disconnected during synthesis: %.*s",
                    static_cast<int>(length),
                    reinterpret_cast<const char *>(payload));
          request_failed = true;
        } else {
          log_error(
              "TTS 2.0 WebSocket disconnected after %u PCM bytes "
              "(%u chunks queued)",
              static_cast<unsigned int>(audio_bytes_received), queued_chunks);
          request_failed = true;
        }
      } else {
        log_info("TTS 2.0 WebSocket disconnected");
      }
      break;
    case WStype_BIN:
      if (!parseResponse(payload, length)) {
        request_failed = true;
      }
      break;
    case WStype_TEXT:
      request_failed = true;
      log_error("TTS 2.0 text error: %.*s", static_cast<int>(length),
                reinterpret_cast<const char *>(payload));
      break;
    case WStype_ERROR:
      request_failed = true;
      log_error("TTS 2.0 WebSocket error");
      break;
    case WStype_PING:
      log_debug("TTS Ping");
      break;
    case WStype_PONG:
      log_debug("TTS Pong");
      break;
    default:
      break;
  }
}

void TtsClient::loop(int delay_time) {
  webSocket.loop();
  if (delay_time > 0) {
    delay(delay_time);
  }
}

bool TtsClient::sendTtsRequest(const String &text) {
  JsonDocument document;
  document["user"]["uid"] = "desk-emoji";
  JsonObject request = document["req_params"].to<JsonObject>();
  request["speaker"] = voice_type;
  request["text"] = text;

  JsonObject audio = request["audio_params"].to<JsonObject>();
  audio["format"] = "pcm";
  audio["sample_rate"] = kTtsSampleRate;
  audio["speech_rate"] = 0;
  audio["loudness_rate"] = 0;

  String json;
  serializeJson(document, json);

  const size_t frame_length = 8 + json.length();
  uint8_t *frame = new (std::nothrow) uint8_t[frame_length];
  if (frame == nullptr) {
    log_error("TTS request allocation failed");
    return false;
  }

  frame[0] = (PROTOCOL_VERSION << 4) | DEFAULT_HEADER_SIZE;
  frame[1] = (kFullClientRequest << 4) | NO_SEQUENCE;
  frame[2] = (kSerializationJson << 4) | NO_COMPRESSION;
  frame[3] = 0;
  volc_speech::write_u32_be(frame + 4, json.length());
  memcpy(frame + 8, json.c_str(), json.length());

  const bool sent = webSocket.sendBIN(frame, frame_length);
  delete[] frame;
  if (!sent) {
    log_error("Send TTS 2.0 request failed");
  }
  return sent;
}

bool TtsClient::TTS(String text) {
  if (text.isEmpty()) {
    log_error("TTS text is empty");
    return false;
  }

  text.trim();
  size_t offset = 0;
  uint16_t segment_number = 0;
  while (offset < text.length()) {
    String segment = nextTtsSegment(text, offset);
    if (segment.isEmpty()) {
      continue;
    }

    ++segment_number;
    log_info("TTS segment %u: %u text bytes", segment_number,
             static_cast<unsigned int>(segment.length()));
    if (!synthesizeSegment(segment, segment_number)) {
      log_error("TTS segment %u failed", segment_number);
      wait_for_playback_complete();
      stop_play();
      return false;
    }
  }

  if (segment_number == 0) {
    log_error("TTS text contains no speakable segment");
    return false;
  }
  wait_for_playback_complete();
  stop_play();
  log_info("TTS complete: %u segment(s)", segment_number);
  return true;
}

bool TtsClient::synthesizeSegment(const String &text,
                                  uint16_t segment_number) {

  final_response_received = false;
  request_failed = false;
  request_active = false;
  audio_bytes_received = 0;

  if (!webSocket.isConnected()) {
    connect();
  }
  if (!webSocket.isConnected()) {
    log_error("TTS 2.0 is not connected");
    return false;
  }
  if (!speaker_is_ready()) {
    log_error("TTS cannot play because speaker I2S is not ready");
    disconnect();
    return false;
  }
  if (!startPlayback()) {
    disconnect();
    return false;
  }
  request_active = true;
  if (!sendTtsRequest(text)) {
    request_active = false;
    finishPlayback(false);
    disconnect();
    return false;
  }

  const unsigned long started_at = millis();
  while (!final_response_received && !request_failed &&
         millis() - started_at < 60000) {
    loop(5);
  }

  bool success = final_response_received && !request_failed &&
                 audio_bytes_received > 0;
  if (!final_response_received && !request_failed) {
    log_error("TTS 2.0 final response wait timeout");
  } else if (final_response_received && !request_failed &&
             audio_bytes_received == 0) {
    log_error("TTS 2.0 returned no PCM audio");
  }

  request_active = false;
  success = finishPlayback(success) && success;
  if (success) {
    log_info("TTS segment %u END (%u PCM bytes received and played)",
             segment_number, static_cast<unsigned int>(audio_bytes_received));
  }
  disconnect();
  return success;
}

bool TtsClient::startPlayback() {
  if (playback_queue != nullptr || !playback_finished) {
    log_error("TTS playback task is already active");
    return false;
  }

  playback_abort = false;
  playback_failed = false;
  playback_finished = false;
  const UBaseType_t queue_depth = psramFound()
                                      ? kPlaybackQueueDepthWithPsram
                                      : kPlaybackQueueDepthWithoutPsram;
  playback_queue = xQueueCreate(queue_depth, sizeof(PcmChunk));
  if (playback_queue == nullptr) {
    playback_finished = true;
    log_error("TTS playback queue allocation failed");
    return false;
  }

  if (xTaskCreate(playbackTaskEntry, "TtsPlaybackTask", 4096, this, 2,
                  &playback_task) != pdPASS) {
    vQueueDelete(playback_queue);
    playback_queue = nullptr;
    playback_finished = true;
    log_error("TTS playback task creation failed");
    return false;
  }
  return true;
}

bool TtsClient::queuePcm16Le(const uint8_t *data, size_t length) {
  if (playback_queue == nullptr || data == nullptr || length == 0) {
    return false;
  }

  uint8_t *copy = nullptr;
  if (psramFound()) {
    copy = static_cast<uint8_t *>(
        heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (copy == nullptr) {
    copy = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_8BIT));
  }
  if (copy == nullptr) {
    log_error("TTS PCM queue allocation failed for %u bytes",
              static_cast<unsigned int>(length));
    return false;
  }

  memcpy(copy, data, length);
  const PcmChunk chunk = {copy, length};
  if (xQueueSend(playback_queue, &chunk, kPlaybackQueueWait) != pdTRUE) {
    heap_caps_free(copy);
    log_error("TTS playback queue remained full");
    return false;
  }
  return true;
}

bool TtsClient::finishPlayback(bool drain_audio) {
  if (playback_queue == nullptr) {
    return false;
  }

  playback_abort = !drain_audio;
  const PcmChunk end_marker = {nullptr, 0};
  xQueueSend(playback_queue, &end_marker, portMAX_DELAY);

  const unsigned long wait_started_at = millis();
  while (!playback_finished &&
         millis() - wait_started_at < kPlaybackFinishTimeoutMs) {
    delay(5);
  }

  if (!playback_finished) {
    playback_abort = true;
    log_error("TTS playback drain timed out");
    return false;
  }

  vQueueDelete(playback_queue);
  playback_queue = nullptr;
  playback_task = nullptr;
  return !playback_failed;
}

void TtsClient::playbackTaskEntry(void *parameter) {
  static_cast<TtsClient *>(parameter)->playbackLoop();
}

void TtsClient::playbackLoop() {
  PcmChunk chunk = {nullptr, 0};
  while (xQueueReceive(playback_queue, &chunk, portMAX_DELAY) == pdTRUE) {
    if (chunk.data == nullptr) {
      break;
    }

    if (!playback_abort && !playPcm16Le(chunk.data, chunk.length)) {
      playback_failed = true;
      playback_abort = true;
    }
    heap_caps_free(chunk.data);
  }

  playback_finished = true;
  playback_task = nullptr;
  vTaskDelete(nullptr);
}

bool TtsClient::parseResponse(const uint8_t *frame, size_t length) {
  if (frame == nullptr || length < 4) {
    log_error("TTS response frame is too short");
    return false;
  }

  const size_t header_length = (frame[0] & 0x0F) * 4;
  const uint8_t message_type = frame[1] >> 4;
  const uint8_t flags = frame[1] & 0x0F;
  const uint8_t compression = frame[2] & 0x0F;
  if (header_length < 4 || header_length > length) {
    log_error("TTS response has an invalid header size");
    return false;
  }

  size_t offset = header_length;
  int32_t sequence = 0;
  if (hasSequenceNumber(flags)) {
    if (!volc_speech::read_i32_be(frame, length, offset, sequence)) {
      log_error("TTS response is missing its sequence number");
      return false;
    }
  }

  uint32_t error_code = 0;
  if (message_type == kErrorResponse &&
      !volc_speech::read_u32_be(frame, length, offset, error_code)) {
    log_error("TTS error response is missing its code");
    return false;
  }

  int32_t event = 0;
  if (flags == kFlagWithEvent) {
    if (!volc_speech::read_i32_be(frame, length, offset, event)) {
      log_error("TTS response is missing its event number");
      return false;
    }

    uint32_t id_length = 0;
    if (eventHasSessionId(event)) {
      if (!volc_speech::read_u32_be(frame, length, offset, id_length) ||
          id_length > length - offset) {
        log_error("TTS response has an invalid session ID");
        return false;
      }
      offset += id_length;
    }
    if (eventHasConnectId(event)) {
      if (!volc_speech::read_u32_be(frame, length, offset, id_length) ||
          id_length > length - offset) {
        log_error("TTS response has an invalid connection ID");
        return false;
      }
      offset += id_length;
    }
  }

  uint32_t payload_length = 0;
  if (!volc_speech::read_u32_be(frame, length, offset, payload_length) ||
      payload_length > length - offset) {
    log_error("TTS response has an invalid payload size");
    return false;
  }

  const uint8_t *response_payload = frame + offset;
  uint8_t *decompressed = nullptr;
  size_t response_length = payload_length;
  if (compression == kCompressionGzip && payload_length > 0) {
    if (!volc_speech::gzip_decompress(response_payload, payload_length,
                                      &decompressed, &response_length)) {
      log_error("TTS response GZIP decompression failed");
      return false;
    }
    response_payload = decompressed;
  }

  bool success = true;
  if (message_type == kAudioOnlyServer) {
    // Audio-only responses carry the PCM payload after the sequence number;
    // they normally do not include event 352.  Accept both the sequenced
    // frames and the event-tagged form used by different service revisions.
    if (response_length > 0) {
      set_led(COLOR_GREEN, 50);
      success = queuePcm16Le(response_payload, response_length);
      if (success) {
        const bool first_audio_frame = audio_bytes_received == 0;
        audio_bytes_received += response_length;
        if (first_audio_frame) {
          log_info("TTS 2.0 PCM audio started (%u bytes)",
                   static_cast<unsigned int>(response_length));
        } else {
          log_debug("TTS PCM frame: %u bytes (total %u)",
                    static_cast<unsigned int>(response_length),
                    static_cast<unsigned int>(audio_bytes_received));
        }
      }
      set_led(COLOR_GREEN, 5);
    }
    // A negative sequence marks the final audio frame.  Some deployments
    // close the socket immediately afterward without sending event 152.
    if (flags == kFlagLastSequence || flags == kFlagNegativeSequence ||
        (hasSequenceNumber(flags) && sequence < 0)) {
      final_response_received = true;
    }
  } else if (message_type == kErrorResponse || event == kConnectionFailed ||
             event == kSessionFailed) {
    String error_message;
    if (response_length > 0) {
      error_message = String(reinterpret_cast<const char *>(response_payload),
                             response_length);
    }
    log_error("TTS 2.0 server error %u (event=%ld): %s", error_code,
              static_cast<long>(event), error_message.c_str());
    final_response_received = true;
    success = false;
  } else if (message_type == kFullServerResponse &&
             event == kSessionFinished) {
    final_response_received = true;
  } else if (event == kTtsSentenceStart || event == kTtsSentenceEnd) {
    log_debug("TTS event: %ld", static_cast<long>(event));
  }

  delete[] decompressed;
  return success;
}

bool TtsClient::playPcm16Le(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0 || (length & 1) != 0) {
    log_error("TTS PCM payload has invalid length: %u",
              static_cast<unsigned int>(length));
    return false;
  }

  static_assert(SAMPLE_RATE == kTtsSampleRate * 2,
                "TTS PCM upsampling expects a 2:1 sample-rate ratio");
  static constexpr size_t kInputChunkSamples = 128;
  int16_t samples[kInputChunkSamples * 2];
  size_t byte_offset = 0;
  while (byte_offset < length) {
    const size_t input_sample_count = min(
        kInputChunkSamples, (length - byte_offset) / sizeof(int16_t));
    for (size_t i = 0; i < input_sample_count; i++) {
      const size_t source = byte_offset + i * 2;
      const int16_t sample = static_cast<int16_t>(
          static_cast<uint16_t>(data[source]) |
          (static_cast<uint16_t>(data[source + 1]) << 8));
      samples[i * 2] = sample;
      samples[i * 2 + 1] = sample;
    }
    play(samples, input_sample_count * 2, volume);
    byte_offset += input_sample_count * sizeof(int16_t);
  }
  return true;
}
