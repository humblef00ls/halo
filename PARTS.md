# Parts List

Complete bill of materials for Halo Hub + Angel.

---

## Halo Hub (Main Board)

### Microcontroller

| Part                          | Description                         | Where Used    | Why Needed                                         |
| ----------------------------- | ----------------------------------- | ------------- | -------------------------------------------------- |
| Waveshare ESP32-C6-DEV-KIT-N8 | Main microcontroller, WiFi + Zigbee | Central brain | Controls everything, has 802.15.4 radio for Zigbee |

### LEDs

| Part                        | Description                | Where Used      | Why Needed                                       |
| --------------------------- | -------------------------- | --------------- | ------------------------------------------------ |
| SK6812 RGBW NeoPixels (×45) | Addressable RGB+White LEDs | LED ring        | Main light output, animations                    |
| 12V nOOds LED Filament      | Flexible 12V LED wire      | Accent lighting | Soft ambient glow, different look than NeoPixels |

### Audio

| Part           | Description                    | Where Used | Why Needed                            |
| -------------- | ------------------------------ | ---------- | ------------------------------------- |
| Passive Buzzer | Piezo buzzer, needs PWM signal | GPIO23     | Startup melodies, alerts, RTTTL tunes |

### Sensors

| Part                   | Description                  | Where Used                      | Why Needed                                          |
| ---------------------- | ---------------------------- | ------------------------------- | --------------------------------------------------- |
| Waveshare mmWave Radar | 24GHz presence/motion sensor | 3.3V power, UART (GPIO2, GPIO3) | Detects people without camera, triggers automations |

_Note: The mmWave radar runs on 3.3V (not 5V). It also has an OT2 pin that outputs HIGH when presence is detected - useful for simple interrupt-based detection without parsing UART._

### Display

| Part                        | Description          | Where Used         | Why Needed                           |
| --------------------------- | -------------------- | ------------------ | ------------------------------------ |
| Adafruit Charlieplex Matrix | 9×16 LED matrix, I2C | I2C (GPIO6, GPIO7) | Status display, notifications, clock |

### Input

| Part                 | Description                     | Where Used                          | Why Needed                               |
| -------------------- | ------------------------------- | ----------------------------------- | ---------------------------------------- |
| Tactile Buttons (×3) | Momentary push buttons          | GPIO10, GPIO11, GPIO13              | Mode select, brightness, manual controls |
| Rotary Encoder       | Infinite rotation + push button | GPIO19 (A), GPIO21 (B), GPIO18 (SW) | Volume-style control, menu navigation    |

---

## Power System

### Main Power

| Part                          | Description              | Where Used                          | Why Needed                           |
| ----------------------------- | ------------------------ | ----------------------------------- | ------------------------------------ |
| 5V 6A Power Supply            | Barrel jack wall adapter | Main input                          | Powers everything when plugged in    |
| Barrel Jack Connector         | DC power input jack      | PCB input                           | Connects wall adapter to PCB         |
| 1000µF Electrolytic Capacitor | Bulk capacitor           | After barrel jack, before NeoPixels | Smooths power for LED current spikes |
| 100nF Ceramic Capacitor       | Decoupling cap           | Near ESP32-C6                       | Filters high-frequency noise for MCU |

### 12V Boost Circuit (for nOOds)

| Part                        | Description                | Where Used                 | Why Needed                                  |
| --------------------------- | -------------------------- | -------------------------- | ------------------------------------------- |
| MT3608 Boost Module #1      | 5V → 12V step-up converter | 12V power rail             | nOOds need 12V, main power is 5V            |
| IRLB8721 N-MOSFET           | Logic-level MOSFET         | Between 12V rail and nOOds | ESP32 switches 12V load with 3.3V signal    |
| 1kΩ Resistor (R2)           | Gate resistor              | GPIO22 → MOSFET gate       | Limits current, protects GPIO               |
| 10kΩ Resistor (R3)          | Gate pull-down             | MOSFET gate → GND          | Ensures MOSFET stays OFF when GPIO floating |
| 10µF Electrolytic Capacitor | Boost output cap           | MT3608 output              | Stabilizes 12V output                       |

### Battery Backup Circuit

| Part                       | Description                 | Where Used                 | Why Needed                                       |
| -------------------------- | --------------------------- | -------------------------- | ------------------------------------------------ |
| MT3608 Boost Module #2     | 3.7V → 5V step-up converter | Battery backup path        | Battery is 3.7V, system needs 5V                 |
| Schottky Diode D1 (1N5819) | Main power diode            | 5V barrel → 5V rail        | Prevents backflow from battery circuit           |
| Schottky Diode D2 (1N5819) | Battery power diode         | MT3608 #2 output → 5V rail | Prevents backflow from main power                |
| 10kΩ Resistor (×2)         | Voltage divider             | Barrel jack input → GPIO1  | Detects power loss (scales 5V to safe ADC level) |

---

## Angel (Camera Module)

Angel runs as an always-on 24/7 security camera. It operates identically whether wall power is present or not.

| Part                       | Description                     | Where Used     | Why Needed                               |
| -------------------------- | ------------------------------- | -------------- | ---------------------------------------- |
| XIAO ESP32S3 Sense         | Tiny ESP32-S3 with camera + mic | Angel module   | Camera, microphone, AI processing        |
| 3.7V 380mAh LiPo Battery   | Lithium polymer battery         | Powers Angel   | Portable operation, backup for Halo      |
| JST-SH 1.0mm Adapter Cable | Battery connector adapter       | Battery → XIAO | Your battery has 2.0mm, XIAO needs 1.0mm |
| 3-Wire Cable               | Connection to Halo Hub          | Between boards | Power, ground, battery backup            |

---

## Connectors & Cables

| Part                  | Description                    | Where Used             | Why Needed                     |
| --------------------- | ------------------------------ | ---------------------- | ------------------------------ |
| 4-Wire Pogo Connector | Carries 5V, GND, Batt+, DATA   | Between Halo and Angel | Magnetic detachable connection |
| JST-SH 1.0mm Adapter  | 2.0mm to 1.0mm battery adapter | Battery to XIAO        | Battery connector mismatch     |

---

## GPIO Pin Assignments

### ESP32-C6 (Halo Hub)

| GPIO | Function        | Type        | Notes                                        |
| ---- | --------------- | ----------- | -------------------------------------------- |
| 1    | Power detect    | ADC input   | Voltage divider from barrel jack (before D1) |
| 2    | mmWave TX       | UART TX     | To radar RX                                  |
| 3    | mmWave RX       | UART RX     | From radar TX                                |
| 4    | NeoPixel data   | Digital out | RMT peripheral                               |
| 5    | Melody button   | Digital in  | Internal pull-up                             |
| 6    | I2C SDA         | I2C         | Matrix display                               |
| 7    | I2C SCL         | I2C         | Matrix display                               |
| 8    | Onboard RGB LED | Digital out | Status indicator                             |
| 9    | Boot button     | Digital in  | Strapping pin, power on/off                  |
| 10   | Button 1        | Digital in  | Internal pull-up                             |
| 11   | Button 2        | Digital in  | Internal pull-up                             |
| 13   | Button 3        | Digital in  | Internal pull-up                             |
| 15   | Angel DATA      | Digital I/O | Signal line to/from Angel (future use)       |
| 18   | Encoder SW      | Digital in  | Encoder push button                          |
| 19   | Encoder A       | Digital in  | Quadrature input                             |
| 20   | Radar OT2       | Digital in  | mmWave presence interrupt (HIGH = detected)  |
| 21   | Encoder B       | Digital in  | Quadrature input                             |
| 22   | MOSFET gate     | Digital out | Controls 12V nOOds                           |
| 23   | Buzzer          | PWM out     | LEDC peripheral                              |

### XIAO ESP32S3 Sense (Angel)

Angel runs continuously as an always-on security camera. No wake signal needed.

| GPIO | Function        | Type     | Notes                        |
| ---- | --------------- | -------- | ---------------------------- |
| -    | Camera          | Built-in | OV2640                       |
| -    | Microphone      | Built-in | PDM mic                      |
| -    | SD Card         | Built-in | Local storage                |
| -    | Battery charger | Built-in | JST-SH 1.0mm, auto-charges   |
| -    | 5V input        | Power    | From Halo when wall power on |

---

## 4-Wire Cable (Halo ↔ Angel)

Angel runs 24/7 as an always-on security camera. Uses a 4-pin pogo magnetic connector for easy attachment/detachment.

| Pin | Wire  | Halo Side     | Angel Side          | Purpose                       |
| --- | ----- | ------------- | ------------------- | ----------------------------- |
| 1   | 5V    | 5V rail       | XIAO 5V pin         | Powers XIAO, charges battery  |
| 2   | GND   | Ground        | XIAO GND            | Common ground                 |
| 3   | Batt+ | MT3608 #2 IN+ | Battery + (Y-split) | Battery provides backup power |
| 4   | DATA  | GPIO 15       | XIAO GPIO (TBD)     | Signal line for future use    |

When wall power is present, 5V flows to Angel and the XIAO's built-in charger tops up the battery. When wall power is lost, the battery powers both Angel and Halo (via the MT3608 boost converter).

---

## Shopping List

### Already Have

- [x] Waveshare ESP32-C6-DEV-KIT-N8
- [x] 45× SK6812 RGBW NeoPixels
- [x] Passive buzzer
- [x] 5V 6A power supply
- [x] MT3608 boost module (for 12V)
- [x] IRLB8721 N-MOSFET
- [x] 1000µF electrolytic capacitor

### Ordered

- [x] XIAO ESP32S3 Sense
- [x] Waveshare mmWave Radar
- [x] 3.7V 380mAh LiPo battery

### To Buy

| Part                        | Qty | Est. Cost | Link/Notes                       |
| --------------------------- | --- | --------- | -------------------------------- |
| MT3608 boost module         | 1   | ~$1       | Same as the one you have         |
| 1N5819 Schottky diodes      | 2   | ~$0.10    | Or SS14, any 1A Schottky         |
| 10kΩ resistors              | 4   | ~$0.05    | 2 for MOSFET, 2 for power detect |
| 1kΩ resistor                | 1   | ~$0.02    | MOSFET gate resistor             |
| 100nF ceramic capacitor     | 1   | ~$0.05    | Decoupling for ESP32             |
| 10µF electrolytic capacitor | 1   | ~$0.10    | Boost converter output           |
| Adafruit Charlieplex Matrix | 1   | ~$15      | 9×16 LED matrix                  |
| 12V nOOds LED Filament      | 1   | ~$8       | Flexible LED wire                |
| Rotary encoder              | 1   | ~$3       | With push button                 |
| Tactile buttons             | 3   | ~$2       | 6mm through-hole or SMD          |
| Barrel jack connector       | 1   | ~$0.50    | 5.5×2.1mm                        |
| JST-SH 1.0mm adapter cable  | 1   | ~$2       | For battery connector            |

**Estimated total for remaining parts: ~$32**

---

## Notes

- **Strapping pins:** GPIO0, GPIO8, GPIO9 are strapping pins on ESP32-C6. GPIO9 is used for boot button (safe). GPIO8 is used for onboard LED (safe after boot).
- **Power detection:** GPIO1 has ADC capability, reads voltage divider to detect power loss.
- **All resistors:** 1/4W through-hole or 0805 SMD are fine.
- **All capacitors:** Voltage rating should be 2× operating voltage (10V+ for 5V rail, 25V+ for 12V rail).
