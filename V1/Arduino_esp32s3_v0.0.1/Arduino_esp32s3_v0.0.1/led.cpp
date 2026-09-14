#include "led.h"

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
static bool led_ready = false;

void setup_led() {
  // 2026-09-11: Do not configure or drive the LED pin when the module is absent.
#if !DESK_EMOJI_ENABLE_LED
  log_info("LED disabled; skipping initialization.");
  return;
#endif
  strip.begin();
  strip.show();
  led_ready = true;
  log_info("LED is Ready.");
}

void set_led(uint32_t color, uint8_t brightness) {
  // 2026-09-11: Make status-light calls harmless when the LED is disabled.
  if (!led_ready) {
    return;
  }
  brightness = constrain(brightness, 0, 100);
  strip.setBrightness(map(brightness, 0, 100, 0, 255));
  strip.setPixelColor(0, color);
  strip.show();
}

void blink_led(uint32_t color, uint8_t times, uint8_t brightness) {
  // 2026-09-11: Avoid artificial blink delays when no LED was initialized.
  if (!led_ready) {
    return;
  }
  for (uint8_t i = 0; i < times; i++) {
    set_led(color, brightness);
    delay(50);
    set_led(COLOR_BLACK);
    delay(50);
  }
}

// 2026-09-11: Expose runtime readiness without leaking the NeoPixel object state.
bool led_is_ready() {
  return led_ready;
}
