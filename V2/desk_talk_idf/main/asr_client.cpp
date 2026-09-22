// 2026-09-19: Port V1 streaming microphone upload and Volcengine binary response parsing to esp_websocket_client.
#include "asr_client.h"

#include <algorithm>
#include <cstring>

#include "audio.h"
#include "cJSON.h"
// 2026-09-20: Restore the microphone's red status-light feedback from the Arduino version.
#include "device_ui.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hardware_config.h"
#include "latency_trace.h"
#include "memory_diagnostics.h"
#include "volc_speech_protocol.h"
#include "wifi_manager.h"

namespace desk_talk::cloud {
namespace {

constexpr char kLogTag[] = "asr";
constexpr char kAsrUri[] =
    "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";
constexpr char kResourceId[] = "volc.seedasr.sauc.duration";
constexpr char kUserId[] = "desk-emoji";
constexpr uint8_t kFlagPositiveSequence = 0x01;
constexpr uint8_t kFlagLast = 0x02;
constexpr uint8_t kFlagWithEvent = 0x04;
constexpr uint8_t kCompressionGzip = 0x01;
constexpr uint8_t kSerializationJson = 0x01;
constexpr uint8_t kSerializationNone = 0x00;
constexpr uint32_t kConnectedBit = BIT0;
constexpr uint32_t kMessageBit = BIT1;
constexpr uint32_t kFinalBit = BIT2;
constexpr uint32_t kFailureBit = BIT3;
constexpr uint32_t kConnectTimeoutMs = 7000;
constexpr uint32_t kResponseTimeoutMs = 5000;
constexpr uint32_t kFinalResponseTimeoutMs = 10000;
constexpr uint32_t kRecordSeconds = 30;
// 2026-09-22: Restore the 500 ms endpoint because short-pause validation produced multiple truncated phrases with the 400 ms setting.
constexpr uint32_t kSilenceTimeoutMs = 500;
// 2026-09-22: Keep the shorter endpoint available for a later controlled comparison after speech-boundary quality is stable.
// constexpr uint32_t kSilenceTimeoutMs = 400;
constexpr uint32_t kStartSpeechTimeoutSeconds = 15;
// 2026-09-20: Preserve the former 200 ms block setting for comparison with the latency baseline.
// constexpr size_t kAudioBlockSamples = hardware::kAudioSampleRate / 5;
// 2026-09-22: Keep 100 ms blocks so the 500 ms endpoint remains observable with bounded block-read granularity.
constexpr size_t kAudioBlockSamples = hardware::kAudioSampleRate / 10;

int64_t milliseconds() { return esp_timer_get_time() / 1000; }

}  // namespace

bool AsrClient::connect() {
  if (!network::wifi_manager().connected()) {
    ESP_LOGE(kLogTag, "Wi-Fi is not connected");
    return false;
  }
  disconnect();
  if (events_ == nullptr) {
    events_ = xEventGroupCreate();
  }
  if (events_ == nullptr) {
    return false;
  }
  xEventGroupClearBits(events_, kConnectedBit | kMessageBit | kFinalBit |
                                   kFailureBit);
  const std::string request_id = volc_speech::generate_uuid();
  const std::string headers = volc_speech::make_auth_headers(
      kResourceId, request_id, false);
  esp_websocket_client_config_t configuration = {};
  configuration.uri = kAsrUri;
  configuration.headers = headers.c_str();
  configuration.disable_auto_reconnect = true;
  configuration.network_timeout_ms = kConnectTimeoutMs;
  configuration.crt_bundle_attach = esp_crt_bundle_attach;
  // 2026-09-20: Increase the client frame buffer for Volcengine binary responses; the separate transport handshake buffer is configured globally.
  configuration.buffer_size = 4096;

  diagnostics::log_memory_snapshot("ASR before-connect");
  websocket_ = esp_websocket_client_init(&configuration);
  if (websocket_ == nullptr) {
    diagnostics::log_memory_snapshot("ASR init-failed");
    return false;
  }
  esp_websocket_register_events(websocket_, WEBSOCKET_EVENT_ANY,
                                websocket_event, this);
#if 0
  // 2026-09-20: Retain the combined check; a failed WebSocket task allocation was incorrectly reported as a network timeout.
  if (esp_websocket_client_start(websocket_) != ESP_OK ||
      !wait_for(kConnectedBit, kConnectTimeoutMs)) {
    ESP_LOGE(kLogTag, "WebSocket connection timed out");
    disconnect();
    return false;
  }
#endif
  // 2026-09-20: Distinguish task/transport startup failure from an actual connection timeout when diagnosing concurrent ASR/TTS memory use.
  const esp_err_t start_result = esp_websocket_client_start(websocket_);
  if (start_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Unable to start ASR WebSocket: %s",
             esp_err_to_name(start_result));
    diagnostics::log_memory_snapshot("ASR start-failed");
    disconnect();
    return false;
  }
  if (!wait_for(kConnectedBit, kConnectTimeoutMs)) {
    ESP_LOGE(kLogTag, "WebSocket connection timed out");
    disconnect();
    return false;
  }
  diagnostics::log_memory_snapshot("ASR connected");
  return true;
}

void AsrClient::disconnect() {
  if (websocket_ == nullptr) {
    return;
  }
  diagnostics::log_memory_snapshot("ASR before-disconnect");
  request_active_ = false;
  if (esp_websocket_client_is_connected(websocket_)) {
    esp_websocket_client_close(websocket_, pdMS_TO_TICKS(1000));
  }
  esp_websocket_client_stop(websocket_);
  esp_websocket_client_destroy(websocket_);
  websocket_ = nullptr;
  diagnostics::log_memory_snapshot("ASR after-disconnect");
}

void AsrClient::websocket_event(void *handler_argument,
                                esp_event_base_t event_base, int32_t event_id,
                                void *event_data) {
  auto *self = static_cast<AsrClient *>(handler_argument);
  if (self == nullptr || self->events_ == nullptr) {
    return;
  }
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    xEventGroupSetBits(self->events_, kConnectedBit);
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
      event_id == WEBSOCKET_EVENT_ERROR) {
    if (self->request_active_ && !self->final_response_received_) {
      xEventGroupSetBits(self->events_, kFailureBit);
    }
    return;
  }
  if (event_id != WEBSOCKET_EVENT_DATA || event_data == nullptr) {
    return;
  }

  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
  if (data->payload_offset == 0) {
    self->received_frame_.assign(data->payload_len, 0);
    self->received_opcode_ = data->op_code;
  }
  if (data->data_ptr == nullptr || data->data_len < 0 ||
      data->payload_offset < 0 || data->payload_len < 0 ||
      static_cast<size_t>(data->payload_offset + data->data_len) >
          self->received_frame_.size()) {
    xEventGroupSetBits(self->events_, kFailureBit);
    return;
  }
  std::memcpy(self->received_frame_.data() + data->payload_offset,
              data->data_ptr, data->data_len);
  if (data->payload_offset + data->data_len != data->payload_len) {
    return;
  }
  if (self->received_opcode_ == 0x02) {
    if (!self->parse_response(self->received_frame_.data(),
                              self->received_frame_.size())) {
      xEventGroupSetBits(self->events_, kFailureBit);
    } else {
      xEventGroupSetBits(self->events_, kMessageBit);
    }
  } else if (self->received_opcode_ == 0x01) {
    ESP_LOGE(kLogTag, "Unexpected text response: %.*s",
             static_cast<int>(self->received_frame_.size()),
             reinterpret_cast<const char *>(self->received_frame_.data()));
    xEventGroupSetBits(self->events_, kFailureBit);
  }
}

bool AsrClient::wait_for(EventBits_t bit, uint32_t timeout_ms) {
  const EventBits_t result = xEventGroupWaitBits(
      events_, bit | kFailureBit, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(timeout_ms));
  if ((result & kFailureBit) != 0) {
    return false;
  }
  if ((result & bit) == 0) {
    ESP_LOGE(kLogTag, "Response wait timed out");
    return false;
  }
  xEventGroupClearBits(events_, bit);
  return true;
}

bool AsrClient::send_full_request() {
  cJSON *document = cJSON_CreateObject();
  cJSON *user = cJSON_AddObjectToObject(document, "user");
  cJSON_AddStringToObject(user, "uid", kUserId);
  cJSON *audio_config = cJSON_AddObjectToObject(document, "audio");
  cJSON_AddStringToObject(audio_config, "format", "pcm");
  cJSON_AddStringToObject(audio_config, "codec", "raw");
  cJSON_AddNumberToObject(audio_config, "rate", hardware::kAudioSampleRate);
  cJSON_AddNumberToObject(audio_config, "bits", 16);
  cJSON_AddNumberToObject(audio_config, "channel", 1);
  cJSON *request = cJSON_AddObjectToObject(document, "request");
  cJSON_AddStringToObject(request, "model_name", "bigmodel");
  cJSON_AddBoolToObject(request, "enable_itn", true);
  cJSON_AddBoolToObject(request, "enable_punc", true);
  cJSON_AddBoolToObject(request, "enable_ddc", true);
  cJSON_AddBoolToObject(request, "show_utterances", false);
  cJSON_AddBoolToObject(request, "enable_nonstream", false);
  cJSON_AddStringToObject(request, "result_type", "full");
  char *json = cJSON_PrintUnformatted(document);
  cJSON_Delete(document);
  if (json == nullptr) {
    return false;
  }
  const size_t json_length = std::strlen(json);
  std::vector<uint8_t> frame(8 + json_length);
  frame[0] = (volc_speech::kProtocolVersion << 4) |
             volc_speech::kDefaultHeaderSize;
  frame[1] = volc_speech::kClientFullRequest << 4;
  frame[2] = kSerializationJson << 4;
  frame[3] = 0;
  volc_speech::write_u32_be(frame.data() + 4, json_length);
  std::memcpy(frame.data() + 8, json, json_length);
  cJSON_free(json);
  return esp_websocket_client_send_bin(
             websocket_, reinterpret_cast<const char *>(frame.data()),
             frame.size(), pdMS_TO_TICKS(3000)) ==
         static_cast<int>(frame.size());
}

bool AsrClient::send_audio_request(const uint8_t *data, size_t length,
                                   bool is_last) {
  std::vector<uint8_t> frame(8 + length);
  frame[0] = (volc_speech::kProtocolVersion << 4) |
             volc_speech::kDefaultHeaderSize;
  frame[1] = (volc_speech::kClientAudioOnlyRequest << 4) |
             (is_last ? kFlagLast : 0);
  frame[2] = kSerializationNone << 4;
  frame[3] = 0;
  volc_speech::write_u32_be(frame.data() + 4, length);
  std::memcpy(frame.data() + 8, data, length);
  return esp_websocket_client_send_bin(
             websocket_, reinterpret_cast<const char *>(frame.data()),
             frame.size(), pdMS_TO_TICKS(3000)) ==
         static_cast<int>(frame.size());
}

bool AsrClient::recognize() {
  // 2026-09-20: Show dim red while connecting/waiting for speech; the conversation loop handles failure and idle reset.
  ui::set_status_color(0xFF0000, 10);
  // 2026-09-20: Preserve the ASR-local reset; the conversation loop now owns one trace across retries of the same turn.
  // diagnostics::latency_trace_begin();
  recognized_text_.clear();
  final_response_received_ = false;
  voice_detected_ = false;
  if (!audio::microphone_ready() || !connect()) {
    return false;
  }
  request_active_ = true;
  if (!send_full_request() || !wait_for(kMessageBit, kResponseTimeoutMs)) {
    request_active_ = false;
    return false;
  }
  ESP_LOGI(kLogTag, "Listening; speak now");

  auto *buffer = static_cast<int16_t *>(heap_caps_malloc(
      kAudioBlockSamples * sizeof(int16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<int16_t *>(heap_caps_malloc(
        kAudioBlockSamples * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (buffer == nullptr) {
    request_active_ = false;
    return false;
  }

  bool speech_started = false;
  bool is_silent = true;
  bool sent_last_frame = false;
  int64_t silence_started_ms = 0;
  size_t samples_recorded = 0;
  const size_t maximum_samples = kRecordSeconds * hardware::kAudioSampleRate;
  while (samples_recorded < maximum_samples &&
         (xEventGroupGetBits(events_) & kFailureBit) == 0) {
    const size_t samples_to_read =
        std::min(kAudioBlockSamples, maximum_samples - samples_recorded);
    if (audio::record(buffer, samples_to_read) != ESP_OK) {
      break;
    }
    // 2026-09-20: Capture block-read completion before filtering/LED work; this estimates speech end with the current 100 ms block and I2S backlog uncertainty.
    const uint64_t frame_read_us =
        static_cast<uint64_t>(esp_timer_get_time());
    audio::enhance_voice(buffer, samples_to_read);
    const size_t mean =
        audio::calculate_mean_amplitude(buffer, samples_to_read);
    // 2026-09-20: Match the original red brightness feedback to the same microphone threshold used by speech detection.
    ui::set_status_color(0xFF0000,
                         mean > hardware::kSoundThreshold ? 100 : 10);
    if (voice_detected_ && !speech_started) {
      speech_started = true;
      is_silent = false;
    }
    bool is_last = samples_recorded + samples_to_read >= maximum_samples;
    if (mean > hardware::kSoundThreshold) {
      // 2026-09-20: Update only on loud blocks, so the final stored timestamp precedes the silence timeout instead of hiding endpoint latency.
      diagnostics::latency_trace_mark_at(
          diagnostics::LatencyEvent::kLastVoiceFrame, frame_read_us);
      if (!speech_started) {
        diagnostics::latency_trace_mark(
            diagnostics::LatencyEvent::kVadStart);
      }
      speech_started = true;
      is_silent = false;
      silence_started_ms = 0;
    } else if (!is_silent) {
      is_silent = true;
      silence_started_ms = milliseconds();
    } else if (speech_started &&
               milliseconds() - silence_started_ms >= kSilenceTimeoutMs) {
      is_last = true;
    } else if (!speech_started &&
               samples_recorded + samples_to_read >=
                   hardware::kAudioSampleRate * kStartSpeechTimeoutSeconds) {
      is_last = true;
    }
    if (is_last && speech_started) {
      diagnostics::latency_trace_mark(diagnostics::LatencyEvent::kVadEnd);
    }
    if (!send_audio_request(reinterpret_cast<const uint8_t *>(buffer),
                            samples_to_read * sizeof(int16_t), is_last)) {
      break;
    }
    samples_recorded += samples_to_read;
    if (is_last) {
      diagnostics::latency_trace_mark(
          diagnostics::LatencyEvent::kAudioUploadDone);
      sent_last_frame = true;
      break;
    }
  }
  heap_caps_free(buffer);
  // 2026-09-20: Return to dim red while awaiting the final ASR response, even if the last recorded block contained speech.
  ui::set_status_color(0xFF0000, 10);
  if (!sent_last_frame ||
      !wait_for(kFinalBit, kFinalResponseTimeoutMs)) {
    request_active_ = false;
    return false;
  }
  request_active_ = false;
  return !recognized_text_.empty();
}

bool AsrClient::parse_response(const uint8_t *frame, size_t length) {
  if (frame == nullptr || length < 4) {
    return false;
  }
  const size_t header_length = (frame[0] & 0x0F) * 4;
  const uint8_t message_type = frame[1] >> 4;
  const uint8_t flags = frame[1] & 0x0F;
  const uint8_t serialization = frame[2] >> 4;
  const uint8_t compression = frame[2] & 0x0F;
  if (header_length < 4 || header_length > length) {
    return false;
  }
  size_t offset = header_length;
  int32_t sequence = 0;
  if ((flags & kFlagPositiveSequence) != 0 &&
      !volc_speech::read_i32_be(frame, length, offset, sequence)) {
    return false;
  }
  if ((flags & kFlagWithEvent) != 0) {
    int32_t event = 0;
    if (!volc_speech::read_i32_be(frame, length, offset, event)) {
      return false;
    }
  }
  uint32_t error_code = 0;
  uint32_t payload_length = 0;
  if (message_type == volc_speech::kServerErrorResponse) {
    if (!volc_speech::read_u32_be(frame, length, offset, error_code) ||
        !volc_speech::read_u32_be(frame, length, offset, payload_length)) {
      return false;
    }
  } else if (message_type == volc_speech::kServerFullResponse ||
             message_type == volc_speech::kServerAck) {
    if (!volc_speech::read_u32_be(frame, length, offset, payload_length)) {
      return false;
    }
  } else {
    return false;
  }
  if (payload_length > length - offset) {
    return false;
  }
  const uint8_t *payload = frame + offset;
  size_t payload_size = payload_length;
  uint8_t *decompressed = nullptr;
  if (compression == kCompressionGzip && payload_length > 0) {
    if (!volc_speech::gzip_decompress(payload, payload_length, &decompressed,
                                      &payload_size)) {
      return false;
    }
    payload = decompressed;
  }
  if (message_type == volc_speech::kServerErrorResponse) {
    // 2026-09-19: Match ESP-IDF 5.3's strict printf checking when uint32_t is represented by unsigned long on the host toolchain.
    ESP_LOGE(kLogTag, "Server error %u: %.*s",
             static_cast<unsigned>(error_code),
             static_cast<int>(payload_size),
             reinterpret_cast<const char *>(payload));
    heap_caps_free(decompressed);
    return false;
  }
  if (serialization == kSerializationJson && payload_size > 0) {
    parse_result_json(payload, payload_size);
  }
  heap_caps_free(decompressed);
  if ((flags & kFlagLast) != 0 || sequence < 0) {
    final_response_received_ = true;
    diagnostics::latency_trace_mark(diagnostics::LatencyEvent::kAsrFinal);
    xEventGroupSetBits(events_, kFinalBit);
    ESP_LOGI(kLogTag, "Final result: %s", recognized_text_.c_str());
  }
  return true;
}

void AsrClient::parse_result_json(const uint8_t *payload, size_t length) {
  cJSON *document = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(payload), length);
  if (document == nullptr) {
    return;
  }
  cJSON *result = cJSON_GetObjectItemCaseSensitive(document, "result");
  cJSON *text = cJSON_IsObject(result)
                    ? cJSON_GetObjectItemCaseSensitive(result, "text")
                    : nullptr;
  if (text == nullptr && cJSON_IsArray(result) &&
      cJSON_GetArraySize(result) > 0) {
    text = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(result, 0),
                                            "text");
  }
  if (cJSON_IsString(text) && text->valuestring != nullptr &&
      text->valuestring[0] != '\0') {
    recognized_text_ = text->valuestring;
    voice_detected_ = true;
  }
  cJSON_Delete(document);
}

const std::string &AsrClient::result() const { return recognized_text_; }

}  // namespace desk_talk::cloud
