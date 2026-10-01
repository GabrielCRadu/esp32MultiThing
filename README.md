# ESP32 MultiThing – touchscreen hub for WLED, media, sim racing and more

**ESP32 MultiThing** turns an **ESP32 TouchDown** (480×320 capacitive touch TFT) into a small desk hub: it controls **WLED** lights, shows and controls **Windows media playback**, displays **sim-racing telemetry**, **PC stats** and the **weather**, and can even act as a tiny **secondary Windows monitor** over USB.

Everything runs on your LAN. A Python server on the Windows PC bridges the things the ESP32 cannot reach on its own (media sessions, volume, PC sensors, game telemetry).

> Hobby project focused on embedded UI, HTTP/UDP communication and integration with WLED and the Windows desktop.

---

## Apps

The home screen shows a large clock, the date and a weather widget. **APPS** opens the App Drawer with these apps:

| App | What it does |
|-----|--------------|
| **WLED** | Preset colours, brightness, on/off. Modes: **Solid**, **AmbiWLED** and **Music Cover** (the LEDs follow the colours of the current album art). |
| **Music** | Album art as a spinning vinyl, scrolling title/artist, progress, previous / play-pause / next. Swipe along the top bar to change the Windows volume. Works with any player that shows up in Windows media controls (Spotify, browsers, VLC, ...). |
| **Racing** | Live dashboard for **Assetto Corsa**, **Forza Horizon (FH6)** and **BeamNG.drive**: RPM, gear, speed, pedals, lap times, plus a telemetry tab (tyre temperatures, boost). Optional **WLED rev lights**. |
| **USB Display** | The TFT becomes a 480×320 secondary Windows monitor over the USB cable, with touch working as the mouse. See [tools/usb_display/README.md](tools/usb_display/README.md). |
| **PC Stats** | CPU, GPU, RAM and disk usage, plus temperatures when available, refreshed every 2 s. |
| **Settings** | Time zone (UTC offset) and NTP time sync. |
| **Weather** | Current conditions from [Open-Meteo](https://open-meteo.com/) plus an optional local BME280 sensor, with a history graph. Read directly by the ESP32, no PC needed. |

---

## Repository layout

```
Arduino/esp32MultiThing/   ESP32 sketch (Arduino IDE): UI, touch, HTTP/UDP clients
Arduino/Resources/         Source PNGs of the media control icons
Python/music.py            FastAPI server on the PC (port 8000): media, volume, stats, game telemetry
Python/ac_relay.py         UDP relay for Assetto Corsa telemetry (AC only listens on localhost)
Python/chrome_extension/   Optional browser extension: exact playback position for web players
Python/stats.py            Optional standalone stats/FPS server (port 8001, uses PresentMon)
tools/usb_display/         PC side of the USB Display app
```

---

## Architecture

```
                         WiFi (LAN)
 ESP32 TouchDown  ---- HTTP -------> WLED controller
       |          ---- HTTP -------> Python/music.py (PC, :8000)
       |                               |-- Windows media controls (SMTC), volume (pycaw)
       |                               |-- CPU / GPU / RAM / disk stats
       |                               '-- Forza (UDP 5300) and BeamNG (UDP 4445) telemetry
       |          ---- UDP --------> Python/ac_relay.py (PC, :9997) --> Assetto Corsa (127.0.0.1:9996)
       |          ---- HTTPS ------> Open-Meteo,  HTTP --> BME280 sensor
       '---- USB serial -----------> tools/usb_display/usb_display.py (secondary monitor)
```

Every network request has a short timeout (1 s to connect on the LAN), so an offline PC, sensor or WLED does not freeze the touch UI.

---

## Hardware

| Component | Notes |
|-----------|-------|
| **ESP32 TouchDown** | ESP32 with an ILI9488 480×320 SPI TFT, FT6206 capacitive touch (I2C) and a CP2102 USB-serial bridge |
| **WLED controller** | Optional, any WLED device on the LAN |
| **Windows 10/11 PC** | Runs `music.py`; needed for Music, PC Stats, Racing and USB Display |
| **BME280 sensor** | Optional, a device that serves `/api/json` with temperature, humidity and pressure |

---

## Getting started

### 1. Firmware (ESP32)

1. Install the **ESP32** board package in the Arduino IDE and these libraries:
   `TFT_eSPI`, `Adafruit_FT6206`, `ArduinoJson`, `TJpg_Decoder`.
2. Configure `TFT_eSPI` for the board (in its `User_Setup_Select.h` / a custom setup file):
   ```cpp
   #define ILI9488_DRIVER
   #define TFT_MISO 19
   #define TFT_MOSI 23
   #define TFT_SCLK 18
   #define TFT_CS   15
   #define TFT_DC    2
   #define TFT_RST   4
   #define TFT_BL   32
   #define SPI_FREQUENCY 27000000
   ```
3. Create `Arduino/esp32MultiThing/secrets.h` (it is git-ignored):
   ```cpp
   #define WIFI_SSID     "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"
   ```
4. Set the addresses of your devices at the top of `esp32MultiThing.ino`:
   ```cpp
   const char* serverName     = "http://192.168.1.135";       // WLED
   const char* mediaServerUrl = "http://192.168.1.136:8000";  // PC running music.py
   const char* acServerIp     = "192.168.1.136";              // PC running ac_relay.py
   ```
   The BME280 address and the Open-Meteo coordinates are in `fetchWeatherData()`.
5. Board **ESP32 Dev Module**, default partition scheme, then upload. With the CLI:
   ```bash
   arduino-cli compile -b esp32:esp32:esp32 Arduino/esp32MultiThing/
   arduino-cli upload  -b esp32:esp32:esp32 -p COM3 Arduino/esp32MultiThing/
   ```

### 2. PC server (Windows)

```bash
cd Python
pip install fastapi uvicorn winsdk Pillow pycaw psutil
pip install pystray pynvml        # optional: tray icon, NVIDIA GPU stats
python music.py
```

The server listens on `http://0.0.0.0:8000`. `start.vbs` starts it hidden, with a tray icon. Main endpoints:

| Endpoint | Use |
|----------|-----|
| `GET /media`, `/media/thumbnail`, `/media/colors` | Track info, album art, dominant colours |
| `POST /control/play`, `/pause`, `/next`, `/previous`, `/toggle` | Playback control |
| `GET /volume`, `POST /volume/up`, `/volume/down` | Windows master volume |
| `GET /stats` | CPU / GPU / RAM / disk |
| `GET /forza`, `/beamng` | Latest game telemetry |

Allow Python through the Windows firewall on private networks so the ESP32 can reach it.

### 3. Optional extras

- **Assetto Corsa:** run `python ac_relay.py` on the gaming PC next to `music.py`.
- **Forza / BeamNG:** point the game's UDP telemetry output to the PC (`127.0.0.1`, ports 5300 / 4445).
- **Browser players:** load `Python/chrome_extension` as an unpacked extension for an exact playback position.
- **USB Display:** follow [tools/usb_display/README.md](tools/usb_display/README.md) (virtual monitor driver + `usb_display.py`).

---

## Troubleshooting

- **ESP32 not connecting to WiFi:** check `secrets.h`; the ESP32 only supports 2.4 GHz networks.
- **WLED commands do nothing:** check `serverName`; test `http://WLED_IP/win&T=2` in a browser.
- **Music / PC Stats empty:** make sure `music.py` runs, `mediaServerUrl` points to the PC and the firewall allows port 8000. Test `http://PC_IP:8000/media`.
- **Touch feels slow:** an offline device can still cost up to 1 s per request; remove or fix addresses you do not use.
- **Album art missing:** the player must publish artwork to Windows media controls.

---

## License

This project is open source and available under the [MIT License](LICENSE).

---

## Author

**Radu Gabriel Claudiu**

---

## Acknowledgments

- **TFT_eSPI** and **TJpg_Decoder** libraries by Bodmer
- **Adafruit** for the FT6206 library
- **WLED** project by Aircoookie
- **FastAPI** framework by Sebastián Ramírez
- **Open-Meteo** for the free weather API
- ESP32 TouchDown hardware design

---

**Made with ❤️ for the maker community**
