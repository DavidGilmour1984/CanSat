#include <Wire.h>
#include <MPU6050_light.h>
#include <BMP180I2C.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <RF24.h>

// ============================================================
//  ROCKET TRANSMITTER — CanSat Flight Computer (STM32F405RGT6)
//  Board part number: Generic F405RGTx
//  USB serial = Serial   |  GY-87 on I2C1 (PB6=SCL, PB7=SDA)
//  GPS L80 via SoftwareSerial (PA1=RX, PA0=TX)
//  NRF24 on SPI1: SCK=PA5, MISO=PA6, MOSI=PA7, CE=PB4, CSN=PB3
//  LED PC13 (active-low)
// ============================================================

// ---- I2C addresses ----
#define BMP180_I2C_ADDR 0x77
#define MPU6050_ADDR    0x68
#define HMC5883L_ADDR   0x1E
#define QMC5883L_ADDR   0x0D

#define LED_PIN PC13

// ---- NRF24 ----
#define NRF_CE  PB4
#define NRF_CSN PB3
RF24 radio(NRF_CE, NRF_CSN);
const byte address[6] = "CSAT1";   // must match the ground station

MPU6050 mpu(Wire);
BMP180I2C bmp(BMP180_I2C_ADDR);
TinyGPSPlus gps;
SoftwareSerial gpsSerial(PA1, PA0);   // (RX, TX)

byte magType = 0;   // 0 none, 1 HMC5883L, 2 QMC5883L
unsigned long lastSend = 0;
unsigned long sendCount = 0;
unsigned long successCount = 0;

// Telemetry packet — keep <= 32 bytes total for a single NRF24 payload.
// 8 floats = 32 bytes exactly.
struct __attribute__((packed)) Telemetry {
  float accX;
  float accY;
  float accZ;
  float gyroZ;
  float temperature;
  float pressure;
  float lat;
  float lng;
};
Telemetry tlm;

bool i2cPresent(byte addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

void enableMpuBypass() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x37); Wire.write(0x02);   // INT_PIN_CFG: I2C_BYPASS_EN
  Wire.endTransmission();
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B); Wire.write(0x00);   // PWR_MGMT_1: wake
  Wire.endTransmission();
}

void setupMagnetometer() {
  if (i2cPresent(HMC5883L_ADDR)) {
    magType = 1;
    Wire.beginTransmission(HMC5883L_ADDR); Wire.write(0x00); Wire.write(0x70); Wire.endTransmission();
    Wire.beginTransmission(HMC5883L_ADDR); Wire.write(0x01); Wire.write(0x20); Wire.endTransmission();
    Wire.beginTransmission(HMC5883L_ADDR); Wire.write(0x02); Wire.write(0x00); Wire.endTransmission();
    Serial.println("Magnetometer: HMC5883L");
  } else if (i2cPresent(QMC5883L_ADDR)) {
    magType = 2;
    Wire.beginTransmission(QMC5883L_ADDR); Wire.write(0x0B); Wire.write(0x01); Wire.endTransmission();
    Wire.beginTransmission(QMC5883L_ADDR); Wire.write(0x09); Wire.write(0x1D); Wire.endTransmission();
    Serial.println("Magnetometer: QMC5883L");
  } else {
    magType = 0;
    Serial.println("Magnetometer: NOT FOUND");
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(9600);
  delay(2000);
  Serial.println();
  Serial.println("===== ROCKET TRANSMITTER =====");

  Wire.begin();

  // ---- MPU6050 ----
  byte status = mpu.begin();
  Serial.print("MPU6050: ");
  Serial.println(status == 0 ? "OK" : "FAILED");
  if (status == 0) mpu.calcOffsets();

  enableMpuBypass();
  delay(10);

  // ---- BMP180 ----
  Serial.print("BMP180: ");
  if (bmp.begin()) {
    bmp.resetToDefaults();
    bmp.setSamplingMode(BMP180MI::MODE_UHR);
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }

  // ---- Magnetometer ----
  setupMagnetometer();

  // ---- GPS ----
  gpsSerial.begin(9600);
  Serial.println("GPS started");

  // ---- NRF24 ----
  // SPI1 default pins on this core are PA5/PA6/PA7, matching the board.
  Serial.print("NRF24: ");
  if (radio.begin()) {
    radio.setPALevel(RF24_PA_HIGH);      // good range; use RF24_PA_MAX if supply is solid
    radio.setDataRate(RF24_250KBPS);     // most robust / longest range
    radio.setChannel(108);               // 2.508 GHz, above most WiFi
    radio.openWritingPipe(address);
    radio.stopListening();               // transmitter mode
    Serial.println("OK");
  } else {
    Serial.println("FAILED (check wiring / power)");
  }

  Serial.println("Init complete");
}

void loop() {
  // Keep feeding the GPS parser continuously
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  mpu.update();

  if (millis() - lastSend >= 500) {   // 2 Hz telemetry
    lastSend = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));

    // ---- Fill packet ----
    tlm.accX  = mpu.getAccX();
    tlm.accY  = mpu.getAccY();
    tlm.accZ  = mpu.getAccZ();
    tlm.gyroZ = mpu.getGyroZ();

    float t = NAN, p = NAN;
    if (bmp.measureTemperature()) { while (!bmp.hasValue()) {} t = bmp.getTemperature(); }
    if (bmp.measurePressure())    { while (!bmp.hasValue()) {} p = bmp.getPressure(); }
    tlm.temperature = t;
    tlm.pressure    = p;

    if (gps.location.isValid()) {
      tlm.lat = gps.location.lat();
      tlm.lng = gps.location.lng();
    } else {
      tlm.lat = 0.0f;
      tlm.lng = 0.0f;
    }

    // ---- Transmit ----
    bool ok = radio.write(&tlm, sizeof(tlm));
    sendCount++;
    if (ok) successCount++;

    // ---- Debug to USB ----
    if (ok) {
      Serial.print("[TX OK]   #");
    } else {
      Serial.print("[TX FAIL] #");
    }
    Serial.print(sendCount);
    Serial.print("  (");
    Serial.print(successCount);
    Serial.print("/");
    Serial.print(sendCount);
    Serial.print(" ok)  ");
    Serial.print("Acc:");  Serial.print(tlm.accX,2); Serial.print(",");
    Serial.print(tlm.accY,2); Serial.print(","); Serial.print(tlm.accZ,2);
    Serial.print(" T:");   Serial.print(tlm.temperature,1);
    Serial.print(" P:");   Serial.print(tlm.pressure,0);
    Serial.print(" GPS:"); Serial.print(tlm.lat,5); Serial.print(","); Serial.print(tlm.lng,5);
    Serial.println();
  }
}
