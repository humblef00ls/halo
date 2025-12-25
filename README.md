# Halo

A glowy LED ring thing I'm building. Voice controlled via Google Home, runs on an ESP32-C6.

![status: work in progress](https://img.shields.io/badge/status-work%20in%20progress-yellow)

## What is this?

It's a smart LED controller that drives a ring of 60 RGBW NeoPixels with some nice animations. Eventually it'll also have a charlieplex matrix display and some nOOds (those flexible LED filaments). The whole thing is voice controlled through Google Home.

I wanted something that looks cool on my desk and that I can yell at to change colors.

---

## The Animations

### Cycle Mode (Default)

Automatically switches between Fusion and Wave every 15 seconds.

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

### What I'm Using

| Thing                             | Notes                                                             |
| --------------------------------- | ----------------------------------------------------------------- |
| **Waveshare ESP32-C6-DEV-KIT-N8** | The brains. Has WiFi, plenty of GPIO, and a cute onboard RGB LED  |
| **60× SK6812 RGBW NeoPixels**     | The main event. RGBW means it has a dedicated white LED per pixel |
| **5V 6A Power Supply**            | Enough juice for full brightness (though I run at 25%)            |
| **MT3608 Boost Module**           | Steps 5V up to 12V for the nOOds                                  |
| **IRLB8721 N-MOSFET**             | Switches the 12V nOOds on/off via PWM                             |
| **1000µF Capacitor**              | Smooths out the power. NeoPixels are current-hungry               |

### Wiring

```
                    ┌─────────────────┐
                    │    ESP32-C6     │
                    │                 │
    NeoPixels ◄─────┤ GPIO4           │
                    │                 │
    nOOds PWM ◄─────┤ GPIO5 ──┐       │
                    │         │       │
    I2C SDA ◄───────┤ GPIO6   │       │
    I2C SCL ◄───────┤ GPIO7   │       │
                    │         │       │
    Onboard LED ────┤ GPIO8   │       │
                    └─────────┼───────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │   IRLB8721      │
                    │   N-MOSFET      │
                    │                 │
         Gate ◄─────┤                 │
        (GPIO5)     │                 ├───► nOOds (12V)
                    │     ┌───┐       │
         10K ───────┤─────┤   │       │
        pulldown    │     └───┘       │
                    │      GND        │
                    └─────────────────┘
```

### Power Architecture

```
5V/6A Supply
     │
     ├───────────────────┬───────────────────┐
     │                   │                   │
     ▼                   ▼                   ▼
  ESP32-C6           NeoPixels         MT3608 Boost
  (direct)           (direct)          (5V → 12V)
                        │                    │
                   1000µF cap                ▼
                   (smoothing)        MOSFET → nOOds
```

---

## Software Stack

| Tool                   | What it does                                                   |
| ---------------------- | -------------------------------------------------------------- |
| **ESP-IDF v5.5**       | Espressif's official dev framework. Not Arduino.               |
| **RMT peripheral**     | Hardware peripheral that generates the precise NeoPixel timing |
| **MQTT (Adafruit IO)** | Cloud broker for receiving voice commands                      |
| **IFTTT**              | Bridges Google Assistant to Adafruit IO                        |
| **KiCad**              | For the schematic (living in `cad_mk1/`)                       |

---

## How to Build This Yourself

### 1. Get the hardware

- ESP32-C6 dev board (Waveshare, Seeed, or any)
- SK6812 RGBW LED strip (60 LEDs or however many you want)
- 5V power supply (at least 3A, more if you want full brightness)
- 1000µF capacitor
- Some wire

### 2. Set up ESP-IDF

```bash
# Install ESP-IDF (one time)
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
source export.sh
```

### 3. Clone and configure

```bash
git clone <this-repo>
cd halo

# Set up your credentials
cp main/credentials.h.template main/credentials.h
nano main/credentials.h  # Fill in your WiFi and Adafruit IO creds
```

### 4. Build and flash

```bash
idf.py build
idf.py flash monitor
```

### 5. Set up voice control (optional)

1. Create an [Adafruit IO](https://io.adafruit.com/) account (free)
2. Create a feed called `halo` (or whatever)
3. Create [IFTTT](https://ifttt.com/) applets:
   - **If** Google Assistant "activate rainbow mode"
   - **Then** Adafruit → Send `rainbow` to your feed

---

## Voice Commands

| Say this                                     | It does this                                    |
| -------------------------------------------- | ----------------------------------------------- |
| `cycle`                                      | Auto-switches between fusion and wave (default) |
| `fusion`                                     | White-to-purple gradient                        |
| `wave`                                       | Blue pulse from center                          |
| `meteor`                                     | Rotating spinner                                |
| `rainbow`                                    | Color wheel                                     |
| `breathing`                                  | Pulsing                                         |
| `solid`                                      | Static color                                    |
| `off` / `on`                                 | Self-explanatory                                |
| `slow` / `medium` / `fast`                   | Animation speed                                 |
| `red` / `blue` / `purple` / `white` / `warm` | Named colors                                    |
| `color:FF00FF`                               | Hex color (RRGGBB format)                       |

---

## Startup Sequence

When you power on, this happens:

```
┌────────────────────────────────────────────────────────────────┐
│  ONBOARD LED                │  LED STRIP                       │
├─────────────────────────────┼──────────────────────────────────┤
│  Solid white (1 sec)        │  (not initialized yet)           │
│  Fade to black              │                                  │
│  Breathing light blue       │  Red pixel scan (hardware test)  │
│  ░░░▓▓▓███▓▓▓░░░            │  ●○○○○○○○○ → ○●○○○○○○○ → ...     │
│                             │                                  │
│  (WiFi connecting...)       │  (visual confirmation LEDs work) │
├─────────────────────────────┼──────────────────────────────────┤
│  Solid blue = connected!    │  Clears, starts normal animation │
│  Blinking red = failed :(   │                                  │
└─────────────────────────────┴──────────────────────────────────┘
```

---

## Project Structure

```
halo/
├── main/
│   ├── halo.c                 # All the code lives here
│   ├── credentials.h          # Your secrets (gitignored)
│   └── credentials.h.template # Copy this to get started
├── cad_mk1/
│   └── cad_mk1.kicad_sch      # KiCad schematic
├── build/                      # Compiled stuff (gitignored)
└── README.md                   # You are here
```

---

## Current Settings

```c
#define RGBW_LED_COUNT     15     // Pixels in the ring
#define MASTER_BRIGHTNESS  0.25f  // 25% brightness (easy on the eyes)
```

---

## Future Stuff

- [ ] Matter support (native Google/Apple Home, no IFTTT needed)
- [ ] Charlieplex matrix integration
- [ ] nOOds PWM control
- [ ] Audio-reactive mode
- [ ] Web config UI
- [ ] Home Assistant

---

## License

Do whatever you want with this. It's just a desk lamp.
