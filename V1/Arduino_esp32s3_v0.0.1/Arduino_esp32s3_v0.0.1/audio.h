#ifndef Audio_h
#define Audio_h

#include <Arduino.h>
// 2026-09-11: Disable the legacy I2S header because ESP-SR links the new I2S driver and ESP-IDF aborts when both drivers are present.
#if 0
#include <driver/i2s.h>
#endif
// 2026-09-11: Expose esp_err_t for the shared microphone reader used by the ESP-SR feed task.
#include <esp_err.h>
#include <FFat.h>
#include "common.h"

// microphone and speaker
// 2026-09-11: Remap the INMP441 input to the currently connected GPIO4/5/6 pins.
// #define INMP441_WS GPIO_NUM_46
// #define INMP441_SCK GPIO_NUM_21
// #define INMP441_SD GPIO_NUM_14
#define INMP441_WS GPIO_NUM_5
#define INMP441_SCK GPIO_NUM_6
#define INMP441_SD GPIO_NUM_4

#define MAX98357_LRC GPIO_NUM_18
#define MAX98357_BCLK GPIO_NUM_17
// #define MAX98357_DIN GPIO_NUM_19
// 2026-09-11: Match the existing MAX98357 DIN wiring on GPIO7 and release GPIO19 for the GC2145 D0 signal.
#define MAX98357_DIN GPIO_NUM_7
#define MAX98357_SD GPIO_NUM_3
#define MAX98357_GAIN GPIO_NUM_20

// parameters
#define SAMPLE_RATE 16000
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN 1024
#define SOUND_THRESHOLD 180

// i2s
// 2026-09-11: Keep the former legacy-driver declarations for reference while building only the new I2S implementation.
#if 0
extern i2s_config_t i2sIn_config;
extern i2s_config_t i2sOut_config;
extern const i2s_pin_config_t i2sIn_pin_config;
extern const i2s_pin_config_t i2sOut_pin_config;
#endif

void setup_audio();
bool microphone_is_ready();
bool speaker_is_ready();
// 2026-09-11: Share converted 16-bit mono PCM from one native 32-bit microphone channel with cloud ASR and WakeNet.
esp_err_t read_audio_bytes(void *data, size_t length, size_t *bytes_read, uint32_t timeout_ms);
void record(int16_t *data, size_t length = DMA_BUF_LEN);
void enhanceVoice(int16_t *data, size_t length = DMA_BUF_LEN);
void play(const int16_t *data, size_t length = DMA_BUF_LEN, float volume_ratio = 1.0);
// Wait until samples already copied into the TX DMA buffers have reached the
// speaker before muting the amplifier.
void wait_for_playback_complete();
// Play a short 1 kHz tone through the MAX98357 without involving WiFi or TTS.
void test_speaker();
void stop_play();
size_t calculate_mean(const int16_t *data, size_t length);

#endif
