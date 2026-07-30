# CanSat Telemetry System — Documentation

A wireless telemetry link between a CanSat / model-rocket flight computer and a
ground station. The rocket reads its onboard sensors and transmits them over a
2.4 GHz NRF24L01 radio; the ground station receives the packets, prints them,
and computes altitude from barometric pressure.

---

## 1. System overview

The system has two nodes:

**Rocket node (transmitter)** — a CanSat Flight Computer built around an
STM32F405RGT6. It reads its inertial, barometric, magnetic and GPS sensors and
broadcasts a telemetry packet twice per second.

**Ground node (receiver)** — an ESP32 development board with its own NRF24L01
module. It listens for packets, prints each one to the serial monitor, and
derives altitude from the transmitted pressure.

```
   [ROCKET  - STM32F405 CanSat]                 [GROUND - ESP32]
   MPU6050  (accel / gyro) ---+
   BMP180   (pressure/temp) --+--> STM32 --> NRF24  ~~2.4GHz~~>  NRF24 --> ESP32 --> USB serial
   HMC/QMC  (magnetometer) ---+                                                      (prints packets
   L80      (GPS) ------------+                                                       + altitude)
```

The two radios are configured identically (address, channel, data rate, and
packet structure) so they interoperate directly.

---

## 2. Rocket hardware — CanSat Flight Computer

| Item | Detail |
|------|--------|
| MCU | STM32F405RGT6 (Cortex-M4F, 168 MHz, LQFP64) |
| Arduino core | STM32duino (STMicroelectronics official) |
| Board part number | Generic F405RGTx |
| Onboard LED | PC13 (active-LOW: LOW = on) |
| USB serial object | `Serial` (CDC mode) |

### Onboard sensors (GY-87 module, on I2C1)

I2C1 bus: **SCL = PB6, SDA = PB7**

| Sensor | Function | I2C address | Notes |
|--------|----------|-------------|-------|
| MPU6050 | Accelerometer + gyroscope | 0x68 | Primary IMU |
| BMP180 | Barometer + temperature | 0x77 | Read temperature before pressure |
| HMC5883L *or* QMC5883L | Magnetometer | 0x1E or 0x0D | Behind the MPU6050; needs I2C bypass. Recent GY-87 boards usually carry the QMC5883L |

### GPS

| Item | Detail |
|------|--------|
| Module | L80 |
| Connection | SoftwareSerial, 9600 baud |
| Pins | PA1 = RX (from GPS TX), PA0 = TX (to GPS RX) |

### Radio

| Item | Detail |
|------|--------|
| Module | NRF24L01 (GT24) 2.4 GHz |
| Bus | SPI1 |
| SCK | PA5 |
| MISO | PA6 |
| MOSI | PA7 |
| CE | PB4 |
| CSN | PB3 |
| IRQ | Not connected (not required by the RF24 library) |

These radio pins were confirmed from the board's KiCad schematic rather than
assumed.

---

## 3. Ground hardware — ESP32 + NRF24L01

The NRF24L01 connects to the ESP32's default VSPI bus.

| NRF24 pin | ESP32 pin | Notes |
|-----------|-----------|-------|
| GND | GND | |
| VCC | 3.3V | **Never 5V** — the module is 3.3 V only |
| CE | GPIO 4 | Any free GPIO |
| CSN | GPIO 5 | Any free GPIO |
| SCK | GPIO 18 | VSPI SCK (fixed) |
| MOSI | GPIO 23 | VSPI MOSI (fixed) |
| MISO | GPIO 19 | VSPI MISO (fixed) |
| IRQ | — | Not connected |

CE and CSN can be reassigned to other free GPIOs (update the `#define`s in the
sketch to match). SCK/MOSI/MISO must stay on the VSPI pins so the RF24 library's
default SPI works without extra configuration.

---

## 4. Power and decoupling — the single most important point

The bare NRF24L01 modules draw sharp current spikes on transmit that a marginal
3.3 V supply cannot deliver. The result is a radio that initialises fine
(`radio.begin()` returns OK) and sends the first several packets, then stops
getting acknowledgements — exactly the "some early OKs then all FAIL" pattern.

**Fit a 10 µF capacitor (47 µF or 100 µF is also fine) directly across the VCC
and GND pins of each NRF24 module, at the module itself.** Do this on both the
rocket and the ground radios. This resolves the large majority of NRF24 link
failures. The PA/LNA (external-antenna) variants draw even larger spikes and
make the capacitor mandatory.

For the rocket, follow the flight computer's own power guidance: the 12 V rail
runs the logic and the 12 VA rail runs the nichrome channels; using two separate
batteries avoids a brownout when the nichrome fires. The board runs on 8–12 V
(9–12 V recommended; 9 V battery or 3S LiPo).

---

## 5. Radio link parameters

Both sketches must agree on all four of these. If you change one, change it on
both nodes.

| Parameter | Value | Set by |
|-----------|-------|--------|
| Pipe address | `"CSAT1"` | `openWritingPipe` / `openReadingPipe` |
| Channel | 108 (2.508 GHz, above most Wi-Fi) | `setChannel(108)` |
| Data rate | 250 KBPS (most robust / longest range) | `setDataRate(RF24_250KBPS)` |
| PA level | HIGH | `setPALevel(RF24_PA_HIGH)` |

**Transmit rate:** 2 Hz (every 500 ms).

---

## 6. Telemetry packet format

The payload is a packed struct of 8 floats = **32 bytes**, which is exactly the
NRF24L01's maximum single-payload size. Both sketches declare an identical
struct so the bytes line up on each end.

```c
struct __attribute__((packed)) Telemetry {
  float accX;         // g
  float accY;         // g
  float accZ;         // g
  float gyroZ;        // deg/s
  float temperature;  // Celsius
  float pressure;     // Pascals
  float lat;          // decimal degrees (0.0 when no GPS fix)
  float lng;          // decimal degrees (0.0 when no GPS fix)
};
```

If you add or reorder fields, do it in **both** sketches, and keep the total at
32 bytes or fewer.

---

## 7. Understanding the TX status output

On the rocket's serial monitor each packet prints a line such as:

```
[TX OK]   #42  (40/42 ok)  Acc:0.02,-0.01,1.00 T:27.4 P:101960 GPS:0.00000,0.00000
[TX FAIL] #43  (40/43 ok)  Acc:...
```

Because auto-acknowledgement is enabled, `[TX OK]` means the NRF24 received an
ack back from the ground station — so "OK" confirms the packet was actually
received, not merely sent. This makes the transmitter's own serial output a live
link-health indicator. The running `(40/43 ok)` counter shows how many of the
total packets were acknowledged.

A consequence: if the ground station is off or out of range, the rocket prints
`[TX FAIL]` even though it is transmitting correctly, because nothing is acking.
When the receiver comes online and is in range, the transmitter flips to
`[TX OK]` — that transition is your end-to-end confirmation.

---

## 8. Altitude calculation

The ground station derives altitude from the transmitted pressure using the
standard barometric formula:

```
altitude_m = 44330 * (1 - (pressure / SEA_LEVEL_PA) ^ 0.1903)
```

`SEA_LEVEL_PA` defaults to 101325 Pa. For an accurate reading, set it to the
local QNH (sea-level-adjusted pressure) on the day of the flight. If it is left
at the default and local pressure differs, ground level will read as a non-zero
altitude; setting it to the actual local sea-level pressure zeroes the readout.

---

## 9. Software setup

### Required library

**RF24 by TMRh20** — install via the Arduino Library Manager (search "RF24",
author TMRh20). This is the maintained version and matches the `nRF24/RF24`
repository. Both nodes need it.

### Rocket node (STM32) — Arduino IDE settings

| Setting | Value |
|---------|-------|
| Board | Generic STM32F4 series |
| Board part number | Generic F405RGTx |
| Upload method | STM32CubeProgrammer (DFU) |
| USB support | CDC (generic 'Serial' supersede U(S)ART) |
| U(S)ART support | Enabled (generic 'Serial') |
| Serial monitor baud | 9600 |

Additional rocket libraries: `MPU6050_light` (rfetick), `BMP180MI`
(christandlg — include `<BMP180I2C.h>`), `TinyGPSPlus` (mikalhart).

**Upload procedure:** short BOOT0 to +3V3, plug in USB, upload, then return BOOT0
to its run position and press reset. The PC13 LED blinking once per second
confirms the sketch is running. Note that the board re-enumerates as a different
USB device (new COM port) between bootloader and run modes, so re-select the port
after it starts running.

### Ground node (ESP32) — Arduino IDE settings

| Setting | Value |
|---------|-------|
| Board | Your ESP32 dev board |
| Serial monitor baud | 115200 |

---

## 10. Bring-up checklist

1. Fit the decoupling capacitor on both NRF24 modules.
2. Confirm NRF24 VCC is on 3.3 V (ESP32 side) — never 5 V.
3. Flash the ESP32 receiver; open its serial monitor at 115200. It should print
   `NRF24: OK` and `Waiting for telemetry...`.
4. Flash the rocket transmitter; open its serial monitor at 9600. Confirm the
   sensor init lines and which magnetometer was detected.
5. Watch the rocket's TX lines. With the receiver running and in range they
   should read `[TX OK]`.
6. Confirm the ESP32 prints `-------- PACKET --------` blocks.
7. Take the rocket outdoors with clear sky for a few minutes to obtain a GPS fix;
   the lat/lng fields will change from zeros to real coordinates.
8. On flight day, set `SEA_LEVEL_PA` in the receiver to the local QNH.

---

## 11. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| All `[TX FAIL]`, some OKs early then none | NRF24 brownout | Add / increase the decoupling cap on both modules; ensure a solid 3.3 V supply |
| `radio.begin()` FAILED | Wiring or power | Check SPI wiring, CE/CSN pins, 3.3 V present |
| begin OK but never any packets | Address / channel / data-rate mismatch, or missing cap | Verify all four link parameters match; check the cap |
| BMP180 reads `nan` | Wrong API or bypass closed | Use measure → wait → get; assert I2C bypass after MPU init |
| Magnetometer read fails | Board has QMC5883L not HMC5883L | The sketch auto-detects both; confirm the startup line reports which was found |
| GPS always `no fix` / zeros | Indoors, no sky view | Test outdoors; a fix takes a few minutes |
| Radios too close, packets flaky | RX front-end overload at very short range | Separate the modules by a metre or more, or lower PA level |
| No serial output at all (STM32) | Wrong COM port after re-enumeration, or BOOT0 still high | Re-select the port; set BOOT0 low and reset (LED should blink) |

---

## 12. Possible extensions

- **Data logging:** have the ESP32 write incoming packets to an SD card, or push
  them over Wi-Fi to a laptop for live plotting.
- **More telemetry:** the packet has room if you drop precision elsewhere;
  remember the 32-byte limit and update both structs together.
- **Magnetometer heading:** the rocket already detects and configures the
  magnetometer; add a heading calculation if you need orientation.
- **Ground display:** parse the serial output into a simple dashboard
  (altitude vs time, GPS track).

---

*This system was developed and verified incrementally: sensor bring-up on the
STM32 flight computer first, then the NRF24 link, with radio pin assignments
taken directly from the board's KiCad schematic. Field testing confirmed the
decoupling capacitor as the decisive factor in link reliability.*
