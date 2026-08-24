#include <SPI.h>
#include <RF24.h>

// ============================================================
//  ESP32 GROUND STATION  (NRF24L01) — CSV OUTPUT + SERVO CMD
//  For use with the web serial dashboard (GUI.html).
//
//  NRF24 wiring to ESP32 (VSPI):
//    GND->GND  VCC->3.3V(never 5V)  CE->GPIO4  CSN->GPIO5
//    SCK->GPIO18  MOSI->GPIO23  MISO->GPIO19  IRQ->n/c
//  Add a 10uF cap across the NRF24 VCC/GND at the module.
//  Serial monitor / dashboard baud: 115200.
//
//  Telemetry out (one CSV line per received packet):
//    T,accX,accY,accZ,gyroZ,tempC,pressurePa,alt_m,lat,lng
//  Servo command in (from dashboard, over USB serial):
//    a line "S:<angle>\n" where <angle> is 0..180
//  The angle is returned to the rocket as an NRF24 ACK PAYLOAD.
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

struct __attribute__((packed)) Command {
  uint8_t servoAngle;
};
Command cmd = { 90 };   // default centre

unsigned long lastPacket = 0;

const float SEA_LEVEL_PA = 101325.0;   // set to local QNH for accurate altitude

float altitudeFromPressure(float pressurePa) {
  if (isnan(pressurePa) || pressurePa <= 0) return NAN;
  return 44330.0 * (1.0 - pow(pressurePa / SEA_LEVEL_PA, 0.1903));
}

// ---- Parse "S:<angle>" commands arriving from the dashboard over USB ----
String inLine = "";
void handleSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inLine.startsWith("S:")) {
        int a = inLine.substring(2).toInt();
        if (a < 0)   a = 0;
        if (a > 180) a = 180;
        cmd.servoAngle = (uint8_t)a;
        // Preload the ack payload so it goes out on the next received packet.
        radio.writeAckPayload(0, &cmd, sizeof(cmd));
        Serial.print("#servo set to ");
        Serial.println(a);
      }
      inLine = "";
    } else {
      inLine += c;
      if (inLine.length() > 16) inLine = "";  // guard against runaway
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("#CanSat telemetry CSV: T,accX,accY,accZ,gyroZ,tempC,pressurePa,alt_m,lat,lng");

  if (radio.begin()) {
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_250KBPS);
    radio.setChannel(108);
    radio.enableAckPayload();          // allow returning data to the rocket
    radio.enableDynamicPayloads();     // required for ack payloads
    radio.openReadingPipe(0, address);
    radio.startListening();            // receiver (PRX) mode
    // Preload an initial ack payload (centre) so the first ack carries a value.
    radio.writeAckPayload(0, &cmd, sizeof(cmd));
    Serial.println("#NRF24 OK - waiting for telemetry");
  } else {
    Serial.println("#NRF24 FAILED - check wiring / 3.3V / decoupling cap");
  }
}

void loop() {
  handleSerialInput();

  if (radio.available()) {
    radio.read(&tlm, sizeof(tlm));
    lastPacket = millis();

    // Re-arm the ack payload so the NEXT packet also returns the current angle.
    radio.writeAckPayload(0, &cmd, sizeof(cmd));

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
