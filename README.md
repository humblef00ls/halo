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

**ESP32-C6** (main controller) - the brains, always on

- Controls LEDs, buzzer, Zigbee, presence detection
- Handles WiFi and MQTT commands

**XIAO ESP32S3 Sense** (camera module) - sleeps until needed

- Has camera and microphone built-in
- Wakes up when the radar detects someone
- Takes photos, uploads to cloud for AI analysis

### What's Connected Where

#### ESP32-C6 Connections

```
ESP32-C6
├── GPIO 4  → NeoPixel LED Ring (45 LEDs)
├── GPIO 23 → Passive Buzzer
├── GPIO 22 → MOSFET gate (12V nOOds)
├── GPIO 1  → Potentiometer (brightness)
│
├── UART (GPIO 2, 3)
│   └── mmWave Radar (presence detection)
│
├── I2C (GPIO 6, 7)
│   └── Charlieplex Matrix (future)
│
├── I2S (GPIO 10, 11, 12)
│   └── INMP441 Microphone (future)
│
├── Buttons & Encoder (GPIO 13-20)
│   ├── Button 1, 2, 3
│   └── Rotary Encoder (A, B, click)
│
├── GPIO 21 → Wake signal to XIAO
│
├── GPIO 9  → Boot button (power on/off)
├── GPIO 5  → Melody button
├── GPIO 8  → Onboard RGB LED
│
└── Internal 802.15.4 radio → Zigbee (no GPIO needed)
```

#### XIAO ESP32S3 Sense Connections

```
XIAO ESP32S3 Sense
├── Built-in camera (OV2640)
├── Built-in microphone (PDM)
├── Built-in SD card slot
│
├── GPIO 1 ← Wake input from ESP32-C6
│
└── Power from 3.3V (shared with ESP32-C6)
```

### Power

Everything runs off a 5V 6A power supply.

```
5V 6A Supply
├── ESP32-C6 (directly)
├── NeoPixels (directly, with 1000µF cap for smoothing)
├── XIAO ESP32S3 (via 3.3V from ESP32-C6)
│
└── MT3608 Boost (5V → 12V)
    └── MOSFET
        └── 12V nOOds
```

### Parts List

**Have:**

- Waveshare ESP32-C6-DEV-KIT-N8
- 45× SK6812 RGBW NeoPixels
- Passive buzzer
- 10K potentiometer
- 5V 6A power supply
- MT3608 boost module
- IRLB8721 N-MOSFET
- 1000µF capacitor

**Ordered:**

- XIAO ESP32S3 Sense (camera + mic)
- Waveshare mmWave Radar

**To buy:**

- INMP441 I2S microphone (~$5)
- Adafruit Charlieplex Matrix (~$15)
- 12V nOOds LED Filament (~$8)
- Rotary encoder (~$3)
- 3× tactile buttons (~$2)

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

Here's how it works:

1. **mmWave radar** is always watching (very low power, no camera involved)
2. **Nobody home?** XIAO is in deep sleep
3. **Someone detected?** ESP32-C6 wakes XIAO via GPIO
4. **Camera turns on**, takes photos every 2 seconds
5. Photos go to cloud storage
6. Cloud function runs AI (GPT-4V or similar) to check: is this me or a stranger?
7. **Stranger?** Send a Telegram/SMS alert with the photo
8. **It's me?** Log it, maybe trigger a "welcome home" thing

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
│   ├── halo.c                 # Main code
│   ├── zigbee_hub.c/.h        # Zigbee coordinator
│   ├── zigbee_devices.c/.h    # Device storage (NVS)
│   ├── credentials.h          # Your secrets (gitignored)
│   └── credentials.h.template
├── cad_mk1/
│   └── cad_mk1.kicad_sch      # KiCad schematic
├── xiao_camera/               # (Future) XIAO firmware
├── partitions.csv
├── sdkconfig.defaults
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

- [ ] mmWave presence detection
- [ ] XIAO camera integration
- [ ] AI face recognition + alerts
- [ ] INMP441 microphone (music visualization, clap detection)
- [ ] Charlieplex matrix display
- [ ] 12V nOOds accent lighting
- [ ] Rotary encoder + buttons
- [ ] Voice commands via Whisper API
- [ ] Matter support (native HomeKit/Google Home)

---

## License

Do whatever you want with this. It's just a cool light that got out of hand.
