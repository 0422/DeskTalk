#include "head.h"

int X_CENTER = 90;
int Y_CENTER = 90;
int X_MIN = X_CENTER - X_OFFSET;
int X_MAX = X_CENTER + X_OFFSET;
int Y_MIN = Y_CENTER - Y_OFFSET;
int Y_MAX = Y_CENTER + Y_OFFSET;

Servo servo_x;
Servo servo_y;
static bool head_ready = false;

// 2026-09-11: Recompute limits whenever calibration changes and prevent either axis from crossing 0 or 180 degrees.
static void update_head_limits() {
  X_CENTER = constrain(X_CENTER, SERVO_MIN_ANGLE + X_OFFSET,
                       SERVO_MAX_ANGLE - X_OFFSET);
  Y_CENTER = constrain(Y_CENTER, SERVO_MIN_ANGLE + Y_OFFSET,
                       SERVO_MAX_ANGLE - Y_OFFSET);
  X_MIN = X_CENTER - X_OFFSET;
  X_MAX = X_CENTER + X_OFFSET;
  Y_MIN = Y_CENTER - Y_OFFSET;
  Y_MAX = Y_CENTER + Y_OFFSET;
}

// 2026-09-11: Reject empty or non-numeric calibration files instead of interpreting them as a dangerous zero-degree center.
static bool parse_center_angle(String value, int &angle) {
  value.trim();
  if (value.isEmpty()) {
    return false;
  }

  for (unsigned int i = 0; i < value.length(); i++) {
    if (!isDigit(value.charAt(i))) {
      return false;
    }
  }

  angle = value.toInt();
  return angle >= SERVO_MIN_ANGLE && angle <= SERVO_MAX_ANGLE;
}

void servo_init() {
  // 2026-09-11: Never attach PWM outputs when the current hardware profile has no servos.
#if !DESK_EMOJI_ENABLE_HEAD
  return;
#endif
  // 2026-09-11: Avoid reattaching active servos during factory calibration because that can disturb PWM output.
  update_head_limits();
  if (!servo_x.attached()) {
    servo_x.attach(X_PIN);
  }
  if (!servo_y.attached()) {
    servo_y.attach(Y_PIN);
  }
  servo_x.write(X_CENTER);
  servo_y.write(Y_CENTER);
  head_ready = servo_x.attached() && servo_y.attached();
  // 2026-09-11: Expose the active calibration at INFO level so startup posture faults can be diagnosed without reading flash files.
  log_info("Servo calibration: X=%d range=%d..%d, Y=%d range=%d..%d",
           X_CENTER, X_MIN, X_MAX, Y_CENTER, Y_MIN, Y_MAX);
}

void setup_head() {
  // 2026-09-11: Skip calibration files, PWM attachment, and movement when the head is not installed.
#if !DESK_EMOJI_ENABLE_HEAD
  log_info("Head disabled; skipping initialization.");
  return;
#endif
  update_x_center();
  update_y_center();
  servo_init();
  // 2026-09-11: Continue startup if either optional servo channel could not be attached.
  if (!head_ready) {
    log_error("Head initialization failed; continuing without servo movement.");
    return;
  }
  head_center();
  delay(500);
  log_info("Head is Ready.");
}

void adjust_x_center(int offset) {
  // 2026-09-11: Apply calibration immediately, clamp it to a center that preserves the configured travel, and refresh limits.
  int requested_center = X_CENTER + offset;
  X_CENTER = constrain(requested_center, SERVO_MIN_ANGLE + X_OFFSET,
                       SERVO_MAX_ANGLE - X_OFFSET);
  update_head_limits();
  if (X_CENTER != requested_center) {
    log_warn("[Factory] X center request %d limited to %d", requested_center,
             X_CENTER);
  }

  File file = FFat.open(X_CENTER_FILE, FILE_WRITE);
  if (file) {
    file.print(String(X_CENTER));
    file.close();
    log_info("[Factory] Adjust X_CENTER to %d", X_CENTER);
  } else {
    log_error("Could not write file %s", X_CENTER_FILE);
  }
}

void adjust_y_center(int offset) {
  // 2026-09-11: Apply calibration immediately, clamp it to a center that preserves the configured travel, and refresh limits.
  int requested_center = Y_CENTER + offset;
  Y_CENTER = constrain(requested_center, SERVO_MIN_ANGLE + Y_OFFSET,
                       SERVO_MAX_ANGLE - Y_OFFSET);
  update_head_limits();
  if (Y_CENTER != requested_center) {
    log_warn("[Factory] Y center request %d limited to %d", requested_center,
             Y_CENTER);
  }

  File file = FFat.open(Y_CENTER_FILE, FILE_WRITE);
  if (file) {
    file.print(String(Y_CENTER));
    file.close();
    log_info("[Factory] Adjust Y_CENTER to %d", Y_CENTER);
  } else {
    log_error("Could not write file %s", Y_CENTER_FILE);
  }
}

void update_x_center() {
  File file = FFat.open(X_CENTER_FILE, FILE_READ);
  if (file) {
    String valueStr = file.readString();
    file.close();
    // 2026-09-11: Recover a corrupt saved center to 90 degrees and persist the repaired value.
    int saved_center = X_CENTER;
    if (parse_center_angle(valueStr, saved_center)) {
      X_CENTER = saved_center;
      int unclamped_center = X_CENTER;
      update_head_limits();
      if (X_CENTER != unclamped_center) {
        log_warn("Saved X center %d limited to %d", unclamped_center, X_CENTER);
        adjust_x_center(0);
      }
    } else {
      X_CENTER = 90;
      log_warn("Invalid saved X center; restored to %d", X_CENTER);
      adjust_x_center(0);
    }
  } else {
    adjust_x_center(0);
  }
  log_debug("X center angle: %d", X_CENTER);
}

void update_y_center() {
  File file = FFat.open(Y_CENTER_FILE, FILE_READ);
  if (file) {
    String valueStr = file.readString();
    file.close();
    // 2026-09-11: Recover a corrupt saved center to 90 degrees and persist the repaired value.
    int saved_center = Y_CENTER;
    if (parse_center_angle(valueStr, saved_center)) {
      Y_CENTER = saved_center;
      int unclamped_center = Y_CENTER;
      update_head_limits();
      if (Y_CENTER != unclamped_center) {
        log_warn("Saved Y center %d limited to %d", unclamped_center, Y_CENTER);
        adjust_y_center(0);
      }
    } else {
      Y_CENTER = 90;
      log_warn("Invalid saved Y center; restored to %d", Y_CENTER);
      adjust_y_center(0);
    }
  } else {
    adjust_y_center(0);
  }
  log_debug("Y center angle: %d", Y_CENTER);
}

void head_move(int x_offset, int y_offset, int servo_delay) {
  // 2026-09-11: Make all higher-level head actions safe when servo initialization was skipped.
  if (!head_ready) {
    return;
  }
  int x_angle = servo_x.read();
  int y_angle = servo_y.read();
  int to_x_angle = constrain(x_angle + x_offset, X_MIN, X_MAX);
  int to_y_angle = constrain(y_angle + y_offset, Y_MIN, Y_MAX);

  while (x_angle != to_x_angle || y_angle != to_y_angle) {
    if (x_angle != to_x_angle) {
      x_angle += (to_x_angle > x_angle ? STEP : -STEP);
      servo_x.write(x_angle);
    }
    if (y_angle != to_y_angle) {
      y_angle += (to_y_angle > y_angle ? STEP : -STEP);
      servo_y.write(y_angle);
    }
    delay(servo_delay);
  }
}

void head_center(int servo_delay) {
  // 2026-09-11: Avoid reading unattached Servo instances.
  if (!head_ready) {
    return;
  }
  int x_angle = servo_x.read();
  int y_angle = servo_y.read();
  head_move(X_CENTER - x_angle, Y_CENTER - y_angle, servo_delay);
}

void head_right(int offset) {
  head_move(offset, 0);
}

void head_left(int offset) {
  head_move(-offset, 0);
}

void head_down(int offset) {
  head_move(0, offset);
}

void head_up(int offset) {
  head_move(0, -offset);
}

void head_nod(int servo_delay) {
  // 2026-09-11: Avoid motion delays when no head is present.
  if (!head_ready) {
    return;
  }
  for (int i = 0; i < 2; i++) {
    head_move(0, 20, servo_delay);
    delay(50);
    head_move(0, -20, servo_delay);
    delay(50);
  }
}

void head_shake(int servo_delay) {
  // 2026-09-11: Avoid motion delays when no head is present.
  if (!head_ready) {
    return;
  }
  head_move(-10, 0, servo_delay);
  delay(50);
  head_move(20, 0, servo_delay);
  delay(50);
  head_move(-20, 0, servo_delay);
  delay(50);
  head_move(20, 0, servo_delay);
  delay(50);
  head_move(-10, 0, servo_delay);
  delay(50);
}

void head_roll_left(int servo_delay) {
  // 2026-09-11: Avoid multi-step movement when no head is present.
  if (!head_ready) {
    return;
  }
  head_center();
  head_down(Y_OFFSET / 2 / 2 + 5);
  head_move(-X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_move(-X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_center();
}

void head_roll_right(int servo_delay) {
  // 2026-09-11: Avoid multi-step movement when no head is present.
  if (!head_ready) {
    return;
  }
  head_center();
  head_down(Y_OFFSET / 2 / 2 + 5);
  head_move(X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(-X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(-X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_move(X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_center();
}

// 2026-09-11: Report whether both servo channels were attached successfully.
bool head_is_ready() {
  return head_ready;
}
