// 2026-09-19: Provide one native configuration boundary for credentials and cloud endpoint constants.
#pragma once

#if 0
// 2026-09-19: Retain the optional include for reference; Ninja did not track a secrets file that appeared after the first compile.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

// 2026-09-19: Require the private configuration header so credential changes are tracked and cannot silently fall back to empty values.
#include "secrets.h"

#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY ""
#endif
#ifndef VOLC_APP_ID
#define VOLC_APP_ID ""
#endif
#ifndef VOLC_ACCESS_TOKEN
#define VOLC_ACCESS_TOKEN ""
#endif
#ifndef VOLC_API_KEY
#define VOLC_API_KEY ""
#endif
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef DEFAULT_WIFI_PASSWORD
#define DEFAULT_WIFI_PASSWORD ""
#endif

namespace desk_talk::config {

inline constexpr char kVersion[] = "3.0.0-idf";
inline constexpr char kDeepSeekApiKey[] = DEEPSEEK_API_KEY;
inline constexpr char kVolcAppId[] = VOLC_APP_ID;
inline constexpr char kVolcAccessToken[] = VOLC_ACCESS_TOKEN;
inline constexpr char kVolcApiKey[] = VOLC_API_KEY;
inline constexpr char kDefaultWifiSsid[] = DEFAULT_WIFI_SSID;
inline constexpr char kDefaultWifiPassword[] = DEFAULT_WIFI_PASSWORD;

}  // namespace desk_talk::config
