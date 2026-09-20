#ifndef Secrets_h
#define Secrets_h

// 2026-09-14: Document every local credential expected in the Git-ignored secrets.h file.
#define DEEPSEEK_API_KEY ""
#define VOLC_APP_ID ""
#define VOLC_ACCESS_TOKEN ""
#define VOLC_API_KEY ""
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""
// 2026-09-18: The gateway experiment is disabled for board-direct firmware;
// preserve these historical names without exposing them as active settings.
#if 0
#define GATEWAY_HOST ""
#define GATEWAY_PORT 8765
#define GATEWAY_PATH "/chat"
#define GATEWAY_TOKEN ""
#define GATEWAY_TLS false
#endif

#endif
