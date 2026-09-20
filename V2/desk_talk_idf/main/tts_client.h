// 2026-09-19: Define a persistent Volcengine V3 TTS connection with independent sentence and PCM worker queues.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace desk_talk::cloud {

class TtsClient {
 public:
  bool initialize();
  bool warmup_persistent_connection();
  bool persistent_connection_ready() const;
  void release_persistent_connection();
  bool begin_turn();
  bool enqueue_sentence(const std::string &sentence);
  bool finish_turn(uint32_t timeout_ms = 90000);
  bool last_turn_had_audio() const;

 private:
  struct SentenceItem {
    char *text;
    bool end_turn;
  };
  struct PcmItem {
    uint8_t *data;
    size_t length;
    bool end_turn;
  };

  static void websocket_event(void *handler_argument,
                              esp_event_base_t event_base, int32_t event_id,
                              void *event_data);
  static void sentence_task_entry(void *parameter);
  static void playback_task_entry(void *parameter);
  void sentence_loop();
  void playback_loop();

  bool connect();
  bool send_event(int32_t event, const std::string &session_id,
                  const std::string &payload);
  bool synthesize(const std::string &text);
  bool parse_frame(const uint8_t *data, size_t length);
  bool queue_pcm(const uint8_t *data, size_t length);
  void disconnect();

  esp_websocket_client_handle_t websocket_ = nullptr;
  EventGroupHandle_t events_ = nullptr;
  QueueHandle_t sentence_queue_ = nullptr;
  QueueHandle_t pcm_queue_ = nullptr;
  TaskHandle_t sentence_task_ = nullptr;
  TaskHandle_t playback_task_ = nullptr;
  SemaphoreHandle_t connection_mutex_ = nullptr;
  std::vector<uint8_t> received_frame_;
  uint8_t received_opcode_ = 0;
  std::string session_id_;
  volatile bool connection_ready_ = false;
  volatile bool turn_active_ = false;
  volatile bool turn_failed_ = false;
  volatile size_t session_audio_bytes_ = 0;
  volatile size_t turn_audio_bytes_ = 0;
  uint32_t last_sequence_ = 0;
  bool sequence_seen_ = false;
};

TtsClient &tts_client();

}  // namespace desk_talk::cloud
