#ifndef OLED_H
#define OLED_H

#include <Adafruit_SSD1306.h>
#include "common.h"

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
// 2026-09-11: Pin the OLED to I2C0 GPIO8/9 so the camera can use the same 0x3C address safely on I2C1 GPIO1/2.
#define OLED_SDA 8
#define OLED_SCL 9

extern Adafruit_SSD1306 oled;

// Functions
void setup_oled();
bool oled_is_ready();
void oled_print(String text, int delay_time = 100);
void oled_println(String text, int delay_time = 100);

#endif
