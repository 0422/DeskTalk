// 2026-09-19: Define the native stdin JSON command and factory-control task migrated from the Arduino serial loop.
#pragma once

#include "esp_err.h"

namespace desk_talk::console {

esp_err_t start();

}  // namespace desk_talk::console
