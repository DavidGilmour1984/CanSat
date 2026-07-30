#include <SPI.h>
#include <RF24.h>

// ============================================================
//  ESP32 GROUND STATION RECEIVER  (NRF24L01) — CSV OUTPUT
//  For use with the web serial dashboard (telemetry_dashboard.html).
//
//  NRF24 wiring to ESP32 (VSPI):
//    GND->GND  VCC->3.3V(never 5V)  CE->GPIO4  CSN->GPIO5
//    SCK->GPIO18  MOSI->GPIO23  MISO->GPIO19  IRQ->n/c
//  Add a 10uF cap across the NRF24 VCC/GND at the module.
//  Serial monitor / dashboard baud: 115200.
//
//  Output line format (one CSV line per received packet):
//    T,accX,accY,accZ,gyroZ,tempC,pressurePa,alt_m,lat,lng
//  Lines beginning with '#' are status/comment lines.
// ============================================================

#define NRF_CE  4
#define NRF_CSN 5
RF24 radio(NRF_CE, NRF_CSN);
const byte address[6] = "CSAT1";

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

// Local sea-level pressure (Pa) for altitude — set to local QNH.
const float SEA_LEVEL_PA = 101325.0;

float altitudeFromPressure(float pressurePa) {
  if (isnan(pressurePa) || pressurePa <= 0) return NAN;
  return 44330.0 * (1.0 - pow(pressurePa / SEA_LEVEL_PA, 0.1903));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("#CanSat telemetry CSV: T,accX,accY,accZ,gyroZ,tempC,pressurePa,alt_m,lat,lng");

  if (radio.begin()) {
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_250KBPS);
    radio.setChannel(108);
    radio.openReadingPipe(0, address);
    radio.startListening();
    Serial.println("#NRF24 OK - waiting for telemetry");
  } else {
    Serial.println("#NRF24 FAILED - check wiring / 3.3V / decoupling cap");
  }
}

void loop() {
  if (radio.available()) {
    radio.read(&tlm, sizeof(tlm));
    lastPacket = millis();

    float alt = altitudeFromPressure(tlm.pressure);

    Serial.print(millis());        Serial.print(',');
    Serial.print(tlm.accX, 3);     Serial.print(',');
    Serial.print(tlm.accY, 3);     Serial.print(',');
    Serial.print(tlm.accZ, 3);     Serial.print(',');
    Serial.print(tlm.gyroZ, 2);    Serial.print(',');
    Serial.print(tlm.temperature, 2); Serial.print(',');
    Serial.print(tlm.pressure, 1); Serial.print(',');
    Serial.print(alt, 2);          Serial.print(',');
    Serial.print(tlm.lat, 6);      Serial.print(',');
    Serial.println(tlm.lng, 6);
  }

  if (millis() - lastPacket > 3000 && lastPacket != 0) {
    Serial.println("#link lost");
    lastPacket = millis();
  }
}
