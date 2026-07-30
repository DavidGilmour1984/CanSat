#include <SPI.h>
#include <RF24.h>

// ============================================================
//  ESP32 GROUND STATION RECEIVER  (NRF24L01)
//  NRF24 wiring to ESP32 (VSPI):
//    GND  -> GND
//    VCC  -> 3.3V   (never 5V)
//    CE   -> GPIO 4
//    CSN  -> GPIO 5
//    SCK  -> GPIO 18  (VSPI SCK)
//    MOSI -> GPIO 23  (VSPI MOSI)
//    MISO -> GPIO 19  (VSPI MISO)
//    IRQ  -> not connected
//  Add a 10uF cap across the NRF24 VCC/GND at the module.
//  Serial monitor: 115200 baud.
// ============================================================

#define NRF_CE  4
#define NRF_CSN 5
RF24 radio(NRF_CE, NRF_CSN);
const byte address[6] = "CSAT1";   // must match the rocket transmitter

// Must match the transmitter's struct EXACTLY (order + types)
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

unsigned long lastPacket = 0;

// Local sea-level pressure (Pa) for altitude estimate — set to local QNH.
const float SEA_LEVEL_PA = 101325.0;

float altitudeFromPressure(float pressurePa) {
  if (isnan(pressurePa) || pressurePa <= 0) return NAN;
  return 44330.0 * (1.0 - pow(pressurePa / SEA_LEVEL_PA, 0.1903));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("===== ESP32 GROUND STATION RECEIVER =====");

  Serial.print("NRF24: ");
  if (radio.begin()) {
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_250KBPS);   // MUST match transmitter
    radio.setChannel(108);             // MUST match transmitter
    radio.openReadingPipe(0, address);
    radio.startListening();
    Serial.println("OK");
  } else {
    Serial.println("FAILED (check wiring / 3.3V power / decoupling cap)");
  }

  Serial.println("Waiting for telemetry...");
}

void loop() {
  if (radio.available()) {
    radio.read(&tlm, sizeof(tlm));
    lastPacket = millis();

    float alt = altitudeFromPressure(tlm.pressure);

    Serial.println("-------- PACKET --------");
    Serial.print("Accel  X:"); Serial.print(tlm.accX, 3);
    Serial.print("  Y:");      Serial.print(tlm.accY, 3);
    Serial.print("  Z:");      Serial.println(tlm.accZ, 3);

    Serial.print("Gyro Z:");   Serial.print(tlm.gyroZ, 2);
    Serial.println(" deg/s");

    Serial.print("Temp:  ");   Serial.print(tlm.temperature, 1); Serial.println(" C");
    Serial.print("Press: ");   Serial.print(tlm.pressure, 0);    Serial.println(" Pa");
    Serial.print("Alt:   ");   Serial.print(alt, 1);             Serial.println(" m");

    if (tlm.lat != 0.0f || tlm.lng != 0.0f) {
      Serial.print("GPS:   "); Serial.print(tlm.lat, 6);
      Serial.print(", ");      Serial.println(tlm.lng, 6);
    } else {
      Serial.println("GPS:   no fix");
    }
    Serial.println();
  }

  if (millis() - lastPacket > 3000 && lastPacket != 0) {
    Serial.println("... no telemetry for 3s (link lost?) ...");
    lastPacket = millis();
  }
}
