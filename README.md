# TFT Touch WLED & Media Controller (ESP32 TouchDown v1.1)

**TFT Touch WLED & Media Controller** is an IoT touchscreen controller based on **ESP32 TouchDown v1.1**, enabling control of **WLED** devices and **Windows media playback** over WiFi. The graphical interface runs directly on the integrated TFT display and provides quick control over **RGB LED colors**, **brightness**, and **music playback**.

> Educational project focused on embedded UI, HTTP communication, and integration with smart home ecosystems (WLED) and desktop media control, using an all-in-one board with display and touch capabilities.

---

## Main Features

### WLED Control
- RGB color selection via tactile buttons
- LED brightness control (BRIGHT+ / BRIGHT−)
- Preset color palette (RED, ORANGE, YELLOW, BLUE, VIOLET, PINK, WHITE, etc.)
- Mode switching (Solid / AmbiWLED)
- LED ON/OFF toggle
- HTTP communication with local WLED server

### Media Control
- Previous/Next track navigation
- Play/Pause toggle functionality
- Real-time display of currently playing track information (title, artist, album)
- Album artwork display (128×128px thumbnail)
- Automatic UI refresh on track changes
- Playback position and duration tracking
- Works with any Windows media player (Spotify, YouTube, VLC, etc.)

### User Interface
- 480×320px TFT graphical interface
- Full capacitive touch control (FT6206)
- Multi-menu navigation system (Main Menu → WLED / Music)
- Optimized layout for touch interaction (large buttons, rounded corners)
- Custom transparent bitmap rendering for media control icons
- Software-corrected touch coordinate mapping

### Technical Features
- Fast response time without external applications
- Easy integration into local network (LAN)
- Bidirectional communication (ESP32 ↔ PC)
- RESTful API architecture
- Asynchronous operations for smooth UI performance

---

## Hardware Components

| Component                  | Description                                                                 |
|----------------------------|-----------------------------------------------------------------------------|
| **ESP32 TouchDown v1.1**   | All-in-one ESP32 board with WiFi, TFT, and integrated touchscreen          |
| **TFT 480×320**            | Integrated color display, `TFT_eSPI` compatible                             |
| **FT6206**                 | Capacitive touchscreen controller (I2C), integrated on board               |
| **WLED Server**            | HTTP API-compatible RGB LED controller                                      |
| **Windows PC**             | Runs FastAPI server for media control integration                           |
| **WiFi Network**           | Local communication between ESP32 TouchDown, WLED, and PC                   |

---

## Software and Libraries

### ESP32 Firmware
- **Language:** C++ (Arduino)
- **Platform:** Arduino IDE
- **Libraries:**
  - `TFT_eSPI` – TFT graphics rendering
  - `Adafruit_FT6206` – Capacitive touch reading
  - `WiFi.h` – WiFi network connectivity
  - `HTTPClient.h` – HTTP request handling
  - `ArduinoJson.h` – JSON parsing for media data
  - `TJpg_Decoder.h` – JPEG decoding for album artwork
- **Configuration file:** `secrets.h`  
  (contains WiFi SSID and password)

### Python FastAPI Server
- **Language:** Python 3.8+
- **Framework:** FastAPI
- **Libraries:**
  - `fastapi` – RESTful API server
  - `uvicorn` – ASGI server
  - `winsdk` – Windows Media Transport Controls integration
  - `Pillow (PIL)` – Image processing for thumbnails
- **Features:**
  - Control endpoints (POST): `/control/play`, `/control/pause`, `/control/next`, `/control/previous`, `/control/toggle`
  - Data endpoints (GET): `/media`, `/media/thumbnail`
  - CORS middleware for cross-origin access
  - Async operations for non-blocking performance

---

## System Architecture

### WLED Integration (HTTP API)
The controller sends HTTP requests directly to the WLED instance on the local network:
- RGB color setting
- Global brightness adjustment
- Mode/preset switching
- Power toggle

All commands are sent **locally**, without external cloud services, ensuring minimal latency and complete control.

### Media Control Integration
The system uses a Python FastAPI server running on Windows to interface with the Windows Media Transport Controls API:
```
┌─────────────┐      HTTP GET/POST      ┌──────────────┐      Windows API     ┌─────────────┐
│  ESP32      │ ◄─────────────────────► │  FastAPI     │ ◄──────────────────► │   Media     │
│  TouchDown  │      WiFi (LAN)         │  Server      │                      │   Players   │
└─────────────┘                         └──────────────┘                      └─────────────┘
```

**Data flow:**
1. ESP32 requests current media info via GET `/media`
2. FastAPI queries Windows SMTC and returns JSON with track metadata
3. ESP32 downloads album artwork via GET `/media/thumbnail`
4. User touch triggers POST requests to `/control/*` endpoints
5. FastAPI executes corresponding media control commands

---

## Getting Started

### Prerequisites
- ESP32 TouchDown v1.1 board
- Arduino IDE with ESP32 board support
- Python 3.8+ installed on Windows PC
- WLED controller on local network (optional)
- Required Arduino libraries installed
- Required Python packages installed

### ESP32 Setup

1. **Install Arduino Libraries:**
```
   - TFT_eSPI
   - Adafruit_FT6206
   - ArduinoJson
   - TJpg_Decoder
```

2. **Configure WiFi:**
   Create `secrets.h` file:
```cpp
   #define WIFI_SSID "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"
```

3. **Update Server URLs:**
   In main sketch, update:
```cpp
   const char* serverName = "http://192.168.1.135";        // WLED IP
   const char* mediaServerUrl = "http://192.168.1.136:8000"; // PC IP
```

4. **Upload Sketch:**
   - Connect ESP32 TouchDown via USB
   - Select board: "ESP32 Dev Module"
   - Upload the sketch

### Python Server Setup

1. **Install Dependencies:**
```bash
   pip install fastapi uvicorn winsdk Pillow
```

2. **Run the Server:**
```bash
   python media_server.py
```
   Server will start on `http://0.0.0.0:8000`

3. **Verify Endpoints:**
   - Open browser: `http://localhost:8000/media`
   - Should return current media information (if playing)

---

## User Interface Guide

### Main Menu
- **WLED Button** – Access WLED control panel
- **Music Button** – Access media control panel

### WLED Menu
- **Color Buttons** – Select preset RGB colors
- **BRIGHT+/BRIGHT−** – Adjust LED brightness (±25 steps)
- **MODE** – Toggle between Solid and AmbiWLED presets
- **ON/OFF** – Toggle LED power state
- **BACK** – Return to main menu

### Music Menu
- **Album Artwork** – 128×128px thumbnail (left side)
- **Track Info** – Title and artist display
- **Previous (⏮)** – Skip to previous track
- **Play/Pause (⏯)** – Toggle playback
- **Next (⏭)** – Skip to next track
- **Auto-refresh** – Updates every 1 second when active

---

## Configuration

### TFT_eSPI Configuration
Ensure `User_Setup.h` matches ESP32 TouchDown v1.1 pinout:
```cpp
#define ILI9488_DRIVER
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
```

### Touch Coordinate Mapping
The sketch includes software correction for proper touch mapping:
```cpp
uint16_t x = map(p.y, 0, 480, 0, tft.width());
uint16_t y = map(p.x, 0, 320, tft.height(), 0);
```

---

## Future Development

-  Dynamic RGB/HSV color picker
-  Save and recall color presets
-  WLED Sound Reactive integration
-  MQTT control protocol
-  Home Assistant integration
-  Volume control for media playback
-  Seek bar for track position
-  Radio/streaming service logos
-  Notification system
-  Auto brightness based on time of day

---

## Screenshots

*Coming soon*

---

## Troubleshooting

### ESP32 not connecting to WiFi
- Verify `secrets.h` credentials
- Check WiFi signal strength
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)

### WLED commands not working
- Verify WLED IP address in `serverName`
- Test WLED API manually: `http://WLED_IP/win&T=2`
- Check firewall settings

### Media control not responding
- Ensure FastAPI server is running on PC
- Verify PC IP address in `mediaServerUrl`
- Check that media is playing on Windows
- Test endpoint manually: `http://PC_IP:8000/media`

### Album artwork not displaying
- Ensure `TJpg_Decoder` library is installed
- Check server logs for thumbnail errors
- Verify media player provides artwork via SMTC

---

## License

This project is open source and available under the [MIT License](LICENSE).

---

## Author

**Radu Gabriel Claudiu**
- Software architecture
- TFT UI design
- Touch logic implementation
- HTTP communication
- WLED integration
- FastAPI media server development
- Windows Media Transport Controls integration

---

## Acknowledgments

- **TFT_eSPI** library by Bodmer
- **Adafruit** for FT6206 library
- **WLED** project by Aircoookie
- **FastAPI** framework by Sebastián Ramírez
- ESP32 TouchDown hardware design

---

## Support

For issues, questions, or contributions, please open an issue on the GitHub repository.

---

**Made with ❤️ for the maker community**
