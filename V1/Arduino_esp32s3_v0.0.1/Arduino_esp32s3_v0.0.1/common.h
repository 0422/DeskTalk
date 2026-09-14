#ifndef Common_h
#define Common_h

// 2026-09-11: Make the selected hardware profile available to every peripheral module.
#include "hardware_config.h"
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <FFat.h>
// 2026-09-11: The ESP-SR 16M partition provides SPIFFS rather than FFat, so keep existing FFat calls compatible with that partition.
#include <SPIFFS.h>
// 2026-09-11: Redirect the project's existing FFat storage calls to SPIFFS without deleting the original FFat-based code.
#define FFat SPIFFS
#include <time.h>
#include "logger.h"
#include "led.h"
#include "oled.h"

// 2026-09-14: Load credentials from the local Git-ignored secrets.h file when available.
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

// First, re-define WEBSOCKETS_MAX_DATA_SIZE to (150 * 1024) in WebSocketsClient.h

// Global variables
#define VERSION "2.0.1"

// 2026-09-14: Credentials previously defined here were moved to secrets.h.
// #define APPID ""
// #define TOKEN ""

// 2026-09-14: Provide non-secret defaults so a fresh checkout can compile after
// copying secrets.example.h to secrets.h and filling in the required values.
#ifndef VOLC_APP_ID
#define VOLC_APP_ID ""
#endif

#ifndef VOLC_ACCESS_TOKEN
#define VOLC_ACCESS_TOKEN ""
#endif

#ifndef VOLC_API_KEY
#define VOLC_API_KEY ""
#endif

#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY ""
#endif

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

#ifndef DEFAULT_WIFI_PASSWORD
#define DEFAULT_WIFI_PASSWORD ""
#endif

// 2026-09-14: Keep the existing speech protocol names mapped to secrets.h values.
#define APPID VOLC_APP_ID
#define TOKEN VOLC_ACCESS_TOKEN

// Constants from Python code
#define PROTOCOL_VERSION 0x01
#define DEFAULT_HEADER_SIZE 0x01

// Message Types
#define CLIENT_FULL_REQUEST 0x01
#define CLIENT_AUDIO_ONLY_REQUEST 0x02
#define SERVER_FULL_RESPONSE 0x09
#define SERVER_ACK 0x0B
#define SERVER_ERROR_RESPONSE 0x0F

// Message Type Specific Flags
#define NO_SEQUENCE 0x00
#define POS_SEQUENCE 0x01
#define NEG_SEQUENCE 0x02
#define NEG_SEQUENCE_1 0x03

// Message Serialization
#define NO_SERIALIZATION 0x00
#define JSON 0x01
#define THRIFT 0x03
#define CUSTOM_TYPE 0x0F

// Message Compression
#define NO_COMPRESSION 0x00
#define GZIP 0x01
#define CUSTOM_COMPRESSION 0x0F

// Global variables
extern unsigned long last_time;
extern bool enable_act;
extern bool start_chat;

// Functions
void setup_FFat();
String get_mac_address();
String generate_uuid();
String get_byte_str(uint8_t* payload, size_t length);
String get_local_ip();

#endif
