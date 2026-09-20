// 2026-09-19: Replace Arduino display, NeoPixel, Servo, emoji, animation, and command helpers with native IDF device controls.
#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace desk_talk::ui {

enum class FaceState {
  kNeutral,
  kListening,
  kThinking,
  kHappy,
  kSad,
  kAngry,
  kSurprised,
  kLeft,
  kRight,
  kSleeping,
  kError,
};

esp_err_t initialize();
void set_face(FaceState state);
void set_status_color(uint32_t rgb, uint8_t brightness = 10);
void blink_status(uint32_t rgb, uint8_t times = 1);
void center_head(uint32_t step_delay_ms = 3);
void move_head(int x_offset, int y_offset, uint32_t step_delay_ms = 3);
bool adjust_head_center(int x_offset, int y_offset);
void set_idle_actions_enabled(bool enabled);
bool idle_actions_enabled();
void play_random_idle_action();
void play_animation(int index, int loop_count = 3);
void execute_action(const std::string &action);
void camera_initialize_disabled();
void gesture_initialize_disabled();

}  // namespace desk_talk::ui
