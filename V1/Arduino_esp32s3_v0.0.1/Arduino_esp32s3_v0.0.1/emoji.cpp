#include "emoji.h"
#include "animation.h"

// Adjustable
const int ref_eye_height = 40;
const int ref_eye_width = 40;
const int ref_space_between_eye = 10;
const int ref_corner_radius = 10;

// Current state of the eyes
int left_eye_height = ref_eye_height;
int left_eye_width = ref_eye_width;
int right_eye_x = 32 + ref_eye_width + ref_space_between_eye;
int left_eye_x = 32;
int left_eye_y = 32;
int right_eye_y = 32;
int right_eye_height = ref_eye_height;
int right_eye_width = ref_eye_width;

void saccade(int direction_x, int direction_y) {
  // 2026-09-11: Skip animation state and timing when the OLED is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  //quick movement of the eye, no size change. stay at position after movement, will not move back,  call again with opposite direction
  //direction == -1 :  move left
  //direction == 1 :  move right

  int direction_x_movement_amplitude = 8;
  int direction_y_movement_amplitude = 6;
  int eye_blink_amplitude = 8;

  for (int i = 0; i < 1; i++) {
    left_eye_x += direction_x_movement_amplitude * direction_x;
    right_eye_x += direction_x_movement_amplitude * direction_x;
    left_eye_y += direction_y_movement_amplitude * direction_y;
    right_eye_y += direction_y_movement_amplitude * direction_y;
    right_eye_height -= eye_blink_amplitude;
    left_eye_height -= eye_blink_amplitude;
    draw_eyes();
    delay(1);
  }

  for (int i = 0; i < 1; i++) {
    left_eye_x += direction_x_movement_amplitude * direction_x;
    right_eye_x += direction_x_movement_amplitude * direction_x;
    left_eye_y += direction_y_movement_amplitude * direction_y;
    right_eye_y += direction_y_movement_amplitude * direction_y;
    right_eye_height += eye_blink_amplitude;
    left_eye_height += eye_blink_amplitude;
    draw_eyes();
    delay(1);
  }
}

void move_eye(int direction) {  // MOVES TO RIGHT OR LEFT DEPENDING ON 1 OR -1 INPUT.
  // 2026-09-11: Skip animation state and timing when the OLED is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  //direction == -1 :  move left
  //direction == 1 :  move right

  int direction_oversize = 1;
  int direction_movement_amplitude = 2;
  int eye_blink_amplitude = 5;

  for (int i = 0; i < 3; i++) {
    left_eye_x += direction_movement_amplitude * direction;
    right_eye_x += direction_movement_amplitude * direction;
    right_eye_height -= eye_blink_amplitude;
    left_eye_height -= eye_blink_amplitude;
    if (direction > 0) {
      right_eye_height += direction_oversize;
      right_eye_width += direction_oversize;
    } else {
      left_eye_height += direction_oversize;
      left_eye_width += direction_oversize;
    }

    draw_eyes();
    delay(1);
  }
  for (int i = 0; i < 3; i++) {
    left_eye_x += direction_movement_amplitude * direction;
    right_eye_x += direction_movement_amplitude * direction;
    right_eye_height += eye_blink_amplitude;
    left_eye_height += eye_blink_amplitude;
    if (direction > 0) {
      right_eye_height += direction_oversize;
      right_eye_width += direction_oversize;
    } else {
      left_eye_height += direction_oversize;
      left_eye_width += direction_oversize;
    }
    draw_eyes();
    delay(1);
  }

  delay(1000);

  for (int i = 0; i < 3; i++) {
    left_eye_x -= direction_movement_amplitude * direction;
    right_eye_x -= direction_movement_amplitude * direction;
    right_eye_height -= eye_blink_amplitude;
    left_eye_height -= eye_blink_amplitude;
    if (direction > 0) {
      right_eye_height -= direction_oversize;
      right_eye_width -= direction_oversize;
    } else {
      left_eye_height -= direction_oversize;
      left_eye_width -= direction_oversize;
    }
    draw_eyes();
    delay(1);
  }
  for (int i = 0; i < 3; i++) {
    left_eye_x -= direction_movement_amplitude * direction;
    right_eye_x -= direction_movement_amplitude * direction;
    right_eye_height += eye_blink_amplitude;
    left_eye_height += eye_blink_amplitude;
    if (direction > 0) {
      right_eye_height -= direction_oversize;
      right_eye_width -= direction_oversize;
    } else {
      left_eye_height -= direction_oversize;
      left_eye_width -= direction_oversize;
    }
    draw_eyes();
    delay(1);
  }
  eye_center();
}

void draw_eyes(bool update) {
  // 2026-09-11: Protect the Adafruit display buffer when oled.begin() was skipped or failed.
  if (!oled_is_ready()) {
    return;
  }
  oled.clearDisplay();
  //draw from center
  int x = int(left_eye_x - left_eye_width / 2);
  int y = int(left_eye_y - left_eye_height / 2);
  oled.fillRoundRect(x, y, left_eye_width, left_eye_height, ref_corner_radius, SSD1306_WHITE);
  x = int(right_eye_x - right_eye_width / 2);
  y = int(right_eye_y - right_eye_height / 2);
  oled.fillRoundRect(x, y, right_eye_width, right_eye_height, ref_corner_radius, SSD1306_WHITE);
  if (update) {
    oled.display();
  }
}

void eye_center(bool update) {
  //move eyes to the center of the display, defined by SCREEN_WIDTH, SCREEN_HEIGHT
  left_eye_height = ref_eye_height;
  left_eye_width = ref_eye_width;
  right_eye_height = ref_eye_height;
  right_eye_width = ref_eye_width;

  left_eye_x = SCREEN_WIDTH / 2 - ref_eye_width / 2 - ref_space_between_eye / 2;
  left_eye_y = SCREEN_HEIGHT / 2;
  right_eye_x = SCREEN_WIDTH / 2 + ref_eye_width / 2 + ref_space_between_eye / 2;
  right_eye_y = SCREEN_HEIGHT / 2;

  draw_eyes(update);
}

void eye_blink(int speed) {
  // 2026-09-11: Skip animation timing when the OLED is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  draw_eyes();
  for (int i = 0; i < 3; i++) {
    left_eye_height = left_eye_height - speed;
    right_eye_height = right_eye_height - speed;
    draw_eyes();
  }
  for (int i = 0; i < 3; i++) {
    left_eye_height = left_eye_height + speed;
    right_eye_height = right_eye_height + speed;
    draw_eyes();
  }
}

void eye_sleep() {  // DRAWS A LINE TO LOOK LIKE SLEEPING
  left_eye_height = 2;
  right_eye_height = 2;
  draw_eyes(true);
}

void eye_wakeup() {  // WAKE UP THE EYES FROM AN LINE TO ROUND CORNERED SQUARE
  // 2026-09-11: Skip animation timing when the OLED is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  eye_sleep();
  for (int h = 0; h <= ref_eye_height; h += 2) {
    left_eye_height = h;
    right_eye_height = h;
    draw_eyes(true);
  }
}

void eye_happy() {
  // 2026-09-11: Protect direct OLED drawing when the display is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  eye_center(false);
  //draw inverted triangle over eye lower part
  int offset = ref_eye_height / 2;
  for (int i = 0; i < 10; i++) {
    oled.fillTriangle(
      left_eye_x - left_eye_width / 2 - 1, left_eye_y + offset,
      left_eye_x + left_eye_width / 2 + 1, left_eye_y + 5 + offset,
      left_eye_x - left_eye_width / 2 - 1, left_eye_y + left_eye_height + offset,
      SSD1306_BLACK);
    oled.fillTriangle(
      right_eye_x + right_eye_width / 2 + 1, right_eye_y + offset,
      right_eye_x - left_eye_width / 2 - 1, right_eye_y + 5 + offset,
      right_eye_x + right_eye_width / 2 + 1, right_eye_y + right_eye_height + offset,
      SSD1306_BLACK);
    offset -= 2;
    oled.display();
  }
  oled.display();
  delay(100);
}

void eye_sad() {
  // 2026-09-11: Protect direct OLED drawing when the display is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  eye_center(false);
  int offset = ref_eye_height / 2;
  for (int i = 0; i < 10; i++) {
    oled.fillTriangle(
      left_eye_x - left_eye_width / 2 - 5, left_eye_y - offset + 5,
      left_eye_x + left_eye_width / 2 + 5, left_eye_y - 5 - offset,
      left_eye_x - left_eye_width / 2 - 5, left_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    oled.fillTriangle(
      right_eye_x + right_eye_width / 2 + 5, right_eye_y - offset + 5,
      right_eye_x - left_eye_width / 2 - 5, right_eye_y - 5 - offset,
      right_eye_x + right_eye_width / 2 + 5, right_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    offset -= 2;
    oled.display();
  }
  oled.display();
  delay(100);
}

void eye_anger() {
  // 2026-09-11: Protect direct OLED drawing when the display is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  eye_center(false);
  int offset = ref_eye_height / 2;
  for (int i = 0; i < 10; i++) {
    oled.fillTriangle(
      left_eye_x + left_eye_width / 2 + 5, left_eye_y - offset + 5,
      left_eye_x - left_eye_width / 2 - 5, left_eye_y - 5 - offset,
      left_eye_x + left_eye_width / 2 + 5, left_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    oled.fillTriangle(
      right_eye_x - right_eye_width / 2 - 5, right_eye_y - offset + 5,
      right_eye_x + right_eye_width / 2 + 5, right_eye_y - 5 - offset,
      right_eye_x - right_eye_width / 2 - 5, right_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    offset -= 2;
    oled.display();
  }
  oled.display();
  delay(100);
}

void eye_surprise() {
  // 2026-09-11: Protect direct OLED drawing when the display is unavailable.
  if (!oled_is_ready()) {
    return;
  }
  eye_center(false);
  int initial_width = left_eye_width;
  int initial_height = left_eye_height;
  int min_width = 10;
  int min_height = 10;
  int corner_radius = ref_corner_radius;

  while (initial_width > min_width && initial_height > min_height) {
    oled.clearDisplay();
    int x = int(left_eye_x - initial_width / 2);
    int y = int(left_eye_y - initial_height / 2);
    oled.fillRoundRect(x, y, initial_width, initial_height, corner_radius, SSD1306_WHITE);

    x = int(right_eye_x - initial_width / 2);
    y = int(right_eye_y - initial_height / 2);
    oled.fillRoundRect(x, y, initial_width, initial_height, corner_radius, SSD1306_WHITE);
    oled.display();

    initial_width -= 2;
    initial_height -= 2;
    corner_radius = max(corner_radius - 1, 1);
  }
  delay(100);
}

void eye_right() {
  move_eye(1);
}

void eye_left() {
  move_eye(-1);
}

// 2026-09-18: Frame the existing eyes with compact ear shapes to acknowledge active ASR without an animation delay.
void eye_listening() {
  if (!oled_is_ready()) {
    return;
  }
  eye_center(false);
  draw_eyes(false);
  oled.drawRoundRect(3, 21, 15, 27, 7, SSD1306_WHITE);
  oled.drawRoundRect(110, 21, 15, 27, 7, SSD1306_WHITE);
  oled.drawFastVLine(11, 29, 11, SSD1306_WHITE);
  oled.drawFastVLine(116, 29, 11, SSD1306_WHITE);
  oled.display();
}

// 2026-09-18: Show an upward, asymmetric gaze while ASR finalization and cloud response are pending.
void eye_thinking() {
  if (!oled_is_ready()) {
    return;
  }
  eye_center(false);
  left_eye_y -= 5;
  right_eye_y += 4;
  right_eye_height = 24;
  draw_eyes(false);
  oled.fillCircle(83, 8, 2, SSD1306_WHITE);
  oled.fillCircle(96, 11, 3, SSD1306_WHITE);
  oled.fillCircle(109, 8, 2, SSD1306_WHITE);
  oled.display();
}

void emoji_init() {
  // 2026-09-11: Do not run the startup animation without an initialized OLED.
  if (!oled_is_ready()) {
    return;
  }
  // 2026-09-18: Measure each startup OLED animation step because the wake
  // model is ready before Desk-Emoji reports setup completion.
  const unsigned long started_at = millis();
  unsigned long step_started_at = started_at;
  eye_sleep();
  log_info("BOOT emoji eye_sleep=%lu ms", millis() - step_started_at);
  step_started_at = millis();
  eye_wakeup();
  log_info("BOOT emoji eye_wakeup=%lu ms", millis() - step_started_at);
  step_started_at = millis();
  eye_center();
  log_info("BOOT emoji eye_center=%lu ms", millis() - step_started_at);
  step_started_at = millis();
  eye_blink();
  log_info("BOOT emoji eye_blink=%lu ms, total=%lu ms",
           millis() - step_started_at, millis() - started_at);
}
