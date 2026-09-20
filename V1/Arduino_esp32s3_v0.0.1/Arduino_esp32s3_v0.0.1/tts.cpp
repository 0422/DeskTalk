#include "tts.h"

#include <esp_heap_caps.h>
#include <new>

#include "volc_speech_protocol.h"
// 2026-09-17: Separate TTS network arrival from actual I2S playback in the V1 latency baseline.
#include "latency_trace.h"

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

// 2026-09-18: Volcengine V3 bidirectional connection/session events.
constexpr int32_t kStartConnection = 1;
constexpr int32_t kFinishConnection = 2;
constexpr int32_t kConnectionStarted = 50;
constexpr int32_t kConnectionFailed = 51;
constexpr int32_t kConnectionFinished = 52;
constexpr int32_t kStartSession = 100;
constexpr int32_t kFinishSession = 102;
constexpr int32_t kSessionStarted = 150;
constexpr int32_t kSessionFinished = 152;
constexpr int32_t kSessionFailed = 153;
constexpr int32_t kTaskRequest = 200;
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
// 2026-09-17: Retry transient TTS sessions only when they ended before delivering any PCM audio.
constexpr uint8_t kTtsZeroAudioMaxAttempts = 3;
// constexpr unsigned long kTtsRetryBackoffMs = 500;
// 2026-09-18: Retry a zero-PCM provider close quickly; no speech was delivered,
// so a short backoff reduces the exceptional-path delay without duplication.
constexpr unsigned long kTtsRetryBackoffMs = 150;
// 2026-09-18: Bound every bidirectional protocol transition so a failed
// provider connection can fall back to the existing one-shot interface.
constexpr unsigned long kTtsSocketConnectTimeoutMs = 7000;
constexpr unsigned long kTtsConnectionStartTimeoutMs = 5000;
constexpr unsigned long kTtsSessionStartTimeoutMs = 5000;
constexpr unsigned long kTtsConnectionFinishTimeoutMs = 2000;
// 2026-09-18: Retain a warm TTS TLS socket only when enough contiguous
// internal RAM remains for the next DeepSeek TLS handshake.
constexpr uint32_t kPersistentTtsMinFreeHeap = 96U * 1024U;
constexpr uint32_t kPersistentTtsMinLargestBlock = 48U * 1024U;
// 2026-09-18: Prefer one natural TTS request; only unusually long answers are split near their midpoint at sentence punctuation.
constexpr size_t kPreferredSingleRequestCodePoints = 90;
constexpr size_t kMinimumSplitSideCodePoints = 24;
// Short requests avoid the Seed-TTS WebSocket ending after only the first
// part of a long answer. Keep UTF-8 code points and sentence punctuation
// intact so consecutive requests still sound natural.
// constexpr size_t kTtsSegmentMaxBytes = 120;
// 2026-09-17: Use semantic punctuation for normal segmentation and reserve queue capacity for a complete short reply.
// constexpr size_t kSoftBoundaryMinimumCodePoints = 12;
// constexpr UBaseType_t kSentenceQueueDepth = 8;
// 2026-09-17: Merge short sentences and split on commas only as a long-sentence fallback, reducing TTS connection churn.
constexpr size_t kMinimumStandalonePhraseCodePoints = 10;
constexpr size_t kSoftBoundaryMinimumCodePoints = 28;
// constexpr UBaseType_t kSentenceQueueDepth = 16;
// 2026-09-17: Hold every minimum-size phrase allowed by the 192-token LLM limit before the TLS handoff releases playback.
constexpr UBaseType_t kSentenceQueueDepth = 24;
// constexpr unsigned long kSentenceStreamFinishTimeoutMs = 150000;

// 2026-09-18: Keep a non-invasive RAM snapshot enabled while deciding whether
// ESP-IDF migration is necessary. The snapshots distinguish total internal
// heap pressure from the largest contiguous block required by TLS/queues.
constexpr bool kEnableTtsMemoryDiagnostics = true;

void logTtsMemoryDiagnostics(const char *stage) {
  if (!kEnableTtsMemoryDiagnostics) {
    return;
  }
  const size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_min =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
  const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  const size_t psram_free = psramFound()
                                ? heap_caps_get_free_size(
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                : 0;
  log_info(
      "MEM TTS %s: heap=%u heap_min=%u max=%u internal=%u "
      "internal_min=%u internal_max=%u dma=%u dma_max=%u psram=%u",
      stage == nullptr ? "unknown" : stage,
      static_cast<unsigned int>(ESP.getFreeHeap()),
      static_cast<unsigned int>(ESP.getMinFreeHeap()),
      static_cast<unsigned int>(ESP.getMaxAllocHeap()),
      static_cast<unsigned int>(internal_free),
      static_cast<unsigned int>(internal_min),
      static_cast<unsigned int>(internal_largest),
      static_cast<unsigned int>(dma_free),
      static_cast<unsigned int>(dma_largest),
      static_cast<unsigned int>(psram_free));
}

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

// 2026-09-18: Count UTF-8 characters so Chinese length decisions do not depend on their three-byte encoding.
size_t countUtf8CodePoints(const String &text, size_t start = 0) {
  size_t count = 0;
  size_t cursor = start;
  while (cursor < text.length()) {
    size_t length =
        utf8CodePointLength(static_cast<uint8_t>(text[cursor]));
    if (cursor + length > text.length()) {
      length = 1;
    }
    cursor += length;
    ++count;
  }
  return count;
}

// 2026-09-18: Find one balanced sentence boundary outside quotation marks; zero means the full answer stays in one request.
size_t findLongAnswerSplit(const String &text) {
  const size_t total_code_points = countUtf8CodePoints(text);
  if (total_code_points <= kPreferredSingleRequestCodePoints) {
    return 0;
  }

  const size_t target = total_code_points / 2;
  size_t best_offset = 0;
  size_t best_distance = total_code_points;
  size_t cursor = 0;
  size_t code_points = 0;
  bool in_ascii_quote = false;
  bool in_chinese_quote = false;
  while (cursor < text.length()) {
    const size_t code_point_length =
        utf8CodePointLength(static_cast<uint8_t>(text[cursor]));
    if (cursor + code_point_length > text.length()) {
      break;
    }

    if (text[cursor] == '"') {
      in_ascii_quote = !in_ascii_quote;
    } else if (text.startsWith("“", cursor) ||
               text.startsWith("「", cursor)) {
      in_chinese_quote = true;
    } else if (text.startsWith("”", cursor) ||
               text.startsWith("」", cursor)) {
      in_chinese_quote = false;
    }

    ++code_points;
    const size_t remaining = total_code_points - code_points;
    if (!in_ascii_quote && !in_chinese_quote &&
        isSentenceEnd(text, cursor) &&
        code_points >= kMinimumSplitSideCodePoints &&
        remaining >= kMinimumSplitSideCodePoints) {
      const size_t distance = code_points > target
                                  ? code_points - target
                                  : target - code_points;
      if (distance < best_distance) {
        best_distance = distance;
        best_offset = cursor + code_point_length;
      }
    }
    cursor += code_point_length;
  }
  return best_offset;
}

// 2026-09-17: Treat commas and colons as optional phrase boundaries only after enough spoken characters have accumulated.
bool isPhraseEnd(const String &text, size_t offset) {
  const char c = text[offset];
  if (c == ',' || c == ':') {
    return true;
  }
  // return text.startsWith("，", offset) || text.startsWith("、", offset) ||
  //        text.startsWith("：", offset);
  // 2026-09-17: Never open a new TTS connection for an enumeration separator such as "鸡蛋、牛奶".
  return text.startsWith("，", offset) || text.startsWith("：", offset);
}

#if 0
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
#endif

// 2026-09-17: Keep complete sentences and sufficiently long clauses together instead of splitting at a fixed byte count.
String nextSemanticTtsSegment(const String &text, size_t &offset) {
  const size_t text_length = text.length();
  while (offset < text_length &&
         (text[offset] == ' ' || text[offset] == '\r' ||
          text[offset] == '\n' || text[offset] == '\t')) {
    ++offset;
  }
  if (offset >= text_length) {
    return "";
  }

#if 0
  const size_t previous_start = offset;
  size_t previous_cursor = previous_start;
  size_t previous_code_points = 0;
  while (previous_cursor < text_length) {
    size_t previous_code_point_length =
        utf8CodePointLength(static_cast<uint8_t>(text[previous_cursor]));
    if (previous_cursor + previous_code_point_length > text_length) {
      previous_code_point_length = 1;
    }
    ++previous_code_points;
    previous_cursor += previous_code_point_length;
    const bool previous_hard_boundary =
        isSentenceEnd(text, previous_cursor - previous_code_point_length) &&
        previous_code_points >= kMinimumStandalonePhraseCodePoints;
    const bool previous_soft_boundary =
        previous_code_points >= kSoftBoundaryMinimumCodePoints &&
        isPhraseEnd(text, previous_cursor - previous_code_point_length);
    if (previous_hard_boundary || previous_soft_boundary) {
      offset = previous_cursor;
      String previous_segment = text.substring(previous_start, offset);
      previous_segment.trim();
      return previous_segment;
    }
  }
#endif
  // 2026-09-18: Return the entire remaining answer unless one balanced hard boundary is needed for an unusually long request.
  const size_t start = offset;
  const String remaining = text.substring(start);
  const size_t relative_split = findLongAnswerSplit(remaining);
  offset = relative_split == 0 ? text_length : start + relative_split;
  String segment = text.substring(start, offset);
  segment.trim();
  return segment;
}

}  // namespace

TtsClient::TtsClient() = default;

// 2026-09-18: Reset per-Session diagnostics before accepting any PCM so a
// previous explicit end marker cannot validate a later truncated response.
void TtsClient::resetResponseTracking() {
  final_response_received = false;
  request_failed = false;
  request_active = false;
  audio_bytes_received = 0;
  explicit_response_end_received = false;
  completion_from_disconnect = false;
  audio_frame_count = 0;
  audio_sequence_seen = false;
  audio_sequence_gap = false;
  last_audio_sequence = 0;
}

bool TtsClient::persistentConnectionReady() {
  return webSocket.isConnected() &&
         connection_mode == ConnectionMode::BIDIRECTIONAL &&
         bidirectional_connection_ready &&
         !bidirectional_connection_failed;
}

// 2026-09-18: Establish the reusable V3 Connection after startup hardware has
// allocated its buffers, moving its handshake out of the first spoken turn.
bool TtsClient::warmupPersistentConnection() {
  if (persistentConnectionReady()) {
    return true;
  }
  const bool ready = connectBidirectional();
  if (ready) {
    log_info("TTS persistent connection warmup is ready");
  } else {
    log_warn("TTS persistent connection warmup failed; one-shot fallback remains available");
  }
  return ready;
}

// 2026-09-18: Protect the next LLM TLS handshake from internal-heap
// fragmentation. A healthy persistent socket is otherwise left open.
void TtsClient::prepareForCloudRequest() {
  // 2026-09-18: Capture the baseline immediately before ASR/LLM TLS so the
  // following provider allocation can be compared with TTS connection logs.
  logTtsMemoryDiagnostics("before-cloud-request");
  if (!persistentConnectionReady()) {
    return;
  }
  const uint32_t free_heap = ESP.getFreeHeap();
  const uint32_t largest_block = ESP.getMaxAllocHeap();
  log_info("TTS reuse before cloud request: heap=%u, largest=%u",
           static_cast<unsigned int>(free_heap),
           static_cast<unsigned int>(largest_block));
  if (free_heap < kPersistentTtsMinFreeHeap ||
      largest_block < kPersistentTtsMinLargestBlock) {
    log_warn("TTS persistent connection released before cloud request because internal heap is low");
    releasePersistentConnection();
  }
}

// 2026-09-18: Immediately release the optional warm socket when a cloud
// transport retry needs its TLS memory; the one-shot TTS path remains intact.
void TtsClient::releasePersistentConnection() {
  if (connection_mode != ConnectionMode::BIDIRECTIONAL) {
    return;
  }
  disconnect();
  log_info("TTS persistent connection released");
}

void TtsClient::connect() {
  if (webSocket.isConnected() &&
      connection_mode == ConnectionMode::UNIDIRECTIONAL) {
    return;
  }
  // 2026-09-18: A fallback request must not reuse a bidirectional socket with
  // the wrong frame protocol.
  if (webSocket.isConnected()) {
    disconnect();
  }

  if (!event_handler_ready) {
    webSocket.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
      handleWebSocketEvent(type, payload, length);
    });
    event_handler_ready = true;
  }

  connection_mode = ConnectionMode::UNIDIRECTIONAL;
  // 2026-09-18: The one-shot endpoint owns the socket only for one request;
  // persistent heartbeat belongs exclusively to the V3 bidirectional mode.
  webSocket.disableHeartbeat();
  const String request_id = generate_uuid();
  String headers = volc_speech::make_auth_headers(resource_id, request_id, true);
  // The usage-token return header is optional.  Keep only the four required
  // authentication headers so the final header block remains well-formed.
  // 2026-09-18: Record the internal contiguous heap immediately before the
  // unidirectional TLS allocation under investigation.
  logTtsMemoryDiagnostics("before-unidirectional-beginSSL");
  webSocket.beginSSL(host, 443, tts_url);
  webSocket.setExtraHeaders(headers.c_str());
  logTtsMemoryDiagnostics("after-unidirectional-beginSSL");
  // webSocket.setReconnectInterval(0);
  // 2026-09-17: Space failed TLS handshakes so WebSocketsClient does not reconnect on every 10 ms loop iteration.
  webSocket.setReconnectInterval(500);

  const unsigned long started_at = millis();
  while (!webSocket.isConnected() &&
         millis() - started_at < kTtsSocketConnectTimeoutMs) {
    webSocket.loop();
    delay(10);
  }
  webSocket.setReconnectInterval(UINT32_MAX);
  if (!webSocket.isConnected()) {
    log_error("TTS 2.0 WebSocket connection timed out");
    logTtsMemoryDiagnostics("unidirectional-connect-failed");
  }
}

void TtsClient::disconnect() {
  // 2026-09-18: Compare memory before and after releasing the provider socket
  // to detect TLS buffers that remain allocated or heap fragmentation.
  const bool had_tts_connection = connection_mode != ConnectionMode::NONE;
  if (had_tts_connection) {
    logTtsMemoryDiagnostics("before-disconnect");
  }
  // 2026-09-18: Explicit releases must not trigger WebSocketsClient's idle
  // persistent reconnect loop.
  webSocket.setReconnectInterval(UINT32_MAX);
  webSocket.disableHeartbeat();
  if (webSocket.isConnected()) {
    webSocket.disconnect();
  }
  connection_mode = ConnectionMode::NONE;
  bidirectional_connecting = false;
  bidirectional_transport_connected = false;
  bidirectional_connection_ready = false;
  bidirectional_connection_failed = false;
  bidirectional_start_requested = false;
  bidirectional_session_started = false;
  bidirectional_session_id = "";
  if (had_tts_connection) {
    logTtsMemoryDiagnostics("after-disconnect");
  }
}

// 2026-09-18: Encode the V3 event envelope used by StartConnection,
// StartSession, TaskRequest, FinishSession, and FinishConnection.
bool TtsClient::sendBidirectionalFrame(int32_t event,
                                       const String &session_id,
                                       const String &payload) {
  if (!webSocket.isConnected()) {
    return false;
  }

  const bool has_session_id = !session_id.isEmpty();
  const size_t frame_length = 4 + 4 +
      (has_session_id ? 4 + session_id.length() : 0) + 4 + payload.length();
  uint8_t *frame = new (std::nothrow) uint8_t[frame_length];
  if (frame == nullptr) {
    log_error("TTS bidirectional frame allocation failed");
    return false;
  }

  frame[0] = (PROTOCOL_VERSION << 4) | DEFAULT_HEADER_SIZE;
  frame[1] = (kFullClientRequest << 4) | kFlagWithEvent;
  frame[2] = (kSerializationJson << 4) | NO_COMPRESSION;
  frame[3] = 0;
  size_t offset = 4;
  volc_speech::write_i32_be(frame + offset, event);
  offset += 4;
  if (has_session_id) {
    volc_speech::write_u32_be(frame + offset, session_id.length());
    offset += 4;
    memcpy(frame + offset, session_id.c_str(), session_id.length());
    offset += session_id.length();
  }
  volc_speech::write_u32_be(frame + offset, payload.length());
  offset += 4;
  memcpy(frame + offset, payload.c_str(), payload.length());

  const bool sent = webSocket.sendBIN(frame, frame_length);
  delete[] frame;
  if (!sent) {
    log_error("Send TTS bidirectional event %ld failed",
              static_cast<long>(event));
    return false;
  }
  if (event == kTaskRequest) {
    latency_trace_mark(LatencyEvent::TTS_REQUEST_SENT);
  }
  return true;
}

// 2026-09-18: Open one physical WebSocket and complete StartConnection before
// queued DeepSeek sentences create sequential synthesis sessions on it.
bool TtsClient::connectBidirectional() {
  if (webSocket.isConnected() &&
      connection_mode == ConnectionMode::BIDIRECTIONAL &&
      bidirectional_connection_ready) {
    return true;
  }
  if (webSocket.isConnected()) {
    disconnect();
  }

  if (!event_handler_ready) {
    webSocket.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
      handleWebSocketEvent(type, payload, length);
    });
    event_handler_ready = true;
  }

  connection_mode = ConnectionMode::BIDIRECTIONAL;
  bidirectional_connecting = true;
  bidirectional_transport_connected = false;
  bidirectional_connection_ready = false;
  bidirectional_connection_failed = false;
  bidirectional_start_requested = false;
  bidirectional_session_started = false;
  request_failed = false;

  const String connect_id = generate_uuid();
  String headers = volc_speech::make_auth_headers(
      resource_id, connect_id, true, true);
  // 2026-09-18: Capture the same boundary for the optional V3 persistent
  // transport, even though startup warmup is currently disabled.
  logTtsMemoryDiagnostics("before-bidirectional-beginSSL");
  webSocket.beginSSL(host, 443, tts_bidirectional_url);
  webSocket.setExtraHeaders(headers.c_str());
  logTtsMemoryDiagnostics("after-bidirectional-beginSSL");
  webSocket.setReconnectInterval(500);

  const unsigned long socket_started_at = millis();
  // while (!webSocket.isConnected() && !bidirectional_connection_failed &&
  //        millis() - socket_started_at < kTtsSocketConnectTimeoutMs) {
  // 2026-09-18: Let WebSocketsClient retry for the complete bounded window;
  // its intermediate DISCONNECTED callback is not a terminal handshake error.
  while (!bidirectional_transport_connected &&
         millis() - socket_started_at < kTtsSocketConnectTimeoutMs) {
    webSocket.loop();
    delay(10);
  }
  bidirectional_connecting = false;
  if (!bidirectional_transport_connected || !webSocket.isConnected()) {
    log_error("TTS bidirectional WebSocket connection timed out");
    logTtsMemoryDiagnostics("bidirectional-connect-failed");
    disconnect();
    return false;
  }

  if (!bidirectional_start_requested) {
    // 2026-09-18: Retry once outside the WebSocket CONNECTED callback in case
    // the library was not yet ready to send a binary frame from that callback.
    bidirectional_connection_failed = false;
    if (!sendBidirectionalFrame(kStartConnection, "", "{}")) {
      disconnect();
      return false;
    }
    bidirectional_start_requested = true;
  }

  const unsigned long protocol_started_at = millis();
  while (!bidirectional_connection_ready &&
         !bidirectional_connection_failed &&
         millis() - protocol_started_at < kTtsConnectionStartTimeoutMs) {
    loop(5);
  }
  if (!bidirectional_connection_ready || bidirectional_connection_failed) {
    log_error("TTS bidirectional StartConnection failed or timed out");
    disconnect();
    return false;
  }

  // 2026-09-18: Keep the established transport alive between turns and let
  // the normal main-loop pump reconnect it during idle time.
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);
  log_info("TTS bidirectional connection is ready");
  return true;
}

// 2026-09-18: End the reusable connection only after every queued Session has
// completed; a missing ConnectionFinished response must not block cleanup.
bool TtsClient::finishBidirectionalConnection() {
  if (connection_mode != ConnectionMode::BIDIRECTIONAL ||
      !webSocket.isConnected()) {
    return true;
  }

  const bool sent = sendBidirectionalFrame(kFinishConnection, "", "{}");
  const unsigned long started_at = millis();
  while (sent && webSocket.isConnected() &&
         bidirectional_connection_ready &&
         millis() - started_at < kTtsConnectionFinishTimeoutMs) {
    loop(5);
  }
  const bool failed = bidirectional_connection_failed;
  disconnect();
  return sent && !failed;
}

void TtsClient::handleWebSocketEvent(WStype_t type, uint8_t *payload,
                                     size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      if (connection_mode == ConnectionMode::BIDIRECTIONAL) {
        // 2026-09-18: A successful retry supersedes transient handshake
        // errors emitted by earlier WebSocketsClient connection attempts.
        if (!request_active) {
          request_failed = false;
          bidirectional_connection_failed = false;
        }
        bidirectional_transport_connected = true;
        bidirectional_connecting = false;
        // 2026-09-18: An idle automatic reconnect must repeat the V3
        // StartConnection exchange before any new Session can reuse it.
        if (!request_active && !bidirectional_start_requested) {
          bidirectional_start_requested = sendBidirectionalFrame(
              kStartConnection, "", "{}");
          if (!bidirectional_start_requested) {
            bidirectional_connection_failed = true;
          }
        }
      }
      log_info("TTS 2.0 WebSocket connected");
      // 2026-09-18: Capture the post-handshake low-water mark so TLS
      // allocation can be compared with the pre-connect snapshot.
      logTtsMemoryDiagnostics(
          connection_mode == ConnectionMode::BIDIRECTIONAL
              ? "bidirectional-connected"
              : "unidirectional-connected");
      // 2026-09-18: Isolate TLS/WebSocket connection time from cloud synthesis time in the latency trace.
      latency_trace_mark(LatencyEvent::TTS_CONNECTED);
      break;
    case WStype_DISCONNECTED:
      // 2026-09-18: A bidirectional socket must remain open between Sessions;
      // unlike the one-shot endpoint, a clean TCP close is not a success
      // signal for an active Session.
      if (connection_mode == ConnectionMode::BIDIRECTIONAL) {
        bidirectional_transport_connected = false;
        bidirectional_connection_ready = false;
        bidirectional_start_requested = false;
        // 2026-09-18: During beginSSL, WebSocketsClient reports each failed
        // attempt as DISCONNECTED before its configured retry interval. Keep
        // trying until connectBidirectional reaches the real timeout.
        if (bidirectional_connecting && !request_active) {
          log_debug("TTS bidirectional connection attempt disconnected");
        } else if (request_active && !final_response_received) {
          bidirectional_connection_failed = true;
          request_failed = true;
          log_error("TTS bidirectional WebSocket disconnected during session");
        } else {
          bidirectional_connection_failed = true;
          log_debug("TTS bidirectional WebSocket disconnected");
        }
      } else if (request_active && !final_response_received) {
        const unsigned int queued_chunks = playback_queue == nullptr
                                               ? 0
                                               : uxQueueMessagesWaiting(
                                                     playback_queue);
        // This unidirectional endpoint normally closes the WebSocket after
        // sending its last audio frame. Some deployments omit both event 152
        // and a negative sequence marker, so a clean close after PCM is the
        // only end-of-stream signal. Keep draining the already queued audio.
        if (audio_bytes_received > 0 && (payload == nullptr || length == 0)) {
          // 2026-09-18: A close without a final sequence is ambiguous: it can
          // be the provider's normal one-shot lifecycle or an early network
          // loss. Drain accepted PCM, but expose the distinction in logs.
          completion_from_disconnect = !explicit_response_end_received;
          final_response_received = true;
          if (completion_from_disconnect) {
            log_warn(
                "TTS socket closed without an explicit final frame after %u "
                "PCM bytes (%u frames, %u chunks queued)",
                static_cast<unsigned int>(audio_bytes_received),
                static_cast<unsigned int>(audio_frame_count), queued_chunks);
          } else {
            log_info(
                "TTS 2.0 stream complete after %u PCM bytes "
                "(%u frames, %u chunks queued)",
                static_cast<unsigned int>(audio_bytes_received),
                static_cast<unsigned int>(audio_frame_count), queued_chunks);
          }
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
        // log_info("TTS 2.0 WebSocket disconnected");
        // 2026-09-17: Expected pre-request and post-session disconnects stay at debug level to keep the serial log readable.
        log_debug("TTS 2.0 WebSocket disconnected");
      }
      break;
    case WStype_BIN:
      if (!parseResponse(payload, length)) {
        request_failed = true;
      }
      break;
    case WStype_TEXT:
      // 2026-09-18: Treat pre-connect provider text as one failed handshake
      // attempt; an active Session still fails immediately and safely.
      if (connection_mode == ConnectionMode::BIDIRECTIONAL &&
          bidirectional_connecting && !request_active) {
        bidirectional_transport_connected = false;
        bidirectional_connection_ready = false;
        log_debug("TTS bidirectional connection attempt returned text: %.*s",
                  static_cast<int>(length),
                  reinterpret_cast<const char *>(payload));
        break;
      }
      request_failed = true;
      if (connection_mode == ConnectionMode::BIDIRECTIONAL) {
        bidirectional_transport_connected = false;
        bidirectional_connection_ready = false;
        bidirectional_connection_failed = true;
      }
      log_error("TTS 2.0 text error: %.*s", static_cast<int>(length),
                reinterpret_cast<const char *>(payload));
      break;
    case WStype_ERROR:
      // 2026-09-18: WebSocketsClient can emit ERROR before its scheduled
      // reconnect. Keep the bounded connect loop alive until a later success
      // or the real timeout; errors after connection remain terminal.
      if (connection_mode == ConnectionMode::BIDIRECTIONAL &&
          bidirectional_connecting && !request_active) {
        bidirectional_transport_connected = false;
        bidirectional_connection_ready = false;
        log_debug("TTS bidirectional connection attempt reported an error");
        break;
      }
      request_failed = true;
      if (connection_mode == ConnectionMode::BIDIRECTIONAL) {
        bidirectional_connection_ready = false;
        bidirectional_connection_failed = true;
      }
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
  } else {
    // 2026-09-18: Mark the first successfully submitted TTS request before waiting for PCM.
    latency_trace_mark(LatencyEvent::TTS_REQUEST_SENT);
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
    // String segment = nextTtsSegment(text, offset);
    // 2026-09-17: The non-streaming fallback follows the same punctuation-based policy as DeepSeek streaming TTS.
    String segment = nextSemanticTtsSegment(text, offset);
    if (segment.isEmpty()) {
      continue;
    }

    ++segment_number;
    log_info("TTS segment %u: %u text bytes", segment_number,
             static_cast<unsigned int>(segment.length()));
    // if (!synthesizeSegment(segment, segment_number)) {
    // 2026-09-17: Recover the non-streaming fallback from connections that close before returning PCM.
    if (!synthesizeSegmentWithRetry(segment, segment_number)) {
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
  if (!wait_for_playback_complete()) {
    stop_play();
    return false;
  }
  stop_play();
  log_info("TTS complete: %u segment(s)", segment_number);
  return true;
}

// 2026-09-17: Prepare one waiting TTS consumer so SSE reception only queues text and never competes for a second TLS connection.
bool TtsClient::beginSentenceStream() {
  if (sentence_queue != nullptr || !sentence_stream_finished) {
    log_error("TTS sentence stream is already active");
    return false;
  }
  if (!speaker_is_ready()) {
    log_error("TTS sentence stream cannot start because speaker I2S is not ready");
    return false;
  }

  sentence_queue = xQueueCreate(kSentenceQueueDepth, sizeof(SentenceChunk));
  if (sentence_queue == nullptr) {
    log_error("TTS sentence queue allocation failed");
    return false;
  }

  sentence_stream_failed = false;
  sentence_stream_finished = false;
  sentence_stream_accepting = true;
  sentence_stream_released = false;
  sentence_stream_count = 0;
  sentence_stack_high_water = 0;
  if (xTaskCreate(sentenceTaskEntry, "TtsSentenceTask", 8192, this, 2,
                  &sentence_task) != pdPASS) {
    vQueueDelete(sentence_queue);
    sentence_queue = nullptr;
    sentence_stream_accepting = false;
    sentence_stream_finished = true;
    log_error("TTS sentence task creation failed");
    return false;
  }
  // 2026-09-18: Measure queue/task allocation overhead before the LLM starts
  // its HTTPS request; this identifies board-side fragmentation separately
  // from provider TLS allocation.
  logTtsMemoryDiagnostics("after-sentence-stream-allocation");
  return true;
}

// 2026-09-17: Copy each natural phrase into owned memory before crossing from the LLM task to the TTS task.
bool TtsClient::enqueueSentence(const String &sentence) {
  // if (!sentence_stream_accepting || sentence_stream_failed ||
  //     sentence_queue == nullptr) {
  // 2026-09-18: A provider failure must not abort DeepSeek SSE parsing;
  // continue draining text while the consumer attempts its safe fallback.
  if (!sentence_stream_accepting || sentence_queue == nullptr) {
    return false;
  }

  String cleaned = sentence;
  cleaned.trim();
  if (cleaned.isEmpty()) {
    return true;
  }

  const size_t allocation_size = cleaned.length() + 1;
  char *copy = nullptr;
  bool allocated_in_psram = false;
  if (psramFound()) {
    copy = static_cast<char *>(heap_caps_malloc(
        allocation_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    allocated_in_psram = copy != nullptr;
  }
  if (copy == nullptr) {
    copy = static_cast<char *>(
        heap_caps_malloc(allocation_size, MALLOC_CAP_8BIT));
  }
  if (copy == nullptr) {
    sentence_stream_failed = true;
    log_error("TTS sentence allocation failed for %u bytes",
              static_cast<unsigned int>(allocation_size));
    return false;
  }
  memcpy(copy, cleaned.c_str(), allocation_size);
  // 2026-09-18: Make the sentence-buffer allocation pool explicit while
  // measuring whether internal RAM is being used unnecessarily.
  log_info("TTS sentence buffer: bytes=%u pool=%s",
           static_cast<unsigned int>(allocation_size),
           allocated_in_psram ? "PSRAM" : "internal");

  const SentenceChunk chunk = {copy};
  // if (xQueueSend(sentence_queue, &chunk, portMAX_DELAY) != pdTRUE) {
  // 2026-09-17: Never deadlock SSE reception if a malformed reply exceeds the bounded pre-release phrase queue.
  if (xQueueSend(sentence_queue, &chunk, 0) != pdTRUE) {
    heap_caps_free(copy);
    sentence_stream_failed = true;
    log_error("TTS sentence queue send failed");
    return false;
  }
  return true;
}

// 2026-09-17: Wake the TTS consumer only after the DeepSeek socket is closed, preventing two simultaneous TLS handshakes.
bool TtsClient::releaseSentenceStream() {
  if (sentence_stream_released) {
    return true;
  }
  if (sentence_queue == nullptr || sentence_task == nullptr ||
      sentence_stream_finished) {
    return false;
  }
  sentence_stream_released = true;
  xTaskNotifyGive(sentence_task);
  return true;
}

// 2026-09-18: Close the producer side and wait until every complete-answer segment and final DMA sample is played.
bool TtsClient::finishSentenceStream() {
  if (sentence_queue == nullptr) {
    return false;
  }

  sentence_stream_accepting = false;
  // 2026-09-17: Malformed/error paths may not receive the LLM ready callback, so cleanup must also release the waiting consumer.
  if (!releaseSentenceStream()) {
    sentence_stream_failed = true;
    log_error("TTS sentence stream could not release its consumer task");
  }
  const SentenceChunk end_marker = {nullptr};
  if (xQueueSend(sentence_queue, &end_marker, portMAX_DELAY) != pdTRUE) {
    sentence_stream_failed = true;
    log_error("TTS sentence stream end marker failed");
  }

  // const unsigned long wait_started_at = millis();
  // while (!sentence_stream_finished &&
  //        millis() - wait_started_at < kSentenceStreamFinishTimeoutMs) {
  //   delay(5);
  // }
  // if (!sentence_stream_finished) {
  //   sentence_stream_failed = true;
  //   log_error("TTS sentence stream finish timed out");
  //   return false;
  // }
  // 2026-09-17: Do not return while the worker still owns WebSocketsClient; each synthesis request already has a bounded timeout.
  while (!sentence_stream_finished) {
    delay(5);
  }

  const bool success = !sentence_stream_failed && sentence_stream_count > 0;
  // 2026-09-18: Report the worker's minimum remaining stack once the complete
  // answer and playback path have finished. The value uses FreeRTOS units as
  // returned by this Arduino-ESP32 build.
  log_info("STACK TtsSentenceTask: high_water=%u configured=8192",
           static_cast<unsigned int>(sentence_stack_high_water));
  vQueueDelete(sentence_queue);
  sentence_queue = nullptr;
  sentence_task = nullptr;
  if (success) {
    // log_info("TTS complete: %u streamed sentence(s)",
    //          static_cast<unsigned int>(sentence_stream_count));
    // 2026-09-18: Report actual TTS request segments after switching from SSE sentence fragments to complete answers.
    log_info("TTS complete: %u answer segment(s)",
             static_cast<unsigned int>(sentence_stream_count));
  } else if (sentence_stream_count == 0) {
    log_error("TTS sentence stream received no speakable text");
  }
  return success;
}

// 2026-09-18: Run the reusable Connection and sequential Sessions on the
// dedicated sentence task so network reception never blocks the main loop.
void TtsClient::sentenceTaskEntry(void *parameter) {
  static_cast<TtsClient *>(parameter)->sentenceStreamLoop();
}

// 2026-09-18: Consume phrases in order and release every cross-task allocation
// even after a bidirectional Session or fallback request fails.
void TtsClient::sentenceStreamLoop() {
  // 2026-09-18: Keep the release gate so no TTS Session starts while DeepSeek
  // is generating. The optional idle V3 socket is retained only after the
  // internal-heap guard confirms another TLS request can coexist with it.
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  // 2026-09-18: Prefer the already-warm V3 physical connection. If startup
  // warmup or cross-turn reuse is unavailable, retain the proven one-shot
  // request without waiting here for another bidirectional handshake.
  bool bidirectional_available = persistentConnectionReady();
  SentenceChunk fast_chunk = {nullptr};
  while (xQueueReceive(sentence_queue, &fast_chunk, portMAX_DELAY) == pdTRUE) {
    if (fast_chunk.text == nullptr) {
      break;
    }

    ++sentence_stream_count;
    const String sentence(fast_chunk.text);
    heap_caps_free(fast_chunk.text);
    log_info("TTS fast answer %u (%u bytes): %s",
             static_cast<unsigned int>(sentence_stream_count),
             static_cast<unsigned int>(sentence.length()), sentence.c_str());
    bool segment_success = false;
    if (bidirectional_available) {
      // 2026-09-18: A warm Session gets one attempt. If it produced no audio,
      // switch immediately to the proven one-shot endpoint instead of adding
      // multiple V3 reconnect timeouts to the user's wait.
      segment_success = synthesizeBidirectionalSegment(
          sentence, sentence_stream_count);
    }

    // 2026-09-18: Fall back only before any PCM was accepted; replaying after
    // partial speech would duplicate words the user already heard.
    if (!segment_success &&
        (!bidirectional_available || audio_bytes_received == 0)) {
      if (bidirectional_available) {
        if (playback_queue != nullptr) {
          finishPlayback(false);
          stop_play();
        }
        releasePersistentConnection();
        bidirectional_available = false;
        log_warn("TTS persistent Session unavailable; using one-shot fallback");
      }
      segment_success = synthesizeSegmentWithRetry(
          sentence, sentence_stream_count);
    }

    if (!segment_success) {
      sentence_stream_failed = true;
      log_error("TTS fast answer %u failed",
                static_cast<unsigned int>(sentence_stream_count));
    }
  }

  // 2026-09-18: V3 Sessions share one playback queue for this answer while
  // the physical WebSocket remains open for the next dialogue turn.
  if (playback_queue != nullptr && !finishPlayback(true)) {
    sentence_stream_failed = true;
  }

  // 2026-09-18: Retain the bidirectional multi-segment implementation for
  // longer-answer experiments, but keep it inactive for the one-sentence
  // low-latency direct path.
#if 0
  // 2026-09-18: After DeepSeek closes, establish one reusable connection for
  // every queued sentence Session in this answer.
  bool bidirectional_available = connectBidirectional();
  if (!bidirectional_available) {
    log_warn("TTS bidirectional connection unavailable; using one-shot fallback");
  }

  SentenceChunk chunk = {nullptr};
  while (xQueueReceive(sentence_queue, &chunk, portMAX_DELAY) == pdTRUE) {
    if (chunk.text == nullptr) {
      break;
    }

    // if (!sentence_stream_failed) {
#if 0
    ++sentence_stream_count;
    const String previous_sentence(chunk.text);
    log_info("TTS streamed sentence %u (%u bytes): %s",
             static_cast<unsigned int>(sentence_stream_count),
             static_cast<unsigned int>(previous_sentence.length()),
             previous_sentence.c_str());
    if (!synthesizeSegmentWithRetry(previous_sentence,
                                    sentence_stream_count)) {
      sentence_stream_failed = true;
      log_error("TTS streamed sentence %u failed; continuing queued speech",
                static_cast<unsigned int>(sentence_stream_count));
    }
    heap_caps_free(chunk.text);
#endif
    // 2026-09-18: Prefer one complete-answer request and create a second request only for a balanced, unusually long answer.
    const String answer(chunk.text);
    heap_caps_free(chunk.text);
    size_t answer_offset = 0;
    while (answer_offset < answer.length()) {
      String segment = nextSemanticTtsSegment(answer, answer_offset);
      if (segment.isEmpty()) {
        continue;
      }

      ++sentence_stream_count;
      log_info("TTS answer segment %u (%u bytes): %s",
               static_cast<unsigned int>(sentence_stream_count),
               static_cast<unsigned int>(segment.length()), segment.c_str());
      bool segment_success = false;
      if (bidirectional_available) {
        segment_success = synthesizeBidirectionalSegmentWithRetry(
            segment, sentence_stream_count);
      }

      // 2026-09-18: Fall back only before any PCM from this sentence was
      // accepted, otherwise retrying through the old endpoint would repeat
      // speech the user already heard.
      if (!segment_success &&
          (!bidirectional_available || audio_bytes_received == 0)) {
        if (connection_mode == ConnectionMode::BIDIRECTIONAL) {
          finishBidirectionalConnection();
        }
        bidirectional_available = false;
        if (playback_queue != nullptr) {
          finishPlayback(true);
          wait_for_playback_complete();
          stop_play();
        }
        log_warn("TTS segment %u switching to one-shot fallback",
                 static_cast<unsigned int>(sentence_stream_count));
        segment_success = synthesizeSegmentWithRetry(
            segment, sentence_stream_count);
      }

      if (!segment_success) {
        sentence_stream_failed = true;
        log_error("TTS answer segment %u failed; continuing queued speech",
                  static_cast<unsigned int>(sentence_stream_count));
      }
    }
  }

  if (connection_mode == ConnectionMode::BIDIRECTIONAL &&
      !finishBidirectionalConnection()) {
    log_warn("TTS bidirectional connection did not close cleanly");
  }
  // 2026-09-18: One playback queue spans all bidirectional Sessions, allowing
  // the next sentence to synthesize while the prior sentence is still heard.
  if (playback_queue != nullptr) {
    if (!finishPlayback(true)) {
      sentence_stream_failed = true;
    }
  }
#endif
  if (sentence_stream_count > 0 && !wait_for_playback_complete()) {
    sentence_stream_failed = true;
  }
  stop_play();
  // 2026-09-18: Store the task's lifetime low-water mark before self-delete;
  // finishSentenceStream() prints it from the main task.
  sentence_stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
  sentence_task = nullptr;
  sentence_stream_finished = true;
  vTaskDelete(nullptr);
}

// 2026-09-18: Run one sentence as a Session on the reusable bidirectional
// connection. Audio is appended to the stream-wide playback queue.
bool TtsClient::synthesizeBidirectionalSegment(const String &text,
                                               uint16_t segment_number) {
  resetResponseTracking();
  bidirectional_session_started = false;

  const bool connection_reused = persistentConnectionReady();
  if (!connectBidirectional()) {
    return false;
  }
  if (connection_reused) {
    // 2026-09-18: Make reuse visible in both the serial log and latency trace;
    // this milestone should be effectively zero after LLM_DONE.
    log_info("TTS persistent connection reused");
    latency_trace_mark(LatencyEvent::TTS_CONNECTED);
  }
  if (!speaker_is_ready()) {
    log_error("TTS cannot play because speaker I2S is not ready");
    return false;
  }
  if (playback_queue == nullptr && !startPlayback()) {
    return false;
  }

  bidirectional_session_id = generate_uuid();
  JsonDocument start_document;
  start_document["user"]["uid"] = "desk-emoji";
  start_document["event"] = kStartSession;
  start_document["namespace"] = "BidirectionalTTS";
  JsonObject start_request =
      start_document["req_params"].to<JsonObject>();
  start_request["speaker"] = voice_type;
  JsonObject audio = start_request["audio_params"].to<JsonObject>();
  audio["format"] = "pcm";
  audio["sample_rate"] = kTtsSampleRate;
  audio["speech_rate"] = 0;
  audio["loudness_rate"] = 0;
  String start_json;
  serializeJson(start_document, start_json);

  request_active = true;
  if (!sendBidirectionalFrame(kStartSession, bidirectional_session_id,
                              start_json)) {
    request_active = false;
    disconnect();
    return false;
  }

  const unsigned long session_started_at = millis();
  while (!bidirectional_session_started && !request_failed &&
         millis() - session_started_at < kTtsSessionStartTimeoutMs) {
    loop(5);
  }
  if (!bidirectional_session_started || request_failed) {
    log_error("TTS bidirectional Session %u failed to start",
              static_cast<unsigned int>(segment_number));
    request_active = false;
    disconnect();
    return false;
  }

  JsonDocument task_document;
  task_document["user"]["uid"] = "desk-emoji";
  task_document["event"] = kTaskRequest;
  task_document["namespace"] = "BidirectionalTTS";
  task_document["req_params"]["text"] = text;
  String task_json;
  serializeJson(task_document, task_json);
  if (!sendBidirectionalFrame(kTaskRequest, bidirectional_session_id,
                              task_json) ||
      !sendBidirectionalFrame(kFinishSession, bidirectional_session_id,
                              "{}")) {
    request_active = false;
    disconnect();
    return false;
  }

  const unsigned long response_started_at = millis();
  while (!final_response_received && !request_failed &&
         millis() - response_started_at < 60000) {
    loop(5);
  }

  const bool success = final_response_received &&
                       explicit_response_end_received && !request_failed &&
                       !audio_sequence_gap && audio_bytes_received > 0;
  if (!final_response_received && !request_failed) {
    log_error("TTS bidirectional Session %u timed out",
              static_cast<unsigned int>(segment_number));
    disconnect();
  } else if (final_response_received && !request_failed &&
             audio_bytes_received == 0) {
    log_error("TTS bidirectional Session %u returned no PCM audio",
              static_cast<unsigned int>(segment_number));
  }

  request_active = false;
  bidirectional_session_started = false;
  bidirectional_session_id = "";
  if (success) {
    log_info("TTS bidirectional segment %u complete (%u PCM bytes, %u frames)",
             static_cast<unsigned int>(segment_number),
             static_cast<unsigned int>(audio_bytes_received),
             static_cast<unsigned int>(audio_frame_count));
  }
  return success;
}

// 2026-09-18: Retry only Sessions that produced no PCM, using a fresh
// session_id each time so provider-side deduplication cannot suppress audio.
bool TtsClient::synthesizeBidirectionalSegmentWithRetry(
    const String &text, uint16_t segment_number) {
  for (uint8_t attempt = 1; attempt <= kTtsZeroAudioMaxAttempts; ++attempt) {
    if (synthesizeBidirectionalSegment(text, segment_number)) {
      return true;
    }
    if (audio_bytes_received > 0 || attempt == kTtsZeroAudioMaxAttempts) {
      return false;
    }
    log_warn("TTS bidirectional sentence %u returned no PCM; retrying (%u/%u, "
             "backoff=%lu ms)",
             static_cast<unsigned int>(segment_number),
             static_cast<unsigned int>(attempt + 1),
             static_cast<unsigned int>(kTtsZeroAudioMaxAttempts),
             static_cast<unsigned long>(kTtsRetryBackoffMs * attempt));
    delay(kTtsRetryBackoffMs * attempt);
  }
  return false;
}

bool TtsClient::synthesizeSegment(const String &text,
                                  uint16_t segment_number) {

  resetResponseTracking();

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
                 !audio_sequence_gap && audio_bytes_received > 0;
  if (!final_response_received && !request_failed) {
    log_error("TTS 2.0 final response wait timeout");
  } else if (final_response_received && !request_failed &&
             audio_bytes_received == 0) {
    log_error("TTS 2.0 returned no PCM audio");
  }

  request_active = false;
  success = finishPlayback(success) && success;
  if (success) {
    const uint32_t pcm_duration_ms = static_cast<uint32_t>(
        (static_cast<uint64_t>(audio_bytes_received) * 1000ULL) /
        (kTtsSampleRate * sizeof(int16_t)));
    log_info(
        "TTS segment %u END (%u PCM bytes, %u ms, %u frames, "
        "completion=%s, seq_last=%u, gap=%u)",
        segment_number, static_cast<unsigned int>(audio_bytes_received),
        static_cast<unsigned int>(pcm_duration_ms),
        static_cast<unsigned int>(audio_frame_count),
        explicit_response_end_received ? "explicit" : "socket-close",
        static_cast<unsigned int>(last_audio_sequence),
        audio_sequence_gap ? 1U : 0U);
  } else {
    // 2026-09-18: Keep a compact failed-attempt summary so a no-audio,
    // sequence-gap, or I2S failure can be diagnosed without guessing from
    // the retry line alone.
    const uint32_t pcm_duration_ms = static_cast<uint32_t>(
        (static_cast<uint64_t>(audio_bytes_received) * 1000ULL) /
        (kTtsSampleRate * sizeof(int16_t)));
    log_warn(
        "TTS segment %u FAILED (%u PCM bytes, %u ms, %u frames, "
        "explicit=%u, seq_last=%u, gap=%u)",
        segment_number, static_cast<unsigned int>(audio_bytes_received),
        static_cast<unsigned int>(pcm_duration_ms),
        static_cast<unsigned int>(audio_frame_count),
        explicit_response_end_received ? 1U : 0U,
        static_cast<unsigned int>(last_audio_sequence),
        audio_sequence_gap ? 1U : 0U);
  }
  disconnect();
  return success;
}

// 2026-09-17: Retry a sentence only when the failed attempt delivered zero PCM bytes, so already-spoken audio is never duplicated.
bool TtsClient::synthesizeSegmentWithRetry(const String &text,
                                           uint16_t segment_number) {
  for (uint8_t attempt = 1; attempt <= kTtsZeroAudioMaxAttempts; ++attempt) {
    if (synthesizeSegment(text, segment_number)) {
      return true;
    }
    if (audio_bytes_received > 0 || attempt == kTtsZeroAudioMaxAttempts) {
      return false;
    }

    log_warn("TTS sentence %u returned no PCM; retrying (%u/%u, backoff=%lu ms)",
             static_cast<unsigned int>(segment_number),
             static_cast<unsigned int>(attempt + 1),
             static_cast<unsigned int>(kTtsZeroAudioMaxAttempts),
             static_cast<unsigned long>(kTtsRetryBackoffMs * attempt));
    delay(kTtsRetryBackoffMs * attempt);
  }
  return false;
}

#if 0
// 2026-09-18: Preserve the gateway-only PCM bridge without compiling it in
// board-direct mode. The direct TTS path below remains unchanged.
// 2026-09-18: Let the gateway use the same bounded playback queue without opening a second provider WebSocket on the ESP32.
bool TtsClient::beginExternalStream() {
  if (!speaker_is_ready()) {
    log_error("Gateway audio cannot start because speaker I2S is not ready");
    return false;
  }
  // 2026-09-18: Clear the previous gateway turn's drain state before preparing playback.
  external_audio_received_ = false;
  return startPlayback();
}

// 2026-09-18: Timestamp and enqueue the first gateway PCM frame using the existing 8 kHz-to-16 kHz playback path.
bool TtsClient::queueExternalPcm(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0 || (length & 1) != 0) {
    return false;
  }
  latency_trace_mark(LatencyEvent::TTS_FIRST_AUDIO);
  // return queuePcm16Le(data, length);
  // 2026-09-18: Record only PCM successfully accepted into the playback queue.
  const bool queued = queuePcm16Le(data, length);
  external_audio_received_ = external_audio_received_ || queued;
  return queued;
}

// 2026-09-18: Drain every received gateway PCM frame even after a partial failure, without replaying already spoken text.
bool TtsClient::finishExternalStream(bool complete) {
  if (playback_queue == nullptr) {
    return false;
  }
  // const bool finished = finishPlayback(complete);
  // 2026-09-18: A gateway disconnect must not cut off audio already accepted into the playback queue.
  const bool drain_received_audio = complete || external_audio_received_;
  const bool finished = finishPlayback(drain_received_audio);
  // if (finished && complete) {
  bool dma_drained = true;
  if (finished && drain_received_audio) {
    dma_drained = wait_for_playback_complete();
  }
  stop_play();
  return finished && dma_drained && complete;
}
#endif  // 2026-09-18: gateway-only TTS bridge disabled.

bool TtsClient::startPlayback() {
  if (playback_queue != nullptr || !playback_finished) {
    log_error("TTS playback task is already active");
    return false;
  }

  playback_abort = false;
  playback_failed = false;
  playback_finished = false;
  // 2026-09-18: Reset per-attempt PCM and resource counters before the
  // playback task starts accepting provider frames.
  playback_bytes_queued = 0;
  playback_bytes_played = 0;
  playback_bytes_discarded = 0;
  playback_psram_bytes = 0;
  playback_internal_bytes = 0;
  playback_chunks_queued = 0;
  playback_chunks_played = 0;
  playback_chunks_discarded = 0;
  playback_queue_peak = 0;
  playback_stack_high_water = 0;
  const UBaseType_t queue_depth = psramFound()
                                      ? kPlaybackQueueDepthWithPsram
                                      : kPlaybackQueueDepthWithoutPsram;
  playback_queue_capacity = queue_depth;
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
  // 2026-09-18: Record the RAM consumed by the PCM queue and playback task.
  logTtsMemoryDiagnostics("after-playback-allocation");
  return true;
}

bool TtsClient::queuePcm16Le(const uint8_t *data, size_t length) {
  if (playback_queue == nullptr || data == nullptr || length == 0) {
    return false;
  }

  uint8_t *copy = nullptr;
  bool allocated_in_psram = false;
  if (psramFound()) {
    copy = static_cast<uint8_t *>(
        heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    allocated_in_psram = copy != nullptr;
  }
  if (copy == nullptr) {
    copy = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_8BIT));
  }
  if (copy == nullptr) {
    // 2026-09-18: Count provider PCM that could not enter the playback queue
    // as discarded so the final summary remains conservation-of-bytes based.
    playback_bytes_discarded += length;
    ++playback_chunks_discarded;
    logTtsMemoryDiagnostics("pcm-allocation-failed");
    log_error("TTS PCM queue allocation failed for %u bytes",
              static_cast<unsigned int>(length));
    return false;
  }

  memcpy(copy, data, length);
  const PcmChunk chunk = {copy, length};
  if (xQueueSend(playback_queue, &chunk, kPlaybackQueueWait) != pdTRUE) {
    heap_caps_free(copy);
    // 2026-09-18: Count a full-queue drop as discarded PCM so the final
    // playback summary cannot mistake it for a provider truncation.
    playback_bytes_discarded += length;
    ++playback_chunks_discarded;
    log_error("TTS playback queue remained full");
    return false;
  }
  // 2026-09-18: Account only successfully queued PCM and expose whether
  // payload storage really used PSRAM or fell back to scarce internal RAM.
  playback_bytes_queued += length;
  ++playback_chunks_queued;
  if (allocated_in_psram) {
    playback_psram_bytes += length;
  } else {
    playback_internal_bytes += length;
  }
  const UBaseType_t queued_now = uxQueueMessagesWaiting(playback_queue);
  if (queued_now > playback_queue_peak) {
    playback_queue_peak = queued_now;
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

  // 2026-09-18: Keep the task-stack measurement searchable independently of
  // the PCM conservation summary below.
  log_info("STACK TtsPlaybackTask: high_water=%u configured=4096",
           static_cast<unsigned int>(playback_stack_high_water));
  // 2026-09-18: One summary proves whether every accepted provider byte was
  // written to I2S, and reports queue/stack headroom for right-sizing tasks.
  const uint32_t queued_duration_ms = static_cast<uint32_t>(
      (static_cast<uint64_t>(playback_bytes_queued) * 1000ULL) /
      (kTtsSampleRate * sizeof(int16_t)));
  const uint32_t played_duration_ms = static_cast<uint32_t>(
      (static_cast<uint64_t>(playback_bytes_played) * 1000ULL) /
      (kTtsSampleRate * sizeof(int16_t)));
  log_info(
      "TTS PLAYBACK: queued=%u/%u chunks, played=%u/%u, discarded=%u/%u, "
      "duration_ms=%u/%u, queue_peak=%u/%u, psram=%u, internal=%u, "
      "stack_hwm=%u, stack_configured=4096, failed=%u",
      static_cast<unsigned int>(playback_bytes_queued),
      static_cast<unsigned int>(playback_chunks_queued),
      static_cast<unsigned int>(playback_bytes_played),
      static_cast<unsigned int>(playback_chunks_played),
      static_cast<unsigned int>(playback_bytes_discarded),
      static_cast<unsigned int>(playback_chunks_discarded),
      static_cast<unsigned int>(queued_duration_ms),
      static_cast<unsigned int>(played_duration_ms),
      static_cast<unsigned int>(playback_queue_peak),
      static_cast<unsigned int>(playback_queue_capacity),
      static_cast<unsigned int>(playback_psram_bytes),
      static_cast<unsigned int>(playback_internal_bytes),
      static_cast<unsigned int>(playback_stack_high_water),
      playback_failed ? 1U : 0U);

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

    if (!playback_abort) {
      if (playPcm16Le(chunk.data, chunk.length)) {
        playback_bytes_played += chunk.length;
        ++playback_chunks_played;
      } else {
        playback_failed = true;
        playback_abort = true;
        playback_bytes_discarded += chunk.length;
        ++playback_chunks_discarded;
      }
    } else {
      playback_bytes_discarded += chunk.length;
      ++playback_chunks_discarded;
    }
    heap_caps_free(chunk.data);
  }

  // 2026-09-18: Store the playback task's lifetime low-water mark before it
  // self-deletes; finishPlayback() prints the value after synchronization.
  playback_stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
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
  String response_session_id;
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
      response_session_id = String(
          reinterpret_cast<const char *>(frame + offset), id_length);
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
  const bool session_matches = response_session_id.isEmpty() ||
      bidirectional_session_id.isEmpty() ||
      response_session_id == bidirectional_session_id;

  // 2026-09-18: Drive the bidirectional Connection/Session state machine from
  // official V3 server events before routing any PCM payload.
  if (event == kConnectionStarted) {
    bidirectional_connection_ready = true;
    bidirectional_connection_failed = false;
  } else if (event == kConnectionFinished) {
    bidirectional_connection_ready = false;
  } else if (event == kSessionStarted && session_matches) {
    bidirectional_session_started = true;
  }

  if (connection_mode == ConnectionMode::BIDIRECTIONAL &&
      !session_matches) {
    log_warn("Ignoring TTS event %ld for an inactive session",
             static_cast<long>(event));
  } else if (message_type == kAudioOnlyServer) {
    // Audio-only responses carry the PCM payload after the sequence number;
    // they normally do not include event 352.  Accept both the sequenced
    // frames and the event-tagged form used by different service revisions.
    if (response_length > 0) {
      set_led(COLOR_GREEN, 50);
      // 2026-09-17: Timestamp the first valid PCM payload before queueing it for the playback task.
      const bool first_audio_frame = audio_bytes_received == 0;
      if (first_audio_frame) {
        latency_trace_mark(LatencyEvent::TTS_FIRST_AUDIO);
      }
      success = queuePcm16Le(response_payload, response_length);
      if (success) {
        // 2026-09-18: Verify monotonic PCM sequence numbers when the provider
        // supplies them, allowing an otherwise silent missing frame to fail
        // the Session instead of being reported as complete.
        if (hasSequenceNumber(flags)) {
          const uint32_t sequence_index = sequence < 0
              ? static_cast<uint32_t>(-static_cast<int64_t>(sequence))
              : static_cast<uint32_t>(sequence);
          if (audio_sequence_seen &&
              sequence_index != last_audio_sequence + 1U) {
            audio_sequence_gap = true;
            log_error("TTS PCM sequence gap: expected %u, received %u",
                      static_cast<unsigned int>(last_audio_sequence + 1U),
                      static_cast<unsigned int>(sequence_index));
          }
          audio_sequence_seen = true;
          last_audio_sequence = sequence_index;
        }
        ++audio_frame_count;
        // const bool first_audio_frame = audio_bytes_received == 0;
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
    // if (flags == kFlagLastSequence || flags == kFlagNegativeSequence ||
    //     (hasSequenceNumber(flags) && sequence < 0)) {
    // 2026-09-18: Bidirectional reuse must wait for SessionFinished before
    // opening the next Session, even if an audio frame marks its own end.
    if (connection_mode != ConnectionMode::BIDIRECTIONAL &&
        (flags == kFlagLastSequence || flags == kFlagNegativeSequence ||
         (hasSequenceNumber(flags) && sequence < 0))) {
      explicit_response_end_received = true;
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
    if (event == kConnectionFailed ||
        (connection_mode == ConnectionMode::BIDIRECTIONAL &&
         message_type == kErrorResponse)) {
      bidirectional_connection_ready = false;
      bidirectional_connection_failed = true;
    }
    final_response_received = true;
    success = false;
  } else if (message_type == kFullServerResponse &&
             event == kSessionFinished) {
    // 2026-09-18: V3 SessionFinished is the authoritative completion signal;
    // the physical WebSocket remains reusable for the next dialogue turn.
    explicit_response_end_received = true;
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
    // 2026-09-18: Propagate a partial/failed I2S write to the playback queue
    // owner so the Session cannot be reported as fully played.
    if (!play(samples, input_sample_count * 2, volume)) {
      return false;
    }
    byte_offset += input_sample_count * sizeof(int16_t);
  }
  return true;
}
