// 2026-09-19: Implement reusable V3 TTS Sessions so synthesis/playback can overlap DeepSeek SSE without reopening TLS each turn.
#include "tts_client.h"

#include <algorithm>
#include <cstring>

#include "app_config.h"
#include "audio.h"
#include "cJSON.h"
// 2026-09-20: Drive the green status light from PCM playback so it tracks audible output instead of network packet timing.
#include "device_ui.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
// 2026-09-20: Timestamp the first decoded PCM before a queue handoff can wake playback on the other core.
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "latency_trace.h"
#include "memory_diagnostics.h"
#include "volc_speech_protocol.h"

namespace desk_talk::cloud {
namespace {

constexpr char kLogTag[] = "tts";
constexpr char kEndpoint[] =
    "wss://openspeech.bytedance.com/api/v3/tts/bidirection";
constexpr char kResourceId[] = "seed-tts-2.0";
constexpr char kVoice[] = "zh_male_naiqimengwa_uranus_bigtts";
constexpr uint32_t kConnectedBit = BIT0;
constexpr uint32_t kConnectionStartedBit = BIT1;
constexpr uint32_t kSessionStartedBit = BIT2;
constexpr uint32_t kSessionFinishedBit = BIT3;
constexpr uint32_t kFailureBit = BIT4;
constexpr uint32_t kTurnFinishedBit = BIT5;
constexpr uint8_t kFullClientRequest = 0x01;
constexpr uint8_t kFullServerResponse = 0x09;
constexpr uint8_t kAudioOnlyServer = 0x0B;
constexpr uint8_t kErrorResponse = 0x0F;
constexpr uint8_t kFlagWithEvent = 0x04;
constexpr uint8_t kSerializationJson = 0x01;
constexpr uint8_t kCompressionGzip = 0x01;
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
constexpr size_t kSentenceQueueDepth = 24;
constexpr size_t kPcmQueueDepth = 48;
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kProtocolTimeoutMs = 6000;
constexpr uint32_t kSessionTimeoutMs = 60000;
constexpr float kVolume = 0.30F;

bool event_has_session_id(int32_t event) {
  return event != kConnectionStarted && event != kConnectionFailed &&
         event != kConnectionFinished;
}

bool event_has_connection_id(int32_t event) {
  return event == kConnectionStarted || event == kConnectionFailed ||
         event == kConnectionFinished;
}

bool has_sequence(uint8_t flags) {
  return flags == 0x01 || flags == 0x02 || flags == 0x03;
}

bool wait_bit(EventGroupHandle_t group, EventBits_t bit, uint32_t timeout_ms) {
  const EventBits_t result = xEventGroupWaitBits(
      group, bit | kFailureBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
  return (result & bit) != 0 && (result & kFailureBit) == 0;
}

char *copy_to_psram(const std::string &text) {
  void *memory = heap_caps_malloc(text.size() + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == nullptr) {
    memory = heap_caps_malloc(text.size() + 1,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (memory != nullptr) {
    std::memcpy(memory, text.c_str(), text.size() + 1);
  }
  return static_cast<char *>(memory);
}

}  // namespace

TtsClient &tts_client() {
  static TtsClient instance;
  return instance;
}

bool TtsClient::initialize() {
  if (events_ != nullptr) {
    return sentence_task_ != nullptr && playback_task_ != nullptr;
  }
  events_ = xEventGroupCreate();
  sentence_queue_ = xQueueCreate(kSentenceQueueDepth, sizeof(SentenceItem));
  pcm_queue_ = xQueueCreate(kPcmQueueDepth, sizeof(PcmItem));
  connection_mutex_ = xSemaphoreCreateMutex();
  if (events_ == nullptr || sentence_queue_ == nullptr ||
      pcm_queue_ == nullptr || connection_mutex_ == nullptr) {
    ESP_LOGE(kLogTag, "Unable to allocate TTS synchronization objects");
    return false;
  }
  // 2026-09-19: Keep 12 KiB of long-lived TTS worker stacks out of internal RAM so concurrent TLS has a larger contiguous pool.
  if (xTaskCreateWithCaps(sentence_task_entry, "tts_sessions", 8192, this, 4,
                          &sentence_task_,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(kLogTag, "Unable to create TTS Session worker");
    return false;
  }
  if (xTaskCreateWithCaps(playback_task_entry, "tts_playback", 4096, this, 5,
                          &playback_task_,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    // 2026-09-19: Tear down a partially created external-stack worker so later initialization cannot report a false ready state.
    vTaskDeleteWithCaps(sentence_task_);
    sentence_task_ = nullptr;
    ESP_LOGE(kLogTag, "Unable to create TTS workers");
    return false;
  }
  return true;
}

bool TtsClient::warmup_persistent_connection() {
  return initialize() && connect();
}

bool TtsClient::persistent_connection_ready() const {
  return websocket_ != nullptr && connection_ready_ &&
         esp_websocket_client_is_connected(websocket_);
}

bool TtsClient::begin_turn() {
  if (!initialize() || turn_active_) return false;
  xEventGroupClearBits(events_, kTurnFinishedBit | kFailureBit);
  turn_failed_ = false;
  turn_audio_bytes_ = 0;
  turn_active_ = true;
  return true;
}

bool TtsClient::enqueue_sentence(const std::string &sentence) {
  if (!turn_active_ || sentence.empty()) return false;
  // 2026-09-20: Also cover the non-streaming fallback; the streaming segmenter's earlier mark wins when already present.
  diagnostics::latency_trace_mark(
      diagnostics::LatencyEvent::kFirstSentenceReady);
  char *copy = copy_to_psram(sentence);
  if (copy == nullptr) return false;
  const SentenceItem item{copy, false};
  if (xQueueSend(sentence_queue_, &item, 0) != pdTRUE) {
    heap_caps_free(copy);
    ESP_LOGE(kLogTag, "Sentence queue is full");
    turn_failed_ = true;
    return false;
  }
  return true;
}

bool TtsClient::finish_turn(uint32_t timeout_ms) {
  if (!turn_active_) return false;
  const SentenceItem marker{nullptr, true};
  if (xQueueSend(sentence_queue_, &marker, portMAX_DELAY) != pdTRUE) {
    turn_active_ = false;
    return false;
  }
  // 2026-09-19: A provider failure must not end the turn before its PCM marker drains; otherwise the next turn can consume a stale completion bit.
  const EventBits_t completion = xEventGroupWaitBits(
      events_, kTurnFinishedBit, pdTRUE, pdFALSE,
      pdMS_TO_TICKS(timeout_ms));
  const bool completed = (completion & kTurnFinishedBit) != 0;
  turn_active_ = false;
  // 2026-09-20: Retain the former queue-only success check; an empty turn can drain normally without producing any speech.
  // return completed && !turn_failed_;
  // 2026-09-20: Require received PCM after the existing playback completion wait so zero-audio turns fail without leaving stale queue markers.
  const bool had_audio = last_turn_had_audio();
  if (completed && !had_audio) {
    ESP_LOGW(kLogTag, "TTS turn ended without audio");
  }
  return completed && !turn_failed_ && had_audio;
}

bool TtsClient::last_turn_had_audio() const { return turn_audio_bytes_ > 0; }

bool TtsClient::connect() {
  if (persistent_connection_ready()) return true;
  if (xSemaphoreTake(connection_mutex_, pdMS_TO_TICKS(kConnectTimeoutMs)) !=
      pdTRUE) {
    return false;
  }
  if (persistent_connection_ready()) {
    xSemaphoreGive(connection_mutex_);
    return true;
  }
  disconnect();
  xEventGroupClearBits(events_, kConnectedBit | kConnectionStartedBit |
                                   kFailureBit);
  const std::string connect_id = volc_speech::generate_uuid();
  const std::string headers = volc_speech::make_auth_headers(
      kResourceId, connect_id, true, true);
  esp_websocket_client_config_t configuration = {};
  configuration.uri = kEndpoint;
  configuration.headers = headers.c_str();
  configuration.disable_auto_reconnect = true;
  configuration.network_timeout_ms = kConnectTimeoutMs;
  configuration.crt_bundle_attach = esp_crt_bundle_attach;
  // 2026-09-20: Match ASR's client frame capacity for streamed PCM; the temporary transport handshake buffer is configured globally.
  configuration.buffer_size = 4096;
  diagnostics::log_memory_snapshot("TTS before-connect");
  websocket_ = esp_websocket_client_init(&configuration);
  if (websocket_ != nullptr) {
    esp_websocket_register_events(websocket_, WEBSOCKET_EVENT_ANY,
                                  websocket_event, this);
  }
  bool success = websocket_ != nullptr &&
                 esp_websocket_client_start(websocket_) == ESP_OK &&
                 wait_bit(events_, kConnectedBit, kConnectTimeoutMs);
  if (success) {
    success = send_event(kStartConnection, "", "{}") &&
              wait_bit(events_, kConnectionStartedBit, kProtocolTimeoutMs);
  }
  connection_ready_ = success;
  if (success) {
    diagnostics::latency_trace_mark(
        diagnostics::LatencyEvent::kTtsConnected);
    diagnostics::log_memory_snapshot("TTS persistent-ready");
    ESP_LOGI(kLogTag, "Persistent bidirectional connection is ready");
  } else {
    ESP_LOGE(kLogTag, "Persistent connection failed");
    disconnect();
  }
  xSemaphoreGive(connection_mutex_);
  return success;
}

void TtsClient::release_persistent_connection() {
  if (connection_mutex_ == nullptr ||
      xSemaphoreTake(connection_mutex_, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return;
  }
  if (persistent_connection_ready()) {
    send_event(kFinishConnection, "", "{}");
  }
  disconnect();
  xSemaphoreGive(connection_mutex_);
}

void TtsClient::disconnect() {
  connection_ready_ = false;
  if (websocket_ == nullptr) return;
  if (esp_websocket_client_is_connected(websocket_)) {
    esp_websocket_client_close(websocket_, pdMS_TO_TICKS(1000));
  }
  esp_websocket_client_stop(websocket_);
  esp_websocket_client_destroy(websocket_);
  websocket_ = nullptr;
  diagnostics::log_memory_snapshot("TTS disconnected");
}

bool TtsClient::send_event(int32_t event, const std::string &session_id,
                           const std::string &payload) {
  if (websocket_ == nullptr ||
      !esp_websocket_client_is_connected(websocket_)) return false;
  const size_t length = 4 + 4 +
      (session_id.empty() ? 0 : 4 + session_id.size()) + 4 + payload.size();
  std::vector<uint8_t> frame(length);
  frame[0] = (volc_speech::kProtocolVersion << 4) |
             volc_speech::kDefaultHeaderSize;
  frame[1] = (kFullClientRequest << 4) | kFlagWithEvent;
  frame[2] = kSerializationJson << 4;
  frame[3] = 0;
  size_t offset = 4;
  volc_speech::write_i32_be(frame.data() + offset, event);
  offset += 4;
  if (!session_id.empty()) {
    volc_speech::write_u32_be(frame.data() + offset, session_id.size());
    offset += 4;
    std::memcpy(frame.data() + offset, session_id.data(), session_id.size());
    offset += session_id.size();
  }
  volc_speech::write_u32_be(frame.data() + offset, payload.size());
  offset += 4;
  std::memcpy(frame.data() + offset, payload.data(), payload.size());
  const int sent = esp_websocket_client_send_bin(
      websocket_, reinterpret_cast<const char *>(frame.data()), frame.size(),
      pdMS_TO_TICKS(3000));
  if (sent != static_cast<int>(frame.size())) return false;
  if (event == kTaskRequest) {
    diagnostics::latency_trace_mark(
        diagnostics::LatencyEvent::kTtsRequestSent);
  }
  return true;
}

bool TtsClient::synthesize(const std::string &text) {
  if (!connect()) return false;
  xEventGroupClearBits(events_, kSessionStartedBit | kSessionFinishedBit |
                                   kFailureBit);
  session_audio_bytes_ = 0;
  sequence_seen_ = false;
  last_sequence_ = 0;
  session_id_ = volc_speech::generate_uuid();

  cJSON *start = cJSON_CreateObject();
  // 2026-09-19: Build the V3 user member as an object directly so ownership and JSON type are unambiguous.
  cJSON *start_user = cJSON_AddObjectToObject(start, "user");
  cJSON_AddStringToObject(start_user, "uid", "desk-emoji");
  cJSON_AddNumberToObject(start, "event", kStartSession);
  cJSON_AddStringToObject(start, "namespace", "BidirectionalTTS");
  cJSON *parameters = cJSON_AddObjectToObject(start, "req_params");
  cJSON_AddStringToObject(parameters, "speaker", kVoice);
  cJSON *audio_parameters = cJSON_AddObjectToObject(parameters, "audio_params");
  cJSON_AddStringToObject(audio_parameters, "format", "pcm");
  cJSON_AddNumberToObject(audio_parameters, "sample_rate", 8000);
  cJSON_AddNumberToObject(audio_parameters, "speech_rate", 0);
  cJSON_AddNumberToObject(audio_parameters, "loudness_rate", 0);
  char *start_json = cJSON_PrintUnformatted(start);
  cJSON_Delete(start);
  bool success = start_json != nullptr &&
                 send_event(kStartSession, session_id_, start_json);
  cJSON_free(start_json);
  success = success &&
            wait_bit(events_, kSessionStartedBit, kProtocolTimeoutMs);
  if (!success) return false;

  cJSON *task = cJSON_CreateObject();
  cJSON *user = cJSON_AddObjectToObject(task, "user");
  cJSON_AddStringToObject(user, "uid", "desk-emoji");
  cJSON_AddNumberToObject(task, "event", kTaskRequest);
  cJSON_AddStringToObject(task, "namespace", "BidirectionalTTS");
  cJSON *request = cJSON_AddObjectToObject(task, "req_params");
  cJSON_AddStringToObject(request, "text", text.c_str());
  char *task_json = cJSON_PrintUnformatted(task);
  cJSON_Delete(task);
  success = task_json != nullptr &&
            send_event(kTaskRequest, session_id_, task_json) &&
            send_event(kFinishSession, session_id_, "{}");
  cJSON_free(task_json);
  success = success &&
            wait_bit(events_, kSessionFinishedBit, kSessionTimeoutMs) &&
            session_audio_bytes_ > 0;
  session_id_.clear();
  return success;
}

void TtsClient::sentence_task_entry(void *parameter) {
  static_cast<TtsClient *>(parameter)->sentence_loop();
}

void TtsClient::sentence_loop() {
  SentenceItem item{};
  while (xQueueReceive(sentence_queue_, &item, portMAX_DELAY) == pdTRUE) {
    if (item.end_turn) {
      const PcmItem marker{nullptr, 0, true};
      xQueueSend(pcm_queue_, &marker, portMAX_DELAY);
      continue;
    }
    if (item.text == nullptr) continue;
    const std::string sentence(item.text);
    heap_caps_free(item.text);
    if (!synthesize(sentence)) {
      ESP_LOGE(kLogTag, "TTS Session failed: %s", sentence.c_str());
      turn_failed_ = true;
      // 2026-09-19: Drop only the failed connection; already queued PCM is preserved so partial speech is never replayed.
      release_persistent_connection();
    }
  }
}

bool TtsClient::queue_pcm(const uint8_t *data, size_t length) {
  void *copy = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (copy == nullptr) {
    copy = heap_caps_malloc(length, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (copy == nullptr) return false;
  std::memcpy(copy, data, length);
  const PcmItem item{static_cast<uint8_t *>(copy), length, false};
  if (xQueueSend(pcm_queue_, &item, pdMS_TO_TICKS(1500)) != pdTRUE) {
    heap_caps_free(copy);
    return false;
  }
  return true;
}

void TtsClient::playback_task_entry(void *parameter) {
  static_cast<TtsClient *>(parameter)->playback_loop();
}

void TtsClient::playback_loop() {
  PcmItem item{};
  while (xQueueReceive(pcm_queue_, &item, portMAX_DELAY) == pdTRUE) {
    if (item.end_turn) {
      if (!audio::wait_for_playback_complete()) turn_failed_ = true;
      audio::stop_playback();
      // 2026-09-20: Dim green only after the queued PCM and I2S DMA have drained, before handing LED ownership back to the conversation loop.
      ui::set_status_color(0x00FF00, 5);
      xEventGroupSetBits(events_, kTurnFinishedBit);
      continue;
    }
    if (item.data == nullptr || (item.length & 1U) != 0) {
      turn_failed_ = true;
      continue;
    }
    // 2026-09-20: Show bright green throughout TTS playback; the UI helper suppresses redundant refreshes between PCM blocks.
    if (item.length > 0) ui::set_status_color(0x00FF00, 50);
    constexpr size_t kInputSamples = 128;
    int16_t output[kInputSamples * 2];
    for (size_t offset = 0; offset < item.length;) {
      const size_t count = std::min(kInputSamples,
                                    (item.length - offset) / 2);
      for (size_t index = 0; index < count; ++index) {
        const uint16_t encoded = item.data[offset + index * 2] |
            (static_cast<uint16_t>(item.data[offset + index * 2 + 1]) << 8);
        const int16_t sample = static_cast<int16_t>(encoded);
        output[index * 2] = sample;
        output[index * 2 + 1] = sample;
      }
      if (!audio::play(output, count * 2, kVolume)) turn_failed_ = true;
      offset += count * 2;
    }
    heap_caps_free(item.data);
  }
}

void TtsClient::websocket_event(void *handler_argument,
                                esp_event_base_t event_base, int32_t event_id,
                                void *event_data) {
  auto *self = static_cast<TtsClient *>(handler_argument);
  if (self == nullptr || self->events_ == nullptr) return;
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    xEventGroupSetBits(self->events_, kConnectedBit);
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
      event_id == WEBSOCKET_EVENT_ERROR) {
    self->connection_ready_ = false;
    xEventGroupSetBits(self->events_, kFailureBit);
    return;
  }
  if (event_id != WEBSOCKET_EVENT_DATA || event_data == nullptr) return;
  auto *event = static_cast<esp_websocket_event_data_t *>(event_data);
  if (event->payload_offset == 0) {
    self->received_frame_.assign(event->payload_len, 0);
    self->received_opcode_ = event->op_code;
  }
  if (event->data_ptr == nullptr || event->data_len < 0 ||
      event->payload_offset < 0 || event->payload_len < 0 ||
      static_cast<size_t>(event->payload_offset + event->data_len) >
          self->received_frame_.size()) {
    xEventGroupSetBits(self->events_, kFailureBit);
    return;
  }
  std::memcpy(self->received_frame_.data() + event->payload_offset,
              event->data_ptr, event->data_len);
  if (event->payload_offset + event->data_len == event->payload_len &&
      self->received_opcode_ == 0x02 &&
      !self->parse_frame(self->received_frame_.data(),
                         self->received_frame_.size())) {
    xEventGroupSetBits(self->events_, kFailureBit);
  }
}

bool TtsClient::parse_frame(const uint8_t *frame, size_t length) {
  if (frame == nullptr || length < 4) return false;
  const size_t header_length = (frame[0] & 0x0F) * 4;
  const uint8_t type = frame[1] >> 4;
  const uint8_t flags = frame[1] & 0x0F;
  const uint8_t compression = frame[2] & 0x0F;
  if (header_length < 4 || header_length > length) return false;
  size_t offset = header_length;
  int32_t sequence = 0;
  if (has_sequence(flags) &&
      !volc_speech::read_i32_be(frame, length, offset, sequence)) return false;
  uint32_t error_code = 0;
  if (type == kErrorResponse &&
      !volc_speech::read_u32_be(frame, length, offset, error_code)) return false;
  int32_t event = 0;
  std::string response_session;
  if (flags == kFlagWithEvent) {
    if (!volc_speech::read_i32_be(frame, length, offset, event)) return false;
    uint32_t id_length = 0;
    if (event_has_session_id(event)) {
      if (!volc_speech::read_u32_be(frame, length, offset, id_length) ||
          id_length > length - offset) return false;
      response_session.assign(reinterpret_cast<const char *>(frame + offset),
                              id_length);
      offset += id_length;
    }
    if (event_has_connection_id(event)) {
      if (!volc_speech::read_u32_be(frame, length, offset, id_length) ||
          id_length > length - offset) return false;
      offset += id_length;
    }
  }
  uint32_t payload_length = 0;
  if (!volc_speech::read_u32_be(frame, length, offset, payload_length) ||
      payload_length > length - offset) return false;
  const uint8_t *payload = frame + offset;
  uint8_t *decompressed = nullptr;
  size_t decoded_length = payload_length;
  if (compression == kCompressionGzip && payload_length > 0) {
    if (!volc_speech::gzip_decompress(payload, payload_length, &decompressed,
                                      &decoded_length)) return false;
    payload = decompressed;
  }
  bool success = true;
  const bool session_matches = response_session.empty() ||
                               session_id_.empty() ||
                               response_session == session_id_;
  if (event == kConnectionStarted) {
    connection_ready_ = true;
    xEventGroupSetBits(events_, kConnectionStartedBit);
  } else if (event == kConnectionFinished) {
    connection_ready_ = false;
  } else if (event == kSessionStarted && session_matches) {
    xEventGroupSetBits(events_, kSessionStartedBit);
  } else if ((event == kSessionFailed || event == kConnectionFailed ||
              type == kErrorResponse) && session_matches) {
    ESP_LOGE(kLogTag, "Provider error: event=%ld code=%u",
             static_cast<long>(event), static_cast<unsigned>(error_code));
    xEventGroupSetBits(events_, kFailureBit);
    success = false;
  } else if (type == kAudioOnlyServer && session_matches &&
             decoded_length > 0) {
    const bool first = session_audio_bytes_ == 0;
    if (has_sequence(flags)) {
      const uint32_t current = sequence < 0
          ? static_cast<uint32_t>(-static_cast<int64_t>(sequence))
          : static_cast<uint32_t>(sequence);
      if (sequence_seen_ && current != last_sequence_ + 1) success = false;
      sequence_seen_ = true;
      last_sequence_ = current;
    }
    if (success) {
      // 2026-09-20: Keep arrival time before queue_pcm, but publish it only for audio accepted by the playback queue.
      const uint64_t audio_ready_us =
          static_cast<uint64_t>(esp_timer_get_time());
      success = queue_pcm(payload, decoded_length);
      if (success) {
        // 2026-09-20: Playback can run before xQueueSend returns; recording the captured time prevents a negative queue-latency interval.
        if (first) {
          diagnostics::latency_trace_mark_at(
              diagnostics::LatencyEvent::kTtsFirstAudio, audio_ready_us);
        }
        session_audio_bytes_ += decoded_length;
        turn_audio_bytes_ += decoded_length;
      }
    }
#if 0
    // 2026-09-20: Retain the post-queue timestamp; it could occur after PLAYBACK_START on the other core.
    if (first && success) {
      diagnostics::latency_trace_mark(
          diagnostics::LatencyEvent::kTtsFirstAudio);
    }
#endif
  } else if (event == kSessionFinished && session_matches &&
             type == kFullServerResponse) {
    xEventGroupSetBits(events_, kSessionFinishedBit);
  }
  heap_caps_free(decompressed);
  return success;
}

}  // namespace desk_talk::cloud
