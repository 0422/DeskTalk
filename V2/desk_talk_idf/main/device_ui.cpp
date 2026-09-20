// 2026-09-19: Provide native SSD1306 status faces and LEDC servo movement while preserving disabled camera/gesture boundaries.
#include "device_ui.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "animation_assets.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
// 2026-09-20: Serialize status-light updates from conversation, microphone and playback tasks.
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware_config.h"
#include "led_strip.h"
#include "storage.h"

namespace desk_talk::ui {
namespace {

constexpr char kLogTag[] = "ui";
constexpr i2c_port_t kI2cPort = I2C_NUM_0;
constexpr int kServoFrequency = 50;
constexpr int kServoResolutionBits = 14;
constexpr int kHeadXOffset = 25;
constexpr int kHeadYOffset = 45;
bool oled_ready = false;
bool head_ready = false;
led_strip_handle_t status_strip = nullptr;
// 2026-09-20: Use a static mutex and cached color to avoid concurrent RMT refreshes and redundant updates on every audio block.
StaticSemaphore_t status_mutex_buffer;
SemaphoreHandle_t status_mutex = nullptr;
uint32_t status_rgb = 0;
uint8_t status_brightness = 0;
int head_x = 90;
int head_y = 90;
int head_x_center = 90;
int head_y_center = 90;
bool idle_enabled = true;
std::array<uint8_t, 128 * 8> oled_frame{};

void set_pixel(int x, int y, bool enabled = true) {
  if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
  uint8_t &value = oled_frame[x + (y / 8) * 128];
  const uint8_t mask = static_cast<uint8_t>(1U << (y & 7));
  if (enabled) value |= mask;
  else value &= static_cast<uint8_t>(~mask);
}

void fill_rectangle(int left, int top, int right, int bottom,
                    bool enabled = true) {
  for (int y = top; y <= bottom; ++y)
    for (int x = left; x <= right; ++x) set_pixel(x, y, enabled);
}

esp_err_t oled_command(uint8_t command) {
  const uint8_t data[2] = {0x00, command};
  return i2c_master_write_to_device(kI2cPort, hardware::kOledAddress, data,
                                    sizeof(data), pdMS_TO_TICKS(100));
}

esp_err_t oled_data(const uint8_t *data, size_t length) {
  std::array<uint8_t, 17> packet{};
  packet[0] = 0x40;
  for (size_t offset = 0; offset < length;) {
    const size_t chunk = std::min<size_t>(16, length - offset);
    std::memcpy(packet.data() + 1, data + offset, chunk);
    const esp_err_t result = i2c_master_write_to_device(
        kI2cPort, hardware::kOledAddress, packet.data(), chunk + 1,
        pdMS_TO_TICKS(100));
    if (result != ESP_OK) return result;
    offset += chunk;
  }
  return ESP_OK;
}

void flush_oled() {
  if (!oled_ready) return;
  oled_command(0x21);
  oled_command(0);
  oled_command(127);
  oled_command(0x22);
  oled_command(0);
  oled_command(7);
  oled_data(oled_frame.data(), oled_frame.size());
}

void draw_face(FaceState state) {
  if (!oled_ready) return;
  oled_frame.fill(0);
  if (state == FaceState::kListening) {
    fill_rectangle(22, 18, 48, 45);
    fill_rectangle(79, 18, 105, 45);
    for (int y = 22; y <= 42; ++y) {
      set_pixel(7, y); set_pixel(120, y);
    }
  } else if (state == FaceState::kThinking) {
    fill_rectangle(22, 18, 48, 32);
    fill_rectangle(79, 25, 105, 42);
    fill_rectangle(91, 8, 94, 11);
    fill_rectangle(104, 4, 108, 8);
  } else if (state == FaceState::kHappy) {
    for (int x = 18; x <= 51; ++x)
      for (int width = 0; width < 4; ++width)
        set_pixel(x, 24 + (x - 35) * (x - 35) / 42 + width);
    for (int x = 76; x <= 109; ++x)
      for (int width = 0; width < 4; ++width)
        set_pixel(x, 24 + (x - 93) * (x - 93) / 42 + width);
  } else if (state == FaceState::kSad || state == FaceState::kError) {
    for (int x = 18; x <= 51; ++x)
      for (int width = 0; width < 4; ++width)
        set_pixel(x, 36 - (x - 35) * (x - 35) / 42 + width);
    for (int x = 76; x <= 109; ++x)
      for (int width = 0; width < 4; ++width)
        set_pixel(x, 36 - (x - 93) * (x - 93) / 42 + width);
  } else if (state == FaceState::kAngry) {
    fill_rectangle(20, 22, 49, 45);
    fill_rectangle(78, 22, 107, 45);
    for (int offset = 0; offset < 18; ++offset) {
      fill_rectangle(20 + offset, 18 + offset / 2, 20 + offset, 22 + offset / 2,
                     false);
      fill_rectangle(107 - offset, 18 + offset / 2, 107 - offset,
                     22 + offset / 2, false);
    }
  } else if (state == FaceState::kSurprised) {
    fill_rectangle(27, 22, 43, 39);
    fill_rectangle(84, 22, 100, 39);
  } else if (state == FaceState::kLeft) {
    fill_rectangle(15, 18, 39, 45);
    fill_rectangle(62, 18, 90, 45);
  } else if (state == FaceState::kRight) {
    fill_rectangle(37, 18, 65, 45);
    fill_rectangle(88, 18, 112, 45);
  } else if (state == FaceState::kSleeping) {
    fill_rectangle(18, 31, 50, 33);
    fill_rectangle(77, 31, 109, 33);
  } else {
    fill_rectangle(24, 18, 46, 45);
    fill_rectangle(81, 18, 103, 45);
  }
  flush_oled();
}

bool parse_center(const std::string &text, int minimum, int maximum,
                  int &value) {
  if (text.empty()) return false;
  char *end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < minimum || parsed > maximum)
    return false;
  value = static_cast<int>(parsed);
  return true;
}

void draw_bitmap_frame(const uint8_t *source) {
  if (!oled_ready || source == nullptr) return;
  oled_frame.fill(0);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const uint8_t mask = static_cast<uint8_t>(0x80U >> (x & 7));
      if ((source[y * 8 + x / 8] & mask) != 0) set_pixel(x + 32, y);
    }
  }
  flush_oled();
}

void play_frames_native(const uint8_t *frames, size_t frame_count,
                        int loop_count) {
  if (!oled_ready || frames == nullptr || frame_count == 0) return;
  for (int loop = 0; loop < std::max(1, loop_count); ++loop) {
    for (size_t frame = 0; frame < frame_count; ++frame) {
      draw_bitmap_frame(frames + frame * 512);
      vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY));
    }
  }
}

constexpr const char *kAnimationNames[] = {
    "heart", "calendar", "face_id", "cola", "laugh", "dumbbell",
    "skateboard", "battery", "basketball", "rugby", "alarm", "screen",
    "wifi", "youtube", "tv", "movie", "cat", "write", "phone",
    "sunny", "cloudy", "rainy", "windy", "snow", "beer", "walk",
    "shit", "cry", "puzzled", "football", "volleyball", "badminton",
    "rice", "gym", "boat", "thinking", "money", "wait", "plane",
    "rocket", "ok", "love"};

uint32_t servo_duty(int degrees) {
  degrees = std::clamp(degrees, 0, 180);
  const uint32_t maximum = (1U << kServoResolutionBits) - 1U;
  const uint32_t pulse_us = 500U + static_cast<uint32_t>(degrees) * 2000U / 180U;
  return pulse_us * maximum / 20000U;
}

void write_head(int x, int y) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, servo_duty(x));
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, servo_duty(y));
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

}  // namespace

esp_err_t initialize() {
  if (hardware::kEnableLed) {
    // 2026-09-20: Establish the status-light lock before audio tasks can request a color change.
    status_mutex = xSemaphoreCreateMutexStatic(&status_mutex_buffer);
    // 2026-09-19: Drive the single GPIO48 WS2812 with the native RMT component instead of Adafruit_NeoPixel.
    led_strip_config_t strip_configuration = {};
    strip_configuration.strip_gpio_num = hardware::kStatusLed;
    strip_configuration.max_leds = 1;
    strip_configuration.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    strip_configuration.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt_configuration = {};
    rmt_configuration.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_configuration.resolution_hz = 10 * 1000 * 1000;
    if (led_strip_new_rmt_device(&strip_configuration, &rmt_configuration,
                                 &status_strip) == ESP_OK) {
      led_strip_clear(status_strip);
    } else {
      status_strip = nullptr;
      ESP_LOGW(kLogTag, "Status LED unavailable");
    }
  }
  if (hardware::kEnableOled) {
    i2c_config_t configuration = {};
    configuration.mode = I2C_MODE_MASTER;
    configuration.sda_io_num = hardware::kOledSda;
    configuration.scl_io_num = hardware::kOledScl;
    configuration.sda_pullup_en = GPIO_PULLUP_ENABLE;
    configuration.scl_pullup_en = GPIO_PULLUP_ENABLE;
    configuration.master.clk_speed = 400000;
    esp_err_t result = i2c_param_config(kI2cPort, &configuration);
    if (result == ESP_OK) result = i2c_driver_install(kI2cPort,
        I2C_MODE_MASTER, 0, 0, 0);
    if (result == ESP_OK) {
      const uint8_t commands[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
          0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F,
          0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
      for (uint8_t command : commands) {
        result = oled_command(command);
        if (result != ESP_OK) break;
      }
    }
    oled_ready = result == ESP_OK;
    if (!oled_ready) ESP_LOGW(kLogTag, "OLED unavailable");
  }
  if (hardware::kEnableHead) {
    // 2026-09-19: Restore V1 servo centers from SPIFFS and reject corrupt values before enabling PWM.
    std::string saved_x;
    std::string saved_y;
    if (!storage::read_text_file("/spiffs/X_CENTER.txt", saved_x) ||
        !parse_center(saved_x, kHeadXOffset, 180 - kHeadXOffset,
                      head_x_center)) {
      head_x_center = 90;
    }
    if (!storage::read_text_file("/spiffs/Y_CENTER.txt", saved_y) ||
        !parse_center(saved_y, kHeadYOffset, 180 - kHeadYOffset,
                      head_y_center)) {
      head_y_center = 90;
    }
    head_x = head_x_center;
    head_y = head_y_center;
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_14_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = kServoFrequency;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t result = ledc_timer_config(&timer);
    for (int index = 0; index < 2 && result == ESP_OK; ++index) {
      ledc_channel_config_t channel = {};
      channel.gpio_num = index == 0 ? hardware::kHeadX : hardware::kHeadY;
      channel.speed_mode = LEDC_LOW_SPEED_MODE;
      channel.channel = index == 0 ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1;
      channel.intr_type = LEDC_INTR_DISABLE;
      channel.timer_sel = LEDC_TIMER_0;
      channel.duty = servo_duty(index == 0 ? head_x_center : head_y_center);
      result = ledc_channel_config(&channel);
    }
    head_ready = result == ESP_OK;
  }
  draw_face(FaceState::kNeutral);
  ESP_LOGI(kLogTag, "Servo centers: x=%d y=%d", head_x_center,
           head_y_center);
  return ESP_OK;
}

void set_face(FaceState state) { draw_face(state); }

void set_status_color(uint32_t rgb, uint8_t brightness) {
  if (status_strip == nullptr) return;
  // 2026-09-20: Protect the single LED pixel buffer through the complete RMT refresh, without adding delays to ASR or TTS.
  if (status_mutex == nullptr ||
      xSemaphoreTake(status_mutex, portMAX_DELAY) != pdTRUE) return;
  brightness = std::min<uint8_t>(brightness, 100);
  // 2026-09-20: Refresh only when the visible status changes, rather than once per microphone/PCM block.
  if (rgb == status_rgb && brightness == status_brightness) {
    xSemaphoreGive(status_mutex);
    return;
  }
  const uint8_t red = static_cast<uint8_t>(((rgb >> 16) & 0xFF) * brightness / 100);
  const uint8_t green = static_cast<uint8_t>(((rgb >> 8) & 0xFF) * brightness / 100);
  const uint8_t blue = static_cast<uint8_t>((rgb & 0xFF) * brightness / 100);
#if 0
  // 2026-09-20: Retain the former unchecked writes; cache a color only after the LED driver accepts and transmits it.
  led_strip_set_pixel(status_strip, 0, red, green, blue);
  led_strip_refresh(status_strip);
#endif
  // 2026-09-20: Keep failed refreshes retryable and release the shared light lock on either outcome.
  if (led_strip_set_pixel(status_strip, 0, red, green, blue) == ESP_OK &&
      led_strip_refresh(status_strip) == ESP_OK) {
    status_rgb = rgb;
    status_brightness = brightness;
  } else {
    ESP_LOGW(kLogTag, "Status LED update failed");
  }
  xSemaphoreGive(status_mutex);
}

void blink_status(uint32_t rgb, uint8_t times) {
  // 2026-09-20: A disabled/unavailable LED must not add artificial delays to conversation recovery.
  if (status_strip == nullptr || status_mutex == nullptr) return;
  for (uint8_t index = 0; index < times; ++index) {
    set_status_color(rgb, 10);
    vTaskDelay(pdMS_TO_TICKS(50));
    set_status_color(0, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void move_head(int x_offset, int y_offset, uint32_t step_delay_ms) {
  if (!head_ready) return;
  const int target_x = std::clamp(head_x + x_offset,
                                  head_x_center - kHeadXOffset,
                                  head_x_center + kHeadXOffset);
  const int target_y = std::clamp(head_y + y_offset,
                                  head_y_center - kHeadYOffset,
                                  head_y_center + kHeadYOffset);
  while (head_x != target_x || head_y != target_y) {
    if (head_x != target_x) head_x += target_x > head_x ? 1 : -1;
    if (head_y != target_y) head_y += target_y > head_y ? 1 : -1;
    write_head(head_x, head_y);
    vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
  }
}

void center_head(uint32_t step_delay_ms) {
  move_head(head_x_center - head_x, head_y_center - head_y, step_delay_ms);
}

bool adjust_head_center(int x_offset, int y_offset) {
  // 2026-09-19: Persist factory calibration while preserving the V1 mechanical travel envelope.
  head_x_center = std::clamp(head_x_center + x_offset, kHeadXOffset,
                             180 - kHeadXOffset);
  head_y_center = std::clamp(head_y_center + y_offset, kHeadYOffset,
                             180 - kHeadYOffset);
  const bool saved_x = storage::write_text_file(
      "/spiffs/X_CENTER.txt", std::to_string(head_x_center));
  const bool saved_y = storage::write_text_file(
      "/spiffs/Y_CENTER.txt", std::to_string(head_y_center));
  center_head();
  return saved_x && saved_y;
}

void set_idle_actions_enabled(bool enabled) { idle_enabled = enabled; }

bool idle_actions_enabled() { return idle_enabled; }

void play_random_idle_action() {
  if (!idle_enabled) return;
  const uint32_t choice = esp_random() % 100U;
  if (choice < 20) set_face(FaceState::kHappy);
  else if (choice >= 90) set_face(FaceState::kRight);
  else if (choice >= 80) set_face(FaceState::kLeft);
  else {
    set_face(FaceState::kSleeping);
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  vTaskDelay(pdMS_TO_TICKS(300));
  set_face(FaceState::kNeutral);
}

void play_animation(int index, int loop_count) {
  // 2026-09-19: Dispatch all original V1 bitmap arrays through the native SSD1306 framebuffer.
#define PLAY_ANIMATION_CASE(number)                                            \
  case number:                                                                \
    play_frames_native(&frames##number[0][0],                                 \
                       sizeof(frames##number) / sizeof(frames##number[0]),     \
                       loop_count);                                            \
    break
  switch (index) {
    PLAY_ANIMATION_CASE(0); PLAY_ANIMATION_CASE(1);
    PLAY_ANIMATION_CASE(2); PLAY_ANIMATION_CASE(3);
    PLAY_ANIMATION_CASE(4); PLAY_ANIMATION_CASE(5);
    PLAY_ANIMATION_CASE(6); PLAY_ANIMATION_CASE(7);
    PLAY_ANIMATION_CASE(8); PLAY_ANIMATION_CASE(9);
    PLAY_ANIMATION_CASE(10); PLAY_ANIMATION_CASE(11);
    PLAY_ANIMATION_CASE(12); PLAY_ANIMATION_CASE(13);
    PLAY_ANIMATION_CASE(14); PLAY_ANIMATION_CASE(15);
    PLAY_ANIMATION_CASE(16); PLAY_ANIMATION_CASE(17);
    PLAY_ANIMATION_CASE(18); PLAY_ANIMATION_CASE(19);
    PLAY_ANIMATION_CASE(20); PLAY_ANIMATION_CASE(21);
    PLAY_ANIMATION_CASE(22); PLAY_ANIMATION_CASE(23);
    PLAY_ANIMATION_CASE(24); PLAY_ANIMATION_CASE(25);
    PLAY_ANIMATION_CASE(26); PLAY_ANIMATION_CASE(27);
    PLAY_ANIMATION_CASE(28); PLAY_ANIMATION_CASE(29);
    PLAY_ANIMATION_CASE(30); PLAY_ANIMATION_CASE(31);
    PLAY_ANIMATION_CASE(32); PLAY_ANIMATION_CASE(33);
    PLAY_ANIMATION_CASE(34); PLAY_ANIMATION_CASE(35);
    PLAY_ANIMATION_CASE(36); PLAY_ANIMATION_CASE(37);
    PLAY_ANIMATION_CASE(38); PLAY_ANIMATION_CASE(39);
    PLAY_ANIMATION_CASE(40); PLAY_ANIMATION_CASE(41);
    default:
      ESP_LOGW(kLogTag, "Unknown animation index: %d", index);
      return;
  }
#undef PLAY_ANIMATION_CASE
  vTaskDelay(pdMS_TO_TICKS(300));
  set_face(FaceState::kSleeping);
  vTaskDelay(pdMS_TO_TICKS(80));
  set_face(FaceState::kNeutral);
}

void execute_action(const std::string &action) {
  if (action == "eye_blink") {
    set_face(FaceState::kSleeping);
    vTaskDelay(pdMS_TO_TICKS(80));
    set_face(FaceState::kNeutral);
  } else if (action == "eye_happy") set_face(FaceState::kHappy);
  else if (action == "eye_sad") set_face(FaceState::kSad);
  else if (action == "eye_anger") set_face(FaceState::kAngry);
  else if (action == "eye_surprise") set_face(FaceState::kSurprised);
  else if (action == "eye_left") set_face(FaceState::kLeft);
  else if (action == "eye_right") set_face(FaceState::kRight);
  else if (action == "eye_center")
    set_face(FaceState::kNeutral);
  else if (action == "head_left") move_head(-20, 0);
  else if (action == "head_right") move_head(20, 0);
  else if (action == "head_up") move_head(0, -20);
  else if (action == "head_down") move_head(0, 20);
  else if (action == "head_center") center_head();
  else if (action == "head_nod") {
    move_head(0, 20, 3); move_head(0, -20, 3);
  } else if (action == "head_shake") {
    move_head(-15, 0, 3); move_head(30, 0, 3); move_head(-15, 0, 3);
  } else {
    for (size_t index = 0;
         index < sizeof(kAnimationNames) / sizeof(kAnimationNames[0]);
         ++index) {
      if (action == kAnimationNames[index]) {
        play_animation(static_cast<int>(index));
        return;
      }
    }
    ESP_LOGW(kLogTag, "Unknown action: %s", action.c_str());
  }
}

void camera_initialize_disabled() {
  ESP_LOGI(kLogTag, "Camera disabled by hardware profile");
}

void gesture_initialize_disabled() {
  ESP_LOGI(kLogTag, "Gesture sensor disabled by hardware profile");
}

}  // namespace desk_talk::ui
