#include "sensor.h"


RevEng_PAJ7620 sensor = RevEng_PAJ7620();
static bool gesture_sensor_ready = false;

void setup_sensor() {
  // 2026-09-11: Skip I2C probing when the gesture sensor is not part of this hardware profile.
#if !DESK_EMOJI_ENABLE_GESTURE
  log_info("Gesture sensor disabled; skipping initialization.");
  return;
#endif
  if (!sensor.begin()) {
    log_error("Gesture Sensor Initiate Failed.");
    // Skip
  } else {
    gesture_sensor_ready = true;
    log_info("Gesture Sensor is Ready.");
  }
}

void handle_gesture() {
  // 2026-09-11: Never poll a gesture sensor that was disabled or failed initialization.
  if (!gesture_sensor_ready) {
    return;
  }
  Gesture gesture = sensor.readGesture();

  if (gesture != GES_NONE) {
    // servo_x.write(X_CENTER);
    // servo_y.write(Y_CENTER);
    // 2026-09-11: Center through the guarded head API instead of writing possibly unattached Servo objects.
    head_center(0);
    last_time = millis();
  }

  switch (gesture) {
    case GES_FORWARD:
      log_info("Gesture: Forward");
      eye_surprise();
      eye_blink();
      start_chat = true;
      break;
    case GES_BACKWARD:
      log_info("Gesture: Backward");
      eye_blink();
      break;
    case GES_UP:
      log_info("Gesture: Left");
      head_left(20);
      delay(1000);
      break;
    case GES_DOWN:
      log_info("Gesture: Right");
      head_right(20);
      delay(1000);
      break;
    case GES_RIGHT:
      log_info("Gesture: Up");
      head_up(20);
      break;
    case GES_LEFT:
      log_info("Gesture: Down");
      head_down(20);
      break;
    case GES_CLOCKWISE:
      log_info("Gesture: Clockwise");
      eye_happy();
      head_center();
      eye_blink();
      break;
    case GES_ANTICLOCKWISE:
      log_info("Gesture: Anticlockwise");
      play_animation(random(0, 42));
      break;
    case GES_WAVE:
      log_info("Gesture: Wave");
      eye_happy();
      head_shake(3);
      eye_blink();
      break;
    case GES_NONE:
      break;
  }
}
