// 2026-09-19: Centralize the verified N16R8 bench hardware profile for the native ESP-IDF migration.
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"

namespace desk_talk::hardware {

// 2026-09-19: Preserve the V1 feature profile while only the audio subsystem is active in this migration stage.
inline constexpr bool kEnableOled = true;
inline constexpr bool kEnableHead = true;
inline constexpr bool kEnableLed = true;
inline constexpr bool kEnableCamera = false;
inline constexpr bool kEnableGesture = false;
inline constexpr bool kEnableAudio = true;
inline constexpr bool kEnableWakeWord = true;

// 2026-09-19: Preserve the INMP441 wiring validated by the V1 firmware.
inline constexpr gpio_num_t kMicrophoneWordSelect = GPIO_NUM_5;
inline constexpr gpio_num_t kMicrophoneBitClock = GPIO_NUM_6;
inline constexpr gpio_num_t kMicrophoneData = GPIO_NUM_4;

// 2026-09-19: Preserve the MAX98357 wiring validated by the V1 firmware.
inline constexpr gpio_num_t kSpeakerWordSelect = GPIO_NUM_18;
inline constexpr gpio_num_t kSpeakerBitClock = GPIO_NUM_17;
inline constexpr gpio_num_t kSpeakerData = GPIO_NUM_7;
inline constexpr gpio_num_t kSpeakerShutdown = GPIO_NUM_3;
inline constexpr gpio_num_t kSpeakerGain = GPIO_NUM_20;

// 2026-09-19: Migrate the V1 OLED, status LED, and pan/tilt wiring into the native driver boundary.
inline constexpr gpio_num_t kOledSda = GPIO_NUM_8;
inline constexpr gpio_num_t kOledScl = GPIO_NUM_9;
inline constexpr uint8_t kOledAddress = 0x3C;
inline constexpr gpio_num_t kStatusLed = GPIO_NUM_48;
inline constexpr gpio_num_t kHeadX = GPIO_NUM_12;
inline constexpr gpio_num_t kHeadY = GPIO_NUM_13;

// 2026-09-19: Keep the established 16 kHz cloud/ESP-SR PCM contract and explicitly fit each DMA buffer below 4092 bytes.
inline constexpr uint32_t kAudioSampleRate = 16000;
inline constexpr uint32_t kAudioDmaDescriptorCount = 8;
inline constexpr uint32_t kAudioDmaFrameCount = 1023;
inline constexpr size_t kAudioBlockSamples = 1024;
inline constexpr uint32_t kSoundThreshold = 180;

// 2026-09-19: Keep power-on silent; enable this only for a deliberate one-second speaker wiring test.
inline constexpr bool kRunSpeakerSelfTestOnBoot = false;

static_assert(!kEnableWakeWord || kEnableAudio,
              "Wake word support requires the audio subsystem");

}  // namespace desk_talk::hardware
