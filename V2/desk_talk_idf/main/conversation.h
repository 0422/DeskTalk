// 2026-09-19: Define the native conversation orchestrator that owns ASR, streaming LLM, persistent TTS, and wake-word transitions.
#pragma once

#include "esp_err.h"

namespace desk_talk::conversation {

esp_err_t start();

}  // namespace desk_talk::conversation
