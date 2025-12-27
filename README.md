# Halo

A glowy LED ring thing that got out of hand. Started as a voice-controlled desk lamp, now it's a smart home hub with presence detection, a security camera, and Zigbee control. Runs on ESP32-C6 + XIAO ESP32S3.

![status: work in progress](https://img.shields.io/badge/status-work%20in%20progress-yellow)

## What is this?

It's a smart LED controller that drives a ring of 45 RGBW NeoPixels with some nice animations. It also has:

- Voice control through Google Home
- Zigbee coordinator for smart blinds
- mmWave radar for presence detection
- A camera that wakes up when someone's detected
- A buzzer that plays melodies
- More stuff planned (matrix display, 12V accent lights)

I wanted something that looks cool on my desk and that I can yell at to change colors. Then I kept adding features.

---

## The Animations

### Cycle Mode (Default)

Automatically switches between Fusion, Wave, Tetris, and Stars every 12.5 seconds.

### Fusion

A gradient that blends from soft white on one end to purple on the other. Static, no movement.

```
WHITE                                    PURPLE
  ┃                                        ┃
  ▼                                        ▼
  ████████████████████████████████████████
  W=85 ─────────────────────────────► R=80,B=180
```

### Wave

A blue pulse that starts at the center, radiates outward in both directions, fully exits the strip, then regenerates from the center again.

```
Frame 1:  ░░░░░░░██░░░░░░░   (pulse at center)
Frame 2:  ░░░░██░░░░░██░░░░   (expanding outward)
Frame 3:  ░░██░░░░░░░░░██░░   (approaching edges)
Frame 4:  ██░░░░░░░░░░░░░██   (at edges)
Frame 5:  ░░░░░░░░░░░░░░░░░   (fully exits - all dark)
Frame 6:  ░░░░░░░██░░░░░░░   (regenerates from center)
          ↑                 ↑
        edge             edge
```

### Meteor

A rotating gradient spinner. One bright "head" pixel with a tail that wraps around the entire ring. The tail fades smoothly down to almost nothing, then jumps back to bright at the head.

```
    HEAD
      ↓
  ████████░░░░░░░░░░░░░░░░░░░░░░░░██████
  ← tail wraps around ←←←←←←←←←←←←
```

### Meteor Shower

Multiple meteors (4-5) traveling in the same direction with rainbow trails. Each meteor has its own brightness/tail length.

```
  🔴━━━░░░░🟠━━░░░░░🟡━━━━░░░🟢━░░░░🔵━━━░
       →        →         →       →      →
```

### Stars

Twinkling stars that randomly appear and fade across the strip. Runs at 45 FPS for a gentle twinkle.

```
  ░░░★░░░░░░★░░░░░░░★░░░░░★░░░░░░░★░░
      ↓       ↓         ↓     ↓        ↓
  (random positions, random brightness, fade in/out)
```

### Tetris

Falling blocks that stack at the bottom like the classic game.

```
Frame 1:  █░░░░░░░░░░░░░░   (block at top)
Frame 2:  ░░░█░░░░░░░░░░░   (falling)
Frame 3:  ░░░░░░░█░░░░░░░   (falling)
Frame 4:  ░░░░░░░░░░░░░░█   (lands at bottom)
Frame 5:  █░░░░░░░░░░░░░█   (new block spawns)
```

### Rainbow

Classic HSV color wheel cycling around the ring. Each pixel is a different hue, and the whole thing rotates.

```
  🔴🟠🟡🟢🔵🟣🔴🟠🟡🟢🔵🟣🔴🟠🟡
       → rotates →
```

### Breathing

All pixels pulse in unison. Fades up, fades down, repeat. Uses the current color.

```
  ████████████████████████████████  (bright)
  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  (dim)
  ████████████████████████████████  (bright)
```

### Solid

Just... a solid color. No animation. Set a color and it stays.

---

## Hardware

### The Two Boards

**Halo Hub - ESP32-C6** (main controller) - the brains, always on

- Controls LEDs, buzzer, Zigbee, presence detection
- Handles WiFi and MQTT commands
- Monitors power status, enters low power mode on battery

**Angel - XIAO ESP32S3 Sense** (camera module) - always on

- Has camera, microphone, and SD card built-in
- Has built-in LiPo battery charger (JST-SH 1.0mm connector)
- Runs 24/7 as a security camera (always watching)
- Takes photos, uploads to cloud for AI analysis
- Can detach and run independently on battery
- Operates identically whether plugged in or not
- Battery provides backup power to Halo when main power goes out

### What's Connected Where

#### ESP32-C6 Connections

```
ESP32-C6
├── GPIO 1  → Power detect (ADC, voltage divider from barrel jack)
├── GPIO 4  → NeoPixel LED Ring (45 LEDs)
├── GPIO 23 → Passive Buzzer
├── GPIO 22 → MOSFET gate (12V nOOds)
│
├── UART (GPIO 2, 3)
│   └── mmWave Radar (presence detection)
│
├── I2C (GPIO 6, 7)
│   └── Charlieplex Matrix
│
├── Buttons (GPIO 10, 11, 13)
│   └── Button 1, 2, 3
│
├── Rotary Encoder (GPIO 19, 21, 18)
│   └── A, B, Switch
│
├── GPIO 20 → mmWave radar OT2 (presence interrupt)
│
├── GPIO 9  → Boot button (power on/off)
├── GPIO 5  → Melody button
├── GPIO 8  → Onboard RGB LED
│
└── Internal 802.15.4 radio → Zigbee (no GPIO needed)
```

#### Angel Connections (XIAO ESP32S3 Sense)

Angel runs 24/7 as an always-on security camera. It operates identically whether wall power is present or not - no wake signal needed.

```
XIAO ESP32S3 Sense
├── Built-in camera (OV2640)
├── Built-in microphone (PDM)
├── Built-in SD card slot
├── Built-in LiPo charger (JST-SH 1.0mm connector)
│
└── 4-Wire Pogo Cable to Halo Hub
    ├── 5V      → From Halo 5V rail (charges battery when wall power on)
    ├── GND     → Common ground
    ├── Batt+   → Battery+ to Halo MT3608 #2 (backup power)
    └── DATA    → GPIO 15 (signal line for future use)
```

When detached, Angel runs independently on battery. When connected, wall power charges the battery and the battery provides backup power to Halo if wall power fails.

### Power

Main power is 5V 6A from a barrel jack. There's also a battery backup so the system doesn't just die when power goes out.

```
5V Barrel Jack
├── D1 (Schottky) ──► 5V Rail
│                     ├── ESP32-C6
│                     ├── NeoPixels (with 1000µF cap)
│                     └── MT3608 #1 (5V → 12V)
│                         └── MOSFET → 12V nOOds
│
└── XIAO 5V pin ──► XIAO ESP32S3
                    └── Built-in charger ──► LiPo Battery
                                              │
                                              └── MT3608 #2 (3.7V → 5V)
                                                  └── D2 (Schottky) ──► 5V Rail
```

**When plugged in:** Main 5V powers everything through D1. XIAO charges the battery.

**When unplugged:** Battery → MT3608 #2 → D2 → 5V rail. Halo enters low power mode (dim nightlight only).

**Power loss detection:** Voltage divider (2× 10kΩ) on GPIO1 monitors the barrel jack input (before D1). When it drops to 0V, ESP32-C6 knows it's running on battery and enters low power mode.

### Parts List

See **[PARTS.md](PARTS.md)** for the full bill of materials with descriptions, GPIO assignments, and shopping list.

Quick summary:

| Category | Key Parts                                                     |
| -------- | ------------------------------------------------------------- |
| Halo Hub | ESP32-C6, 45× NeoPixels, buzzer, mmWave radar, matrix display |
| Angel    | XIAO ESP32S3 Sense, 380mAh LiPo battery                       |
| Power    | 5V 6A supply, 2× MT3608 boost, 2× Schottky diodes, MOSFET     |
| Input    | 3× buttons, rotary encoder                                    |
| Accent   | 12V nOOds LED filament                                        |

---

## Software

Built with ESP-IDF v5.5 (not Arduino). Uses:

- RMT peripheral for NeoPixel timing
- esp-zigbee-sdk for Zigbee coordinator
- MQTT via Adafruit IO for voice commands
- IFTTT to bridge Google Assistant

---

## Voice Commands

Say these through Google Home (via IFTTT → Adafruit IO → MQTT):

| Command                                           | What it does                     |
| ------------------------------------------------- | -------------------------------- |
| `cycle`                                           | Auto-switches between animations |
| `fusion` / `wave` / `meteor` / `stars` / `tetris` | Pick an animation                |
| `rainbow` / `breathing` / `solid`                 | Classic modes                    |
| `off` / `on`                                      | Power control                    |
| `slow` / `medium` / `fast`                        | Animation speed                  |
| `red` / `blue` / `purple` / `white` / `warm`      | Named colors                     |
| `blinds:open` / `blinds:close`                    | Zigbee blind control             |

---

## The Security Camera Thing

Angel runs 24/7 as an always-on security camera. Here's how it works:

1. **Camera is always watching** - continuous operation, battery or wall power
2. Takes photos periodically or on motion detection
3. Photos go to cloud storage
4. Cloud function runs AI (GPT-4V or similar) to check: is this me or a stranger?
5. **Stranger?** Send a Telegram/SMS alert with the photo
6. **It's me?** Log it, maybe trigger a "welcome home" thing

**mmWave radar** on Halo provides additional presence detection for automations (lights, alerts, etc.)

The XIAO can also stream live video (MJPEG at 10-15 FPS) if you want to check in remotely.

---

## Startup Sequence

When you power on:

1. **Onboard LED** goes solid white for 1 second, then fades to black
2. **Buzzer** plays startup melody
3. **LED strip** does an RGB scan (red → green → blue) then ramps up white
4. **Buzzer** does a frequency sweep (4 seconds up, 1 second down)
5. **WiFi** connects in the background
6. **Zigbee** enters finder mode (onboard LED sweeps green)
7. After 60 seconds (or when a device is found), enters main loop

If WiFi fails, it blinks red. Press the boot button to enter standby mode.

---

## Project Structure

```
halo/
├── main/
│   ├── halo.c                 # Halo Hub main code
│   ├── zigbee_hub.c/.h        # Zigbee coordinator
│   ├── zigbee_devices.c/.h    # Device storage (NVS)
│   ├── credentials.h          # Your secrets (gitignored)
│   └── credentials.h.template
├── angel/                     # (Future) XIAO ESP32S3 firmware
│   └── angel.c                # Camera, mic, cloud upload
├── schematics/
│   └── halo.kicad_sch         # KiCad schematic
├── partitions.csv
├── sdkconfig.defaults
├── PARTS.md                   # Full bill of materials + GPIO map
└── README.md
```

---

## Building

```bash
# Set up ESP-IDF (one time)
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
source export.sh

# Clone and configure
git clone <this-repo>
cd halo
cp main/credentials.h.template main/credentials.h
# Edit credentials.h with your WiFi and Adafruit IO creds

# Build and flash
idf.py build
idf.py flash monitor
```

---

## Future Plans

### Halo Hub (ESP32-C6)

- [ ] mmWave presence detection
- [ ] Charlieplex matrix display
- [ ] 12V nOOds accent lighting
- [ ] Rotary encoder + buttons
- [ ] Battery backup with low power mode
- [ ] Power loss detection (auto-switch to nightlight mode)
- [ ] Matter support (native HomeKit/Google Home)

### Angel (XIAO ESP32S3 Sense)

- [ ] Camera integration with Halo
- [ ] AI face recognition + stranger alerts
- [ ] Photo capture on presence detection
- [ ] Cloud upload (Google Cloud Storage or similar)
- [ ] Live video streaming (MJPEG)
- [ ] Voice commands via built-in mic + Whisper API
- [ ] Portable/detachable operation on battery
- [ ] Backup battery for Halo when main power goes out

---

## License

Do whatever you want with this. It's just a cool light that got out of hand.
