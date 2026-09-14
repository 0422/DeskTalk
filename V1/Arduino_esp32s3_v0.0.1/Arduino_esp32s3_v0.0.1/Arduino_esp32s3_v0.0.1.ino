#include "net.h"
#include "oled.h"
#include "sensor.h"
#include "audio.h"
#include "emoji.h"
#include "head.h"
#include "cmd.h"
#include "act.h"
#include "animation.h"
// 2026-09-11: Add local wake-word control so a voice trigger can replace the unavailable gesture sensor.
#include "wake_word.h"
// 2026-09-11: Add local owner face recognition and pan/tilt following for the DVP camera.
#include "face_tracking.h"

unsigned long loop_start_time;

WifiClient wifiClient;
AsrClient asrClient;
TtsClient ttsClient;
LLM llm;

void websocket_loop() {
    asrClient.loop();
    ttsClient.loop();
}

void setup() {
  Serial.begin(115200);
  Serial.flush();
  // Set default log level
  log_set_level(LOG_LEVEL_INFO);
  log_info("Initializing...");
  setup_oled();
  setup_FFat();
  setup_head();
  setup_led();
  // 2026-09-11: Initialize the camera and restore enrolled owner identities before idle interaction begins.
  setup_face_tracking();
  wifiClient.setup_wifi();
  wifiClient.setup_udp();
  // 2026-09-11: Preserve the former fixed hardware omission; setup_sensor() now follows hardware_config.h and fails open.
  // setup_sensor();
  setup_sensor();
  setup_audio();
  // 2026-09-11: Start local WakeNet after the shared microphone I2S input has been initialized.
  setup_wake_word();
  emoji_init();
  log_info("Desk-Emoji is Ready.");
  last_time = millis();
}

void loop() {
  // 2026-09-11: Preserve the former fixed hardware omission; handle_gesture() is now a no-op unless setup succeeded.
  // handle_gesture();
  handle_gesture();
  // 2026-09-11: Transfer a locally detected wake word into the existing start_chat control flow.
  handle_wake_word();
  handle_cmd();
  // 2026-09-11: Process one low-rate face frame only when enrollment or owner following is active.
  handle_face_tracking();
  // 2026-09-11: Recover from a hotspot or router drop without blocking the main loop.
  wifiClient.maintain_wifi();
  websocket_loop();

  if (millis() - loop_start_time > 1000) {
    wifiClient.send_ip();
    loop_start_time = millis();
  }

  if (start_chat) {
    start_chat = false;
    // 2026-09-11: Pause local WakeNet before cloud ASR reads from the same I2S microphone.
    pause_wake_word();
    // 2026-09-11: Pause face inference and servo following while the conversation owns CPU, memory, and head actions.
    pause_face_tracking();
    loop_start_time = millis();
    while (millis() - loop_start_time < 600000) {  // max chat time 10 minutes
      // Speech-to-Text
      eye_happy();
      if (!asrClient.ASR()) {
        log_error("ASR Failed!");
      }
      websocket_loop();
      asrClient.disconnect();
      if (asrClient.asrResult() == "") {
        log_info("Exit Chat...");
        start_chat = false;
        blink_led(COLOR_RED, 3);
        break;
      }

      // Thinking...
      set_led(COLOR_YELLOW, 10);
      llm.chat(asrClient.asrResult());
      websocket_loop();
      async_sequent_act(llm.actions());

      // Text-to-Speech
      if (llm.answer().isEmpty()) {
        log_error("LLM returned no speakable answer; skipping TTS");
      } else if (!ttsClient.TTS(llm.answer())) {
        log_error("TTS Failed!");
      }
      websocket_loop();
    }
    // 2026-09-11: Restore local wake-word listening after the current conversation has ended.
    resume_wake_word();
    // 2026-09-11: Restore idle owner following after all conversation actions have ended.
    resume_face_tracking();
  }

  // if (enable_act && millis() - last_time > random(6, 10) * 1000) {
  // 2026-09-11: Do not let random idle motion fight face enrollment or owner following for the same servos.
  if (enable_act && !face_tracking_owns_head() && millis() - last_time > random(6, 10) * 1000) {
    random_act();
    last_time = millis();
  }

  delay(10);
}

// {"factory": "reboot"}
// {"factory": "reset_wifi"}
// {"factory": "adjust_x -10"}   -left, +right
// {"factory": "adjust_y -10"}    -up, +down
// 2026-09-11: Face commands: enroll_owner, clear_owner, face_follow_on, face_follow_off, face_status.
