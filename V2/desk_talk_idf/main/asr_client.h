// 2026-09-19: Define the native event-driven Volcengine ASR client and local VAD recording contract.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"

namespace desk_talk::cloud {

class AsrClient {
 public:
  bool recognize();
  void disconnect();
  const std::string &result() const;

 private:
  static void websocket_event(void *handler_argument,
                              esp_event_base_t event_base, int32_t event_id,
                              void *event_data);
  bool connect();
  bool send_full_request();
  bool send_audio_request(const uint8_t *data, size_t length, bool is_last);
  bool wait_for(EventBits_t bit, uint32_t timeout_ms);
  bool parse_response(const uint8_t *frame, size_t length);
  void parse_result_json(const uint8_t *payload, size_t length);

  esp_websocket_client_handle_t websocket_ = nullptr;
  EventGroupHandle_t events_ = nullptr;
  std::vector<uint8_t> received_frame_;
  uint8_t received_opcode_ = 0;
  std::string recognized_text_;
  bool request_active_ = false;
  bool final_response_received_ = false;
  bool voice_detected_ = false;
};

}  // namespace desk_talk::cloud
