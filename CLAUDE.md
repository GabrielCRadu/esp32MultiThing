# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A multimedia remote control system with two components:
- **Arduino (ESP32)**: Touchscreen UI controller with TFT display
- **Python (Windows)**: FastAPI backend that bridges Windows System Media Transport Controls (SMTC) to the ESP32

The ESP32 communicates over WiFi to control WLED smart lights (`192.168.1.135`) and query/control media playback via the Python server (`192.168.1.136:8000`).

## Build & Flash (Arduino)

The Arduino component uses Arduino IDE — no CMake or Makefile.

**With Arduino CLI:**
```bash
# Compile
arduino-cli compile -b esp32:esp32:esp32 Arduino/esp32MultiThing/

# Upload (replace /dev/ttyUSB0 with actual port)
arduino-cli upload -b esp32:esp32:esp32 -p /dev/ttyUSB0 Arduino/esp32MultiThing/
```

**Required Arduino libraries:**
- TFT_eSPI
- Adafruit_FT6206
- ArduinoJson
- TJpg_Decoder

## Run Python Backend

```bash
cd Python
pip install fastapi uvicorn pillow winsdk pycaw

python music.py
```

Server runs on `http://0.0.0.0:8000`. Requires Windows 10/11.

**Test endpoints manually:**
```bash
curl http://192.168.1.136:8000/media
curl http://192.168.1.136:8000/media/colors
curl http://192.168.1.136:8000/media/thumbnail --output art.jpg
```

## Architecture

### Menu System (ESP32)
Three menus managed by a `currentMenu` integer:
- **Menu 0**: Main screen — buttons to enter WLED or Music menus
- **Menu 1**: WLED control — 10 color presets, brightness +/-, on/off toggle, solid/AmbiWLED mode switch
- **Menu 2**: Music player — 128×128 album art, scrolling title/artist, progress bar, transport controls

### Data Flow
```
ESP32 (TFT + FT6206 touch)
  ├─ HTTP GET → WLED JSON API (light control)
  └─ HTTP GET/POST → Python FastAPI (media info + control)
                          └─ Windows SMTC API → active media player
```

### Python Backend Design
`music.py` implements position estimation for unreliable SMTC timelines (especially Chrome/YouTube). It maintains `base_position + timestamp + playback_rate` and re-syncs when the reported position jumps ≥1 second. Volume control uses pycaw to set Windows master volume via COM.

Color extraction pipeline: saturated pixels from album art → filter by saturation >0.2 and value >0.1 → deduplicate (RGB distance >80) → adjust brightness target to 0.35 → return two hex colors.

### Display
- 480×320 TFT, SPI, rotation=1
- FT6206 capacitive touchscreen over I2C (pin 40)
- Album art: JPEG downloaded and decoded via TJpg_Decoder, rendered at 128×128
- Backgrounds: vertical/horizontal gradients derived from album art dominant colors (`drawVerticalGradient`, `drawHorizontalGradient` with dithering)
- Icons (`images.h`): RGB565 bitmaps, 50×50px for transport buttons

## Key Files

| File | Role |
|------|------|
| [Arduino/esp32MultiThing/esp32MultiThing.ino](Arduino/esp32MultiThing/esp32MultiThing.ino) | Main sketch — all UI, HTTP, touch, and display logic |
| [Arduino/esp32MultiThing/secrets.h](Arduino/esp32MultiThing/secrets.h) | WiFi SSID and password |
| [Arduino/esp32MultiThing/images.h](Arduino/esp32MultiThing/images.h) | Bitmap data for previous/pause/next icons |
| [Arduino/esp32MultiThing/Free_Fonts.h](Arduino/esp32MultiThing/Free_Fonts.h) | TFT_eSPI font macros |
| [Python/music.py](Python/music.py) | FastAPI server with SMTC position estimation + Windows volume control |

## Network Configuration

Hardcoded addresses in `esp32MultiThing.ino` and `secrets.h`:
- WiFi credentials: `secrets.h`
- WLED: `http://192.168.1.135`
- Media server: `http://192.168.1.136:8000`
