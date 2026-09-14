#include "act.h"

namespace {
constexpr uint8_t kMaxReplyActions = 3;
}

void sequent_act(String actions) {
  if (actions.isEmpty()) {
    return;
  }
  int startIndex = 0;
  int commaIndex;
  uint8_t action_count = 0;

  while (startIndex < actions.length() && action_count < kMaxReplyActions) {
    commaIndex = actions.indexOf(',', startIndex);
    String cmd;
  
    if (commaIndex != -1) {
      cmd = actions.substring(startIndex, commaIndex);
      startIndex = commaIndex + 1;
    } else {
      cmd = actions.substring(startIndex);
      startIndex = actions.length();
    }
    
    cmd.trim();
    executeCommand(cmd);
    action_count++;
  }
}

void random_act() {
  int random_num = random(100);

  // Idle behavior must not move the head unexpectedly. Head movement remains
  // available through explicit commands and conversation actions.
  if (random_num < 20) {
    eye_happy();
  } else if (random_num > 90) {
    eye_right();
  } else if (random_num > 80) {
    eye_left();
  } else {
    eye_blink();
  }
}

void async_sequent_act(String actions) {
  xTaskCreate(
    [](void* parameter) {
      String* actions = (String*)parameter;
      sequent_act(*actions);
      delete actions;
      vTaskDelete(NULL);
    },
    "SequentActTask",
    4096,                        // Stack size
    new String(actions),         // Pass actions as parameter
    1,                           // Priority
    NULL                         // Task handle
  );
}

void async_random_act() {
  xTaskCreate(
    [](void* parameter) {
      randomSeed(esp_random());  // Must seed random number generator
      random_act();
      vTaskDelete(NULL);
    },
    "RandomActTask", 
    4096,                        // Stack size
    NULL,                        // No parameters needed
    2,                           // Priority
    NULL                         // Task handle
  );
}

