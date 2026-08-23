#include "emoji.h"
#include "animation.h"

// 增加codex额度显示
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup_emoji() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("[Error] SSD1306 Initiate Failed.");
    while (1);  // Pause
  } else {
    Serial.println("SSD1306 is Ready.");
    display.clearDisplay();
  }
  play_animation(35);
  eye_sleep();
  eye_wakeup();
  eye_center();
  eye_blink();
}

void saccade(int direction_x, int direction_y) {
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
  display.clearDisplay();
  //draw from center
  int x = int(left_eye_x - left_eye_width / 2);
  int y = int(left_eye_y - left_eye_height / 2);
  display.fillRoundRect(x, y, left_eye_width, left_eye_height, ref_corner_radius, SSD1306_WHITE);
  x = int(right_eye_x - right_eye_width / 2);
  y = int(right_eye_y - right_eye_height / 2);
  display.fillRoundRect(x, y, right_eye_width, right_eye_height, ref_corner_radius, SSD1306_WHITE);
  if (update) {
    display.display();
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
  eye_sleep();
  for (int h = 0; h <= ref_eye_height; h += 2) {
    left_eye_height = h;
    right_eye_height = h;
    draw_eyes(true);
  }
}

void eye_happy() {
  eye_center(false);
  //draw inverted triangle over eye lower part
  int offset = ref_eye_height / 2;
  for (int i = 0; i < 10; i++) {
    display.fillTriangle(
      left_eye_x - left_eye_width / 2 - 1, left_eye_y + offset,
      left_eye_x + left_eye_width / 2 + 1, left_eye_y + 5 + offset,
      left_eye_x - left_eye_width / 2 - 1, left_eye_y + left_eye_height + offset,
      SSD1306_BLACK);
    display.fillTriangle(
      right_eye_x + right_eye_width / 2 + 1, right_eye_y + offset,
      right_eye_x - left_eye_width / 2 - 1, right_eye_y + 5 + offset,
      right_eye_x + right_eye_width / 2 + 1, right_eye_y + right_eye_height + offset,
      SSD1306_BLACK);
    offset -= 2;
    display.display();
  }
  display.display();
  delay(100);
}

void eye_sad() {
  eye_center(false);
  int offset = ref_eye_height / 2;
  for (int i = 0; i < 10; i++) {
    display.fillTriangle(
      left_eye_x - left_eye_width / 2 - 5, left_eye_y - offset + 5,
      left_eye_x + left_eye_width / 2 + 5, left_eye_y - 5 - offset,
      left_eye_x - left_eye_width / 2 - 5, left_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    display.fillTriangle(
      right_eye_x + right_eye_width / 2 + 5, right_eye_y - offset + 5,
      right_eye_x - left_eye_width / 2 - 5, right_eye_y - 5 - offset,
      right_eye_x + right_eye_width / 2 + 5, right_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    offset -= 2;
    display.display();
  }
  display.display();
  delay(100);
}

void eye_anger() {
  eye_center(false);
  int offset = ref_eye_height / 2;
  for (int i = 0; i < 10; i++) {
    display.fillTriangle(
      left_eye_x + left_eye_width / 2 + 5, left_eye_y - offset + 5,
      left_eye_x - left_eye_width / 2 - 5, left_eye_y - 5 - offset,
      left_eye_x + left_eye_width / 2 + 5, left_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    display.fillTriangle(
      right_eye_x - right_eye_width / 2 - 5, right_eye_y - offset + 5,
      right_eye_x + right_eye_width / 2 + 5, right_eye_y - 5 - offset,
      right_eye_x - right_eye_width / 2 - 5, right_eye_y - left_eye_height - offset,
      SSD1306_BLACK);
    offset -= 2;
    display.display();
  }
  display.display();
  delay(100);
}

void eye_surprise() {
  eye_center(false);
  int initial_width = left_eye_width;
  int initial_height = left_eye_height;
  int min_width = 10;
  int min_height = 10;
  int corner_radius = ref_corner_radius;

  while (initial_width > min_width && initial_height > min_height) {
    display.clearDisplay();
    int x = int(left_eye_x - initial_width / 2);
    int y = int(left_eye_y - initial_height / 2);
    display.fillRoundRect(x, y, initial_width, initial_height, corner_radius, SSD1306_WHITE);

    x = int(right_eye_x - initial_width / 2);
    y = int(right_eye_y - initial_height / 2);
    display.fillRoundRect(x, y, initial_width, initial_height, corner_radius, SSD1306_WHITE);
    display.display();

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

// 增加codex额度显示
// 2026-08-21 修改：字号改大（size 2，超宽则回退size 1）并居中显示；显示3秒后自动恢复默认表情，
// 避免PC端紧接着发送的表情/动画命令把文字瞬间覆盖掉（之前表现为“一闪而过”）
void show_text(const char* text) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    int16_t x1, y1;
    uint16_t w, h;
    display.setTextSize(2);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    if (w > SCREEN_WIDTH - 4) {
        display.setTextSize(1);
        display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    }

    int16_t cursor_x = (SCREEN_WIDTH - w) / 2 - x1;
    int16_t cursor_y = (SCREEN_HEIGHT - h) / 2 - y1;
    display.setCursor(cursor_x, cursor_y);
    display.print(text);
    display.display();

    delay(3000);
    eye_center(true);
}

void show_text_scroll(const char* text, int delay_ms) {
    int text_width = strlen(text) * 6; // 每个字符约6像素宽
    int screen_width = SCREEN_WIDTH;
    
    // 如果文字比屏幕短，直接显示
    if (text_width < screen_width) {
        show_text(text);
        return;
    }
    
    // 滚动显示
    for (int offset = 0; offset < text_width - screen_width + 1; offset++) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(-offset, 0);
        display.println(text);
        display.display();
        delay(delay_ms);
    }
}