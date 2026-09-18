#ifndef Secrets_h
#define Secrets_h

// 2026-09-14: Document every local credential expected in the Git-ignored secrets.h file.
#define DEEPSEEK_API_KEY ""
#define VOLC_APP_ID ""
#define VOLC_ACCESS_TOKEN ""
#define VOLC_API_KEY ""
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""
// 2026-09-18: Leave the optional gateway disabled until its address and shared token are configured in secrets.h.
#define GATEWAY_HOST ""
#define GATEWAY_PORT 8765
#define GATEWAY_PATH "/chat"
#define GATEWAY_TOKEN ""
#define GATEWAY_TLS false

#endif
