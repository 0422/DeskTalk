// 2026-09-11: Provide an isolated INMP441 hardware test for the Desk Emoji ESP32-S3 wiring.
#include <Arduino.h>
#include <ESP_I2S.h>

// 2026-09-11: Match the microphone wiring used by the main esp32s3_v2.0.1 firmware.
static constexpr int MIC_BCLK_PIN = 6;
static constexpr int MIC_WS_PIN = 5;
static constexpr int MIC_DATA_PIN = 4;
static constexpr uint32_t MIC_SAMPLE_RATE = 16000;
static constexpr size_t SAMPLE_COUNT = 256;

// 2026-09-11: INMP441 drives the left slot when its L/R pin is connected to GND.
static constexpr i2s_std_slot_mask_t MIC_SLOT_MASK = I2S_STD_SLOT_LEFT;

// 2026-09-11: Use 32-bit I2S frames because INMP441 provides 24-bit samples in a 32-bit slot.
I2SClass microphone;
int32_t samples[SAMPLE_COUNT];

void setup() {
  // 2026-09-11: Delay briefly so the USB-to-serial connection can settle before diagnostics begin.
  Serial.begin(115200);
  delay(1000);
  Serial.println();

  // 2026-09-11: Configure receive-only standard I2S with the microphone's native frame width.
  microphone.setPins(MIC_BCLK_PIN, MIC_WS_PIN, -1, MIC_DATA_PIN);
  if (!microphone.begin(I2S_MODE_STD, MIC_SAMPLE_RATE,
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO,
                        MIC_SLOT_MASK)) {
    Serial.println(-1);
    while (true) {
      delay(1000);
    }
  }
  Serial.println(1);
}

void loop() {
  // 2026-09-11: Read one short frame so the displayed level remains responsive to speech and claps.
  size_t bytes_read = microphone.readBytes(
      reinterpret_cast<char *>(samples), sizeof(samples));
  size_t sample_count = bytes_read / sizeof(samples[0]);

  if (sample_count == 0) {
    // 2026-09-11: Emit status 3 when the I2S driver returns no sample data.
    Serial.print(0);
    Serial.write('\t');
    Serial.print(0);
    Serial.write('\t');
    Serial.println(3);
    delay(200);
    return;
  }

  // 2026-09-11: Convert left-aligned 24-bit samples to magnitudes and calculate level indicators.
  uint64_t magnitude_sum = 0;
  uint32_t peak = 0;
  for (size_t i = 0; i < sample_count; i++) {
    int32_t sample_24bit = samples[i] >> 8;
    uint32_t magnitude = sample_24bit < 0
                             ? static_cast<uint32_t>(-sample_24bit)
                             : static_cast<uint32_t>(sample_24bit);
    magnitude_sum += magnitude;
    if (magnitude > peak) {
      peak = magnitude;
    }
  }

  uint32_t mean = static_cast<uint32_t>(magnitude_sum / sample_count);

  // 2026-09-11: Use status 0 for signal, 1 for all-zero input, and 2 for clipping.
  int status = 0;
  if (mean == 0 && peak == 0) {
    status = 1;
  } else if (peak >= 0x7F0000UL) {
    status = 2;
  }

  Serial.print(mean);
  Serial.write('\t');
  Serial.print(peak);
  Serial.write('\t');
  Serial.println(status);
  delay(100);
}
