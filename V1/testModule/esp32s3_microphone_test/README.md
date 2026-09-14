# ESP32-S3 INMP441 Microphone Test

This isolated Arduino sketch checks whether the INMP441 produces changing I2S
samples before the microphone is used by WakeNet or cloud ASR.

## Wiring

| INMP441 | ESP32-S3 |
| --- | --- |
| VDD | 3.3V |
| GND | GND |
| SCK/BCLK | GPIO6 |
| WS/LRCL | GPIO5 |
| SD | GPIO4 |
| L/R | GND |

Do not power the INMP441 from 5V. The sketch reads the left I2S slot, so L/R
must be connected to GND rather than left floating.

## Test

1. Close the Desk Emoji PC client and any other program using the COM port.
2. Open esp32s3_microphone_test.ino in Arduino IDE and upload it.
3. Open Serial Monitor at 115200 baud.
4. Compare the three output columns while quiet, speaking, and clapping.

The first standalone value is 1 when I2S initialization succeeds and -1 when
it fails. Each following row contains mean, peak, and status:

| Status | Meaning |
| --- | --- |
| 0 | Samples are present; compare quiet and speech levels |
| 1 | Samples are all zero; check SD, GND, and L/R |
| 2 | Input is clipping; check wiring and supply |
| 3 | I2S returned no data |

A working microphone produces nonzero mean and peak values that rise clearly
during speech or a clap. After testing, upload the main esp32s3_v2.0.1
firmware again.
