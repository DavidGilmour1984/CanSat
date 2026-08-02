#include <Wire.h>
#include <MPU6050_light.h>
#include <BMP180I2C.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <RF24.h>

// ============================================================
//  CANSAT COMPUTER — CanSat Flight Computer (STM32F405RGT6)
//  Board part number: Generic F405RGTx
//  USB serial = Serial   |  GY-87 on I2C1 (PB6=SCL, PB7=SDA)
//  GPS L80 via SoftwareSerial (PA1=RX, PA0=TX)
//  NRF24 on SPI1: SCK=PA5, MISO=PA6, MOSI=PA7, CE=PB4, CSN=PB3
//  SERVO on PB8 (servo PWM header) driven by HardwareTimer (TIM4_CH3)
//  LED PC13 (active-low)
//
//  Bidirectional link: the rocket transmits telemetry; the ground
//  station returns a servo angle (0-180) as an NRF24 ACK PAYLOAD,
//  which the rocket applies to the servo on PB8.
// ============================================================

// ---- I2C addresses ----
#define BMP180_I2C_ADDR 0x77
#define MPU6050_ADDR    0x68
#define HMC5883L_ADDR   0x1E
#define QMC5883L_ADDR   0x0D

#define LED_PIN   PC13
#define SERVO_PIN PB8

// Servo driven directly by HardwareTimer (Servo library mis-scales on this
// core). TIM4 is on APB1; its timer clock on the F405 is 84 MHz, so a
// prescaler of 84 gives 1 tick = 1 microsecond.
#define SERVO_PRESCALER 84
#define SERVO_FRAME_US  20000     // 20 ms => 50 Hz
// SG90 usable pulse range. Narrow these if your servo grinds at the ends.
#define SERVO_MIN_US 1000
#define SERVO_MAX_US 2000

// ---- NRF24 ----
#define NRF_CE  PB4
#define NRF_CSN PB3
RF24 radio(NRF_CE, NRF_CSN);
const byte address[6] = "CSAT1";   // must match the ground station

MPU6050 mpu(Wire);
BMP180I2C bmp(BMP180_I2C_ADDR);
TinyGPSPlus gps;
SoftwareSerial gpsSerial(PA1, PA0);   // (RX, TX)

// Servo via HardwareTimer
HardwareTimer *servoTimer;
uint32_t servoChannel;

byte magType = 0;   // 0 none, 1 HMC5883L, 2 QMC5883L
unsigned long lastSend = 0;
unsigned long sendCount = 0;
unsigned long successCount = 0;

byte servoAngle = 90;   // current commanded servo angle (default centre)

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

// Ack payload coming back from the ground station: a single servo angle byte.
struct __attribute__((packed)) Command {
  uint8_t servoAngle;
};
Command cmd;

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

void servoWriteMicros(uint32_t us) {
  servoTimer->setCaptureCompare(servoChannel, us, TICK_COMPARE_FORMAT);
}

void servoWriteAngle(byte angle) {
  if (angle > 180) angle = 180;
  uint32_t us = map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  servoWriteMicros(us);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(9600);
  delay(2000);
  Serial.println();
  Serial.println("===== CANSAT COMPUTER =====");

  // ---- Servo (HardwareTimer on PB8) ----
  {
    PinName pn = digitalPinToPinName(SERVO_PIN);
    TIM_TypeDef *Instance = (TIM_TypeDef *)pinmap_peripheral(pn, PinMap_PWM);
    servoChannel = STM_PIN_CHANNEL(pinmap_function(pn, PinMap_PWM));
    servoTimer = new HardwareTimer(Instance);
    servoTimer->setMode(servoChannel, TIMER_OUTPUT_COMPARE_PWM1, SERVO_PIN);
    servoTimer->setPrescaleFactor(SERVO_PRESCALER);      // 1 tick = 1 us
    servoTimer->setOverflow(SERVO_FRAME_US, TICK_FORMAT); // 50 Hz frame
    servoWriteAngle(servoAngle);                          // centre on startup
    servoTimer->resume();
  }

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
  Serial.print("NRF24: ");
  if (radio.begin()) {
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_250KBPS);
    radio.setChannel(108);
    radio.enableAckPayload();          // allow the receiver to return data
    radio.enableDynamicPayloads();     // required for ack payloads
    radio.openWritingPipe(address);
    radio.stopListening();             // transmitter (PTX) mode
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

    // ---- Transmit, then check for a returned ack payload ----
    bool ok = radio.write(&tlm, sizeof(tlm));
    sendCount++;
    if (ok) {
      successCount++;
      // If the ground station attached a servo command to its ack, read it.
      if (radio.isAckPayloadAvailable()) {
        radio.read(&cmd, sizeof(cmd));
        byte a = cmd.servoAngle;
        if (a <= 180 && a != servoAngle) {   // only act on a real change
          servoAngle = a;
          servoWriteAngle(servoAngle);
        }
      }
    }

    // ---- Debug to USB ----
    Serial.print(ok ? "[TX OK]   #" : "[TX FAIL] #");
    Serial.print(sendCount);
    Serial.print("  (");
    Serial.print(successCount);
    Serial.print("/");
    Serial.print(sendCount);
    Serial.print(" ok)  Servo:");
    Serial.print(servoAngle);
    Serial.print("  Acc:");  Serial.print(tlm.accX,2); Serial.print(",");
    Serial.print(tlm.accY,2); Serial.print(","); Serial.print(tlm.accZ,2);
    Serial.print(" T:");   Serial.print(tlm.temperature,1);
    Serial.print(" P:");   Serial.print(tlm.pressure,0);
    Serial.print(" GPS:"); Serial.print(tlm.lat,5); Serial.print(","); Serial.print(tlm.lng,5);
    Serial.println();
  }
}
