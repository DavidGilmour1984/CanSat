# CanSat Telemetry & Control System — Documentation

A two-way wireless link between a CanSat / model-rocket flight computer and a
ground station. The rocket reads its onboard sensors and transmits them over a
2.4 GHz NRF24L01 radio; the ground station receives the packets, displays them
in a browser dashboard, and sends a servo position back to the rocket over the
same link.

**Files in this system**

| File | Runs on | Purpose |
|------|---------|---------|
| `CanSat_Computer.txt` | CanSat STM32F405 board | Reads sensors, transmits telemetry, drives the servo |
| `ESP32_Ground_Station.txt` | ESP32 | Receives telemetry (CSV out), relays servo commands |
| `GUI.html` | Chrome / Edge browser | Live dashboard + servo slider, via Web Serial |
| `Programming_the_CanSat_in_Arduino_IDE.md` | — | How to flash the CanSat board |

---

## 1. System overview

The system has two nodes with a bidirectional radio link:

**Rocket node (transmitter)** — a CanSat Flight Computer built around an
STM32F405RGT6. It reads its inertial, barometric, magnetic and GPS sensors,
broadcasts a telemetry packet twice per second, and drives a servo whose
position is commanded from the ground.

**Ground node (receiver)** — an ESP32 development board with its own NRF24L01
module. It listens for packets, streams them as CSV to the browser dashboard,
and returns a servo angle to the rocket as a radio acknowledgement payload.

```
   [ROCKET - STM32F405 CanSat]                 [GROUND - ESP32]        [BROWSER]
   MPU6050  (accel / gyro) ---+                                         GUI.html
   BMP180   (pressure/temp) --+--> STM32 --> NRF24 ~~telemetry~~> NRF24 --> ESP32 --USB--> dashboard
   HMC/QMC  (magnetometer) ---+         ^                                              |
   L80      (GPS) ------------+         |                                              |
   SERVO (PB8) <--------------+         +~~~~~~~ servo angle (ack payload) ~~~~~~~ slider
```

The two radios are configured identically (address, channel, data rate) so they
interoperate directly. The return path uses the NRF24's acknowledgement-payload
feature: the servo command rides back on the ack that the receiver already sends
for each telemetry packet.

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
| IRQ | Not connected |

These radio pins were confirmed from the board's KiCad schematic.

### Servo

| Item | Detail |
|------|--------|
| Signal pin | **PB8** (servo PWM header, marked "PWM") |
| Timer | TIM4 channel 3, driven by HardwareTimer |
| Frame | 50 Hz (20 ms) |
| Pulse range | 1000–2000 µs (0–180°) |
| Power | From the servo header's own 5 V rail, **not** the radio or logic 3.3 V |

**Why PB8 and HardwareTimer:** the Arduino `Servo` library mis-scales the pulse
on this core (it produced a compressed, cycling range), and PB12 sits on the
advanced-control timer TIM1 which is fussier. PB8 is on the general-purpose
timer TIM4 and is the pin the board marks as PWM. The firmware drives it with a
`HardwareTimer` configured so one timer tick equals one microsecond (prescaler
84, since TIM4's clock on the F405 is 84 MHz), giving an exact, stable pulse.

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
sketch to match). SCK/MOSI/MISO must stay on the VSPI pins.

---

## 4. Power and decoupling — the single most important point

The bare NRF24L01 modules draw sharp current spikes on transmit that a marginal
3.3 V supply cannot deliver. The result is a radio that initialises fine
(`radio.begin()` returns OK) and sends the first several packets, then stops
getting acknowledgements — the "some early OKs then all FAIL" pattern.

**Fit a 10 µF capacitor (47 µF or 100 µF is also fine) directly across the VCC
and GND pins of each NRF24 module, at the module itself.** Do this on both the
rocket and the ground radios. This resolves the large majority of NRF24 link
failures. The PA/LNA (external-antenna) variants make the capacitor mandatory.

Servo power is separate and equally important: **power the servo from the servo
header's own 5 V rail, not from the 3.3 V that feeds the radio or logic.** A
servo browning out the shared supply causes hunting, buzzing, and can stall.

For the rocket overall, follow the flight computer's power guidance: the 12 V
rail runs the logic and the 12 VA rail runs the nichrome channels; using two
separate batteries avoids a brownout when the nichrome fires. The board runs on
8–12 V (9–12 V recommended; 9 V battery or 3S LiPo).

---

## 5. Radio link parameters

Both sketches must agree on all of these. Change one, change both.

| Parameter | Value | Set by |
|-----------|-------|--------|
| Pipe address | `"CSAT1"` | `openWritingPipe` / `openReadingPipe` |
| Channel | 108 (2.508 GHz, above most Wi-Fi) | `setChannel(108)` |
| Data rate | 250 KBPS (most robust / longest range) | `setDataRate(RF24_250KBPS)` |
| PA level | HIGH | `setPALevel(RF24_PA_HIGH)` |
| Ack payload | enabled | `enableAckPayload()` + `enableDynamicPayloads()` |

**Transmit rate:** 2 Hz (every 500 ms). Raising this (e.g. to 10 Hz) makes the
servo respond faster, since a command only travels when the rocket transmits.

---

## 6. Telemetry packet format

The payload is a packed struct of 8 floats = **32 bytes**, exactly the NRF24L01's
maximum single-payload size. Both sketches declare an identical struct.

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

### Command (ground → rocket) — ack payload

The return path carries a single servo angle byte:

```c
struct __attribute__((packed)) Command {
  uint8_t servoAngle;   // 0..180
};
```

The receiver loads this into the NRF24 ack with `writeAckPayload()`; the rocket
reads it after a successful transmit with `isAckPayloadAvailable()` /
`read()` and applies it to the servo only when the value changes.

---

## 7. Understanding the TX status output

On the rocket's serial monitor each packet prints a line such as:

```
[TX OK]   #42  (40/42 ok)  Servo:90  Acc:0.02,-0.01,1.00 T:27.4 P:101960 GPS:0.00000,0.00000
[TX FAIL] #43  (40/43 ok)  Servo:90  Acc:...
```

Because auto-acknowledgement is enabled, `[TX OK]` means the ground station
received the packet (it acked). So "OK" confirms reception, not merely
transmission, making the transmitter's own output a live link-health indicator.
`Servo:NN` shows the angle currently applied to the servo.

If the ground station is off or out of range, the rocket prints `[TX FAIL]`
because nothing acks back. When the receiver comes online, it flips to
`[TX OK]` — that transition is your end-to-end confirmation.

---

## 8. Altitude calculation

The ground station derives altitude from pressure using the standard barometric
formula:

```
altitude_m = 44330 * (1 - (pressure / SEA_LEVEL_PA) ^ 0.1903)
```

`SEA_LEVEL_PA` defaults to 101325 Pa. Set it to the local QNH on flight day for
an accurate reading; otherwise ground level will read as a non-zero altitude.

---

## 9. Software setup

### Required library

**RF24 by TMRh20** — install via the Arduino Library Manager. Both nodes need it.
The rocket also needs `MPU6050_light` (rfetick), `BMP180MI` (christandlg —
include `<BMP180I2C.h>`), and `TinyGPSPlus` (mikalhart). No Servo library is
required — the servo is driven by the built-in HardwareTimer.

### Rocket node (STM32) — Arduino IDE settings

| Setting | Value |
|---------|-------|
| Board | Generic STM32F4 series |
| Board part number | Generic F405RGTx |
| Upload method | STM32CubeProgrammer (DFU) |
| USB support | CDC (generic 'Serial' supersede U(S)ART) |
| U(S)ART support | Enabled (generic 'Serial') |
| Serial monitor baud | 9600 |

Upload procedure and the BOOT0 / 3V3 jumper are covered in
`Programming_the_CanSat_in_Arduino_IDE.md`.

### Ground node (ESP32) — Arduino IDE settings

| Setting | Value |
|---------|-------|
| Board | Your ESP32 dev board |
| Serial monitor / dashboard baud | 115200 |

---

## 10. Bring-up checklist

1. Fit the decoupling capacitor on both NRF24 modules.
2. Confirm NRF24 VCC is on 3.3 V (ESP32 side) — never 5 V.
3. Power the servo from the servo header's 5 V rail; signal on PB8.
4. Flash `ESP32_Ground_Station.txt` to the ESP32; it should print
   `#NRF24 OK`.
5. Flash `CanSat_Computer.txt`; confirm sensor init lines and which
   magnetometer was detected.
6. Watch the rocket's TX lines; with the receiver running they read `[TX OK]`.
7. Open `GUI.html` in Chrome/Edge, click Connect, choose the ESP32 port.
8. Move the servo slider — the rocket's `Servo:` value and the servo should
   follow within a packet or two.
9. Outdoors with clear sky, wait for a GPS fix; lat/lng change from zeros.
10. On flight day, set `SEA_LEVEL_PA` to the local QNH.

---

## 11. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| All `[TX FAIL]`, some OKs early then none | NRF24 brownout | Add / increase the decoupling cap; ensure a solid 3.3 V supply |
| `radio.begin()` FAILED | Wiring or power | Check SPI wiring, CE/CSN pins, 3.3 V present |
| begin OK but never any packets | Address / channel / rate mismatch, or missing cap | Verify all link parameters match; check the cap |
| BMP180 reads `nan` | Wrong API or bypass closed | Use measure → wait → get; assert I2C bypass after MPU init |
| Magnetometer read fails | Board has QMC5883L not HMC5883L | The sketch auto-detects both; check the startup line |
| Servo jitters / grinds / cycles | Servo library mis-scaling, or wrong pulse range | Use the HardwareTimer method on PB8 (already in the firmware); narrow the pulse range if it grinds at the ends |
| Servo buzzes/stalls, gets hot | Driven past its stop, or weak power | Narrow `SERVO_MIN_US`/`SERVO_MAX_US`; give the servo its own 5 V supply — unplug it if it heats |
| Servo moves only when value changes | By design | The firmware ignores repeats to avoid twice-a-second twitching |
| GPS always `no fix` / zeros | Indoors, no sky view | Test outdoors; a fix takes a few minutes |
| No serial output (STM32) | Wrong COM port after re-enumeration, or BOOT0 high | Re-select the port; set BOOT0 low and reset (LED should blink) |

---

## 12. The browser dashboard — GUI.html

`GUI.html` reads the ESP32's serial output directly over the **Web Serial API**
— no server, no install. It runs in Chrome or Edge on desktop.

The ESP32 must be running `ESP32_Ground_Station.txt`, which prints one
comma-separated line per packet:

```
T,accX,accY,accZ,gyroZ,tempC,pressurePa,alt_m,lat,lng
```

Status/comment lines begin with `#`. To use the dashboard:

1. Close the Arduino Serial Monitor (only one program can hold the COM port).
2. Open `GUI.html` in Chrome/Edge.
3. Click **Connect** and choose the ESP32's COM port.

The dashboard shows live value cards (altitude, pressure, temperature, accel,
gyro, packet rate), a rolling altitude/temperature chart, an acceleration chart,
a GPS panel with a Google Maps link once a fix is present, a raw serial log, and
a **Download CSV log** button.

**Servo control:** the Servo Control panel has a 0–180° slider plus 0 / Centre /
180 buttons. Moving the slider sends `S:<angle>` to the ESP32 over serial; the
ESP32 relays it to the rocket as the ack payload, and the rocket drives the
servo on PB8. There is up to half a second of latency because the command only
travels when the rocket next transmits (2 Hz) — raise the telemetry rate for
snappier response.

---

## 13. Possible extensions

- **Data logging:** have the ESP32 also write incoming packets to an SD card, or
  push them over Wi-Fi for live remote plotting.
- **More telemetry:** the packet has room if you trade precision; keep it at
  32 bytes and update both structs together.
- **Faster servo response:** raise the telemetry rate (change `500` ms in the
  transmitter loop to e.g. `100` ms for 10 Hz).
- **Servo readback:** add the applied angle to the telemetry so the dashboard
  shows the rocket's actual servo position, closing the loop.
- **Ground display:** parse the serial output into an altitude-vs-time plot or a
  live GPS track.

---

*Developed and verified incrementally: sensor bring-up first, then the NRF24
link, then bidirectional servo control. Radio pins were taken directly from the
board's KiCad schematic; the decoupling capacitor proved decisive for link
reliability; and the servo required the HardwareTimer method on PB8 (TIM4) after
the Servo library mis-scaled the pulse.*
