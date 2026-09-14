#include "oled.h"

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool oled_ready = false;

void setup_oled() {
  // 2026-09-11: Skip the I2C transaction entirely when this hardware profile has no OLED.
#if !DESK_EMOJI_ENABLE_OLED
  log_info("OLED disabled; skipping initialization.");
  return;
#endif
  // 2026-09-11: Initialize the OLED bus explicitly before the separate camera SCCB controller is installed.
  Wire.begin(OLED_SDA, OLED_SCL);
  // if (!oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
  // 2026-09-11: Keep Adafruit_SSD1306 from reinitializing the already pinned OLED I2C0 bus.
  if (!oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS, true, false)) {
    log_error("OLED Initiate Failed.");
    // while (true);  // Pause
    // 2026-09-11: A missing optional display must not prevent audio and network startup.
    return;
  } else {
    oled_ready = true;
    log_info("OLED is Ready.");
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled_println("Desk-Emoji v" + String(VERSION), 500);
  }
}

void oled_print(String text, int delay_time) {
  // 2026-09-11: Ignore display output when initialization was disabled or failed.
  if (!oled_ready) {
    return;
  }
  oled.print(text);
  oled.display();
  delay(delay_time);
}

void oled_println(String text, int delay_time) {
  // 2026-09-11: Ignore display output when initialization was disabled or failed.
  if (!oled_ready) {
    return;
  }
  oled.println(text);
  oled.display();
  delay(delay_time);
}

// 2026-09-11: Let animation and network code avoid touching an uninitialized display buffer.
bool oled_is_ready() {
  return oled_ready;
}
