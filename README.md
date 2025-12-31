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

## Physical Controls

### Rotary Encoder

The rotary encoder (GPIO 19/21/18) provides hands-on control:

| Action | Effect |
|--------|--------|
| Rotate clockwise | Increase brightness by 5% |
| Rotate counter-clockwise | Decrease brightness by 5% (min 20%) |
| Short press | Toggle LED on/off |
| Long press (>1s) | Cycle to next animation |

When you adjust the encoder, a brightness gauge appears on the LED ring for 1.5 seconds.

### Boot Button (GPIO 9)

- **Short press**: Enter standby mode (LEDs off, low power)
- **Hold during boot**: Skip hardware tests (dev mode)

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
│   └── A=CLK, B=DT, Switch (brightness + animation control)
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
- esp-matter for native smart home (Google Home, Apple HomeKit, Amazon Alexa)
- MQTT via Adafruit IO for webhook/app control

---

## Voice Commands

### Via Matter (Native Smart Home)

Halo uses **Matter** for native smart home control. This works locally (no cloud required) with:

- **Google Home** - "Hey Google, turn on Halo"
- **Apple HomeKit** - "Hey Siri, set Halo to blue"
- **Amazon Alexa** - "Alexa, open the blinds"

Two Matter devices are exposed:
1. **Halo Light** - Extended Color Light (on/off, brightness, RGB color)
2. **Halo Blinds** - Window Covering (open/close/position)

#### Commissioning (First-Time Setup)

1. Build and flash the firmware
2. Device boots and enters Matter commissioning mode
3. Open Google Home / Apple Home / Alexa app
4. Add device → Matter-enabled device
5. Scan QR code or enter setup code: `12345678`
6. Device appears in your smart home app!

### Via MQTT Commands (Webhook/App Control)

MQTT provides a webhook-style API for custom apps and automation:

Direct MQTT commands to the `commands` feed:

| Command                                           | What it does                        |
| ------------------------------------------------- | ----------------------------------- |
| `cycle`                                           | Auto-switches between animations    |
| `fusion` / `wave` / `meteor` / `stars` / `tetris` | Pick an animation                   |
| `rainbow` / `breathing` / `solid`                 | Classic modes                       |
| `off` / `on`                                      | Power control                       |
| `brightness:50` (0-100)                           | Set brightness percentage           |
| `color:FF0000` (hex RGB/RGBW)                     | Set color by hex code               |
| `effect:rainbow` / `effect:fire` / etc.           | Set animation by name               |
| `slow` / `medium` / `fast`                        | Animation speed                     |
| `red` / `blue` / `purple` / `white` / `warm`      | Named colors                        |
| `blinds:open` / `blinds:close` / `blinds:stop`    | Zigbee blind control                |
| `blinds:50` (any number 0-100)                    | Move blinds to percentage           |
| `blinds:status` / `blinds:query`                  | Debug: show paired devices/position |
| `blinds:reset`                                    | Clear all paired Zigbee devices     |

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
│   ├── matter_devices.c/.h    # Matter endpoints (Light + Blinds)
│   ├── rotary_encoder.c/.h    # Rotary encoder driver
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

### Option 1: Local ESP-IDF Installation

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

### Option 2: DevContainer (Recommended)

If you're having issues with local ESP-IDF setup (serial port problems, path issues, etc.), use the DevContainer:

1. Install Docker Desktop and VS Code with the "Dev Containers" extension
2. Open the project folder in VS Code
3. Click "Reopen in Container" when prompted (or Cmd+Shift+P → "Dev Containers: Reopen in Container")
4. The container has ESP-IDF v5.3 pre-installed with all tools configured

The DevContainer uses `espressif/idf:v5.3` base image with privileged mode for USB passthrough.

---

## Zigbee: MoES / Tuya Blind Control

The Zigbee coordinator runs on the ESP32-C6's built-in 802.15.4 radio. It forms a network and waits for devices to join.

### Pairing a Device

1. Power on Halo - it enters "finder mode" with green LED sweep
2. Put your Zigbee device in pairing mode (for MoES blinds: hold the button until it beeps)
3. The device should join within 60 seconds
4. Device info is stored in NVS and persists across reboots

### ⚠️ After ESP32 Reboot - Wake Up the Blind!

**Important:** Tuya/MoES blinds go into deep sleep and don't automatically reconnect after the coordinator reboots. You must **press any button** on the blind (or its remote) to wake it up.

- The blind will rejoin within seconds
- Commands will then work normally
- This is a Tuya device behavior, not a bug in our code

**Signs the blind needs waking:**

- Commands are sent but blind doesn't move
- No "Received TO_CLI custom command" errors in logs
- Status shows device as "ONLINE" but it's actually sleeping

### MQTT Commands

| Command          | What it does                               |
| ---------------- | ------------------------------------------ |
| `blinds:open`    | Open the blinds (0% = fully open)          |
| `blinds:close`   | Close the blinds (100% = fully closed)     |
| `blinds:stop`    | Stop movement                              |
| `blinds:50`      | Move to 50% position (any number 0-100)    |
| `blinds:status`  | Print network status and paired devices    |
| `blinds:query`   | Query current blind position               |
| `blinds:debug`   | Start periodic position queries (every 5s) |
| `blinds:nodebug` | Stop periodic queries                      |
| `blinds:reset`   | Clear all paired devices from NVS          |

### Tuya Private Cluster (0xEF00)

**Important:** MoES blinds (and most Tuya/SmartLife Zigbee devices) do NOT use standard ZCL clusters. They use Tuya's proprietary cluster `0xEF00` with custom "data points" (DPs).

Standard ZCL Window Covering cluster (`0x0102`) won't work. The code detects Tuya devices by checking for `0xEF00` in the cluster list and registers them as `ZIGBEE_DEVICE_TYPE_TUYA_BLIND`.

**Tuya DP IDs for blinds:**

- `0x01` - Control (0=open, 1=stop, 2=close) — **Note: open/close are inverted from what you'd expect!**
- `0x02` - Percent position (0-100)
- `0x03` - Percent control (set position)
- `0x05` - Direction setting
- `0x07` - Work state

**Command inversion:** The physical open/close directions were backwards, so the code swaps them:

```c
#define TUYA_BLIND_OPEN   0x02  // Tuya calls this "close" but it physically opens
#define TUYA_BLIND_CLOSE  0x00  // Tuya calls this "open" but it physically closes
```

### Device Discovery Flow

The correct way to discover Zigbee device capabilities:

1. Device announces itself → we get short address
2. Send `esp_zb_zdo_active_ep_req()` → get list of active endpoints
3. For each endpoint, send `esp_zb_zdo_simple_desc_req()` → get cluster list
4. Check cluster list for known clusters (`0xEF00` for Tuya, `0x0102` for Window Covering, etc.)
5. Register device with appropriate type

**Don't rely on `esp_zb_zdo_match_cluster()` callback parameters** - they're often garbage (`0xFFFF` address, `0xFF` endpoint). Use the proper ZDO requests instead.

### NVS Persistence

Paired devices are stored in NVS (non-volatile storage) and automatically restored on boot. The Zigbee network also persists - you don't need to re-pair devices after power cycling.

- Devices stored in NVS namespace `"zigbee_dev"`
- Network state stored in partition `zb_storage`
- Factory reset partition at `zb_fct`

### WiFi/Zigbee Coexistence

The ESP32-C6 runs both WiFi and Zigbee on the same 2.4GHz radio. The coexistence firmware manages this, but you may see occasional timing issues:

```
I (3507) coexist: coex firmware version: b0bcc39
```

If WiFi becomes flaky when Zigbee is active (or vice versa), this is expected behavior. The radios time-share and can't both transmit simultaneously.

---

## Roadmap

### ✅ Phase 1: LED Control

- [x] 45-pixel RGBW LED ring with animations
- [x] MQTT control via Adafruit IO
- [x] Rotary encoder brightness control (+ button for on/off and animation cycling)
- [x] Buzzer feedback and melodies

### ✅ Phase 2: Zigbee Smart Home

- [x] ESP32-C6 as Zigbee coordinator
- [x] Tuya/MoES blind control (0xEF00 cluster)
- [x] Device persistence in NVS
- [x] Auto-reconnect on boot
- [x] Dev mode (hold BOOT to skip hardware tests)

### ✅ Phase 3: Native Smart Home (Matter)

- [x] Matter SDK integration (esp-matter)
- [x] Extended Color Light endpoint (LED ring)
- [x] Window Covering endpoint (blinds)
- [x] Works with Google Home, Apple HomeKit, Amazon Alexa
- [x] Local control (no cloud required)

### 📋 Phase 4: Display & Sensors

- [ ] Charlieplex LED matrix display
- [ ] Stock ticker / weather / notifications
- [ ] mmWave presence detection
- [ ] 12V accent lighting

### 📋 Phase 5: Angel Camera Module

- [ ] XIAO ESP32S3 Sense integration
- [ ] Presence-triggered photo capture
- [ ] AI face recognition + stranger alerts
- [ ] Live video streaming

---

## Troubleshooting & Known Issues

### WiFi: "No Networks Found" / Radio Failure

Sometimes the ESP32-C6 WiFi scan returns zero networks even when you're sitting next to the router. This appears to be a hardware/radio initialization issue, not a software bug.

**What the code does:**

1. Scans for networks
2. If `ap_count == 0` (no networks at all), triggers aggressive retry:
   - Full radio reset: `esp_wifi_stop()` → `esp_wifi_start()`
   - Wait 500ms for radio to stabilize
   - Retry scan up to 3 times
3. If still no networks after 3 resets → likely hardware issue or severe interference

**Log output when this happens:**

```
W (5947) wifi:   ❌ NO NETWORKS FOUND! Radio may need reset.
W (5957) wifi: ║  🔄 RADIO RESET RETRY 1/3 - No networks found!
...
E (15857) wifi: ║  ❌ RADIO FAILURE - No networks after 3 resets!
E (15867) wifi: ║  This may be a hardware issue or severe interference.
```

**Possible causes:**

- WiFi/Zigbee coexistence issues (both use 2.4GHz)
- Bad antenna connection or damaged antenna trace
- Interference from nearby 2.4GHz devices
- Cold start timing issues with the radio

**Workarounds:**

- Power cycle the device
- Check antenna connections
- Move away from interference sources (microwaves, other 2.4GHz devices)
- If it happens consistently, may need to replace the board

### Zigbee: Device Shows "No Blind Found"

If the device paired but commands fail with "No blind device found":

1. **Check device type:** Run `blinds:status` via MQTT to see what type the device was registered as
2. **Tuya devices:** Must be detected as `TUYA_BLIND`, not `BLIND` or `LIGHT`
3. **Invalid address:** Device might have been stored with address `0xFFFF` (invalid) - run `blinds:reset` and re-pair
4. **Re-pair:** Put device in pairing mode again and wait for it to rejoin

### Zigbee: Commands "Fail" But Actually Work

The Tuya cluster command API often returns `ESP_FAIL` even when the command was successfully sent and executed. The code logs this at DEBUG level, not ERROR. If the blind moves, the command worked regardless of what the return code says.

### Blind Overshoots Physical Limits

If the blind keeps moving after reaching the top/bottom:

- This is a physical device issue, not software
- You need to set the motor limits on the blind itself
- Check the blind's manual for limit setting procedure (usually involves holding buttons in a specific sequence)
- Alternatively, use percentage commands (`blinds:90` instead of `blinds:open`) to stay within safe range

### Linter Errors in VS Code

The ESP-IDF linter in VS Code often shows false positives like:

- `'sys/reent.h' file not found`
- Various "undefined" errors for ESP-IDF types

These are IDE configuration issues, not real errors. If `idf.py build` succeeds, the code is fine.

---

## License

**PolyForm Noncommercial License 1.0.0**

This project is free for personal, educational, and non-commercial use. Commercial use is prohibited without explicit permission.

See [LICENSE](LICENSE) for full terms.
