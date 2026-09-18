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
  // 2026-09-18: Prepare a worker and queue one validated DeepSeek answer before releasing its TTS request.
  bool beginSentenceStream();
  bool enqueueSentence(const String &sentence);
  // 2026-09-17: Release queued speech only after DeepSeek has closed its TLS connection and validated the complete JSON reply.
  bool releaseSentenceStream();
  bool finishSentenceStream();
  // 2026-09-18: Reuse the existing PCM queue and I2S playback for audio streamed by an optional cloud gateway.
  bool beginExternalStream();
  bool queueExternalPcm(const uint8_t *data, size_t length);
  bool finishExternalStream(bool complete);

 private:
  struct PcmChunk {
    uint8_t *data;
    size_t length;
  };

  // 2026-09-17: Pass owned UTF-8 text buffers between the LLM producer and the single TTS consumer task.
  struct SentenceChunk {
    char *text;
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
  // 2026-09-18: Remember whether partial gateway audio must be drained on a provider disconnect.
  bool external_audio_received_ = false;
  QueueHandle_t sentence_queue = nullptr;
  TaskHandle_t sentence_task = nullptr;
  volatile bool sentence_stream_accepting = false;
  volatile bool sentence_stream_failed = false;
  volatile bool sentence_stream_finished = true;
  volatile bool sentence_stream_released = false;
  volatile uint16_t sentence_stream_count = 0;

  void handleWebSocketEvent(WStype_t type, uint8_t *payload, size_t length);
  bool synthesizeSegment(const String &text, uint16_t segment_number);
  // 2026-09-17: Retry only failures that produced no audio, preventing both lost sentences and duplicated partial speech.
  bool synthesizeSegmentWithRetry(const String &text,
                                  uint16_t segment_number);
  bool sendTtsRequest(const String &text);
  bool parseResponse(const uint8_t *frame, size_t length);
  bool startPlayback();
  bool queuePcm16Le(const uint8_t *data, size_t length);
  bool finishPlayback(bool drain_audio);
  static void playbackTaskEntry(void *parameter);
  void playbackLoop();
  bool playPcm16Le(const uint8_t *data, size_t length);
  // 2026-09-17: Own all WebSocket synthesis calls on one task after DeepSeek releases the queued phrases.
  static void sentenceTaskEntry(void *parameter);
  void sentenceStreamLoop();
};

#endif
