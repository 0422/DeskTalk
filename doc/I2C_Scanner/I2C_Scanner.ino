#include <Wire.h>

constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;

void scanI2C() {
  int deviceCount = 0;

  Serial.println("Scanning I2C bus...");

  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("Found device at 0x");
      if (address < 0x10) {
        Serial.print('0');
      }
      Serial.println(address, HEX);
      ++deviceCount;
    } else if (error == 4) {
      Serial.print("Unknown error at 0x");
      if (address < 0x10) {
        Serial.print('0');
      }
      Serial.println(address, HEX);
    }
  }

  if (deviceCount == 0) {
    Serial.println("No I2C devices found.");
  } else {
    Serial.print("Scan complete. Devices found: ");
    Serial.println(deviceCount);
  }

  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  Serial.println();
  Serial.println("ESP32 I2C scanner");
  Serial.print("SDA GPIO: ");
  Serial.println(I2C_SDA_PIN);
  Serial.print("SCL GPIO: ");
  Serial.println(I2C_SCL_PIN);
}

void loop() {
  scanI2C();
  delay(3000);
}
