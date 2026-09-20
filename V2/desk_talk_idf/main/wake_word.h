// 2026-09-19: Define native ESP-SR wake-word ownership and microphone pause/resume controls.
#pragma once

#include "esp_err.h"

namespace desk_talk::wake_word {

esp_err_t initialize();
bool wait_for_detection(unsigned timeout_ms);
void pause();
void resume();
// 2026-09-20: Release ESP-SR between wake detection and a cloud conversation so TLS can reclaim scarce internal RAM.
void shutdown();
bool ready();

}  // namespace desk_talk::wake_word
