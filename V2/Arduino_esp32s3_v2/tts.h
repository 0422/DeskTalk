#ifndef TTS_H
#define TTS_H

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "audio.h"
#include "common.h"

class TtsClient {
 public:
  TtsClient();
  void connect();
  void disconnect();
  void loop(int delay_time = 100);
  bool TTS(String text);

 private:
  struct PcmChunk {
    uint8_t *data;
    size_t length;
  };

  const char *host = "openspeech.bytedance.com";
  const char *tts_url = "/api/v3/tts/unidirectional/stream";
  const char *resource_id = "seed-tts-2.0";
  const char *voice_type =
      "zh_male_naiqimengwa_uranus_bigtts";

  WebSocketsClient webSocket;
  bool event_handler_ready = false;
  bool final_response_received = false;
  bool request_failed = false;
  bool request_active = false;
  size_t audio_bytes_received = 0;
  float volume = 0.3f;
  QueueHandle_t playback_queue = nullptr;
  TaskHandle_t playback_task = nullptr;
  volatile bool playback_abort = false;
  volatile bool playback_failed = false;
  volatile bool playback_finished = true;

  void handleWebSocketEvent(WStype_t type, uint8_t *payload, size_t length);
  bool synthesizeSegment(const String &text, uint16_t segment_number);
  bool sendTtsRequest(const String &text);
  bool parseResponse(const uint8_t *frame, size_t length);
  bool startPlayback();
  bool queuePcm16Le(const uint8_t *data, size_t length);
  bool finishPlayback(bool drain_audio);
  static void playbackTaskEntry(void *parameter);
  void playbackLoop();
  bool playPcm16Le(const uint8_t *data, size_t length);
};

#endif
