# Programming the CanSat Flight Computer in the Arduino IDE

A complete, step-by-step guide to setting up the Arduino IDE and uploading
firmware to the CanSat Flight Computer (STM32F405RGT6) over USB, including the
BOOT0 / 3V3 jumper procedure that the board requires.

---

## 1. What you need

- The CanSat Flight Computer board
- A USB cable (data-capable, not charge-only) to connect the board to the PC
- A jumper wire or shorting link for the BOOT0 → 3V3 step (see Section 6)
- A Windows / macOS / Linux computer

---

## 2. Install the Arduino IDE

1. Download the Arduino IDE from **arduino.cc** (version 2.x is recommended).
2. Install it and open it once so it creates its folders.

---

## 3. Add the STM32 board support (STM32duino)

The CanSat uses an STM32 chip, which is not supported by the Arduino IDE out of
the box. You add support once:

1. In the IDE, go to **File → Preferences**.
2. Find the field **"Additional boards manager URLs"**.
3. Paste in this URL:
   ```
   https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json
   ```
4. Click **OK**.
5. Go to **Tools → Board → Boards Manager** (or the boards icon in the left bar).
6. Search for **STM32**.
7. Install **"STM32 MCU based boards" by STMicroelectronics**.

This is a large download and may take a few minutes.

---

## 4. Install STM32CubeProgrammer

The CanSat uploads firmware using a method called **DFU** (Device Firmware
Upgrade), which is handled by a free tool from ST called **STM32CubeProgrammer**.

1. Download **STM32CubeProgrammer** from the STMicroelectronics website (search
   "STM32CubeProgrammer download"). A free ST account is required.
2. Install it with default settings. On Windows this also installs the USB (DFU)
   driver the board needs.

You do not need to open STM32CubeProgrammer yourself — the Arduino IDE calls it
in the background during upload. It just has to be installed.

---

## 5. Select the board and set the upload options

With a sketch open, go to the **Tools** menu and set every one of these. Getting
these right is what makes the difference between a clean upload and a failed one.

| Tools setting | Value |
|---------------|-------|
| Board | **Generic STM32F4 series** |
| Board part number | **Generic F405RGTx** |
| Upload method | **STM32CubeProgrammer (DFU)** |
| USB support (if available) | **CDC (generic 'Serial' supersede U(S)ART)** |
| U(S)ART support | **Enabled (generic 'Serial')** |
| USB speed (if available) | **Low/Full Speed** |
| C Runtime Library | Newlib Nano (default) |
| Optimize | Smallest (-Os default) |

Notes on the two that matter most:

- **Board part number = Generic F405RGTx** tells the compiler this is the F405
  chip. If it's left on a different part number (for example the Black Pill's
  F411), sketches may compile but behave wrongly.
- **USB support = CDC (generic 'Serial')** is what makes the board appear as a
  USB serial port so you can see `Serial.print()` output. With this set, the USB
  serial object in your code is called `Serial`.

---

## 6. The BOOT0 / 3V3 jumper — how to put the board into upload mode

The STM32 will only accept a USB (DFU) upload when it is told to start in
"bootloader" mode instead of running your program. That is what the **BOOT0**
pin controls:

- **BOOT0 connected to 3V3 (HIGH):** the chip starts in the bootloader — ready
  to receive a new sketch. Use this to upload.
- **BOOT0 connected to GND / left in its normal position (LOW):** the chip runs
  your uploaded program normally. Use this to run.

The board provides a **BOOT0 pin** and a **3V3 pin** next to each other for
exactly this purpose.

### To upload firmware

1. **Short BOOT0 to 3V3.** Connect the BOOT0 pin to the neighbouring 3V3 pin
   with a jumper wire or shorting link. This pulls BOOT0 HIGH.
2. Connect the board to the PC with USB (or press the reset button if already
   connected) so it powers up into the bootloader.
3. In the Arduino IDE, click **Upload** (the arrow button).
4. The IDE compiles, then STM32CubeProgrammer flashes the board. Watch the
   status bar for "Download verified successfully" or similar.

### To run the firmware after uploading

1. **Remove the BOOT0 → 3V3 jumper** so BOOT0 returns LOW (tied to GND / its
   normal state).
2. Press the **reset** button, or unplug and replug USB.
3. The board now runs your sketch.

> **Key point:** if you leave BOOT0 shorted to 3V3 after uploading, the board
> keeps rebooting into the bootloader and your program never runs — no serial
> output, no LED activity. Always move BOOT0 back to LOW to run.

---

## 7. Confirm the board is running

The onboard LED is on pin **PC13** and is **active-LOW** (it turns on when the
pin is driven LOW). Most of the provided sketches blink it once per second.

- **LED blinking once per second → the sketch is running.** BOOT0 is set
  correctly for run mode.
- **LED not blinking → the sketch is not running.** BOOT0 is probably still HIGH
  (still shorted to 3V3), or the upload did not complete.

This single check tells you whether any "no serial output" problem is a
run-mode issue (fix BOOT0) or a serial-port issue (see Section 8).

---

## 8. Seeing serial output

When the board runs your sketch (BOOT0 LOW) it appears as a USB serial COM port.

Important: the board **re-enumerates as a different USB device** between
bootloader mode and run mode, so the COM port number changes after you move
BOOT0 back and reset.

1. After the board is running, open **Tools → Port** and select the port that
   now appears (it may differ from the one used during DFU upload).
2. Open **Tools → Serial Monitor**.
3. Set the Serial Monitor baud rate (bottom-right) to match your sketch —
   **9600** for the sensor/transmitter sketches here.

If no COM port appears at all in run mode:

- Check that **USB support** was set to the CDC option (Section 5) and reflash.
- On Windows, check **Device Manager**. A yellow warning or "Unknown device"
  means the USB serial driver isn't recognised; reinstalling STM32CubeProgrammer
  usually resolves the driver.

---

## 9. Installing the sensor / radio libraries

The flight-computer sketches use these libraries. Install each via
**Tools → Manage Libraries** and search by name:

| Library | Search for | Author |
|---------|-----------|--------|
| IMU (MPU6050) | MPU6050_light | rfetick |
| Barometer (BMP180) | BMP180MI | Gregor Christandl |
| GPS parsing | TinyGPSPlus | Mikal Hart |
| Radio (NRF24) | RF24 | TMRh20 |

The servo does **not** need the Servo library — the firmware drives it directly
with the STM32 `HardwareTimer` on pin PB8, which is included in the core.

A note on pin names: STM32duino uses the chip's pin names, not Arduino numbers.
So to control the LED you write `digitalWrite(PC13, HIGH);`, not
`digitalWrite(13, HIGH);`.

---

## 10. Quick reference — upload cycle

```
1. Set Tools options (once):  Generic STM32F4 series / Generic F405RGTx /
                              STM32CubeProgrammer (DFU) / CDC 'Serial'
2. Short BOOT0 -> 3V3
3. Plug in USB (or press reset)
4. Click Upload
5. Wait for "download successfully"
6. Remove BOOT0 -> 3V3 jumper
7. Press reset
8. LED blinks = running
9. Tools -> Port -> select new port -> Serial Monitor @ 9600
```

---

## 11. Troubleshooting the upload

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No DFU device found" / upload fails | Board not in bootloader | Confirm BOOT0 is shorted to 3V3, then replug USB / press reset before uploading |
| Upload succeeds but nothing runs (no LED, no serial) | BOOT0 still HIGH | Remove the BOOT0 → 3V3 jumper and reset |
| Compiles but wrong behaviour | Wrong board part number | Set **Board part number = Generic F405RGTx** |
| No serial output, LED blinking | Wrong COM port after re-enumeration | Re-select the port that appears in run mode; set baud to 9600 |
| No COM port at all in run mode | USB support option not set | Set USB support to CDC 'Serial' and reflash |
| Serial monitor shows gibberish | Baud mismatch | Match Serial Monitor baud to the sketch (9600 here) |

---

*The BOOT0 / 3V3 jumper is the one piece of this process that is specific to bare
STM32 boards like the CanSat. Once you have the rhythm — jumper on to upload,
jumper off to run — the rest is the same as any Arduino board.*
