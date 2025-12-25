# Hamurabi LED Controller

A smart LED controller for ESP32-C6 that drives multiple LED types with voice control via Google Home/IFTTT.

## Features

- **Multi-LED Support**

  - 60× RGBW NeoPixels (SK6812) on GPIO4
  - Onboard RGB LED (status indicator) on GPIO8
  - 9×16 Charlieplex Matrix via I2C (planned)
  - nOOds 12V LED Filament via PWM (planned)

- **Animations**

  - Meteor spinner (rotating gradient)
  - Rainbow cycle
  - Breathing/pulsing effect
  - Solid color mode
  - Fully customizable colors via hex codes

- **Voice Control (via IFTTT + Adafruit IO)**

  - "Hey Google, activate party mode" → Rainbow animation
  - "Hey Google, set meteor to blue" → Blue solid color
  - Full color and animation control via MQTT

- **Smart Features**
  - WiFi connectivity (connects to home network)
  - Persistent rotation counter (survives reboots)
  - Startup LED sequence (boot status indication)
  - Master brightness control

## Hardware

### Required Components

| Component                                                                | Quantity | Purpose                       |
| ------------------------------------------------------------------------ | -------- | ----------------------------- |
| [ESP32-C6 Dev Board](https://www.waveshare.com/wiki/ESP32-C6-DEV-KIT-N8) | 1        | Main controller               |
| RGBW NeoPixel Strip (SK6812)                                             | 60 LEDs  | Main light source             |
| 12V 5A Power Supply                                                      | 1        | Main power                    |
| Buck Converter (12V→5V, 6A)                                              | 1        | Step down voltage             |
| 330Ω Resistor                                                            | 1        | NeoPixel data line protection |
| 1000µF Capacitor                                                         | 1        | Power smoothing               |

### Optional Components (for full build)

| Component                                                   | Quantity | Purpose                       |
| ----------------------------------------------------------- | -------- | ----------------------------- |
| [IS31FL3731 Driver](https://www.adafruit.com/product/2946)  | 1        | Charlieplex matrix controller |
| [9×16 LED Matrix](https://www.adafruit.com/product/2973)    | 1        | Display matrix                |
| [nOOds LED Filament](https://www.adafruit.com/product/5731) | 1        | Accent lighting               |
| IRLZ44N MOSFET                                              | 1        | 12V switching for nOOds       |
| 10KΩ Resistor                                               | 1        | MOSFET gate pulldown          |
| 4.7KΩ Resistors                                             | 2        | I2C pull-ups                  |

### Wiring

```
ESP32-C6 Pin    →    Device
─────────────────────────────
GPIO4           →    NeoPixel DATA (via 330Ω resistor)
GPIO5           →    MOSFET Gate (for nOOds)
GPIO6 (SDA)     →    IS31FL3731 SDA
GPIO7 (SCL)     →    IS31FL3731 SCL
GPIO8           →    Onboard RGB LED (built-in)
VIN (5V)        →    Buck Converter Output
GND             →    Common Ground
```

See `schematics/led_controller.kicad_sch` for the full schematic.

## Software Setup

### Prerequisites

- [ESP-IDF v5.5+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/)
- [Adafruit IO Account](https://io.adafruit.com/) (free)
- [IFTTT Account](https://ifttt.com/) (for voice control)

### Build & Flash

```bash
# Set up ESP-IDF environment
source ~/esp/v5.5.1/esp-idf/export.sh

# Build
cd ~/esp/blink
idf.py build

# Flash and monitor
idf.py flash monitor
```

### Configuration

#### 1. Set Up Credentials (Required)

```bash
# Copy the template
cp main/credentials.h.template main/credentials.h

# Edit with your values
nano main/credentials.h
```

Fill in your credentials:

```c
#define WIFI_SSID      "YourWiFiName"
#define WIFI_PASSWORD  "YourWiFiPassword"

#define ADAFRUIT_IO_USERNAME    "your_username"
#define ADAFRUIT_IO_KEY         "aio_xxxx..."
#define ADAFRUIT_IO_FEED        "your_feed_name"
```

> ⚠️ `credentials.h` is gitignored - your secrets never get committed!

#### 2. LED Configuration (Optional)

Edit `main/blink_example_main.c`:

```c
#define RGBW_LED_COUNT 60        // Number of NeoPixels
#define MASTER_BRIGHTNESS 0.50f  // 0.0 to 1.0
```

## Voice Commands (IFTTT Setup)

1. Create an [IFTTT](https://ifttt.com/) account
2. Create applets with:
   - **IF**: Google Assistant → Activate scene → "party mode"
   - **THEN**: Adafruit → Send data to feed → `rainbow`

### Supported Commands

| Command                                                | Action                           |
| ------------------------------------------------------ | -------------------------------- |
| `meteor`                                               | Meteor spinner animation         |
| `rainbow`                                              | Rainbow cycle animation          |
| `breathing`                                            | Breathing/pulsing effect         |
| `solid`                                                | Solid color (current color)      |
| `off`                                                  | Turn off all LEDs                |
| `on`                                                   | Turn on (meteor mode)            |
| `slow` / `medium` / `fast`                             | Animation speed                  |
| `red` / `green` / `blue` / `purple` / `white` / `warm` | Named colors                     |
| `color:RRGGBB`                                         | Hex color (e.g., `color:FF00FF`) |

## Project Structure

```
blink/
├── main/
│   ├── blink_example_main.c   # Main application code
│   ├── credentials.h          # Your credentials (gitignored!)
│   ├── credentials.h.template # Template for credentials
│   ├── CMakeLists.txt         # Component build config
│   ├── idf_component.yml      # Component dependencies
│   └── Kconfig.projbuild      # Menu configuration
├── schematics/
│   └── led_controller.kicad_sch  # KiCad schematic
├── .gitignore                 # Git ignore rules
├── CMakeLists.txt             # Project build config
├── sdkconfig.defaults         # Default build settings
└── README.md                  # This file
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ESP32-C6                                        │
│                                                                              │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │   WiFi      │  │    MQTT      │  │  Animation   │  │   LED Drivers    │  │
│  │  Station    │─▶│   Client     │─▶│   Engine     │─▶│  (RMT, I2C, PWM) │  │
│  │             │  │ (Adafruit IO)│  │              │  │                  │  │
│  └─────────────┘  └──────────────┘  └──────────────┘  └────────┬─────────┘  │
│                                                                 │            │
└─────────────────────────────────────────────────────────────────┼────────────┘
                                                                  │
                    ┌─────────────────────────────────────────────┼────────────┐
                    │                                             ▼            │
                    │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │
                    │  │  NeoPixels   │  │  Charlieplex │  │    nOOds     │   │
                    │  │  (60× RGBW)  │  │   Matrix     │  │  (12V PWM)   │   │
                    │  │   GPIO4      │  │   I2C        │  │   GPIO5      │   │
                    │  └──────────────┘  └──────────────┘  └──────────────┘   │
                    │                      LED OUTPUTS                         │
                    └──────────────────────────────────────────────────────────┘
```

## Power Requirements

| Device               | Voltage | Max Current | Max Power |
| -------------------- | ------- | ----------- | --------- |
| 60× NeoPixels (RGBW) | 5V      | 4.8A        | 24W       |
| Charlieplex Matrix   | 5V      | 0.5A        | 2.5W      |
| ESP32-C6             | 5V      | 0.3A        | 1.5W      |
| nOOds 600mm          | 12V     | 0.25A       | 3W        |
| **Total**            |         |             | **~32W**  |

**Recommended:** 12V 5A (60W) power supply with 5V 6A buck converter.

## Status LEDs

The onboard RGB LED indicates system status:

| Color      | Pattern   | Meaning                     |
| ---------- | --------- | --------------------------- |
| White      | Solid 1s  | Boot started                |
| Light Blue | Breathing | Connecting to WiFi          |
| Blue       | Solid     | WiFi connected, MQTT active |
| Red        | Blinking  | WiFi connection failed      |

## Future Plans

- [ ] Matter integration for native Google/Apple Home support
- [ ] Web UI for configuration
- [ ] More animation patterns
- [ ] Audio-reactive mode
- [ ] Home Assistant integration

## License

This project is open source. Feel free to use, modify, and distribute.

## Acknowledgments

- Built with [ESP-IDF](https://github.com/espressif/esp-idf)
- Uses [Adafruit IO](https://io.adafruit.com/) for MQTT
- NeoPixel control via [esp-idf-led-strip](https://components.espressif.com/components/espressif/led_strip)
