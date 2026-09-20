// 2026-09-19: Replace Arduino FFat/Preferences access with native SPIFFS and NVS helpers.
#pragma once

#include <string>

#include "esp_err.h"

namespace desk_talk::storage {

esp_err_t initialize();
bool read_text_file(const char *path, std::string &content);
bool write_text_file(const char *path, const std::string &content);

bool load_wifi_credentials(std::string &ssid, std::string &password);
bool save_wifi_credentials(const std::string &ssid,
                           const std::string &password);
bool clear_wifi_credentials();

}  // namespace desk_talk::storage
