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
  // 2026-09-18: Keep one Volcengine V3 physical connection across dialogue
  // turns; each reply still receives a fresh protocol Session.
  bool warmupPersistentConnection();
  bool persistentConnectionReady();
  void prepareForCloudRequest();
  void releasePersistentConnection();
  // 2026-09-18: Prepare a worker and queue complete streamed phrases while
  // DeepSeek owns the board's constrained TLS resources.
  bool beginSentenceStream();
  bool enqueueSentence(const String &sentence);
  // 2026-09-18: Release queued speech after DeepSeek closes its TLS stream,
  // avoiding two simultaneous TLS connections on the ESP32-S3.
  bool releaseSentenceStream();
  bool finishSentenceStream();
  // 2026-09-18: Keep gateway-only PCM bridge methods out of the board-direct
  // build; their historical implementation remains preserved in tts.cpp.
#if 0
  // 2026-09-18: Reuse the existing PCM queue and I2S playback for audio streamed by an optional cloud gateway.
  bool beginExternalStream();
  bool queueExternalPcm(const uint8_t *data, size_t length);
  bool finishExternalStream(bool complete);
#endif

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
  // 2026-09-18: Use Volcengine's bidirectional V3 endpoint for queued
  // sentence sessions while retaining the old endpoint as a fallback.
  const char *tts_bidirectional_url = "/api/v3/tts/bidirection";
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
  // 2026-09-18: Track the complete PCM path and task/queue pressure so a
  // reported TTS truncation can be separated into receive, queue, and I2S
  // playback stages from one serial capture.
  volatile size_t playback_bytes_queued = 0;
  volatile size_t playback_bytes_played = 0;
  volatile size_t playback_bytes_discarded = 0;
  volatile size_t playback_psram_bytes = 0;
  volatile size_t playback_internal_bytes = 0;
  volatile uint32_t playback_chunks_queued = 0;
  volatile uint32_t playback_chunks_played = 0;
  volatile uint32_t playback_chunks_discarded = 0;
  volatile UBaseType_t playback_queue_peak = 0;
  volatile UBaseType_t playback_queue_capacity = 0;
  volatile UBaseType_t playback_stack_high_water = 0;
  // 2026-09-18: Gateway-only state is retained for reference but excluded
  // from the board-direct build.
#if 0
  // 2026-09-18: Remember whether partial gateway audio must be drained on a provider disconnect.
  bool external_audio_received_ = false;
#endif
  QueueHandle_t sentence_queue = nullptr;
  TaskHandle_t sentence_task = nullptr;
  volatile bool sentence_stream_accepting = false;
  volatile bool sentence_stream_failed = false;
  volatile bool sentence_stream_finished = true;
  volatile bool sentence_stream_released = false;
  volatile uint16_t sentence_stream_count = 0;
  // 2026-09-18: Record the minimum remaining stack reported by FreeRTOS
  // during a complete LLM-to-TTS worker lifetime.
  volatile UBaseType_t sentence_stack_high_water = 0;

  // 2026-09-18: Track the physical bidirectional connection independently
  // from each sequential synthesis session on that connection.
  enum class ConnectionMode : uint8_t {
    NONE,
    UNIDIRECTIONAL,
    BIDIRECTIONAL,
  };
  ConnectionMode connection_mode = ConnectionMode::NONE;
  // 2026-09-18: Distinguish a retrying transport handshake from an active
  // bidirectional connection so transient disconnect callbacks do not abort
  // the whole reconnect window.
  volatile bool bidirectional_connecting = false;
  volatile bool bidirectional_transport_connected = false;
  volatile bool bidirectional_connection_ready = false;
  volatile bool bidirectional_connection_failed = false;
  volatile bool bidirectional_start_requested = false;
  volatile bool bidirectional_session_started = false;
  String bidirectional_session_id;
  // 2026-09-18: Distinguish an explicit protocol completion from an
  // ambiguous clean socket close and detect missing PCM sequence numbers.
  volatile bool explicit_response_end_received = false;
  volatile bool completion_from_disconnect = false;
  uint32_t audio_frame_count = 0;
  bool audio_sequence_seen = false;
  bool audio_sequence_gap = false;
  uint32_t last_audio_sequence = 0;

  void handleWebSocketEvent(WStype_t type, uint8_t *payload, size_t length);
  void resetResponseTracking();
  bool connectBidirectional();
  bool finishBidirectionalConnection();
  bool sendBidirectionalFrame(int32_t event, const String &session_id,
                              const String &payload);
  bool synthesizeBidirectionalSegment(const String &text,
                                      uint16_t segment_number);
  bool synthesizeBidirectionalSegmentWithRetry(const String &text,
                                               uint16_t segment_number);
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
