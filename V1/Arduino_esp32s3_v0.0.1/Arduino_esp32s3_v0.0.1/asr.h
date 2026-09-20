#ifndef ASR_H
#define ASR_H

#include <ArduinoJson.h>
#include <WebSocketsClient.h>

#include "act.h"
#include "audio.h"
#include "common.h"

#define RECORD_TIME 30
// #define MAX_SILENCE_TIME 2
// 2026-09-17: Reduce the fixed endpoint wait while retaining enough pause tolerance for short spoken commands.
// #define MAX_SILENCE_TIME_MS 1200UL
// 2026-09-17: Shorten the requested VAD endpoint wait for faster command-response latency.
// #define MAX_SILENCE_TIME_MS 900UL
// #define MAX_SILENCE_TIME_MS 650UL
// 2026-09-18: Test a 500 ms endpoint as the lowest conservative value that
// still spans multiple 200 ms capture blocks while reducing turn latency.
#define MAX_SILENCE_TIME_MS 500UL
#define START_SPEECH_TIMEOUT 15
#define BUFFER_SIZE (SAMPLE_RATE / 5)

class AsrClient {
 public:
  AsrClient();
  String asrResult();
  void connect();
  void disconnect();
  void loop(int delay_time = 100);
  bool ASR();

 private:
  const char *host = "openspeech.bytedance.com";
  const char *asr_url = "/api/v3/sauc/bigmodel_async";
  const char *resource_id = "volc.seedasr.sauc.duration";
  const char *uid = "desk-emoji";

  WebSocketsClient webSocket;
  bool event_handler_ready = false;
  bool message_received = false;
  bool final_response_received = false;
  bool request_failed = false;
  bool request_active = false;
  bool voice_detected = false;
  String asr_result;

  void handleWebSocketEvent(WStype_t type, uint8_t *payload, size_t length);
  bool waitForMessage(uint32_t timeout_ms = 5000);
  bool waitForFinalResponse(uint32_t timeout_ms = 10000);
  String constructRequest();
  bool sendFullRequest();
  bool sendAudioRequest(const uint8_t *data, size_t length, bool is_last);
  bool parseResponse(const uint8_t *frame, size_t length);
  void parseResultJson(const uint8_t *payload, size_t length);
};

#endif
