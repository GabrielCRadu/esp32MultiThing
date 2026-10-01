#include <TFT_eSPI.h>
#include <Adafruit_FT6206.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <time.h>
#include "secrets.h"
#include "images.h"
#include "ACUDP.h"

TFT_eSPI tft = TFT_eSPI();
Adafruit_FT6206 ctp = Adafruit_FT6206();

// --- Server addresses ---
const char* serverName     = "http://192.168.1.135";
const char* mediaServerUrl = "http://192.168.1.136:8000";
// TCP connect limit for LAN hosts (HTTPClient default is 5 s, setTimeout() does not
// cover it): an offline host would otherwise freeze the loop, touch included.
#define LAN_CONNECT_MS 1000

// --- NTP ---
const char* ntpServer = "pool.ntp.org";
int gmtOffsetHours    = 3;   // Adjust in Settings (Romania: 2 winter / 3 summer)

// --- Display constants ---
#define TFT_BACKGROUND TFT_BLACK
#define MAX_BRIGHTNESS 255

// --- Theme colours (macros so they evaluate at call-time, never in global init) ---
#define CLR_BG       tft.color565( 15,  15,  25)
#define CLR_CARD     tft.color565( 32,  32,  52)
#define CLR_DIVIDER  tft.color565( 55,  55,  80)
#define CLR_WLED     tft.color565(255, 120,  30)
#define CLR_MUSIC    tft.color565(130,  80, 255)
#define CLR_SETTINGS tft.color565(  0, 170, 150)
#define CLR_HDR      tft.color565( 20,  20,  36)
#define CLR_DIM      tft.color565( 45,  45,  65)
#define CLR_LABEL    tft.color565(140, 140, 175)
#define CLR_RACING   tft.color565(220,  45,  45)
#define CLR_PCSTATS  tft.color565( 40, 130, 230)
#define CLR_WEATHER  tft.color565( 30, 170, 220)
#define CLR_USBDISP  tft.color565(120, 200,  60)

#define WEATHER_FETCH_MS  60000UL
#define WX_HIST_LEN       60

// Home screen weather widget bounding box (right half)
#define WX_W_X  244
#define WX_W_Y   10
#define WX_W_W  226
#define WX_W_H  182

// --- Main menu card geometry (kept for App Drawer reuse) ---
#define CARD_Y   188
#define CARD_H   126
#define CARD_W   105
#define CARD1_X   12
#define CARD2_X  129
#define CARD3_X  246
#define CARD4_X  363

// --- App Drawer geometry (4 cols x 2 rows inside 480x266 content area) ---
#define AD_COL1_X    8
#define AD_COL2_X  126
#define AD_COL3_X  244
#define AD_COL4_X  362
#define AD_CARD_W  110
#define AD_ROW1_Y   60
#define AD_ROW2_Y  190
#define AD_CARD_H  118
// --- Home screen APPS button ---
#define APPS_BTN_X   70
#define APPS_BTN_Y  215
#define APPS_BTN_W  340
#define APPS_BTN_H   70

// --- WLED picker bar geometry ---
#define WB_X      25
#define WB_W     430
#define WB_HUE_Y 108
#define WB_HUE_H  50
#define WB_SAT_Y 165
#define WB_SAT_H  40
#define WB_BRI_Y 212
#define WB_BRI_H  40
#define WB_PRV_Y 262
#define WB_PRV_H  50

// --- USB Display (menu 8): serial frame link to tools/usb_display/usb_display.py ---
#define USBD_BAUD            921600UL  // must match --baud of usb_display.py
#define USBD_PROTOCOL        3         // sent in READY; 2 = "FRM2" tiles, 3 = "FRM3" native
#define USBD_RX_BUF          4096      // UART RX ring buffer while the app is open
#define USBD_LOG_BAUD        115200UL  // Serial as set up in setup(), restored on exit
#define USBD_LOG_RX_BUF      256       // HardwareSerial default RX buffer
#define USBD_FRAME_MAX       (48UL * 1024UL)   // largest JPEG accepted (internal RAM)
#define USBD_FRAME_MAX_PSRAM (192UL * 1024UL)  // largest JPEG accepted with PSRAM
#define USBD_FRAME_MIN       (12UL * 1024UL)   // smallest buffer worth allocating
#define USBD_HEAP_RESERVE    (32UL * 1024UL)   // internal heap kept free for WiFi
#define USBD_RX_TIMEOUT_MS   500UL   // frame whose bytes stop arriving is dropped
#define USBD_LINK_TIMEOUT_MS 3000UL  // PC silent this long -> waiting screen
#define USBD_READY_MS        1000UL  // READY repeat period on the waiting screen
#define USBD_TOUCH_POLL_MS   20UL    // touch sampling period (also during decode)
#define USBD_RELEASE_MS      40UL    // release debounce (FT6206 drops single samples)
#define USBD_MOVE_MIN_PX     2       // movement needed before a new M event
#define USBD_EXIT_ZONE       56      // top-left square used by the exit gesture
#define USBD_EXIT_HOLD_MS    2000UL  // hold time that arms the exit
#define USBD_HINT_DELAY_MS   500UL   // exit progress box shows up after this
#define USBD_HINT_W          172     // exit progress box area (top-left)
#define USBD_HINT_H          48
// Touch modes
#define USBD_T_IDLE     0
#define USBD_T_FORWARD  1   // touch is being sent to the PC
#define USBD_T_CORNER   2   // held back: may turn into the exit gesture
#define USBD_T_BACK     3   // "< Inapoi" on the waiting screen
#define USBD_T_IGNORE   4   // swallowed until the finger lifts

// --- Structs ---
struct Button {
  int x, y, w, h, radius;
  uint16_t color;
  const char* label;
};

struct WeatherData {
  float sensorTemp, sensorHum, sensorPres, sensorAlt;
  bool  sensorOk;
  float outdoorTemp, outdoorFeels, outdoorWind;
  int   outdoorHum, weatherCode;
  bool  outdoorOk;
};

struct MediaInfo {
  bool    playing;
  String  title;
  String  artist;
  String  album;
  int     duration;
  int     position;
  String  status;
};

struct DominantColors {
  uint8_t r1, g1, b1;
  uint8_t r2, g2, b2;
};

struct PCStats {
  float cpuUsage    = 0;
  float cpuTemp     = 0;
  float gpuUsage    = 0;
  float gpuTemp     = 0;
  float ramUsedMB   = 0;
  float ramTotalMB  = 0;
  float diskUsedGB  = 0;
  float diskTotalGB = 0;
  bool  hasCpuTemp  = false;
  bool  hasGpu      = false;
  bool  hasGpuTemp  = false;
};

struct ForzaData {
  int      rpm      = 0;
  int      rpmMax   = 8000;
  uint8_t  gear     = 0;        // FH6: 0=N, 1=1st, 2=2nd, ..., ≥10=R
  float    speedKmh = 0.0f;
  uint8_t  accel    = 0;        // 0–255
  uint8_t  brake    = 0;        // 0–255
  float    boost    = 0.0f;     // bar (positive = boost)
  float    fuel     = 0.0f;     // 0.0–1.0
  float    tireTemp[4] = {0, 0, 0, 0};  // FL,FR,RL,RR in °C
  uint16_t lap      = 0;
  float    currentLap = 0.0f;   // seconds
  float    bestLap    = 0.0f;   // seconds
  float    lastLap    = 0.0f;   // seconds
};

// --- Global state ---
uint8_t brightness = MAX_BRIGHTNESS;
uint8_t menu       = 0;
int     ledMode    = 0;

// WLED HSV picker state
int     wledHue      = 0;    // 0-359 degrees
uint8_t wledSat      = 255;  // 0-255
// Exact pixel columns where each indicator was last drawn.
// Stored so erase is pixel-perfect (avoids 1 px ghost from integer rounding).
int wledHueCol = 0;
int wledSatCol = 0;
int wledBriCol = 0;

// Vinyl / album art rotation
// albumBuf holds the raw 128×128 pixels decoded from the JPEG (32 KB).
uint16_t      albumBuf[16384];
bool          albumLoaded    = false;
float         vinylAngle     = 0.0f;
unsigned long lastVinylUpdate = 0;

// Release-to-send tracking (avoids blocking HTTP during drag)
bool wasTouching   = false;
bool wledNeedsSync = false;

// Server availability — exponential backoff when Python server is unreachable
bool          serverAvail     = true;
unsigned long serverFailAt    = 0;
unsigned long serverBackoffMs = 500;

// Music menu — volume swipe state
int16_t       musicSwipeStartX  = -1;     // X at first touch on top bar; -1=idle
int16_t       musicSwipeLastX   = -1;     // X during swipe
int8_t        musicSwipeDir     = 0;      // 0=undecided, 1=up(right), -1=down(left)
unsigned long musicSwipeRepeatAt = 0;     // millis() when to send next repeat command
float         musicVolLevel     = -1.0f;  // last known volume (0.0–1.0)
unsigned long musicVolShowUntil = 0;      // hide indicator after this millis()

// Music Cover WLED mode state
unsigned long lastMusicCoverPoll    = 0;
unsigned long musicCoverStoppedAt   = 0; // millis() when music last stopped; 0 = playing
bool          musicCoverFallbackSent = false;
String        lastMusicCoverTitle   = "";

// Media server availability
bool mediaServerAvailable     = false;
bool prevMediaServerAvailable = false;
bool prevMediaPlaying         = false;
bool prevIsActuallyPlaying    = false;

// --- Settings buttons (colours are constants, safe in global init) ---
Button tzMinusBtn = { 105, 158,  70, 55, 10, 0x39C7 /* ~(55,55,80) */, "-" };
Button tzPlusBtn  = { 305, 158,  70, 55, 10, 0x39C7,                    "+" };
Button syncBtn    = { 170, 238, 140, 52, 10, 0x0AA9 /* ~(0,170,150) */, "Sync NTP" };

// --- Music transport hit-zones (label/color not used, so zero-init is fine) ---
Button previousButton = { 123, 245, 50, 50, 0, 0, nullptr };
Button pauseButton    = { 215, 245, 50, 50, 0, 0, nullptr };
Button nextButton     = { 307, 245, 50, 50, 0, 0, nullptr };

// --- Weather menu ---
WeatherData   wxData;
bool          wxHasData       = false;
unsigned long lastWxFetch     = 0;
uint8_t       wxTab           = 0;          // 0=date curente, 1=grafic
float         wxSensorHist[WX_HIST_LEN];   // indoor temp history (circular)
float         wxOutdoorHist[WX_HIST_LEN];  // outdoor temp history (circular)
int           wxHistHead      = 0;          // next write index
int           wxHistCount     = 0;          // valid readings (0..WX_HIST_LEN)

// --- Assetto Corsa UDP ---
ACUDP            acTelemetry;
bool             acConnected    = false;
const char*      acServerIp     = "192.168.1.136";

RTCarInfo        acData;
bool             acHasData      = false;
unsigned long    acLastPacket   = 0;
unsigned long    acLastConnect  = 0;
unsigned long    acLastSubscribe = 0;

// --- Forza Horizon 6 telemetry (HTTP-polled from Python server) ---
ForzaData        forzaData;
bool             forzaHasData        = false;
bool             forzaGameRunning    = false;  // true when packets received, regardless of is_race_on
bool             prevForzaGameRunning = false;
unsigned long    lastForzaFetch      = 0;
const unsigned long FORZA_FETCH_MS   = 100;

// --- BeamNG telemetry (HTTP-polled from Python /beamng endpoint) ---
ForzaData        beamngData;
bool             beamngHasData        = false;
bool             beamngGameRunning    = false;
bool             prevBeamngGameRunning = false;
unsigned long    lastBeamngFetch      = 0;

// --- Racing UI ---
uint8_t  racingSubMenu      = 0;   // 0=RPM, 1=Telemetry, 2=Setup
uint8_t  racingGame         = 0;   // 0=AC, 1=FH6, 2=BeamNG stub
int      acMaxRpm           = 8000;
int      acShiftPct         = 85;
unsigned long lastRacingRefresh = 0;
const unsigned long RACING_REFRESH_MS = 100;

// Incremental-refresh sentinels
bool     prevAcHasData   = false;  // used to detect first data arrival → force full redraw
float    prevEngineRPM   = -1.0f;
float    prevGas         = -1.0f;
float    prevBrake       = -1.0f;
float    prevClutch      = -1.0f;
int      prevGear        = -99;
float    prevSpeed       = -1.0f;
int      prevLapTime     = -1;
bool     prevShiftFlash  = false;
float    prevTyreSlip[4] = {-99.0f, -99.0f, -99.0f, -99.0f};
float    prevDirty[4]    = {-99.0f, -99.0f, -99.0f, -99.0f};
float    prevAccFrontal  = -99.0f;
float    prevAccHoriz    = -99.0f;
bool     prevAbsAction   = false;
bool     prevTcAction    = false;
// Forza-specific sentinels
float    prevForzaBoost       = -99.0f;
float    prevForzaTireTemp[4] = {-999.0f, -999.0f, -999.0f, -999.0f};

// --- Racing settings buttons ---
Button rpmMinusBtn    = { 130, 160,  60, 40, 8, 0x39C7, "-" };
Button rpmPlusBtn     = { 290, 160,  60, 40, 8, 0x39C7, "+" };
Button shiftMinusBtn  = { 130, 210,  60, 40, 8, 0x39C7, "-" };
Button shiftPlusBtn   = { 290, 210,  60, 40, 8, 0x39C7, "+" };
Button acReconnectBtn = { 250, 278, 205, 36, 8, 0x0AA9, "RECONNECT" };
Button revLightsBtn   = {  25, 278, 200, 36, 8, 0x39C7, "REV LIGHTS" };

// WLED Rev Lights (Racing mode)
bool          wledRevLightsEnabled = false;
unsigned long lastRevLightUpdate   = 0;
uint8_t       prevRevR = 0, prevRevG = 0, prevRevB = 0;
bool          prevRevFlash = false;
bool          revLightsOn  = false;  // current flash state

// --- Media state ---
MediaInfo      mediaData;
DominantColors dominantColors = { 49, 59, 61, 33, 44, 47 };
PCStats        pcStats;
unsigned long  lastPCStatsUpdate          = 0;
const unsigned long PC_STATS_INTERVAL_MS  = 2000;

String        oldTitle;
String        oldArtist;
String        prevTimeStr;
String        prevDateStr;
int           titleCharOffset     = 0;
int           artistCharOffset    = 0;
unsigned long lastTitleScrollTime  = 0;
unsigned long lastArtistScrollTime = 0;
unsigned long lastPositionUpdate   = 0;
int           localPosition        = 0;
const int     scrollDelay          = 500;
const int     textDisplayWidth     = 300;
bool          titleNeedsScroll     = false;
bool          artistNeedsScroll    = false;

// --- USB Display (menu 8) ---
// Buffers exist only while the app is open (allocated on enter, freed on exit).
uint8_t*      usbdBuf[2]         = { nullptr, nullptr };  // JPEG frame buffers
uint8_t       usbdBufCount       = 0;      // 2 = next frame streams in during decode
uint32_t      usbdFrameMax       = 0;      // buffer size = largest JPEG accepted
bool          usbdBufInPsram     = false;
uint8_t*      usbdRow            = nullptr; // native pixels of one row / one JPEG block
// Receiver
uint8_t       usbdRxState        = 0;      // 0=magic 1=length 2=SOI 3=payload 4=position
uint8_t       usbdRxPos          = 0;      // bytes matched in the current state
uint32_t      usbdRxLen          = 0;
uint32_t      usbdRxGot          = 0;
uint8_t       usbdRxKind         = 1;      // 1 = JPEG frame, 2 = JPEG tile, 3 = native rect
uint16_t      usbdRxRect[4];               // x, y (FRM2/FRM3) and w, h (FRM3)
uint8_t       usbdRxIdx          = 0;      // buffer being filled
bool          usbdRxBlocked      = false;  // no free buffer; ACK once a decode ends
int8_t        usbdPendingIdx     = -1;     // complete frame waiting for decode
uint32_t      usbdPendingLen     = 0;
uint8_t       usbdPendingKind    = 1;
uint16_t      usbdPendingRect[4];
int8_t        usbdDecIdx         = -1;     // buffer being decoded
unsigned long usbdLastRxAt       = 0;      // last byte of any kind
unsigned long usbdLastMsgAt      = 0;      // last valid frame header or ping
// Screen
bool          usbdShowingFrame   = false;  // false = waiting screen is visible
uint16_t      usbdFrameW         = 0;
uint16_t      usbdFrameH         = 0;
unsigned long usbdLastReadyAt    = 0;
unsigned long usbdLastSpinAt     = 0;
uint8_t       usbdSpinStep       = 0;
uint8_t       usbdStatusShown    = 255;    // waiting-screen status line on screen
bool          usbdHintShown      = false;  // exit progress box drawn over the frame
bool          usbdHintArmedShown = false;
int           usbdHintBarW       = 0;
// Touch forwarding + exit gesture
uint8_t       usbdTouchMode      = USBD_T_IDLE;
unsigned long usbdLastTouchPoll  = 0;
unsigned long usbdReleaseAt      = 0;      // first empty sample (0 = finger down)
unsigned long usbdTouchStartAt   = 0;
unsigned long usbdTouchLastDown  = 0;
int16_t       usbdTouchX0 = 0, usbdTouchY0 = 0;   // where the touch started
int16_t       usbdTouchX  = 0, usbdTouchY  = 0;   // last position sent / seen
bool          usbdExitArmed      = false;
bool          usbdExitRequested  = false;

// ============================================================
// NTP helpers
// ============================================================
void syncNTP() {
  configTime((long)gmtOffsetHours * 3600L, 0, ntpServer);
}

// getLocalTime() waits up to 5 s by default while NTP is not synced yet; with 0 it
// returns at once, so the 1 s home refresh cannot freeze the loop (and touch).
String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}

String getDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return "Syncing time...";
  char buf[28];
  strftime(buf, sizeof(buf), "%A, %d %b %Y", &timeinfo);
  return String(buf);
}

// ============================================================
// JPEG decoder callback
// ============================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

// ============================================================
// Server availability helpers — prevent long blocks when server is down
// ============================================================
bool canHttp() {
  if (serverAvail) return true;
  return millis() - serverFailAt >= serverBackoffMs;
}
void onHttpOk() {
  serverAvail     = true;
  serverBackoffMs = 500;
}
void onHttpFail() {
  serverAvail     = false;
  serverFailAt    = millis();
  serverBackoffMs = min(serverBackoffMs * 2, 30000UL);
}

// ============================================================
// HTTP helpers — every call has an explicit timeout so the
// device never blocks indefinitely when a server is offline.
// ============================================================
void sendHTTPRequest(const String& params) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(serverName) + params);
  http.setTimeout(3000);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int code = http.GET();
  if (code > 0) Serial.println(http.getString());
  else { Serial.print("WLED HTTP error: "); Serial.println(code); }
  http.end();
}

void sendHTTPPOSTRequest(const String& url) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(url);
  http.setTimeout(3000);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int code = http.POST("");
  if (code > 0) Serial.println(http.getString());
  else { Serial.print("POST error: "); Serial.println(code); }
  http.end();
}

// POST /volume/up or /volume/down; returns new level (0.0–1.0) or -1 on error
float sendVolumeRequest(bool up) {
  if (WiFi.status() != WL_CONNECTED || !canHttp()) return -1.0f;
  HTTPClient http;
  http.begin(String(mediaServerUrl) + (up ? "/volume/up" : "/volume/down"));
  http.setTimeout(600);
  http.setConnectTimeout(LAN_CONNECT_MS);
  float newVol = -1.0f;
  int code = http.POST("");
  if (code == 200) {
    onHttpOk();
    StaticJsonDocument<64> doc;
    if (!deserializeJson(doc, http.getString()))
      newVol = doc["level"] | -1.0f;
  } else {
    onHttpFail();
  }
  http.end();
  return newVol;
}

void sendWLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  sendHTTPRequest("/win&R=" + String(r) + "&G=" + String(g) + "&B=" + String(b));
}
void sendWLEDBrightness(uint8_t b) { sendHTTPRequest("/win&A=" + String(b)); }
void toggleLEDState()              { sendHTTPRequest("/win&T=2"); }
void updateLedMode(int mode)       { sendHTTPRequest("/win&PL=" + String(mode + 1)); }

// ============================================================
// AC UDP connection
// ============================================================

// Helper: reset all incremental-refresh sentinels
static void resetRacingSentinels() {
  prevAcHasData = false;
  prevEngineRPM = -1.0f; prevGas = -1.0f; prevBrake = -1.0f;
  prevClutch = -1.0f; prevGear = -99; prevSpeed = -1.0f;
  prevLapTime = -1; prevShiftFlash = false;
  for (int i = 0; i < 4; i++) { prevTyreSlip[i] = -99.0f; prevDirty[i] = -99.0f; }
  prevAccFrontal = -99.0f; prevAccHoriz = -99.0f;
  prevAbsAction = false; prevTcAction = false;
  prevForzaBoost = -99.0f;
  for (int i = 0; i < 4; i++) prevForzaTireTemp[i] = -999.0f;
}

void acConnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  acTelemetry.begin((char*)acServerIp, 9997, WiFi.localIP()); // relay port (AC binds to loopback only)
  acTelemetry.sendHandshake();
  acTelemetry.sendUpdate();
  acConnected     = true;
  acLastConnect   = millis();
  acLastSubscribe = millis();
}

void acDisconnect() {
  if (!acConnected) return;
  acTelemetry.sendQuit();
  acConnected   = false;
  acHasData     = false;
  acLastConnect = 0;
  resetRacingSentinels();
  if (wledRevLightsEnabled) {
    sendWLEDBrightness(brightness);
  }
}

void acPollUdp() {
  if (!acConnected) return;
  Result res = acTelemetry.read();
  if (res.result && res.carInfo.identifier == 'a') {
    acData       = res.carInfo;
    acHasData    = true;
    acLastPacket = millis();
  }
}

void forzaFetchData() {
  if (WiFi.status() != WL_CONNECTED || !canHttp()) return;
  HTTPClient http;
  http.begin(String(mediaServerUrl) + "/forza");
  http.setTimeout(300);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int code = http.GET();
  if (code == 200) {
    onHttpOk();
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, http.getString())) {
      bool active = doc["active"] | false;
      if (!active) {
        forzaGameRunning = false;
        forzaHasData     = false;
      } else {
        forzaGameRunning = true;
        forzaData.rpm      = doc["rpm"]       | 0;
        forzaData.rpmMax   = doc["rpm_max"]   | 8000;
        forzaData.gear     = (uint8_t)(doc["gear"]  | 0);
        forzaData.speedKmh = doc["speed_kmh"] | 0.0f;
        forzaData.accel    = (uint8_t)(doc["accel"] | 0);
        forzaData.brake    = (uint8_t)(doc["brake"] | 0);
        forzaData.boost    = doc["boost"]     | 0.0f;
        forzaData.fuel     = doc["fuel"]      | 0.0f;
        for (int i = 0; i < 4; i++)
          forzaData.tireTemp[i] = doc["tire_temp"][i] | 0.0f;
        forzaData.lap        = (uint16_t)(doc["lap"] | 0);
        forzaData.currentLap = doc["current_lap"] | 0.0f;
        forzaData.bestLap    = doc["best_lap"]    | 0.0f;
        forzaData.lastLap    = doc["last_lap"]    | 0.0f;
        forzaHasData = true;
      }
    }
    Serial.printf("[FH6] active=%d hasData=%d rpm=%d speed=%.1f\n",
                  forzaGameRunning, forzaHasData, forzaData.rpm, forzaData.speedKmh);
  } else {
    onHttpFail();
    forzaHasData     = false;
    forzaGameRunning = false;
    Serial.printf("[FH6] HTTP error %d\n", code);
  }
  http.end();
}

void beamngFetchData() {
  if (WiFi.status() != WL_CONNECTED || !canHttp()) return;
  HTTPClient http;
  http.begin(String(mediaServerUrl) + "/beamng");
  http.setTimeout(300);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int code = http.GET();
  if (code == 200) {
    onHttpOk();
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, http.getString())) {
      bool active = doc["active"] | false;
      if (!active) {
        beamngGameRunning = false;
        beamngHasData     = false;
      } else {
        beamngGameRunning = true;
        beamngData.rpm      = doc["rpm"]       | 0;
        beamngData.rpmMax   = doc["rpm_max"]   | 8000;
        beamngData.gear     = (uint8_t)(doc["gear"]  | 0);
        beamngData.speedKmh = doc["speed_kmh"] | 0.0f;
        beamngData.accel    = (uint8_t)(doc["accel"] | 0);
        beamngData.brake    = (uint8_t)(doc["brake"] | 0);
        beamngData.boost    = doc["boost"]     | 0.0f;
        beamngData.fuel     = doc["fuel"]      | 0.0f;
        for (int i = 0; i < 4; i++)
          beamngData.tireTemp[i] = doc["tire_temp"][i] | 0.0f;
        beamngData.lap        = (uint16_t)(doc["lap"] | 0);
        beamngData.currentLap = doc["current_lap"] | 0.0f;
        beamngData.bestLap    = doc["best_lap"]    | 0.0f;
        beamngData.lastLap    = doc["last_lap"]    | 0.0f;
        beamngHasData = true;
      }
    }
  } else {
    onHttpFail();
    beamngHasData     = false;
    beamngGameRunning = false;
  }
  http.end();
}

// ============================================================
// Weather data fetch (HTTP → Python /weather → BME280 + Open-Meteo)
// ============================================================
static unsigned long lastOutdoorFetch = 0;
#define OUTDOOR_FETCH_MS  600000UL  // Open-Meteo: refresh every 10 min

void fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();

  // ── BME280 senzor local (HTTP plain, mereu fresh) ──────────
  {
    HTTPClient http;
    http.begin("http://192.168.1.145/api/json");
    http.setTimeout(3000);
    http.setConnectTimeout(LAN_CONNECT_MS);
    int code = http.GET();
    Serial.printf("[WX] BME280 code=%d\n", code);
    if (code == 200) {
      String body = http.getString();
      Serial.printf("[WX] BME280 body: %s\n", body.c_str());
      StaticJsonDocument<192> doc;
      DeserializationError err = deserializeJson(doc, body);
      if (!err) {
        wxData.sensorTemp = doc["temperatura"] | 0.0f;
        wxData.sensorHum  = doc["umiditate"]   | 0.0f;
        wxData.sensorPres = doc["presiune"]    | 0.0f;
        wxData.sensorAlt  = doc["altitudine"]  | 0.0f;
        wxData.sensorOk   = true;
        Serial.printf("[WX] sensor temp=%.1f hum=%.1f pres=%.1f\n",
                      wxData.sensorTemp, wxData.sensorHum, wxData.sensorPres);
      } else {
        Serial.printf("[WX] BME280 JSON err: %s\n", err.c_str());
        wxData.sensorOk = false;
      }
    } else {
      Serial.printf("[WX] BME280 FAIL: %s\n", http.errorToString(code).c_str());
      wxData.sensorOk = false;
    }
    http.end();
  }

  // ── Open-Meteo Timișoara (HTTPS, cached 10 min) ────────────
  if (lastOutdoorFetch == 0 || (now - lastOutdoorFetch) >= OUTDOOR_FETCH_MS) {
    Serial.println("[WX] Fetching Open-Meteo...");
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.begin(client,
      "https://api.open-meteo.com/v1/forecast"
      "?latitude=45.7489&longitude=21.2087"
      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
      "&timezone=auto&wind_speed_unit=kmh");
    https.setTimeout(8000);
    int code = https.GET();
    Serial.printf("[WX] Open-Meteo code=%d\n", code);
    if (code == 200) {
      String body = https.getString();
      Serial.printf("[WX] OM len=%d preview: %.250s\n", body.length(), body.c_str());
      // Filter: only parse "current" object to save RAM
      StaticJsonDocument<64> filter;
      JsonObject fc = filter["current"].to<JsonObject>();
      fc["temperature_2m"]       = true;
      fc["relative_humidity_2m"] = true;
      fc["apparent_temperature"] = true;
      fc["weather_code"]         = true;
      fc["wind_speed_10m"]       = true;
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
      if (!err) {
        JsonObject cur = doc["current"];
        wxData.outdoorTemp   = cur["temperature_2m"]      | 0.0f;
        wxData.outdoorFeels  = cur["apparent_temperature"] | 0.0f;
        wxData.outdoorHum    = cur["relative_humidity_2m"] | 0;
        wxData.outdoorWind   = cur["wind_speed_10m"]       | 0.0f;
        wxData.weatherCode   = cur["weather_code"]         | 0;
        wxData.outdoorOk     = true;
        lastOutdoorFetch     = now;
        Serial.printf("[WX] outdoor temp=%.1f feels=%.1f hum=%d wind=%.1f code=%d\n",
                      wxData.outdoorTemp, wxData.outdoorFeels,
                      wxData.outdoorHum, wxData.outdoorWind, wxData.weatherCode);
      } else {
        Serial.printf("[WX] OM JSON err: %s\n", err.c_str());
        wxData.outdoorOk = false;
      }
    } else {
      Serial.printf("[WX] OM FAIL: %s\n", https.errorToString(code).c_str());
      wxData.outdoorOk = false;
    }
    https.end();
  } else {
    Serial.printf("[WX] outdoor cached (%lus pana la refresh)\n",
                  (OUTDOOR_FETCH_MS - (now - lastOutdoorFetch)) / 1000);
  }

  wxHasData = true;
  wxSensorHist[wxHistHead]  = wxData.sensorTemp;
  wxOutdoorHist[wxHistHead] = wxData.outdoorTemp;
  wxHistHead = (wxHistHead + 1) % WX_HIST_LEN;
  if (wxHistCount < WX_HIST_LEN) wxHistCount++;
}

// ============================================================
// Weather menu — draw helpers
// ============================================================

// Short WMO description (code → ASCII string, no emoji for TFT)
const char* wxCodeDesc(int code) {
  if (code == 0)              return "Cer senin";
  if (code <= 2)              return "Partial noros";
  if (code == 3)              return "Noros";
  if (code <= 48)             return "Ceata";
  if (code <= 55)             return "Burnita";
  if (code <= 65)             return "Ploaie";
  if (code <= 77)             return "Ninsoare";
  if (code <= 82)             return "Averse";
  if (code <= 86)             return "Ninsoare averse";
  if (code <= 99)             return "Furtuna";
  return "—";
}

// Draw one rounded-rect card with a temperature + two sub-values
// cx = center X of card
void _wxDrawCard(int x, int y, int w, int h, uint16_t accent,
                 const char* title, const char* subtitle,
                 float tempVal, bool tempOk,
                 const char* row1Label, float row1Val, const char* row1Unit,
                 const char* row2Label, float row2Val, const char* row2Unit) {
  int cx = x + w / 2;
  tft.fillRoundRect(x, y, w, h, 14, CLR_CARD);
  tft.drawRoundRect(x, y, w, h, 14, accent);

  tft.setTextDatum(MC_DATUM);
  // Title
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(title, cx, y + 18);
  // Subtitle
  tft.setTextColor(CLR_LABEL);
  tft.drawString(subtitle, cx, y + 36);
  // Divider
  tft.drawFastHLine(x + 10, y + 48, w - 20, CLR_DIVIDER);

  if (!tempOk) {
    tft.setTextFont(2);
    tft.setTextColor(CLR_LABEL);
    tft.drawString("Nicio conexiune", cx, y + h / 2);
    return;
  }

  // Temperature — Font 7 integer + Font 4 decimal+unit next to it
  char intBuf[8], decBuf[8];
  int  tempInt = (int)tempVal;
  int  tempDec = abs((int)(tempVal * 10) % 10);
  snprintf(intBuf, sizeof(intBuf), "%d", tempInt);
  snprintf(decBuf, sizeof(decBuf), ".%d C", tempDec);

  tft.setTextFont(7);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MR_DATUM);
  int intW = tft.textWidth(intBuf);
  int numX = cx + intW / 2;
  tft.drawString(intBuf, numX, y + 100);
  tft.setTextFont(4);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(decBuf, numX + 2, y + 85);

  // Row 1
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f %s", row1Val, row1Unit);
  tft.drawString(buf, cx, y + 155);
  tft.setTextFont(2);
  tft.setTextColor(CLR_LABEL);
  tft.drawString(row1Label, cx, y + 172);

  // Row 2
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  snprintf(buf, sizeof(buf), "%.1f %s", row2Val, row2Unit);
  tft.drawString(buf, cx, y + 198);
  tft.setTextFont(2);
  tft.setTextColor(CLR_LABEL);
  tft.drawString(row2Label, cx, y + 215);
}

// Draw the temperature graph (wxTab == 1)
void drawWeatherGraph() {
  tft.fillRect(0, 52, 480, 268, CLR_BG);
  if (wxHistCount < 2) {
    tft.setTextFont(2);
    tft.setTextColor(CLR_LABEL);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Date insuficiente — astept citiri...", 240, 180);
    tft.drawString("(se actualizeaza la 60s)", 240, 200);
    return;
  }

  // Graph frame
  int gx = 42, gy = 60, gw = 428, gh = 210;
  tft.fillRoundRect(gx - 4, gy - 4, gw + 8, gh + 8, 10, CLR_CARD);

  // Find min/max across both series
  float tMin = 999, tMax = -999;
  for (int i = 0; i < wxHistCount; i++) {
    int idx = (wxHistHead - wxHistCount + i + WX_HIST_LEN) % WX_HIST_LEN;
    if (wxSensorHist[idx]  < tMin) tMin = wxSensorHist[idx];
    if (wxSensorHist[idx]  > tMax) tMax = wxSensorHist[idx];
    if (wxOutdoorHist[idx] < tMin) tMin = wxOutdoorHist[idx];
    if (wxOutdoorHist[idx] > tMax) tMax = wxOutdoorHist[idx];
  }
  float margin = max(1.0f, (tMax - tMin) * 0.15f);
  tMin -= margin; tMax += margin;
  float span = tMax - tMin;

  // Horizontal grid lines (3 intervals)
  for (int g = 1; g <= 3; g++) {
    int lineY = gy + gh * g / 4;
    tft.drawFastHLine(gx, lineY, gw, CLR_DIVIDER);
    float labelVal = tMax - span * g / 4;
    char lbuf[8]; snprintf(lbuf, sizeof(lbuf), "%.0f", labelVal);
    tft.setTextFont(2); tft.setTextColor(CLR_LABEL); tft.setTextDatum(MR_DATUM);
    tft.drawString(lbuf, gx - 2, lineY);
  }
  // Y axis labels (top & bottom)
  { char b[8]; snprintf(b, sizeof(b), "%.0f", tMax);
    tft.setTextFont(2); tft.setTextColor(CLR_LABEL); tft.setTextDatum(MR_DATUM);
    tft.drawString(b, gx - 2, gy); }
  { char b[8]; snprintf(b, sizeof(b), "%.0f", tMin);
    tft.setTextFont(2); tft.setTextColor(CLR_LABEL); tft.setTextDatum(MR_DATUM);
    tft.drawString(b, gx - 2, gy + gh); }

  // Draw lines
  int n = wxHistCount;
  uint16_t colSensor  = tft.color565( 80, 190, 255);  // blue = indoor
  uint16_t colOutdoor = tft.color565(255, 150,  50);  // orange = outdoor
  for (int i = 1; i < n; i++) {
    int idx0 = (wxHistHead - n + i - 1 + WX_HIST_LEN) % WX_HIST_LEN;
    int idx1 = (wxHistHead - n + i     + WX_HIST_LEN) % WX_HIST_LEN;
    int x0 = gx + (i - 1) * gw / (n - 1);
    int x1 = gx +  i      * gw / (n - 1);
    int ys0 = gy + gh - (int)((wxSensorHist[idx0]  - tMin) * gh / span);
    int ys1 = gy + gh - (int)((wxSensorHist[idx1]  - tMin) * gh / span);
    int yo0 = gy + gh - (int)((wxOutdoorHist[idx0] - tMin) * gh / span);
    int yo1 = gy + gh - (int)((wxOutdoorHist[idx1] - tMin) * gh / span);
    tft.drawLine(x0, ys0, x1, ys1, colSensor);
    tft.drawLine(x0, yo0, x1, yo1, colOutdoor);
  }

  // Legend
  tft.fillRect(gx + gw - 120, gy + 4, 10, 4, colSensor);
  tft.setTextFont(2); tft.setTextColor(colSensor); tft.setTextDatum(ML_DATUM);
  tft.drawString("Senzor", gx + gw - 107, gy + 6);
  tft.fillRect(gx + gw - 120, gy + 18, 10, 4, colOutdoor);
  tft.setTextColor(colOutdoor);
  tft.drawString("Timisoara", gx + gw - 107, gy + 20);

  // X axis label
  tft.setTextFont(2); tft.setTextColor(CLR_LABEL); tft.setTextDatum(MC_DATUM);
  char xbuf[32];
  snprintf(xbuf, sizeof(xbuf), "ultimele %d min", wxHistCount);
  tft.drawString(xbuf, gx + gw / 2, gy + gh + 16);
}

// Draw tab bar buttons inside the header
void drawWeatherTabs() {
  // [DATE]
  uint16_t c0 = (wxTab == 0) ? CLR_WEATHER : CLR_DIM;
  tft.fillRoundRect(320, 8, 68, 28, 8, c0);
  tft.setTextFont(2); tft.setTextColor(TFT_WHITE); tft.setTextDatum(MC_DATUM);
  tft.drawString("DATE", 354, 22);
  // [GRAFIC]
  uint16_t c1 = (wxTab == 1) ? CLR_WEATHER : CLR_DIM;
  tft.fillRoundRect(396, 8, 76, 28, 8, c1);
  tft.drawString("GRAFIC", 434, 22);
}

// Full weather menu draw
void drawWeatherMenu() {
  tft.fillScreen(CLR_BG);
  // Header
  tft.fillRect(0, 0, 480, 46, CLR_HDR);
  tft.setTextFont(2); tft.setTextColor(tft.color565(110,110,165));
  tft.setTextDatum(ML_DATUM);
  tft.drawString("< Inapoi", 10, 23);
  tft.setTextFont(4); tft.setTextColor(TFT_WHITE); tft.setTextDatum(MC_DATUM);
  tft.drawString("VREME", 200, 23);
  drawWeatherTabs();

  if (wxTab == 0) {
    // Two cards
    if (!wxHasData) {
      tft.setTextFont(2); tft.setTextColor(CLR_LABEL); tft.setTextDatum(MC_DATUM);
      tft.drawString("Se incarca datele meteo...", 240, 180);
    } else {
      // Left card: sensor (smaller, W=193)
      _wxDrawCard(10, 52, 193, 255,
                  CLR_WEATHER,
                  "SENZOR LOCAL", "BME280 192.168.1.145",
                  wxData.sensorTemp, wxData.sensorOk,
                  "umiditate", wxData.sensorHum, "%",
                  "presiune",  wxData.sensorPres, "hPa");
      // Right card: Timișoara (larger, W=263)
      char condBuf[32];
      strncpy(condBuf, wxCodeDesc(wxData.weatherCode), sizeof(condBuf));
      _wxDrawCard(213, 52, 257, 255,
                  CLR_WEATHER,
                  "TIMISOARA", condBuf,
                  wxData.outdoorTemp, wxData.outdoorOk,
                  "simtit", wxData.outdoorFeels, "C",
                  "umiditate", (float)wxData.outdoorHum, "%");
    }
  } else {
    drawWeatherGraph();
  }
}

// Compact weather widget drawn on the home screen (right half)
void drawHomeWeatherWidget() {
  // Clear widget area (keeps gradient-free bg, no corners bleed)
  tft.fillRoundRect(WX_W_X, WX_W_Y, WX_W_W, WX_W_H, 14, CLR_CARD);
  tft.drawRoundRect(WX_W_X, WX_W_Y, WX_W_W, WX_W_H, 14, CLR_WEATHER);

  int cx = WX_W_X + WX_W_W / 2;  // = 357
  tft.setTextDatum(MC_DATUM);

  if (!wxHasData || !wxData.outdoorOk) {
    tft.setTextFont(4); tft.setTextColor(CLR_WEATHER);
    tft.drawString("TIMISOARA", cx, WX_W_Y + 30);
    tft.setTextFont(2); tft.setTextColor(CLR_LABEL);
    tft.drawString(wxHasData ? "offline" : "se incarca...", cx, WX_W_Y + 95);
    return;
  }

  // City name
  tft.setTextFont(4); tft.setTextColor(CLR_WEATHER);
  tft.drawString("TIMISOARA", cx, WX_W_Y + 26);

  // Condition
  tft.setTextFont(2); tft.setTextColor(CLR_LABEL);
  tft.drawString(wxCodeDesc(wxData.weatherCode), cx, WX_W_Y + 46);

  // Temperature — Font 7 integer + Font 4 decimal+C
  char intBuf[6], decBuf[8];
  int  ti = (int)wxData.outdoorTemp;
  int  td = abs((int)(wxData.outdoorTemp * 10) % 10);
  snprintf(intBuf, sizeof(intBuf), "%d", ti);
  snprintf(decBuf, sizeof(decBuf), ".%dC", td);
  tft.setTextFont(7); tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(intBuf, cx + 10, WX_W_Y + 100);
  tft.setTextFont(4); tft.setTextDatum(ML_DATUM);
  tft.drawString(decBuf, cx + 13, WX_W_Y + 82);

  // Sensor indoor temp (small, bottom row)
  if (wxData.sensorOk) {
    char sbuf[24];
    snprintf(sbuf, sizeof(sbuf), "Senz: %.1fC  Hum: %d%%",
             wxData.sensorTemp, wxData.outdoorHum);
    tft.setTextFont(2); tft.setTextColor(CLR_LABEL); tft.setTextDatum(MC_DATUM);
    tft.drawString(sbuf, cx, WX_W_Y + 152);
  }
}

// ============================================================
// Media API
// ============================================================
MediaInfo getMediaInfo() {
  MediaInfo info;
  info.playing  = false;
  info.title    = "";
  info.artist   = "";
  info.album    = "";
  info.duration = 0;
  info.position = 0;
  info.status   = "";

  if (WiFi.status() != WL_CONNECTED || !canHttp()) { mediaServerAvailable = false; return info; }

  HTTPClient http;
  http.begin(String(mediaServerUrl) + "/media");
  http.setTimeout(1500);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int code = http.GET();

  if (code > 0) {
    onHttpOk();
    mediaServerAvailable = true;
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, payload)) {
      info.playing  = doc["playing"];
      info.title    = String((const char*)doc["title"]);
      info.artist   = String((const char*)doc["artist"]);
      info.album    = String((const char*)doc["album"]);
      info.duration = doc["duration"];
      info.position = doc["position"];
      info.status   = String((const char*)doc["status"]);
    }
  } else {
    onHttpFail();
    mediaServerAvailable = false;
    Serial.print("Media server error: "); Serial.println(code);
  }
  http.end();
  return info;
}

DominantColors getAlbumColors() {
  DominantColors colors = { 49, 59, 61, 33, 44, 47 };
  if (!mediaServerAvailable || WiFi.status() != WL_CONNECTED || !canHttp()) return colors;

  HTTPClient http;
  http.begin(String(mediaServerUrl) + "/media/colors");
  http.setTimeout(2000);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int code = http.GET();
  if (code > 0) {
    onHttpOk();
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, http.getString()) && doc.containsKey("colors")) {
      JsonArray arr = doc["colors"];
      if (arr.size() >= 2) {
        colors.r1 = arr[0]["r"]; colors.g1 = arr[0]["g"]; colors.b1 = arr[0]["b"];
        colors.r2 = arr[1]["r"]; colors.g2 = arr[1]["g"]; colors.b2 = arr[1]["b"];
      }
    }
  }
  http.end();
  return colors;
}

// ============================================================
// PC Stats API
// ============================================================
PCStats getPCStats() {
  PCStats s;
  if (WiFi.status() != WL_CONNECTED || !canHttp()) return s;
  HTTPClient http;
  http.begin(String(mediaServerUrl) + "/stats");
  http.setTimeout(500);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int code = http.GET();
  if (code > 0) {
    onHttpOk();
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, http.getString())) {
      s.cpuUsage   = doc["cpu_usage"]    | 0.0f;
      s.ramUsedMB  = doc["ram_used_mb"]  | 0.0f;
      s.ramTotalMB = doc["ram_total_mb"] | 0.0f;
      s.diskUsedGB = doc["disk_used_gb"] | 0.0f;
      s.diskTotalGB= doc["disk_total_gb"]| 0.0f;

      JsonVariant cpuTv = doc["cpu_temp"];
      s.hasCpuTemp = !cpuTv.isNull();
      if (s.hasCpuTemp) s.cpuTemp = cpuTv.as<float>();

      JsonVariant gpuUv = doc["gpu_usage"];
      s.hasGpu     = !gpuUv.isNull();
      if (s.hasGpu)     s.gpuUsage = gpuUv.as<float>();

      JsonVariant gpuTv = doc["gpu_temp"];
      s.hasGpuTemp = !gpuTv.isNull();
      if (s.hasGpuTemp) s.gpuTemp = gpuTv.as<float>();
    }
  } else {
    onHttpFail();
  }
  http.end();
  return s;
}

// Draw a stat card: accent strip, name, big value, progress bar, sub text
void drawStatCard(int x, int y, int w, int h,
                  uint16_t accent, const char* name,
                  float pct, const char* mainVal, const char* subVal) {
  tft.fillRoundRect(x, y, w, h, 10, CLR_CARD);
  // Accent top strip (inset from rounded corners)
  int sW = w - 20;
  tft.fillRect(x + 10, y,      sW, 5, accent);
  tft.fillRect(x,      y,      10, 5, CLR_CARD);
  tft.fillRect(x+w-10, y,      10, 5, CLR_CARD);

  // Name label
  tft.setTextFont(2);
  tft.setTextColor(CLR_LABEL);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(name, x + 10, y + 15);

  // Big value (right-aligned)
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(mainVal, x + w - 8, y + 36);

  // Progress bar
  int bX = x + 10, bY = y + 56, bW = w - 20, bH = 10;
  int fw = (int)(constrain(pct / 100.0f, 0.0f, 1.0f) * bW);
  tft.fillRoundRect(bX, bY, bW, bH, 4, CLR_DIM);
  if (fw > 0) tft.fillRoundRect(bX, bY, fw, bH, 4, accent);

  // Sub text
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(155, 155, 210));
  tft.setTextDatum(ML_DATUM);
  tft.drawString(subVal, x + 10, y + 76);
}

// JPEG callback that writes decoded tiles into albumBuf instead of the screen.
bool album_buf_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  for (int row = 0; row < h; row++) {
    int fy = y + row;
    if (fy >= 128) break;
    for (int col = 0; col < w; col++) {
      int fx = x + col;
      if (fx >= 128) break;
      albumBuf[fy * 128 + fx] = bitmap[row * w + col];
    }
  }
  return 1;
}

// Render the album art buffer to screen (x,y) with a rotation of vinylAngle.
// Uses fixed-point inverse-rotation per pixel + circular clip.
// Each row is pushed as a 128×1 image (same call as original tft_output).
void drawVinylFrame(int x, int y) {
  if (!albumLoaded) return;
  int cosA = (int)(cosf(vinylAngle) * 256.0f);
  int sinA = (int)(sinf(vinylAngle) * 256.0f);
  uint16_t rowBuf[128];
  const int outerR = 63;
  const int innerR = 11; // skip pixels inside pin radius so centre never flickers

  for (int oy = 0; oy < 128; oy++) {
    int dy = oy - 64;
    // Outer circle half-chord
    int r2dy2 = outerR * outerR - dy * dy;
    if (r2dy2 < 0) continue;
    int halfW  = (int)sqrtf((float)r2dy2);
    int xStart = 64 - halfW;
    int xEnd   = 64 + halfW; // inclusive

    // Inner circle half-chord (hole for the pin)
    int ir2dy2 = innerR * innerR - dy * dy;
    int innerHalf = (ir2dy2 > 0) ? (int)sqrtf((float)ir2dy2) : -1;

    if (innerHalf < 0) {
      // Row doesn't intersect inner circle — push full chord
      for (int ox = xStart; ox <= xEnd; ox++) {
        int dx = ox - 64;
        int sx = 64 + ((dx * cosA + dy * sinA) >> 8);
        int sy = 64 + ((-dx * sinA + dy * cosA) >> 8);
        rowBuf[ox - xStart] = ((unsigned)sx < 128 && (unsigned)sy < 128)
                               ? albumBuf[sy * 128 + sx] : 0;
      }
      tft.pushImage(x + xStart, y + oy, xEnd - xStart + 1, 1, rowBuf);
    } else {
      // Row intersects inner circle — push left arc then right arc, skip centre
      int iLeft  = 64 - innerHalf - 1;
      int iRight = 64 + innerHalf + 1;

      // Left segment: xStart .. iLeft
      if (iLeft >= xStart) {
        int segW = iLeft - xStart + 1;
        for (int ox = xStart; ox <= iLeft; ox++) {
          int dx = ox - 64;
          int sx = 64 + ((dx * cosA + dy * sinA) >> 8);
          int sy = 64 + ((-dx * sinA + dy * cosA) >> 8);
          rowBuf[ox - xStart] = ((unsigned)sx < 128 && (unsigned)sy < 128)
                                 ? albumBuf[sy * 128 + sx] : 0;
        }
        tft.pushImage(x + xStart, y + oy, segW, 1, rowBuf);
      }

      // Right segment: iRight .. xEnd
      if (iRight <= xEnd) {
        int segW = xEnd - iRight + 1;
        for (int ox = iRight; ox <= xEnd; ox++) {
          int dx = ox - 64;
          int sx = 64 + ((dx * cosA + dy * sinA) >> 8);
          int sy = 64 + ((-dx * sinA + dy * cosA) >> 8);
          rowBuf[ox - iRight] = ((unsigned)sx < 128 && (unsigned)sy < 128)
                                 ? albumBuf[sy * 128 + sx] : 0;
        }
        tft.pushImage(x + iRight, y + oy, segW, 1, rowBuf);
      }
    }
  }

  // Vinyl centre pin — drawn once, never overwritten by rotation above
  tft.fillCircle(x + 64, y + 64, 10, tft.color565(15, 15, 15));
  tft.fillCircle(x + 64, y + 64,  3, tft.color565(160, 160, 160));
}

// Download loop has an explicit 5-second deadline so it cannot stall forever.
void downloadAndDisplayThumbnail(int x, int y) {
  if (!mediaServerAvailable || WiFi.status() != WL_CONNECTED) {
    tft.fillRect(x, y, 128, 128, TFT_BLACK); return;
  }
  HTTPClient http;
  http.begin(String(mediaServerUrl) + "/media/thumbnail");
  http.setTimeout(5000);
  http.setConnectTimeout(LAN_CONNECT_MS);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    int len = http.getSize();
    if (len > 0) {
      uint8_t* buffer = (uint8_t*)malloc(len);
      if (buffer) {
        int bytesRead = 0;
        unsigned long dlStart = millis();
        while (http.connected() && bytesRead < len) {
          if (millis() - dlStart > 5000) { Serial.println("Thumbnail timeout"); break; }
          size_t avail = stream->available();
          if (avail) bytesRead += stream->readBytes(buffer + bytesRead, avail);
          yield();
        }
        if (bytesRead > 0) {
          TJpgDec.setJpgScale(1);
          // Decode into RAM buffer so we can rotate it each frame
          TJpgDec.setCallback(album_buf_output);
          TJpgDec.drawJpg(0, 0, buffer, bytesRead);
          albumLoaded = true;
          vinylAngle  = 0.0f;
          drawVinylFrame(x, y);
        }
        free(buffer);
      }
    }
  } else {
    tft.fillRect(x, y, 128, 128, TFT_BLACK);
  }
  http.end();
}

// ============================================================
// Racing display helpers
const char* gearChar(int g) {
  static char buf[3];
  if      (g == 0) { buf[0] = 'R'; buf[1] = '\0'; }  // AC: 0=R, 1=N, 2=1st...
  else if (g == 1) { buf[0] = 'N'; buf[1] = '\0'; }
  else             { snprintf(buf, sizeof(buf), "%d", g - 1); }
  return buf;
}

void msToLapStr(int ms, char* out, size_t len) {
  if (ms <= 0) { snprintf(out, len, "--:--.---"); return; }
  int minutes =  ms / 60000;
  int seconds = (ms % 60000) / 1000;
  int millis_ =  ms % 1000;
  snprintf(out, len, "%d:%02d.%03d", minutes, seconds, millis_);
}

void secToLapStr(float sec, char* out, size_t len) {
  msToLapStr((sec > 0.0f) ? (int)(sec * 1000.0f) : 0, out, len);
}

// Generic draw helpers
// ============================================================
void drawButton(const Button& btn, const char* customLabel = nullptr) {
  tft.fillRoundRect(btn.x, btn.y, btn.w, btn.h, btn.radius, btn.color);
  tft.setTextColor(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(customLabel ? customLabel : btn.label,
                 btn.x + btn.w / 2, btn.y + btn.h / 2);
}

void drawBitmapTransparent(int x, int y, int w, int h,
                           const uint16_t* bitmap, uint16_t transparentColor) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      uint16_t color = bitmap[j * w + i];
      if (color != transparentColor) tft.drawPixel(x + i, y + j, color);
    }
}

void adjustBrightness(uint8_t& r, uint8_t& g, uint8_t& b, float factor) {
  r = (uint8_t)(r * factor);
  g = (uint8_t)(g * factor);
  b = (uint8_t)(b * factor);
}

inline bool inRect(int tx, int ty, int x, int y, int w, int h) {
  return (tx >= x && tx < x + w && ty >= y && ty < y + h);
}

bool isButtonPressed(const Button& btn, int tx, int ty) {
  return inRect(tx, ty, btn.x, btn.y, btn.w, btn.h);
}

// ============================================================
// Drawer slide animations — 16 strips of 20 px (~130 ms each)
// ============================================================
void animateDrawerIn() {
  // Wipe from bottom to top (drawer rising up)
  for (int i = 15; i >= 0; i--) {
    tft.fillRect(0, i * 20, 480, 20, CLR_BG);
    delay(8);
  }
}

void animateDrawerOut() {
  // Wipe from top to bottom (drawer sliding away)
  for (int i = 0; i <= 15; i++) {
    tft.fillRect(0, i * 20, 480, 20, CLR_BG);
    delay(8);
  }
}

// ============================================================
// Main menu — dark card layout with live clock
// ============================================================
void drawMenuCard(int x, int y, int w, int h,
                  uint16_t accentColor, const char* label) {
  tft.fillRoundRect(x, y, w, h, 14, CLR_CARD);
  // Accent strip inset from the rounded corners
  tft.fillRect(x + 14, y,      w - 28, 7, accentColor);
  tft.fillRect(x,      y,      14,     7, CLR_CARD);
  tft.fillRect(x+w-14, y,      14,     7, CLR_CARD);
  // Icon circle
  tft.fillCircle(x + w / 2, y + 52, 22, accentColor);
  // Label
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(220, 220, 235));
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h - 20);
}

// clockOnly=true → only refresh time/date text (every second from loop)
// clockOnly=false → full screen redraw
void drawMainMenu(bool clockOnly = false) {
  // Left half center X = 117, right half center X = 357
  const int CLOCK_CX = 117;

  if (!clockOnly) {
    tft.fillScreen(CLR_BG);
    // Top accent bar
    tft.fillRect(0, 0, 480, 4, CLR_DIVIDER);
    // Vertical divider between clock and weather widget
    tft.drawFastVLine(238, 8, 186, CLR_DIVIDER);
    // Thin divider above the APPS button
    tft.fillRect(20, 200, 440, 2, CLR_DIVIDER);
    // APPS launcher button
    tft.fillRoundRect(APPS_BTN_X, APPS_BTN_Y, APPS_BTN_W, APPS_BTN_H, 16, CLR_CARD);
    tft.setTextFont(4);
    tft.setTextColor(tft.color565(100, 100, 155));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("^ ^ ^", 240, APPS_BTN_Y + 24);
    tft.setTextFont(2);
    tft.setTextColor(tft.color565(180, 180, 230));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("APPS", 240, APPS_BTN_Y + 51);
    // Weather widget on right half
    drawHomeWeatherWidget();
  }

  String newTimeStr = getTimeString();
  String newDateStr = getDateString();

  tft.setTextDatum(MC_DATUM);

  // Erase then redraw clock — only LEFT half (X=0..237) to preserve weather widget
  tft.setTextFont(7);
  if (!clockOnly) {
    tft.fillRect(0, 4, 237, 196, CLR_BG);
  } else {
    tft.fillRect(0, 62, 237, 56, CLR_BG);   // covers Font-7 time text on left half
  }
  tft.setTextColor(TFT_WHITE);
  tft.drawString(newTimeStr, CLOCK_CX, 90);

  tft.setTextFont(2);
  if (clockOnly) {
    tft.fillRect(0, 152, 237, 20, CLR_BG);  // covers Font-2 date text on left half
  }
  tft.setTextColor(tft.color565(150, 150, 195));
  tft.drawString(newDateStr, CLOCK_CX, 162);

  prevTimeStr = newTimeStr;
  prevDateStr = newDateStr;
}

// ============================================================
// App Drawer — full-screen grid of every app shortcut
// ============================================================
void drawAppDrawer() {
  tft.fillScreen(CLR_BG);
  // Header
  tft.fillRect(0, 0, 480, 54, CLR_HDR);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("All Apps", 240, 27);
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(110, 110, 165));
  tft.setTextDatum(ML_DATUM);
  tft.drawString("< Home", 12, 27);

  // Row 1: WLED | Music | Racing | USB Display
  drawMenuCard(AD_COL1_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H, CLR_WLED,     "WLED");
  drawMenuCard(AD_COL2_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H, CLR_MUSIC,    "MUSIC");
  drawMenuCard(AD_COL3_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H, CLR_RACING,   "RACING");
  drawMenuCard(AD_COL4_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H, CLR_USBDISP,  "USB DISPLAY");
  // Row 2: PC Stats | Settings | Weather
  drawMenuCard(AD_COL1_X, AD_ROW2_Y, AD_CARD_W, AD_CARD_H, CLR_PCSTATS,  "PC STATS");
  drawMenuCard(AD_COL2_X, AD_ROW2_Y, AD_CARD_W, AD_CARD_H, CLR_SETTINGS, "SETTINGS");
  drawMenuCard(AD_COL3_X, AD_ROW2_Y, AD_CARD_W, AD_CARD_H, CLR_WEATHER,  "WEATHER");
}

// ============================================================
// WLED menu — HSV colour picker
//
// Three gradient bars (Hue / Saturation / Brightness).
// Indicator = 2 px white line drawn once; on drag only those
// 2 columns are touched (+ dependent bars redrawn if needed).
// WLED HTTP commands only fire when the finger lifts.
// ============================================================

// HSV → RGB  (h: 0-359, s/v: 0-255)
void hsvToRgb(int h, uint8_t s, uint8_t v,
              uint8_t& r, uint8_t& g, uint8_t& b) {
  if (s == 0) { r = g = b = v; return; }
  int     region = h / 60;
  int     rem    = (h % 60) * 255 / 60;
  uint8_t p = (uint32_t)v * (255 - s) / 255;
  uint8_t q = (uint32_t)v * (255 - ((uint32_t)s * rem / 255)) / 255;
  uint8_t t = (uint32_t)v * (255 - ((uint32_t)s * (255 - rem) / 255)) / 255;
  switch (region) {
    case 0: r=v; g=t; b=p; break;
    case 1: r=q; g=v; b=p; break;
    case 2: r=p; g=v; b=t; break;
    case 3: r=p; g=q; b=v; break;
    case 4: r=t; g=p; b=v; break;
    default:r=v; g=p; b=q; break;
  }
}

// Current selected RGB (full value; WLED brightness is sent separately)
void getWledRGB(uint8_t& r, uint8_t& g, uint8_t& b) {
  hsvToRgb(wledHue, wledSat, 255, r, g, b);
}

// Colour at pixel column col inside each bar
uint16_t hueColColor(int col) {
  uint8_t r, g, b;
  hsvToRgb(col * 360 / WB_W, 255, 255, r, g, b);
  return tft.color565(r, g, b);
}
uint16_t satColColor(int col) {
  uint8_t r, g, b;
  hsvToRgb(wledHue, (uint8_t)(col * 255 / WB_W), 255, r, g, b);
  return tft.color565(r, g, b);
}
uint16_t briColColor(int col) {
  uint8_t r, g, b;
  hsvToRgb(wledHue, wledSat, (uint8_t)(col * 255 / WB_W), r, g, b);
  return tft.color565(r, g, b);
}

// Draw an entire bar (one VLine per column — fast on ESP32)
void drawPickerBar(int x, int y, int w, int h, uint16_t (*colorFn)(int)) {
  for (int i = 0; i < w; i++)
    tft.drawFastVLine(x + i, y, h, colorFn(i));
  tft.drawRect(x, y, w, h, tft.color565(70, 70, 95));
}

// Draw 2 px white indicator at column col inside a bar
void drawBarIndicator(int barY, int barH, int col) {
  col = constrain(col, 0, WB_W - 2);
  tft.drawFastVLine(WB_X + col,     barY + 2, barH - 4, TFT_WHITE);
  tft.drawFastVLine(WB_X + col + 1, barY + 2, barH - 4, TFT_WHITE);
}

// Move indicator from oldCol to newCol, restoring bar pixels at oldCol
void moveBarIndicator(int barY, int barH,
                      int oldCol, int newCol,
                      uint16_t (*colorFn)(int)) {
  oldCol = constrain(oldCol, 0, WB_W - 2);
  tft.drawFastVLine(WB_X + oldCol,     barY + 2, barH - 4, colorFn(oldCol));
  tft.drawFastVLine(WB_X + oldCol + 1, barY + 2, barH - 4, colorFn(oldCol + 1));
  newCol = constrain(newCol, 0, WB_W - 2);
  tft.drawFastVLine(WB_X + newCol,     barY + 2, barH - 4, TFT_WHITE);
  tft.drawFastVLine(WB_X + newCol + 1, barY + 2, barH - 4, TFT_WHITE);
}

// Preview swatch + current RGB values
void drawWledPreview() {
  uint8_t r, g, b;
  getWledRGB(r, g, b);
  tft.fillRoundRect(WB_X, WB_PRV_Y, 140, WB_PRV_H, 8, tft.color565(r, g, b));
  tft.fillRect(WB_X + 150, WB_PRV_Y, 310, WB_PRV_H, CLR_BG);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("R:" + String(r) + "  G:" + String(g) + "  B:" + String(b),
                 WB_X + 155, WB_PRV_Y + WB_PRV_H / 2);
}

// Mode tab trio (solid / AmbiWLED / Music Cover)
void drawWledModeTabs() {
  int w = (WB_W - 4) / 3; // each tab width (2px gap between tabs)
  uint16_t cSolid = (ledMode == 0) ? CLR_WLED : CLR_DIM;
  uint16_t cAmbi  = (ledMode == 1) ? CLR_WLED : CLR_DIM;
  uint16_t cCover = (ledMode == 2) ? CLR_WLED : CLR_DIM;
  tft.fillRoundRect(WB_X,              60, w,               40, 8, cSolid);
  tft.fillRoundRect(WB_X + w + 2,      60, w,               40, 8, cAmbi);
  tft.fillRoundRect(WB_X + 2*(w + 2),  60, WB_W - 2*(w+2), 40, 8, cCover);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Solid",        WB_X + w / 2,                        80);
  tft.drawString("AmbiWLED",     WB_X + w + 2 + w / 2,                80);
  tft.drawString("Music Cover",  WB_X + 2*(w+2) + (WB_W-2*(w+2)) / 2, 80);
}

void drawWLEDMenu() {
  tft.fillScreen(CLR_BG);

  // Header
  tft.fillRect(0, 0, 480, 54, CLR_HDR);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("WLED", 240, 27);
  tft.setTextFont(2);
  tft.setTextColor(CLR_WLED);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("< Back", 12, 27);
  // ON/OFF pill
  tft.fillRoundRect(390, 9, 82, 36, 10, CLR_DIM);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("ON/OFF", 431, 27);

  // Mode tabs
  drawWledModeTabs();

  // --- Hue bar ---
  drawPickerBar(WB_X, WB_HUE_Y, WB_W, WB_HUE_H, hueColColor);
  wledHueCol = constrain(wledHue * WB_W / 360, 0, WB_W - 2);
  drawBarIndicator(WB_HUE_Y, WB_HUE_H, wledHueCol);

  // --- Saturation bar ---
  drawPickerBar(WB_X, WB_SAT_Y, WB_W, WB_SAT_H, satColColor);
  wledSatCol = constrain((int)wledSat * WB_W / 255, 0, WB_W - 2);
  drawBarIndicator(WB_SAT_Y, WB_SAT_H, wledSatCol);

  // --- Brightness bar ---
  drawPickerBar(WB_X, WB_BRI_Y, WB_W, WB_BRI_H, briColColor);
  wledBriCol = constrain((int)brightness * WB_W / 255, 0, WB_W - 2);
  drawBarIndicator(WB_BRI_Y, WB_BRI_H, wledBriCol);

  // --- Preview swatch ---
  drawWledPreview();
}

// ============================================================
// Music menu
// ============================================================
void drawMusicStatusOverlay(bool serverAvail) {
  uint16_t bg = tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1);
  tft.fillRect(0, 50, 480, 170, bg);
  tft.setTextDatum(MC_DATUM);

  if (!serverAvail) {
    tft.fillCircle(240, 105, 28, tft.color565(180, 45, 45));
    tft.setTextFont(4); tft.setTextColor(TFT_WHITE);
    tft.drawString("!", 240, 97);
    tft.setTextFont(2);
    tft.drawString("Companion app not running", 240, 148);
    tft.setTextColor(tft.color565(200, 200, 200));
    tft.drawString("Start music.py on your PC", 240, 168);
  } else {
    tft.fillCircle(240, 105, 28, tft.color565(55, 55, 100));
    tft.setTextFont(4); tft.setTextColor(TFT_WHITE);
    tft.drawString("~", 240, 95);
    tft.setTextFont(2);
    tft.drawString("Nothing is playing", 240, 148);
    tft.setTextColor(tft.color565(200, 200, 200));
    tft.drawString("Open a media player on your PC", 240, 168);
  }
}

// Returns true when the media session exists AND is actively playing (not paused).
bool isActuallyPlaying() {
  return mediaData.playing && mediaData.status == "Playing";
}

// Draw the centre transport icon: pause bitmap when playing, play triangle when paused.
void drawCenterIcon(uint16_t bgColor) {
  if (isActuallyPlaying()) {
    drawBitmapTransparent(215, 245, 50, 50, pauseIcon, 0x0000);
  } else {
    tft.fillRect(215, 245, 50, 50, bgColor);
    // Right-pointing play triangle, centred in the 50×50 button area
    tft.fillTriangle(224, 252, 224, 288, 261, 270, TFT_WHITE);
  }
}

void drawMusicVolIndicator(float vol) {
  int pct = constrain((int)roundf(vol * 100.0f), 0, 100);
  tft.fillRoundRect(134, 3, 252, 44, 6, tft.color565(30, 30, 30));
  tft.fillRect(144, 36, 232, 6, tft.color565(80, 80, 80));
  if (pct > 0) tft.fillRect(144, 36, 232 * pct / 100, 6, TFT_WHITE);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("VOL", 144, 20);
  tft.setTextDatum(MR_DATUM);
  char buf[6]; snprintf(buf, sizeof(buf), "%d%%", pct);
  tft.drawString(buf, 382, 20);
}

void drawMusicMenu(bool refresh = false) {
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(TL_DATUM);

  mediaData = getMediaInfo();

  bool statusChanged  = (prevMediaServerAvailable != mediaServerAvailable ||
                         prevMediaPlaying         != mediaData.playing);
  bool needFullRedraw = (!refresh || statusChanged);

  if (needFullRedraw) {
    tft.fillRect(0, 0, 480, 50, TFT_WHITE);
    drawBitmapTransparent(8, 8, 35, 35, back, 0xFFFF);
    tft.setTextFont(4);
    tft.setTextColor(tft.color565(30, 30, 30));
    tft.setTextDatum(MR_DATUM);
    tft.drawString(getTimeString(), 472, 25);
    tft.setTextDatum(TL_DATUM);

    if (mediaServerAvailable && mediaData.playing) dominantColors = getAlbumColors();

    uint8_t r1d = dominantColors.r1, g1d = dominantColors.g1, b1d = dominantColors.b1;
    adjustBrightness(r1d, g1d, b1d, 0.6);

    tft.fillRect(0,  50, 480, 170, tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
    tft.fillRect(0, 220, 480, 100, tft.color565(r1d, g1d, b1d));
    drawBitmapTransparent(123, 245, 50, 50, previousIcon, 0x0000);
    drawCenterIcon(tft.color565(r1d, g1d, b1d));
    drawBitmapTransparent(307, 245, 50, 50, nextIcon,     0x0000);

    if (!mediaServerAvailable || !mediaData.playing) {
      albumLoaded = false;
      drawMusicStatusOverlay(mediaServerAvailable);
      prevMediaServerAvailable = mediaServerAvailable;
      prevMediaPlaying         = mediaData.playing;
      oldTitle  = mediaData.title;
      oldArtist = mediaData.artist;
      return;
    }

    tft.fillCircle(21 + 64, 71 + 64, 63, TFT_BLACK);  // clear old art before download
    downloadAndDisplayThumbnail(21, 71);
    titleCharOffset    = 0;
    artistCharOffset   = 0;
    localPosition      = mediaData.position;
    lastPositionUpdate = millis();
    titleNeedsScroll   = tft.textWidth(mediaData.title)  > textDisplayWidth;
    artistNeedsScroll  = tft.textWidth(mediaData.artist) > textDisplayWidth;

    tft.setTextColor(TFT_WHITE);
    tft.fillRect(170, 100, textDisplayWidth, 30,
      tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
    tft.drawString(mediaData.title, 170, 100);
    tft.fillRect(170, 142, textDisplayWidth, 30,
      tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
    tft.drawString(mediaData.artist, 170, 142);

  } else if (mediaServerAvailable && mediaData.playing) {
    // Incremental refresh: progress bar + timestamps only
    // Refresh clock in header (white area, always safe to overwrite)
    tft.fillRect(390, 5, 82, 40, TFT_WHITE);
    tft.setTextFont(4);
    tft.setTextColor(tft.color565(30, 30, 30));
    tft.setTextDatum(MR_DATUM);
    tft.drawString(getTimeString(), 472, 25);

    uint8_t r1d = dominantColors.r1, g1d = dominantColors.g1, b1d = dominantColors.b1;
    adjustBrightness(r1d, g1d, b1d, 0.6);

    if (millis() - lastPositionUpdate >= 1000) {
      localPosition++;
      if (localPosition > mediaData.duration) localPosition = mediaData.duration;
      lastPositionUpdate = millis();
    }
    if (abs(mediaData.position - localPosition) > 3) localPosition = mediaData.position;

    int pb = (mediaData.duration > 0)
             ? map(localPosition, 0, mediaData.duration, 0, 480) : 0;
    tft.fillRect(pb, 219, 480 - pb, 3, tft.color565(r1d, g1d, b1d));
    tft.fillRect(0,  219, pb,       3, TFT_WHITE);

    int sm = localPosition / 60, ss = localPosition % 60;
    tft.fillRect(21, 230, 60, 20, tft.color565(r1d, g1d, b1d));
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(String(sm) + (ss < 10 ? ":0" : ":") + String(ss), 21, 230);

    int dm = mediaData.duration / 60, ds = mediaData.duration % 60;
    tft.fillRect(419, 230, 60, 20, tft.color565(r1d, g1d, b1d));
    tft.setTextDatum(TR_DATUM);
    tft.drawString(String(dm) + (ds < 10 ? ":0" : ":") + String(ds), 459, 230);
    tft.setTextDatum(TL_DATUM);

    // Redraw centre icon if play/pause state changed
    if (isActuallyPlaying() != prevIsActuallyPlaying) {
      drawCenterIcon(tft.color565(r1d, g1d, b1d));
    }
  }

  // Track-change detection
  if (mediaServerAvailable && mediaData.playing &&
      (oldTitle != mediaData.title || oldArtist != mediaData.artist)) {

    dominantColors     = getAlbumColors();
    localPosition      = 0;
    lastPositionUpdate = millis();

    uint8_t r1d = dominantColors.r1, g1d = dominantColors.g1, b1d = dominantColors.b1;
    adjustBrightness(r1d, g1d, b1d, 0.6);

    tft.fillRect(0,  50, 480, 170, tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
    tft.fillRect(0, 220, 480, 100, tft.color565(r1d, g1d, b1d));
    drawBitmapTransparent(123, 245, 50, 50, previousIcon, 0x0000);
    drawCenterIcon(tft.color565(r1d, g1d, b1d));
    drawBitmapTransparent(307, 245, 50, 50, nextIcon,     0x0000);

    tft.fillCircle(21 + 64, 71 + 64, 63, TFT_BLACK);  // clear old art before download
    downloadAndDisplayThumbnail(21, 71);
    titleCharOffset    = 0;
    artistCharOffset   = 0;
    titleNeedsScroll   = tft.textWidth(mediaData.title)  > textDisplayWidth;
    artistNeedsScroll  = tft.textWidth(mediaData.artist) > textDisplayWidth;

    tft.setTextColor(TFT_WHITE);
    tft.fillRect(170, 100, textDisplayWidth + 10, 30,
      tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
    tft.drawString(mediaData.title, 170, 100);
    tft.fillRect(170, 142, textDisplayWidth + 10, 30,
      tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
    tft.drawString(mediaData.artist, 170, 142);
  }

  // Scrolling text
  if (mediaServerAvailable && mediaData.playing) {
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE);

    if (titleNeedsScroll && millis() - lastTitleScrollTime > scrollDelay) {
      String d = mediaData.title.substring(titleCharOffset) + "   " +
                 mediaData.title.substring(0, titleCharOffset);
      tft.fillRect(170, 100, textDisplayWidth + 10, 30,
        tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
      tft.setTextDatum(TL_DATUM);
      tft.drawString(d, 170, 100);
      if (++titleCharOffset >= (int)mediaData.title.length()) titleCharOffset = 0;
      lastTitleScrollTime = millis();
    }

    if (artistNeedsScroll && millis() - lastArtistScrollTime > scrollDelay) {
      String d = mediaData.artist.substring(artistCharOffset) + "   " +
                 mediaData.artist.substring(0, artistCharOffset);
      tft.fillRect(170, 142, textDisplayWidth + 10, 30,
        tft.color565(dominantColors.r1, dominantColors.g1, dominantColors.b1));
      tft.setTextDatum(TL_DATUM);
      tft.drawString(d, 170, 142);
      if (++artistCharOffset >= (int)mediaData.artist.length()) artistCharOffset = 0;
      lastArtistScrollTime = millis();
    }
  }

  prevMediaServerAvailable = mediaServerAvailable;
  prevMediaPlaying         = mediaData.playing;
  prevIsActuallyPlaying    = isActuallyPlaying();
  oldTitle  = mediaData.title;
  oldArtist = mediaData.artist;
}

// ============================================================
// Settings menu
// ============================================================
void refreshSettingsTZDisplay() {
  tft.fillRect(185, tzMinusBtn.y, 110, tzMinusBtn.h, CLR_BG);
  String tzLabel = (gmtOffsetHours >= 0 ? "UTC+" : "UTC") + String(gmtOffsetHours);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(tzLabel, 240, tzMinusBtn.y + tzMinusBtn.h / 2);

  tft.fillRect(0, 296, 480, 24, CLR_BG);
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(120, 120, 165));
  tft.drawString("Current: " + getTimeString() + "   " + getDateString(), 240, 307);
}

void drawSettingsMenu() {
  tft.fillScreen(CLR_BG);

  tft.fillRect(0, 0, 480, 68, CLR_HDR);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Settings", 240, 34);
  tft.setTextFont(2);
  tft.setTextColor(CLR_SETTINGS);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("< Back", 14, 34);

  tft.fillRoundRect(10, 82, 460, 140, 12, tft.color565(26, 26, 44));
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(150, 150, 200));
  tft.setTextDatum(MC_DATUM);
  tft.drawString("TIME ZONE (UTC offset)", 240, 104);

  // Minus / Plus buttons
  tft.fillRoundRect(tzMinusBtn.x, tzMinusBtn.y,
                    tzMinusBtn.w, tzMinusBtn.h, tzMinusBtn.radius, tzMinusBtn.color);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("-", tzMinusBtn.x + tzMinusBtn.w / 2,
                      tzMinusBtn.y + tzMinusBtn.h / 2);

  tft.fillRoundRect(tzPlusBtn.x, tzPlusBtn.y,
                    tzPlusBtn.w, tzPlusBtn.h, tzPlusBtn.radius, tzPlusBtn.color);
  tft.drawString("+", tzPlusBtn.x + tzPlusBtn.w / 2,
                      tzPlusBtn.y + tzPlusBtn.h / 2);

  refreshSettingsTZDisplay();

  tft.fillRoundRect(syncBtn.x, syncBtn.y,
                    syncBtn.w, syncBtn.h, syncBtn.radius, syncBtn.color);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(syncBtn.label,
                 syncBtn.x + syncBtn.w / 2,
                 syncBtn.y + syncBtn.h / 2);
}

// ============================================================
// Racing menu display
// ============================================================

void drawRacingTabs() {
  int w = (WB_W - 4) / 3;
  uint16_t c0 = (racingSubMenu == 0) ? CLR_RACING : CLR_DIM;
  uint16_t c1 = (racingSubMenu == 1) ? CLR_RACING : CLR_DIM;
  uint16_t c2 = (racingSubMenu == 2) ? CLR_RACING : CLR_DIM;
  tft.fillRoundRect(WB_X,             55, w,               32, 6, c0);
  tft.fillRoundRect(WB_X + w + 2,     55, w,               32, 6, c1);
  tft.fillRoundRect(WB_X + 2*(w + 2), 55, WB_W - 2*(w+2), 32, 6, c2);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("RPM",   WB_X + w / 2,                         71);
  tft.drawString("TELEM", WB_X + w + 2 + w / 2,                 71);
  tft.drawString("SETUP", WB_X + 2*(w+2) + (WB_W-2*(w+2)) / 2, 71);
}

void drawRacingNoSignal(bool gameConnected = false) {
  tft.fillRect(0, 90, 480, 230, TFT_BLACK);
  tft.fillCircle(240, 165, 36, CLR_DIM);
  tft.setTextFont(4);
  tft.setTextColor(tft.color565(160, 160, 200));
  tft.setTextDatum(MC_DATUM);
  tft.drawString(gameConnected ? "!" : "?", 240, 158);
  tft.setTextFont(2);
  tft.drawString(gameConnected ? "Enter a race event..." : "Waiting for game...", 240, 220);
  // Reconnect button — always visible without needing the Setup tab
  tft.fillRoundRect(150, 250, 180, 40, 8, CLR_SETTINGS);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("RECONNECT", 240, 270);
}

void drawRacingRPMView(bool full) {
  // Abstract data source based on selected game
  ForzaData& rd  = (racingGame == 2) ? beamngData : forzaData;
  bool  hasData  = (racingGame == 0) ? acHasData  : (racingGame == 1 ? forzaHasData : beamngHasData);
  float rpmVal   = hasData ? ((racingGame == 0) ? acData.engineRPM        : (float)rd.rpm)     : 0.0f;
  int   rpmMax   = (racingGame == 0) ? acMaxRpm   : (rd.rpmMax > 0 ? rd.rpmMax : acMaxRpm);
  int   gearVal  = hasData ? ((racingGame == 0) ? acData.gear             : (int)rd.gear)       : 1;
  float speedVal = hasData ? ((racingGame == 0) ? acData.speed_Kmh        : rd.speedKmh)        : 0.0f;
  int   curLapMs = hasData ? ((racingGame == 0) ? acData.lapTime          : (int)(rd.currentLap * 1000.0f)) : 0;

  int shiftPoint = (int)(rpmMax * acShiftPct / 100.0f);
  float rpmFrac  = constrain(rpmVal / (float)rpmMax, 0.0f, 1.0f);
  bool shifting  = hasData && (rpmVal >= shiftPoint);
  bool flashNow  = shifting && ((millis() / 100) % 2 == 0);

  // --- Zone A: Gear (font 7) + Speed (font 6) + Lap time ---
  int curGear = gearVal;
  if (full || curGear != prevGear) {
    tft.fillRect(0, 88, 130, 52, TFT_BLACK);
    tft.setTextFont(7);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    if (racingGame == 0) {
      tft.drawString(gearChar(curGear), 55, 114);  // AC: -1=R, 0=N, 1=1st...
    } else {
      // FH6: 0=N, 1=1st, ..., 9=9th, ≥10=R
      static char fgbuf[3];
      if      (curGear == 0) { fgbuf[0]='N'; fgbuf[1]='\0'; }
      else if (curGear >= 10){ fgbuf[0]='R'; fgbuf[1]='\0'; }
      else { snprintf(fgbuf, sizeof(fgbuf), "%d", curGear); }
      tft.drawString(fgbuf, 55, 114);
    }
    prevGear = curGear;
  }

  {
    tft.fillRect(130, 88, 175, 52, TFT_BLACK);
    tft.setTextFont(6);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_WHITE);
    char spd[8];
    snprintf(spd, sizeof(spd), "%.0f", speedVal);
    tft.drawString(spd, 305, 114);
    prevSpeed = speedVal;
  }
  if (full) {
    tft.setTextFont(2);
    tft.setTextColor(tft.color565(160, 160, 200));
    tft.setTextDatum(ML_DATUM);
    tft.drawString("km/h", 308, 108);
  }

  if (full || curLapMs != prevLapTime) {
    tft.fillRect(360, 88, 120, 52, TFT_BLACK);
    tft.setTextFont(2);
    tft.setTextColor(tft.color565(160, 200, 160));
    tft.setTextDatum(MR_DATUM);
    char lapBuf[16];
    msToLapStr(curLapMs, lapBuf, sizeof(lapBuf));
    tft.drawString(lapBuf, 476, 108);
    prevLapTime = curLapMs;
  }

  // --- Zone B: Shift indicator circles ---
  if (full || flashNow != prevShiftFlash ||
      fabsf(rpmFrac - (prevEngineRPM >= 0.0f ? prevEngineRPM / (float)rpmMax : 0.0f)) > 0.002f) {
    for (int i = 0; i < 10; i++) {
      float threshold = (i + 1) / 10.0f;
      bool  lit       = rpmFrac >= threshold;
      uint16_t col;
      if      (threshold <= 0.6f) col = tft.color565(  0, 200,   0);
      else if (threshold <= 0.8f) col = tft.color565(220, 200,   0);
      else if (threshold <= 0.9f) col = tft.color565(255, 100,   0);
      else                        col = tft.color565(220,  20,  20);
      uint16_t fillCol = (lit && (!shifting || flashNow)) ? col : CLR_DIM;
      tft.fillCircle(112 + i * 26, 200, 11, fillCol);
      tft.drawCircle(112 + i * 26, 200, 11, col);
    }
    prevShiftFlash = flashNow;
    prevEngineRPM  = rpmVal;
  }

  // --- Zone C: Lap info row ---
  if (full || curLapMs != prevLapTime) {
    tft.fillRect(0, 252, 480, 40, TFT_BLACK);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(tft.color565(160, 160, 220));
    char cur[16], last[16], best[16];
    if (racingGame == 0) {
      msToLapStr(hasData ? acData.lapTime : 0, cur,  sizeof(cur));
      msToLapStr(hasData ? acData.lastLap : 0, last, sizeof(last));
      msToLapStr(hasData ? acData.bestLap : 0, best, sizeof(best));
    } else {
      secToLapStr(hasData ? rd.currentLap : 0.0f, cur,  sizeof(cur));
      secToLapStr(hasData ? rd.lastLap    : 0.0f, last, sizeof(last));
      secToLapStr(hasData ? rd.bestLap    : 0.0f, best, sizeof(best));
    }
    int lapCount = hasData ? ((racingGame == 0) ? acData.lapCount : (int)rd.lap) : 0;
    char row[80];
    snprintf(row, sizeof(row), "LAP %d  |  %s  |  %s  |  %s", lapCount, cur, last, best);
    tft.drawString(row, 240, 270);
  }
}

void drawRacingTelemetryView(bool full) {
  ForzaData& rd  = (racingGame == 2) ? beamngData : forzaData;
  bool hasData = (racingGame == 0) ? acHasData : (racingGame == 1 ? forzaHasData : beamngHasData);

  // Shared bar drawing helper
  auto drawBar = [&](int y, const char* lbl, float val,
                     uint8_t r, uint8_t g, uint8_t b, float& prev) {
    if (!full && fabsf(val - prev) < 0.005f) return;
    int bX = 55, bW = 400, bH = 28;
    int fw = (int)(constrain(val, 0.0f, 1.0f) * bW);
    tft.fillRect(bX, y, fw, bH, tft.color565(r, g, b));
    if (fw < bW) tft.fillRect(bX + fw, y, bW - fw, bH, CLR_DIM);
    if (full) {
      tft.setTextFont(2);
      tft.setTextColor(tft.color565(160, 160, 200));
      tft.setTextDatum(MR_DATUM);
      tft.drawString(lbl, 50, y + bH / 2);
    }
    prev = val;
  };

  // Gas and brake bars — same for all games
  float curGas   = hasData ? ((racingGame == 0) ? acData.gas             : rd.accel / 255.0f) : 0.0f;
  float curBrake = hasData ? ((racingGame == 0) ? acData.brake           : rd.brake / 255.0f) : 0.0f;
  drawBar(100, "GAS", curGas,   0, 180, 0, prevGas);
  drawBar(142, "BRK", curBrake, 200, 0, 0, prevBrake);

  if (racingGame == 0) {
    // --- AC mode: clutch bar + g-forces + ABS/TC ---
    float curClutch = hasData ? acData.clutch : 0.0f;
    drawBar(184, "CLT", curClutch, 200, 180, 0, prevClutch);

    float frontal = hasData ? acData.accG_frontal    : 0.0f;
    float horiz   = hasData ? acData.accG_horizontal : 0.0f;
    if (full || fabsf(frontal - prevAccFrontal) > 0.05f ||
                fabsf(horiz   - prevAccHoriz)   > 0.05f) {
      if (full) {
        tft.setTextFont(2);
        tft.setTextColor(tft.color565(160, 160, 200));
        tft.setTextDatum(ML_DATUM);
        tft.drawString("G-FORCE", 25, 230);
      }
      char gBuf[16];
      tft.fillRect(130, 226, 200, 20, TFT_BLACK);
      snprintf(gBuf, sizeof(gBuf), "F:%.2f  L:%.2f", frontal, horiz);
      tft.setTextFont(2);
      tft.setTextColor(TFT_WHITE);
      tft.setTextDatum(ML_DATUM);
      tft.drawString(gBuf, 130, 230);
      prevAccFrontal = frontal;
      prevAccHoriz   = horiz;
    }

    bool absAct = hasData && acData.isAbsInAction;
    bool tcAct  = hasData && acData.isTcInAction;
    if (full || absAct != prevAbsAction || tcAct != prevTcAction) {
      tft.fillRoundRect( 25, 262, 90, 28, 6,
        absAct ? tft.color565(255, 180, 0)   : CLR_DIM);
      tft.fillRoundRect(130, 262, 90, 28, 6,
        tcAct  ? tft.color565(  0, 180, 255) : CLR_DIM);
      tft.setTextFont(2);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE);
      tft.drawString("ABS",  70, 276);
      tft.drawString("TC",  175, 276);
      prevAbsAction = absAct;
      prevTcAction  = tcAct;
    }

  } else {
    // --- FH6 / BeamNG mode: boost bar + tire temperature grid ---

    // Boost bar (cyan) — scale 0–2 bar
    float curBoost = hasData ? constrain(rd.boost / 2.0f, 0.0f, 1.0f) : 0.0f;
    drawBar(184, "BST", curBoost, 0, 160, 200, prevForzaBoost);

    // Tire temp label (full draw only)
    if (full) {
      tft.setTextFont(2);
      tft.setTextColor(tft.color565(150, 150, 200));
      tft.setTextDatum(ML_DATUM);
      tft.drawString("TIRE TEMP", 8, 213);
    }

    // 4 tire cells in 2×2 grid: FL FR / RL RR
    const char* labels[4] = { "FL", "FR", "RL", "RR" };
    const int cellX[4]    = {   8,  246,    8,  246  };
    const int cellY[4]    = { 220,  220,  266,  266  };
    const int cellW = 228, cellH = 42;

    for (int i = 0; i < 4; i++) {
      float t = hasData ? rd.tireTemp[i] : -1.0f;
      if (!full && fabsf(t - prevForzaTireTemp[i]) < 1.0f) continue;
      prevForzaTireTemp[i] = t;

      uint16_t tCol;
      if      (t < 0)   tCol = CLR_DIM;
      else if (t < 60)  tCol = tft.color565( 30,  80, 220);  // blue = cold
      else if (t < 90)  tCol = tft.color565(  0, 180,   0);  // green = optimal
      else if (t < 120) tCol = tft.color565(255, 140,   0);  // orange = hot
      else              tCol = tft.color565(220,  30,  30);  // red = overheating

      int cx = cellX[i], cy = cellY[i];
      tft.fillRoundRect(cx, cy, cellW, cellH, 5, CLR_DIM);
      tft.fillRect(cx + 5, cy, cellW - 10, 5, tCol);         // colored top strip
      tft.fillRect(cx, cy, 5, 5, CLR_DIM);
      tft.fillRect(cx + cellW - 5, cy, 5, 5, CLR_DIM);

      tft.setTextFont(2);
      tft.setTextColor(tft.color565(180, 180, 210));
      tft.setTextDatum(ML_DATUM);
      tft.drawString(labels[i], cx + 6, cy + 12);

      char tBuf[8];
      if (t < 0) snprintf(tBuf, sizeof(tBuf), "--");
      else        snprintf(tBuf, sizeof(tBuf), "%.0f", t);
      tft.setTextFont(4);
      tft.setTextColor(TFT_WHITE);
      tft.setTextDatum(MR_DATUM);
      tft.drawString(tBuf, cx + cellW - 22, cy + 14);

      tft.setTextFont(2);
      tft.setTextColor(tft.color565(140, 140, 180));
      tft.setTextDatum(ML_DATUM);
      tft.drawString("C", cx + cellW - 18, cy + 14);
    }
  }
}

void drawRacingSetupView() {
  tft.fillRect(0, 90, 480, 230, TFT_BLACK);

  // Game selector
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(150, 150, 200));
  tft.setTextDatum(ML_DATUM);
  tft.drawString("GAME", 25, 100);

  const int gBtnW = 120, gBtnH = 38, gBtnY = 115;
  uint16_t cAC  = (racingGame == 0) ? CLR_RACING : CLR_DIM;
  uint16_t cFH5 = (racingGame == 1) ? CLR_RACING : CLR_DIM;
  uint16_t cBNG = (racingGame == 2) ? CLR_RACING : CLR_DIM;
  tft.fillRoundRect( 25,      gBtnY, gBtnW, gBtnH, 6, cAC);
  tft.fillRoundRect( 25+126,  gBtnY, gBtnW, gBtnH, 6, cFH5);
  tft.fillRoundRect( 25+252,  gBtnY, gBtnW, gBtnH, 6, cBNG);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("AC",      85,       gBtnY + 19);
  tft.drawString("FH6",    85 + 126,  gBtnY + 19);
  tft.drawString("BeamNG", 85 + 252,  gBtnY + 19);

  // Max RPM
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(tft.color565(150, 150, 200));
  tft.setTextFont(2);
  tft.drawString("MAX RPM", 25, 165);
  drawButton(rpmMinusBtn);
  char rpmBuf[8];
  snprintf(rpmBuf, sizeof(rpmBuf), "%d", acMaxRpm);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(rpmBuf, 240, 180);
  drawButton(rpmPlusBtn);

  // Shift %
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(tft.color565(150, 150, 200));
  tft.setTextFont(2);
  tft.drawString("SHIFT AT", 25, 215);
  drawButton(shiftMinusBtn);
  char shiftBuf[8];
  snprintf(shiftBuf, sizeof(shiftBuf), "%d%%", acShiftPct);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(shiftBuf, 240, 230);
  drawButton(shiftPlusBtn);

  // PC IP
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(120, 120, 165));
  tft.setTextDatum(ML_DATUM);
  tft.drawString("PC IP:", 25, 265);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(acServerIp, 80, 265);

  // Rev Lights toggle
  revLightsBtn.color = wledRevLightsEnabled ? (uint16_t)CLR_RACING : (uint16_t)CLR_DIM;
  drawButton(revLightsBtn);

  // Reconnect
  drawButton(acReconnectBtn);
}

void drawRacingMenu(bool refresh = false) {
  bool hasData = (racingGame == 0) ? acHasData : forzaHasData;
  if (!refresh) {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, 480, 54, CLR_HDR);
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("RACING", 240, 27);
    tft.setTextFont(2);
    tft.setTextColor(CLR_RACING);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("< Back", 12, 27);
    drawRacingTabs();
    if (!hasData) { drawRacingNoSignal((racingGame == 1) && forzaGameRunning); return; }
    switch (racingSubMenu) {
      case 0: drawRacingRPMView(true);       break;
      case 1: drawRacingTelemetryView(true); break;
      case 2: drawRacingSetupView();         break;
    }
  } else {
    if (!hasData) return;
    switch (racingSubMenu) {
      case 0: drawRacingRPMView(false);       break;
      case 1: drawRacingTelemetryView(false); break;
    }
  }
}

// ============================================================
// PC Stats menu — real-time system monitor (2 s refresh)
// ============================================================
void drawPCStatsMenu(bool refresh = false) {
  if (!refresh) {
    tft.fillScreen(CLR_BG);
    tft.fillRect(0, 0, 480, 54, CLR_HDR);
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("PC Stats", 240, 27);
    tft.setTextFont(2);
    tft.setTextColor(CLR_PCSTATS);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("< Back", 12, 27);
  }

  pcStats = getPCStats();

  // ---- CPU card (left, row 1) ----
  char cpuMain[12], cpuSub[22];
  snprintf(cpuMain, sizeof(cpuMain), "%.0f%%", pcStats.cpuUsage);
  if (pcStats.hasCpuTemp)
    snprintf(cpuSub, sizeof(cpuSub), "Temp: %.0f C", pcStats.cpuTemp);
  else
    snprintf(cpuSub, sizeof(cpuSub), "No temp sensor");
  drawStatCard(8, 62, 229, 114,
               tft.color565(220, 90, 30), "CPU",
               pcStats.cpuUsage, cpuMain, cpuSub);

  // ---- GPU card (right, row 1) ----
  if (pcStats.hasGpu) {
    char gpuMain[12], gpuSub[22];
    snprintf(gpuMain, sizeof(gpuMain), "%.0f%%", pcStats.gpuUsage);
    if (pcStats.hasGpuTemp)
      snprintf(gpuSub, sizeof(gpuSub), "Temp: %.0f C", pcStats.gpuTemp);
    else
      snprintf(gpuSub, sizeof(gpuSub), "No temp sensor");
    drawStatCard(247, 62, 225, 114,
                 tft.color565(30, 180, 60), "GPU",
                 pcStats.gpuUsage, gpuMain, gpuSub);
  } else {
    tft.fillRoundRect(247, 62, 225, 114, 10, CLR_CARD);
    tft.fillRect(257, 62, 205, 5, CLR_DIM);
    tft.fillRect(247, 62, 10,  5, CLR_CARD);
    tft.fillRect(462, 62, 10,  5, CLR_CARD);
    tft.setTextFont(2);
    tft.setTextColor(CLR_LABEL);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("GPU", 257, 76);
    tft.setTextColor(tft.color565(85, 85, 120));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Not detected", 359, 119);
  }

  // ---- RAM card (left, row 2) ----
  char ramMain[12], ramSub[26];
  float ramPct = (pcStats.ramTotalMB > 0)
                 ? (pcStats.ramUsedMB / pcStats.ramTotalMB * 100.0f) : 0.0f;
  snprintf(ramMain, sizeof(ramMain), "%.0f%%", ramPct);
  snprintf(ramSub,  sizeof(ramSub),  "%.1f / %.1f GB",
           pcStats.ramUsedMB  / 1024.0f,
           pcStats.ramTotalMB / 1024.0f);
  drawStatCard(8, 186, 229, 114,
               tft.color565(130, 80, 255), "RAM",
               ramPct, ramMain, ramSub);

  // ---- DISK card (right, row 2) ----
  char diskMain[12], diskSub[24];
  float diskPct = (pcStats.diskTotalGB > 0)
                 ? (pcStats.diskUsedGB / pcStats.diskTotalGB * 100.0f) : 0.0f;
  snprintf(diskMain, sizeof(diskMain), "%.0f%%", diskPct);
  snprintf(diskSub,  sizeof(diskSub),  "%.1f / %.1f GB",
           pcStats.diskUsedGB, pcStats.diskTotalGB);
  drawStatCard(247, 186, 225, 114,
               tft.color565(0, 170, 150), "DISK",
               diskPct, diskMain, diskSub);
}

// ============================================================
// USB Display (menu 8) - the TFT becomes a small Windows monitor.
// Frames come over the USB serial port from
// tools/usb_display/usb_display.py; touches go back as mouse input.
//
// PC -> ESP32:  "FRM1" + uint32 little-endian length + baseline JPEG
//               (length 0 = keepalive ping)
//               "FRM2" + uint32 LE length + uint16 LE x + uint16 LE y + JPEG:
//               tile with only the changed area, drawn at x,y (16 px grid)
//               "FRM3" + uint32 LE length + uint16 LE x, y, w, h + native data:
//               rect in the ILI9488 18-bit format, row by row, as 1-byte ops
//               (n = low 6 bits + 1):
//                 00nnnnnn  n pixels follow, 3 bytes each (R, G, B, top 6 bits)
//                 01nnnnnn  previous pixel n more times
//                 10nnnnnn  n pixels copied from the row above
//                 11iiiiii  one pixel from the colour cache slot i
//               Every literal pixel is stored in slot usbdNativeSlot(); the
//               cache starts black and nothing carries over between messages.
//               The PC does all the work; here it is byte copies and SPI.
// ESP32 -> PC:  one text line per event
//   READY,<w>,<h>,<max>,<proto>  app open, send a full frame
//                        (max = JPEG limit, proto = USBD_PROTOCOL)
//   ACK                  message handled, the next one may be sent
//   REFRESH              screen was drawn over, resend a full frame
//   ERR,<reason>[,...]   frame dropped (SIZE / TIMEOUT / JPEG / NATIVE)
//   T,<x>,<y>,<D|M|U>    touch down / move / up in screen pixels
//   BYE                  app closed, stop streaming
// The PC waits for ACK before every message, so the UART never overflows
// while a frame is drawn. With two buffers the ACK goes out as soon as a
// frame is received and the next frame streams in during the drawing.
// Exit: hold the top-left corner for 2 s, then lift the finger.
// ============================================================

// Frame buffer in PSRAM when present, otherwise internal RAM as long as
// USBD_HEAP_RESERVE stays free for WiFi/lwIP.
uint8_t* usbdAlloc(size_t size) {
  if (psramFound()) return (uint8_t*)ps_malloc(size);
  if (ESP.getMaxAllocHeap() < size || ESP.getFreeHeap() < size + USBD_HEAP_RESERVE) return nullptr;
  return (uint8_t*)malloc(size);
}

void usbdFreeBuffers() {
  for (int i = 0; i < 2; i++) { free(usbdBuf[i]); usbdBuf[i] = nullptr; }
  free(usbdRow);
  usbdRow      = nullptr;
  usbdBufCount = 0;
  usbdFrameMax = 0;
}

void usbdResetReceiver() {
  usbdRxState    = 0;
  usbdRxPos      = 0;
  usbdRxIdx      = 0;
  usbdRxBlocked  = false;
  usbdPendingIdx = -1;
  usbdDecIdx     = -1;
}

void usbdSendAck() { Serial.print("ACK\n"); }

void usbdSendTouch(int16_t x, int16_t y, char state) {
  Serial.printf("T,%d,%d,%c\n", x, y, state);
}

void usbdSendReady() {
  Serial.printf("READY,%d,%d,%lu,%d\n", tft.width(), tft.height(),
                (unsigned long)usbdFrameMax, USBD_PROTOCOL);
  usbdLastReadyAt = millis();
}

// 0 = nothing heard, 1 = PC talking, 2 = bytes but no valid message, 3 = no buffer
uint8_t usbdLinkStatus() {
  if (usbdBufCount == 0) return 3;
  unsigned long now = millis();
  if (usbdLastMsgAt && now - usbdLastMsgAt < USBD_LINK_TIMEOUT_MS) return 1;
  if (usbdLastRxAt  && now - usbdLastRxAt  < USBD_LINK_TIMEOUT_MS) return 2;
  return 0;
}

void usbdDrawStatus(uint8_t status) {
  static const char* const texts[] = {
    "Astept conexiunea...", "PC conectat, astept imaginea...",
    "Date invalide (baud diferit?)", "Memorie insuficienta"
  };
  tft.fillRect(0, 160, 480, 34, CLR_BG);
  tft.setTextFont(4);
  tft.setTextColor(status >= 2 ? tft.color565(235, 95, 75) : TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(texts[status], 240, 177);
  usbdStatusShown = status;
}

// Eight dots on a circle; the head is bright and the two behind it fade out
void usbdDrawSpinner() {
  for (int i = 0; i < 8; i++) {
    int age = (usbdSpinStep - i + 8) % 8;
    uint16_t c = (age == 0) ? CLR_USBDISP
               : (age == 1) ? tft.color565(85, 140, 50)
               : (age == 2) ? tft.color565(60, 90, 50) : CLR_DIM;
    float a = i * 0.7854f;  // 45 degrees
    tft.fillCircle(240 + (int)(cosf(a) * 30.0f), 108 + (int)(sinf(a) * 30.0f), 6, c);
  }
}

// Waiting screen: shown until the first frame and whenever the PC goes quiet
void drawUsbDisplayMenu() {
  tft.fillScreen(CLR_BG);
  tft.fillRect(0, 0, 480, 54, CLR_HDR);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("USB DISPLAY", 240, 27);
  tft.setTextFont(2);
  tft.setTextColor(CLR_USBDISP);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("< Inapoi", 12, 27);

  if (usbdBufCount > 0) usbdDrawSpinner();
  usbdDrawStatus(usbdLinkStatus());

  char info[64];
  if (usbdBufCount > 0)
    snprintf(info, sizeof(info), "%lu baud  |  buffer %u x %lu KB %s",
             (unsigned long)USBD_BAUD, (unsigned)usbdBufCount,
             (unsigned long)(usbdFrameMax / 1024), usbdBufInPsram ? "PSRAM" : "RAM");
  else
    snprintf(info, sizeof(info), "Heap liber %lu KB, bloc maxim %lu KB",
             (unsigned long)(ESP.getFreeHeap() / 1024),
             (unsigned long)(ESP.getMaxAllocHeap() / 1024));
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(CLR_LABEL);
  tft.drawString("Porneste tools/usb_display/usb_display.py pe PC", 240, 210);
  tft.setTextColor(tft.color565(110, 110, 165));
  tft.drawString(info, 240, 231);

  tft.fillRoundRect(20, 252, 440, 58, 10, CLR_CARD);
  tft.setTextColor(CLR_LABEL);
  tft.drawString("Iesire in timpul transmisiei:", 240, 270);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("tine apasat 2 s coltul stanga-sus, apoi ridica degetul", 240, 292);

  usbdShowingFrame = false;
  usbdHintShown    = false;
}

// Exit progress box drawn over the PC frame while the corner is held
void usbdDrawExitHint() {
  const int bx = 14, by = 30, bw = USBD_HINT_W - 28;
  if (!usbdHintShown) {
    tft.fillRoundRect(4, 4, USBD_HINT_W - 8, USBD_HINT_H - 8, 8, CLR_HDR);
    tft.drawRoundRect(4, 4, USBD_HINT_W - 8, USBD_HINT_H - 8, 8, CLR_USBDISP);
    tft.fillRect(bx, by, bw, 6, CLR_DIM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Iesire: tine apasat", bx, 17);
    usbdHintShown      = true;
    usbdHintArmedShown = false;
    usbdHintBarW       = 0;
  }
  if (usbdExitArmed) {
    if (usbdHintArmedShown) return;
    tft.fillRect(bx, 8, bw, 18, CLR_HDR);
    tft.setTextFont(2);
    tft.setTextColor(CLR_USBDISP);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Ridica degetul", bx, 17);
    tft.fillRect(bx, by, bw, 6, CLR_USBDISP);
    usbdHintArmedShown = true;
    return;
  }
  unsigned long held = usbdTouchLastDown - usbdTouchStartAt;
  int fw = (int)min((unsigned long)bw, held * bw / USBD_EXIT_HOLD_MS);
  if (fw > usbdHintBarW) {
    tft.fillRect(bx + usbdHintBarW, by, fw - usbdHintBarW, 6, CLR_USBDISP);
    usbdHintBarW = fw;
  }
}

void usbdHideExitHint() {
  if (!usbdHintShown) return;
  usbdHintShown = false;
  Serial.print("REFRESH\n");   // the PC repaints what the box covered
}

// A complete frame is queued for decode. If the other buffer is idle the PC
// may send the next frame right away; otherwise the ACK waits for the decode.
void usbdFrameReceived() {
  usbdPendingIdx  = usbdRxIdx;
  usbdPendingLen  = usbdRxLen;
  usbdPendingKind = usbdRxKind;
  memcpy(usbdPendingRect, usbdRxRect, sizeof(usbdPendingRect));
  int8_t other = (usbdBufCount == 2) ? (int8_t)(1 - usbdRxIdx) : -1;
  if (other >= 0 && other != usbdDecIdx) {
    usbdRxIdx = (uint8_t)other;
    usbdSendAck();
  } else {
    usbdRxBlocked = true;
  }
}

// Header confirmed: take the payload, or skip an oversized one (the magic
// hunt passes over its bytes)
void usbdAcceptHeader(unsigned long now) {
  usbdLastMsgAt = now;
  usbdRxPos     = 0;
  if (usbdRxLen > usbdFrameMax) {
    Serial.printf("ERR,SIZE,%lu,%lu\n", (unsigned long)usbdRxLen, (unsigned long)usbdFrameMax);
    usbdRxState = 0;
    usbdSendAck();
  } else if (usbdRxKind == 3) {
    usbdRxGot   = 0;
    usbdRxState = 3;
  } else {                                 // JPEG: the SOI already read goes first
    usbdBuf[usbdRxIdx][0] = 0xFF;
    usbdBuf[usbdRxIdx][1] = 0xD8;
    usbdRxGot   = 2;
    usbdRxState = 3;
  }
}

// Non-blocking receiver: "FRM1" + uint32 LE length + JPEG (length 0 = ping),
// "FRM2" adds the tile position, "FRM3" the native rect (see above).
// Resyncs on the magic after garbage, drops stalled and oversized frames.
void usbdPollSerial() {
  if (usbdBufCount == 0) {                 // no buffer: nothing can be accepted
    while (Serial.available() > 0) Serial.read();
    return;
  }
  unsigned long now = millis();
  if (usbdRxState != 0 && now - usbdLastRxAt > USBD_RX_TIMEOUT_MS) {
    usbdRxState = 0;
    usbdRxPos   = 0;
    Serial.print("ERR,TIMEOUT\n");
    usbdSendAck();
  }
  uint32_t budget = 8192;                  // bounded work per call (also runs inside decode)
  // A complete frame not yet handed to the decoder holds the queue: one more
  // (a small tile can arrive within a single call) would replace it unseen.
  while (!usbdRxBlocked && usbdPendingIdx < 0 && budget > 0) {
    int avail = Serial.available();
    if (avail <= 0) break;
    usbdLastRxAt = now;

    if (usbdRxState == 3) {                // payload goes straight into the frame buffer
      uint32_t want = min(min((uint32_t)avail, usbdRxLen - usbdRxGot), budget);
      uint32_t got  = Serial.read(usbdBuf[usbdRxIdx] + usbdRxGot, want);
      if (got == 0) break;
      usbdRxGot += got;
      budget    -= got;
      if (usbdRxGot < usbdRxLen) continue;
      usbdRxState = 0;
      usbdRxPos   = 0;
      const uint8_t* f = usbdBuf[usbdRxIdx];
      if (usbdRxKind == 3 || (f[usbdRxLen - 2] == 0xFF && f[usbdRxLen - 1] == 0xD9)) {
        usbdLastMsgAt = now;
        usbdFrameReceived();
      } else {                             // truncated or misaligned JPEG
        Serial.print("ERR,JPEG,EOI\n");
        usbdSendAck();
      }
      continue;
    }

    uint8_t c = (uint8_t)Serial.read();
    budget--;
    if (usbdRxState == 0) {                // hunt for "FRM1" / "FRM2" / "FRM3"
      if (usbdRxPos < 3 ? c == (uint8_t)"FRM"[usbdRxPos] : (c >= '1' && c <= '3')) {
        if (++usbdRxPos == 4) {
          usbdRxState = 1; usbdRxPos = 0; usbdRxLen = 0;
          usbdRxKind  = c - '0';
        }
      } else {
        usbdRxPos = (c == 'F') ? 1 : 0;
      }
    } else if (usbdRxState == 1) {         // uint32 little-endian length
      usbdRxLen |= (uint32_t)c << (8 * usbdRxPos);
      if (++usbdRxPos < 4) continue;
      usbdRxPos = 0;
      if (usbdRxLen == 0) {                // keepalive ping
        usbdRxState   = 0;
        usbdLastMsgAt = now;
        usbdSendAck();
      } else if (usbdRxKind != 3 && usbdRxLen < 4) {   // shorter than SOI+EOI: noise
        usbdRxState = 0;
      } else {
        usbdRxState = (usbdRxKind == 1) ? 2 : 4;
        memset(usbdRxRect, 0, sizeof(usbdRxRect));
      }
    } else if (usbdRxState == 4) {         // uint16 LE x, y (+ w, h for FRM3)
      usbdRxRect[usbdRxPos / 2] |= (uint16_t)c << (8 * (usbdRxPos % 2));
      if (++usbdRxPos < (usbdRxKind == 3 ? 8 : 4)) continue;
      usbdRxPos = 0;
      if (usbdRxKind == 2) {
        usbdRxState = 2;
      } else if (usbdRxRect[2] && usbdRxRect[3] &&   // a rect on screen confirms the header
                 usbdRxRect[0] + usbdRxRect[2] <= tft.width() &&
                 usbdRxRect[1] + usbdRxRect[3] <= tft.height()) {
        usbdAcceptHeader(now);
      } else {
        usbdRxState = 0;
      }
    } else {                               // SOI (FF D8) confirms a JPEG header
      if (c != (usbdRxPos == 0 ? 0xFF : 0xD8)) {
        usbdRxState = 0;
        usbdRxPos   = (c == 'F') ? 1 : 0;
        continue;
      }
      if (++usbdRxPos < 2) continue;
      usbdAcceptHeader(now);
    }
  }
}

// Touch -> "T,x,y,D|M|U". A touch that starts in the top-left corner is held
// back until it is clearly not the exit gesture (2 s hold + lift).
void usbdPollTouch() {
  unsigned long now = millis();
  if (now - usbdLastTouchPoll < USBD_TOUCH_POLL_MS) return;
  usbdLastTouchPoll = now;

  TS_Point p = ctp.getPoint();
  bool down = (p.z != 0);
  int16_t x = usbdTouchX, y = usbdTouchY;
  if (down) {
    long mx = map(p.y, 0, 480, 0, tft.width());
    long my = map(p.x, 0, 320, tft.height(), 0);
    x = (int16_t)constrain(mx, 0L, (long)tft.width()  - 1);
    y = (int16_t)constrain(my, 0L, (long)tft.height() - 1);
    usbdReleaseAt     = 0;
    usbdTouchLastDown = now;
  } else if (usbdTouchMode != USBD_T_IDLE) {
    // FT6206 sometimes reports one empty sample mid-drag: debounce the lift
    if (usbdReleaseAt == 0) usbdReleaseAt = now;
    if (now - usbdReleaseAt < USBD_RELEASE_MS) return;
    usbdReleaseAt = 0;
  }

  switch (usbdTouchMode) {
    case USBD_T_IDLE:
      if (!down) break;
      usbdTouchX0 = usbdTouchX = x;
      usbdTouchY0 = usbdTouchY = y;
      usbdTouchStartAt = now;
      if (!usbdShowingFrame) {
        usbdTouchMode = inRect(x, y, 0, 0, 160, 54) ? USBD_T_BACK : USBD_T_IGNORE;
      } else if (x < USBD_EXIT_ZONE && y < USBD_EXIT_ZONE) {
        usbdTouchMode = USBD_T_CORNER;
        usbdExitArmed = false;
      } else {
        usbdTouchMode = USBD_T_FORWARD;
        usbdSendTouch(x, y, 'D');
      }
      break;

    case USBD_T_FORWARD:
      if (!down) {
        usbdSendTouch(usbdTouchX, usbdTouchY, 'U');
        usbdTouchMode = USBD_T_IDLE;
      } else if (abs(x - usbdTouchX) >= USBD_MOVE_MIN_PX || abs(y - usbdTouchY) >= USBD_MOVE_MIN_PX) {
        usbdTouchX = x;
        usbdTouchY = y;
        usbdSendTouch(x, y, 'M');
      }
      break;

    case USBD_T_CORNER: {
      // Held past USBD_HINT_DELAY_MS the touch is an exit attempt, not a click
      bool exitGesture = (usbdTouchLastDown - usbdTouchStartAt >= USBD_HINT_DELAY_MS);
      if (down && x < USBD_EXIT_ZONE && y < USBD_EXIT_ZONE) {
        usbdTouchX = x;
        usbdTouchY = y;
        if (now - usbdTouchStartAt >= USBD_EXIT_HOLD_MS) usbdExitArmed = true;
      } else if (down) {                   // finger slid out of the corner
        if (exitGesture) {                 // exit cancelled, swallow the rest
          usbdExitArmed = false;
          usbdTouchMode = USBD_T_IGNORE;
        } else {                           // quick drag that began in the corner
          usbdSendTouch(usbdTouchX0, usbdTouchY0, 'D');
          usbdSendTouch(x, y, 'M');
          usbdTouchX    = x;
          usbdTouchY    = y;
          usbdTouchMode = USBD_T_FORWARD;
        }
      } else {                             // lifted
        if (usbdExitArmed) {
          usbdExitRequested = true;
        } else if (!exitGesture) {         // quick tap in the corner: normal click
          usbdSendTouch(usbdTouchX0, usbdTouchY0, 'D');
          usbdSendTouch(usbdTouchX0, usbdTouchY0, 'U');
        }
        usbdTouchMode = USBD_T_IDLE;
      }
      break;
    }

    case USBD_T_BACK:
      if (down) {
        usbdTouchX = x;
        usbdTouchY = y;
      } else {
        if (inRect(usbdTouchX, usbdTouchY, 0, 0, 160, 54)) usbdExitRequested = true;
        usbdTouchMode = USBD_T_IDLE;
      }
      break;

    default:                               // USBD_T_IGNORE
      if (!down) usbdTouchMode = USBD_T_IDLE;
      break;
  }
}

// ILI9488 bytes (R, G, B per pixel) to a window in one SPI burst; the caller
// holds tft.startWrite(). pushImage() would send them one byte at a time.
void usbdWriteNative(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint8_t* px) {
  tft.setWindow(x, y, x + w - 1, y + h - 1);
  tft.getSPIinstance().writeBytes(px, (uint32_t)w * h * 3);
}

// TJpgDec callback for USB Display frames: the RGB565 block becomes ILI9488
// bytes in usbdRow. It also services the serial link and the touch panel,
// because a full 480x320 decode takes a few hundred ms.
bool usbd_jpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  usbdPollSerial();
  usbdPollTouch();
  if (usbdExitRequested) return 0;         // abort, the app is closing
  if (y >= tft.height()) return 0;         // rest of the image is off screen
  if (x >= tft.width()) return 1;
  if (usbdHintShown && x < USBD_HINT_W && y < USBD_HINT_H) return 1;  // exit box stays on top
  uint16_t vw = min((int)w, tft.width() - x);
  uint16_t vh = min((int)h, tft.height() - y);
  uint8_t* d = usbdRow;
  for (uint16_t r = 0; r < vh; r++) {
    const uint16_t* s = bitmap + r * w;
    for (uint16_t c = 0; c < vw; c++) {
      uint16_t v = s[c];
      *d++ = (v >> 8) & 0xF8;
      *d++ = (v >> 3) & 0xFC;
      *d++ = v << 3;
    }
  }
  usbdWriteNative(x, y, vw, vh, usbdRow);
  return 1;
}

// Colour cache slot of a literal FRM3 pixel (same formula in usb_display.py)
uint8_t usbdNativeSlot(uint8_t r, uint8_t g, uint8_t b) {
  return ((r >> 2) * 3 + (g >> 2) * 5 + (b >> 2) * 7) & 63;
}

// Expand an FRM3 rect into usbdRow and send it to the TFT row by row (the
// row also serves the "copy from above" op). false = the data does not
// describe exactly w x h pixels, i.e. the stream is out of sync.
bool usbdDrawNative(const uint16_t* rect, const uint8_t* p, uint32_t len) {
  uint16_t x = rect[0], y = rect[1], w = rect[2], h = rect[3];
  uint8_t  cache[64 * 3];
  memset(cache, 0, sizeof(cache));
  uint8_t  r = 0, g = 0, b = 0;            // previous pixel
  uint32_t i = 0, left = (uint32_t)w * h;
  uint16_t col = 0, line = 0;
  while (left > 0) {
    if (i >= len) return false;
    uint8_t op   = p[i++];
    uint8_t kind = op >> 6;
    uint8_t n    = (kind == 3) ? 1 : (op & 0x3F) + 1;
    if (n > left || (kind == 0 && i + 3UL * n > len) || (kind == 2 && line == 0)) return false;
    left -= n;
    while (n--) {
      uint8_t* d = usbdRow + col * 3;
      if (kind == 0) {                     // literal pixel, remembered in the cache
        r = p[i]; g = p[i + 1]; b = p[i + 2];
        i += 3;
        uint8_t* s = cache + usbdNativeSlot(r, g, b) * 3;
        s[0] = r; s[1] = g; s[2] = b;
      } else if (kind == 2) {              // row above: not overwritten yet at this column
        r = d[0]; g = d[1]; b = d[2];
      } else if (kind == 3) {
        const uint8_t* s = cache + (op & 0x3F) * 3;
        r = s[0]; g = s[1]; b = s[2];
      }                                    // kind 1: previous pixel again
      d[0] = r; d[1] = g; d[2] = b;
      if (++col < w) continue;

      // Row complete: out it goes, except where the exit progress box is
      uint16_t skip = 0;
      if (usbdHintShown && y + line < USBD_HINT_H && x < USBD_HINT_W)
        skip = min(w, (uint16_t)(USBD_HINT_W - x));
      if (skip < w) usbdWriteNative(x + skip, y + line, w - skip, 1, usbdRow + skip * 3);
      col = 0;
      line++;
      usbdPollSerial();
      usbdPollTouch();
      if (usbdExitRequested) return true;  // abort quietly, the app is closing
    }
  }
  return i == len;
}

// Draw the queued frame straight onto the TFT (no clear, so no flicker)
void usbdDecodePending() {
  uint8_t  idx  = (uint8_t)usbdPendingIdx;
  uint32_t len  = usbdPendingLen;
  uint8_t  kind = usbdPendingKind;
  uint16_t rect[4];
  memcpy(rect, usbdPendingRect, sizeof(rect));
  usbdPendingIdx = -1;
  usbdDecIdx     = idx;

  // Tiles and native rects patch the frame on screen; over the waiting screen
  // they would leave garbage, so they are dropped and a full frame requested
  bool drop = kind != 1 && (!usbdShowingFrame || rect[0] >= tft.width() || rect[1] >= tft.height());
  bool ok   = true;
  int  res  = JDR_OK;
  if (drop) {
    // nothing drawn
  } else if (kind == 3) {
    tft.startWrite();
    ok = usbdDrawNative(rect, usbdBuf[idx], len);
    tft.endWrite();
  } else {
    uint16_t w = 0, h = 0;
    res = TJpgDec.getJpgSize(&w, &h, usbdBuf[idx], len);
    if (res == JDR_OK) {
      int16_t ox = rect[0], oy = rect[1];  // FRM2: tile position
      if (kind == 1) {
        // Frames smaller than the screen are centred (usb_display.py mirrors this)
        ox = (w < tft.width())  ? (tft.width()  - w) / 2 : 0;
        oy = (h < tft.height()) ? (tft.height() - h) / 2 : 0;
        if ((ox > 0 || oy > 0) && (!usbdShowingFrame || w != usbdFrameW || h != usbdFrameH))
          tft.fillScreen(TFT_BLACK);
        usbdFrameW = w;
        usbdFrameH = h;
      }
      TJpgDec.setJpgScale(1);
      TJpgDec.setCallback(usbd_jpg_output);
      TJpgDec.setSwapBytes(false);         // plain RGB565 for usbd_jpg_output()
      tft.startWrite();
      res = TJpgDec.drawJpg(ox, oy, usbdBuf[idx], len);
      tft.endWrite();
      TJpgDec.setSwapBytes(true);          // as setup() left it for the other apps
    }
    ok = (res == JDR_OK || res == JDR_INTR);  // INTR = clipped at the bottom, or exiting
  }
  usbdDecIdx = -1;

  // This buffer is free again: resume a receiver that was waiting for it
  if (usbdRxBlocked) {
    usbdRxIdx     = idx;
    usbdRxBlocked = false;
    usbdSendAck();
  }

  if (drop) {
    Serial.print("REFRESH\n");
  } else if (ok) {
    if (kind == 1) usbdShowingFrame = true;
  } else {
    if (kind == 3) Serial.print("ERR,NATIVE\n");    // the PC answers with a full frame
    else           Serial.printf("ERR,JPEG,%d\n", res);
    if (!usbdShowingFrame) drawUsbDisplayMenu();     // wipe a half-drawn first frame
  }
}

// Opened from the App Drawer: allocate buffers, turn Serial into the frame link
void usbDisplayEnter() {
  usbdBufInPsram = psramFound();
  // One row of ILI9488 pixels; also holds a converted 16x16 JPEG block
  usbdRow = usbdAlloc((tft.width() > 256 ? tft.width() : 256) * 3);
  for (usbdFrameMax = usbdBufInPsram ? USBD_FRAME_MAX_PSRAM : USBD_FRAME_MAX;
       usbdRow && usbdFrameMax >= USBD_FRAME_MIN; usbdFrameMax /= 2) {
    usbdBuf[0] = usbdAlloc(usbdFrameMax);
    if (!usbdBuf[0]) continue;
    usbdBuf[1] = usbdAlloc(usbdFrameMax);  // optional second buffer
    break;
  }
  usbdBufCount = usbdBuf[0] ? (usbdBuf[1] ? 2 : 1) : 0;
  if (usbdBufCount == 0) usbdFreeBuffers();

  // Serial becomes the frame link: faster, bigger RX buffer, no debug logs
  Serial.flush();
  Serial.end();
  Serial.setRxBufferSize(USBD_RX_BUF);
  Serial.begin(USBD_BAUD);

  usbdResetReceiver();
  usbdLastRxAt      = 0;
  usbdLastMsgAt     = 0;
  usbdLastReadyAt   = 0;               // READY goes out on the first loop pass
  usbdShowingFrame  = false;
  usbdFrameW        = 0;
  usbdFrameH        = 0;
  usbdTouchMode     = USBD_T_IGNORE;   // the tap that opened the app is still down
  usbdReleaseAt     = 0;
  usbdExitArmed     = false;
  usbdExitRequested = false;
  usbdHintShown     = false;
}

// Leave the app: stop the PC, restore Serial as setup() left it, free memory
void usbDisplayExit() {
  if (usbdTouchMode == USBD_T_FORWARD) usbdSendTouch(usbdTouchX, usbdTouchY, 'U');
  Serial.print("BYE\n");
  Serial.flush();
  Serial.end();
  Serial.setRxBufferSize(USBD_LOG_RX_BUF);
  Serial.begin(USBD_LOG_BAUD);

  usbdFreeBuffers();
  usbdResetReceiver();
  usbdTouchMode     = USBD_T_IDLE;
  usbdExitRequested = false;
  usbdHintShown     = false;
  menu = 6;
  drawManager(menu);
}

// Runs instead of the normal loop() body while menu == 8
void usbDisplayLoop() {
  usbdPollSerial();
  usbdPollTouch();
  if (!usbdExitRequested && usbdPendingIdx >= 0) usbdDecodePending();
  if (usbdExitRequested) { usbDisplayExit(); return; }

  unsigned long now = millis();
  // PC went quiet: release a held touch and go back to the waiting screen
  if (usbdShowingFrame && now - usbdLastMsgAt > USBD_LINK_TIMEOUT_MS) {
    if (usbdTouchMode == USBD_T_FORWARD) usbdSendTouch(usbdTouchX, usbdTouchY, 'U');
    if (usbdTouchMode != USBD_T_IDLE) usbdTouchMode = USBD_T_IGNORE;
    usbdExitArmed = false;
    usbdResetReceiver();
    drawUsbDisplayMenu();
    usbdLastReadyAt = 0;
  }

  if (usbdShowingFrame) {
    bool wantHint = (usbdTouchMode == USBD_T_CORNER) &&
                    (usbdTouchLastDown - usbdTouchStartAt >= USBD_HINT_DELAY_MS);
    if (wantHint) usbdDrawExitHint();
    else          usbdHideExitHint();
    return;
  }

  // Waiting screen: announce READY, animate the spinner, keep the status fresh
  if (usbdBufCount == 0) return;
  if (usbdRxState == 0 && usbdPendingIdx < 0 && now - usbdLastReadyAt >= USBD_READY_MS)
    usbdSendReady();
  if (now - usbdLastSpinAt >= 100) {
    usbdLastSpinAt = now;
    usbdSpinStep   = (usbdSpinStep + 1) % 8;
    usbdDrawSpinner();
  }
  uint8_t st = usbdLinkStatus();
  if (st != usbdStatusShown) usbdDrawStatus(st);
}

// ============================================================
// Draw manager
// ============================================================
void drawManager(int menuIndex) {
  switch (menuIndex) {
    case 0: drawMainMenu();     break;
    case 1: drawWLEDMenu();     break;
    case 2: drawMusicMenu();    break;
    case 3: drawSettingsMenu(); break;
    case 4: drawRacingMenu();   break;
    case 5: drawPCStatsMenu();  break;
    case 6: drawAppDrawer();    break;
    case 7: drawWeatherMenu();  break;
    case 8: drawUsbDisplayMenu(); break;
  }
}

// ============================================================
// Touch handlers
// ============================================================
void mainMenu(int touchX, int touchY) {
  if (inRect(touchX, touchY, WX_W_X, WX_W_Y, WX_W_W, WX_W_H)) {
    menu = 7; wxTab = 0; drawManager(menu); delay(50); return;
  }
  if (inRect(touchX, touchY, APPS_BTN_X, APPS_BTN_Y, APPS_BTN_W, APPS_BTN_H)) {
    menu = 6;
    animateDrawerIn();
    drawManager(menu);
    delay(50); return;
  }
}

// WLED: buttons send immediately; bar drags only update visuals and set
// wledNeedsSync — the actual HTTP call fires on finger-lift in loop().
void wledMenu(int touchX, int touchY) {
  // Back → App Drawer
  if (inRect(touchX, touchY, 0, 0, 160, 54)) {
    menu = 6; drawManager(menu); delay(50); return;
  }
  // ON/OFF
  if (inRect(touchX, touchY, 390, 9, 82, 36)) {
    toggleLEDState(); delay(50); return;
  }
  // Mode tabs — three equal thirds across WB_W
  {
    int third = WB_W / 3;
    if (inRect(touchX, touchY, WB_X, 60, third, 40)) {
      if (ledMode != 0) { ledMode = 0; updateLedMode(0); drawWledModeTabs(); }
      delay(50); return;
    }
    if (inRect(touchX, touchY, WB_X + third, 60, third, 40)) {
      if (ledMode != 1) { ledMode = 1; updateLedMode(1); drawWledModeTabs(); }
      delay(50); return;
    }
    if (inRect(touchX, touchY, WB_X + 2 * third, 60, WB_W - 2 * third, 40)) {
      if (ledMode != 2) {
        ledMode = 2;
        updateLedMode(0); // WLED in solid/static mode, we drive the colour
        lastMusicCoverTitle = ""; // force immediate colour push in loop
        // If we already have album colours loaded, send them right away
        if (mediaServerAvailable && mediaData.playing) {
          sendWLEDColor(dominantColors.r1, dominantColors.g1, dominantColors.b1);
          lastMusicCoverTitle    = mediaData.title;
          musicCoverStoppedAt    = 0;
          musicCoverFallbackSent = false;
        }
        drawWledModeTabs();
      }
      delay(50); return;
    }
  }

  // Hue bar
  if (inRect(touchX, touchY, WB_X, WB_HUE_Y, WB_W, WB_HUE_H)) {
    int col    = constrain(touchX - WB_X, 0, WB_W - 2);
    int newHue = col * 360 / WB_W;
    if (col != wledHueCol) {
      moveBarIndicator(WB_HUE_Y, WB_HUE_H, wledHueCol, col, hueColColor);
      wledHueCol = col;
      wledHue    = newHue;
      // Sat and bri bars depend on hue — redraw them, record new indicator cols
      drawPickerBar(WB_X, WB_SAT_Y, WB_W, WB_SAT_H, satColColor);
      wledSatCol = constrain((int)wledSat * WB_W / 255, 0, WB_W - 2);
      drawBarIndicator(WB_SAT_Y, WB_SAT_H, wledSatCol);
      drawPickerBar(WB_X, WB_BRI_Y, WB_W, WB_BRI_H, briColColor);
      wledBriCol = constrain((int)brightness * WB_W / 255, 0, WB_W - 2);
      drawBarIndicator(WB_BRI_Y, WB_BRI_H, wledBriCol);
      drawWledPreview();
      wledNeedsSync = true;
    }
    return;
  }

  // Saturation bar
  if (inRect(touchX, touchY, WB_X, WB_SAT_Y, WB_W, WB_SAT_H)) {
    int col        = constrain(touchX - WB_X, 0, WB_W - 2);
    uint8_t newSat = (uint8_t)(col * 255 / WB_W);
    if (col != wledSatCol) {
      moveBarIndicator(WB_SAT_Y, WB_SAT_H, wledSatCol, col, satColColor);
      wledSatCol = col;
      wledSat    = newSat;
      // Bri bar depends on sat — redraw it, record new indicator col
      drawPickerBar(WB_X, WB_BRI_Y, WB_W, WB_BRI_H, briColColor);
      wledBriCol = constrain((int)brightness * WB_W / 255, 0, WB_W - 2);
      drawBarIndicator(WB_BRI_Y, WB_BRI_H, wledBriCol);
      drawWledPreview();
      wledNeedsSync = true;
    }
    return;
  }

  // Brightness bar
  if (inRect(touchX, touchY, WB_X, WB_BRI_Y, WB_W, WB_BRI_H)) {
    int col        = constrain(touchX - WB_X, 0, WB_W - 2);
    uint8_t newBri = (uint8_t)(col * 255 / WB_W);
    if (col != wledBriCol) {
      moveBarIndicator(WB_BRI_Y, WB_BRI_H, wledBriCol, col, briColColor);
      wledBriCol = col;
      brightness = newBri;
      drawWledPreview();
      wledNeedsSync = true;
    }
    return;
  }
}

void musicMenu(int touchX, int touchY) {
  // Back → App Drawer
  if (inRect(touchX, touchY, 0, 0, 130, 50)) {
    musicSwipeStartX = -1; musicVolShowUntil = 0; musicSwipeDir = 0; musicSwipeRepeatAt = 0;
    menu = 6; drawManager(menu); delay(50); return;
  }
  if (isButtonPressed(previousButton, touchX, touchY)) {
    sendHTTPPOSTRequest(String(mediaServerUrl) + "/control/previous");
    delay(50); return;
  }
  if (isButtonPressed(pauseButton, touchX, touchY)) {
    sendHTTPPOSTRequest(String(mediaServerUrl) + "/control/toggle");
    delay(50); return;
  }
  if (isButtonPressed(nextButton, touchX, touchY)) {
    sendHTTPPOSTRequest(String(mediaServerUrl) + "/control/next");
    delay(50); return;
  }
}

void settingsMenu(int touchX, int touchY) {
  // Back → App Drawer
  if (inRect(touchX, touchY, 0, 0, 160, 68)) {
    menu = 6; drawManager(menu); delay(50); return;
  }
  if (isButtonPressed(tzMinusBtn, touchX, touchY)) {
    if (gmtOffsetHours > -12) gmtOffsetHours--;
    refreshSettingsTZDisplay(); delay(50); return;
  }
  if (isButtonPressed(tzPlusBtn, touchX, touchY)) {
    if (gmtOffsetHours < 14) gmtOffsetHours++;
    refreshSettingsTZDisplay(); delay(50); return;
  }
  if (isButtonPressed(syncBtn, touchX, touchY)) {
    syncNTP();
    delay(1500);
    refreshSettingsTZDisplay();
    delay(50); return;
  }
}

void racingMenu(int touchX, int touchY) {
  bool hasData = (racingGame == 0) ? acHasData : forzaHasData;

  // Back → App Drawer
  if (inRect(touchX, touchY, 0, 0, 160, 54)) {
    acDisconnect();
    menu = 6; drawManager(menu); delay(50); return;
  }
  // Reconnect button on no-signal screen
  if (!hasData && inRect(touchX, touchY, 150, 250, 180, 40)) {
    if (racingGame == 0) { acDisconnect(); acConnect(); }
    else { forzaHasData = false; resetRacingSentinels(); }
    drawRacingNoSignal((racingGame == 1) && forzaGameRunning);
    delay(50); return;
  }

  // Tab row (y=55–89)
  if (inRect(touchX, touchY, WB_X, 55, WB_W, 34)) {
    int third  = WB_W / 3;
    int rel    = touchX - WB_X;
    uint8_t newTab = (rel < third) ? 0 : (rel < 2 * third) ? 1 : 2;
    if (newTab != racingSubMenu) {
      racingSubMenu = newTab;
      resetRacingSentinels();
      tft.fillRect(0, 55, 480, 265, TFT_BLACK);   // clear tabs + content area to avoid artifacts
      drawRacingTabs();
      bool hd = (racingGame == 0) ? acHasData : (racingGame == 1 ? forzaHasData : beamngHasData);
      bool gr  = (racingGame == 1) ? forzaGameRunning : beamngGameRunning;
      if (!hd) {
        drawRacingNoSignal((racingGame != 0) && gr);
      } else {
        switch (racingSubMenu) {
          case 0: drawRacingRPMView(true);       break;
          case 1: drawRacingTelemetryView(true); break;
          case 2: drawRacingSetupView();         break;
        }
      }
    }
    delay(50); return;
  }

  // Setup sub-menu touch targets
  if (racingSubMenu == 2) {
    const int gBtnW = 120, gBtnH = 38, gBtnY = 115;
    // Switch to AC
    if (inRect(touchX, touchY, 25, gBtnY, gBtnW, gBtnH) && racingGame != 0) {
      forzaHasData     = false;
      forzaGameRunning = false;
      prevForzaGameRunning = false;
      resetRacingSentinels();
      racingGame = 0;
      drawRacingSetupView(); delay(50); return;
    }
    // Switch to FH6
    if (inRect(touchX, touchY, 25 + 126, gBtnY, gBtnW, gBtnH) && racingGame != 1) {
      if (racingGame == 0) acDisconnect();
      forzaHasData     = false;
      forzaGameRunning = false;
      prevForzaGameRunning = false;
      resetRacingSentinels();
      racingGame = 1;
      drawRacingSetupView(); delay(50); return;
    }
    // Switch to BeamNG (stub)
    if (inRect(touchX, touchY, 25 + 252, gBtnY, gBtnW, gBtnH) && racingGame != 2) {
      racingGame = 2; drawRacingSetupView(); delay(50); return;
    }

    if (isButtonPressed(rpmMinusBtn, touchX, touchY)) {
      if (acMaxRpm > 2000) acMaxRpm -= 500;
      drawRacingSetupView(); delay(50); return;
    }
    if (isButtonPressed(rpmPlusBtn, touchX, touchY)) {
      if (acMaxRpm < 20000) acMaxRpm += 500;
      drawRacingSetupView(); delay(50); return;
    }
    if (isButtonPressed(shiftMinusBtn, touchX, touchY)) {
      if (acShiftPct > 50) acShiftPct -= 5;
      drawRacingSetupView(); delay(50); return;
    }
    if (isButtonPressed(shiftPlusBtn, touchX, touchY)) {
      if (acShiftPct < 99) acShiftPct += 5;
      drawRacingSetupView(); delay(50); return;
    }
    if (isButtonPressed(acReconnectBtn, touchX, touchY)) {
      if (racingGame == 0) { acDisconnect(); acConnect(); }
      drawRacingSetupView(); delay(50); return;
    }
    if (isButtonPressed(revLightsBtn, touchX, touchY)) {
      wledRevLightsEnabled = !wledRevLightsEnabled;
      if (!wledRevLightsEnabled) sendWLEDBrightness(brightness);
      drawRacingSetupView(); delay(50); return;
    }
  }
}

// ============================================================
// App Drawer touch handler
// ============================================================
void appDrawerMenu(int touchX, int touchY) {
  // "< Home" — animate drawer closing
  if (inRect(touchX, touchY, 0, 0, 120, 54)) {
    menu = 0;
    animateDrawerOut();
    drawManager(menu);
    delay(50); return;
  }
  // Row 1: WLED | Music | Racing | USB Display
  if (inRect(touchX, touchY, AD_COL1_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H)) {
    menu = 1; drawManager(menu); delay(50); return;
  }
  if (inRect(touchX, touchY, AD_COL2_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H)) {
    menu = 2; drawManager(menu); delay(50); return;
  }
  if (inRect(touchX, touchY, AD_COL3_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H)) {
    menu = 4; racingSubMenu = 0; resetRacingSentinels(); drawManager(menu); delay(50); return;
  }
  if (inRect(touchX, touchY, AD_COL4_X, AD_ROW1_Y, AD_CARD_W, AD_CARD_H)) {
    menu = 8; usbDisplayEnter(); drawManager(menu); delay(50); return;
  }
  // Row 2: PC Stats | Settings | Weather
  if (inRect(touchX, touchY, AD_COL1_X, AD_ROW2_Y, AD_CARD_W, AD_CARD_H)) {
    menu = 5; drawManager(menu); delay(50); return;
  }
  if (inRect(touchX, touchY, AD_COL2_X, AD_ROW2_Y, AD_CARD_W, AD_CARD_H)) {
    menu = 3; drawManager(menu); delay(50); return;
  }
  if (inRect(touchX, touchY, AD_COL3_X, AD_ROW2_Y, AD_CARD_W, AD_CARD_H)) {
    menu = 7; wxTab = 0; drawManager(menu); delay(50); return;
  }
}

// PC Stats touch handler
void pcStatsMenu(int touchX, int touchY) {
  // "< Back" → App Drawer
  if (inRect(touchX, touchY, 0, 0, 160, 54)) {
    menu = 6; drawManager(menu); delay(50); return;
  }
}

// ============================================================
// Weather menu touch handler
// ============================================================
void weatherMenu(int touchX, int touchY) {
  // "< Inapoi" → App Drawer
  if (inRect(touchX, touchY, 0, 0, 160, 46)) {
    menu = 6; drawManager(menu); delay(50); return;
  }
  // Tab [DATE]
  if (inRect(touchX, touchY, 320, 8, 68, 28)) {
    if (wxTab != 0) { wxTab = 0; drawWeatherMenu(); }
    delay(50); return;
  }
  // Tab [GRAFIC]
  if (inRect(touchX, touchY, 396, 8, 76, 28)) {
    if (wxTab != 1) { wxTab = 1; drawWeatherMenu(); }
    delay(50); return;
  }
}

// ============================================================
// WLED Rev Lights
// ============================================================
void updateWledRevLights() {
  if (!wledRevLightsEnabled) return;
  bool hasData = (racingGame == 0) ? acHasData : forzaHasData;
  if (!hasData) return;
  unsigned long now = millis();
  if (now - lastRevLightUpdate < 80) return;  // ~12 fps max
  lastRevLightUpdate = now;

  float rpmVal   = (racingGame == 0) ? acData.engineRPM : (float)forzaData.rpm;
  int   rpmMaxV  = (racingGame == 0) ? acMaxRpm : (forzaData.rpmMax > 0 ? forzaData.rpmMax : acMaxRpm);
  float rpmFrac  = constrain(rpmVal / (float)rpmMaxV, 0.0f, 1.0f);
  int   shiftPt  = (int)(rpmMaxV * acShiftPct / 100.0f);
  bool  shifting = rpmVal >= shiftPt;

  // Flash logic: toggle revLightsOn every 80 ms when shifting
  if (shifting) {
    revLightsOn = !revLightsOn;
  } else {
    revLightsOn = true;
  }

  uint8_t r, g, b;
  if (!revLightsOn) {
    r = 0; g = 0; b = 0;
  } else if (rpmFrac < 0.6f) {
    r = 0;   g = 180; b = 0;
  } else if (rpmFrac < 0.8f) {
    r = 220; g = 200; b = 0;
  } else if (rpmFrac < 0.9f) {
    r = 255; g = 80;  b = 0;
  } else {
    r = 220; g = 0;   b = 0;
  }

  // Brightness tracks RPM fraction
  uint8_t bri = revLightsOn ? (uint8_t)(60 + rpmFrac * 195) : 0;

  if (r != prevRevR || g != prevRevG || b != prevRevB || revLightsOn != prevRevFlash) {
    if (revLightsOn) {
      sendWLEDColor(r, g, b);
      sendWLEDBrightness(bri);
    } else {
      sendWLEDBrightness(0);
    }
    prevRevR = r; prevRevG = g; prevRevB = b;
    prevRevFlash = revLightsOn;
  }
}

// ============================================================
// Setup & Loop
// ============================================================
void setup() {
  Serial.begin(115200);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BACKGROUND);
  TJpgDec.setSwapBytes(true);

  if (!ctp.begin(40)) {
    Serial.println("FT6206 not found!");
    while (1);
  }

  syncNTP();
  delay(1000);
  drawManager(menu);
}

unsigned long lastUpdate = 0;
const unsigned long refreshInterval = 1000;

void loop() {
  // USB Display owns the loop while open: its serial link cannot wait for the
  // blocking HTTP polls below (weather, Music Cover); they resume on exit.
  if (menu == 8) { usbDisplayLoop(); return; }

  unsigned long now = millis();

  if (now - lastUpdate >= refreshInterval) {
    lastUpdate = now;
    if (menu == 0) drawMainMenu(true);
    if (menu == 2) drawMusicMenu(true);
  }

  // Dismiss volume indicator after 1.5 s (only clears the overlay zone, no full redraw)
  if (menu == 2 && musicVolShowUntil > 0 && millis() > musicVolShowUntil) {
    musicVolShowUntil = 0;
    tft.fillRect(130, 0, 260, 50, TFT_WHITE);  // erase overlay; clock zone (X≥390) untouched
  }

  // PC Stats — 2-second refresh
  if (menu == 5 && now - lastPCStatsUpdate >= PC_STATS_INTERVAL_MS) {
    lastPCStatsUpdate = now;
    drawPCStatsMenu(true);
  }

  // Music Cover mode — keep WLED colour synced to album art
  if (ledMode == 2) {
    // Poll media when not in the music menu (music menu already polls every 1 s)
    if (menu != 2 && menu != 4 && now - lastMusicCoverPoll >= 10000) {
      lastMusicCoverPoll = now;
      mediaData = getMediaInfo();
      if (mediaServerAvailable && mediaData.playing &&
          mediaData.title != lastMusicCoverTitle) {
        dominantColors = getAlbumColors(); // fetch new album colours
      }
    }

    if (mediaServerAvailable && mediaData.playing) {
      musicCoverStoppedAt = 0;
      if (mediaData.title != lastMusicCoverTitle) {
        // New track — push album colour
        sendWLEDColor(dominantColors.r1, dominantColors.g1, dominantColors.b1);
        lastMusicCoverTitle    = mediaData.title;
        musicCoverFallbackSent = false;
      } else if (musicCoverFallbackSent) {
        // Music resumed after fallback — restore album colour
        sendWLEDColor(dominantColors.r1, dominantColors.g1, dominantColors.b1);
        musicCoverFallbackSent = false;
      }
    } else {
      if (musicCoverStoppedAt == 0) musicCoverStoppedAt = now;
      if (!musicCoverFallbackSent && now - musicCoverStoppedAt >= 5000) {
        // 5 s of silence — fall back to solid HSV colour
        uint8_t r, g, b;
        getWledRGB(r, g, b);
        sendWLEDColor(r, g, b);
        musicCoverFallbackSent = true;
      }
    }
  }

  // AC UDP — poll every iteration; connect only when AC game is selected in menu 4
  acPollUdp();
  if (menu == 4 && racingGame == 0 && !acConnected) {
    acConnect();
  }
  // Re-send SUBSCRIBE every 2s to keep AC streaming
  if (acConnected && menu == 4 && racingGame == 0 && millis() - acLastSubscribe > 2000) {
    acTelemetry.sendUpdate();
    acLastSubscribe = millis();
  }
  if (acHasData && millis() - acLastPacket > 5000) {
    acHasData = false;
    resetRacingSentinels();
    if (menu == 4) drawRacingNoSignal();
    if (acConnected) {
      acTelemetry.sendHandshake();
      acTelemetry.sendUpdate();
      acLastSubscribe = millis();
    }
  }

  // Forza HTTP polling — 100ms interval when in Racing menu with FH6 selected
  if (menu == 4 && racingGame == 1 && now - lastForzaFetch >= FORZA_FETCH_MS) {
    lastForzaFetch = now;
    forzaFetchData();
  }

  // BeamNG HTTP polling — 100ms interval when in Racing menu with BeamNG selected
  if (menu == 4 && racingGame == 2 && now - lastBeamngFetch >= FORZA_FETCH_MS) {
    lastBeamngFetch = now;
    beamngFetchData();
  }

  // Weather polling — every 60s regardless of menu; update home widget if on menu 0
  if (lastWxFetch == 0 || now - lastWxFetch >= WEATHER_FETCH_MS) {
    lastWxFetch = now;
    fetchWeatherData();
    if (menu == 0) drawHomeWeatherWidget();
  }

  // Racing display refresh (10 fps) — unified for AC, FH6, BeamNG
  if (menu == 4) {
    bool hasData      = (racingGame == 0) ? acHasData : (racingGame == 1 ? forzaHasData : beamngHasData);
    bool gameRunning  = (racingGame == 1) ? forzaGameRunning : (racingGame == 2 ? beamngGameRunning : false);
    bool prevGameRun  = (racingGame == 1) ? prevForzaGameRunning : prevBeamngGameRunning;

    if (hasData && !prevAcHasData) {
      // Data just arrived (race started) — full redraw to clear "Waiting" screen
      prevAcHasData = true;
      prevForzaGameRunning  = forzaGameRunning;
      prevBeamngGameRunning = beamngGameRunning;
      lastRacingRefresh = 0;
      drawRacingMenu(false);
    } else if (hasData && racingSubMenu < 2 && now - lastRacingRefresh >= RACING_REFRESH_MS) {
      lastRacingRefresh = now;
      drawRacingMenu(true);
    } else if (!hasData) {
      bool wasRaceActive = prevAcHasData;
      prevAcHasData = false;
      bool fgr = (racingGame != 0) && gameRunning;
      if (wasRaceActive || (racingGame != 0 && gameRunning != prevGameRun)) {
        prevForzaGameRunning  = forzaGameRunning;
        prevBeamngGameRunning = beamngGameRunning;
        drawRacingNoSignal(fgr);
      }
    }
  }

  // WLED Rev Lights update
  if (menu == 4) updateWledRevLights();

  // Vinyl animation tick — ~0.3 rev/s (≈ 33 RPM), runs at 80 ms intervals
  if (menu == 2 && albumLoaded && prevIsActuallyPlaying) {
    if (now - lastVinylUpdate >= 120) {
      lastVinylUpdate = now;
      vinylAngle += 0.10f;
      if (vinylAngle >= 6.2832f) vinylAngle -= 6.2832f;
      drawVinylFrame(21, 71);
    }
  }

  bool nowTouching = ctp.touched();

  if (nowTouching) {
    TS_Point p = ctp.getPoint();
    uint16_t x = map(p.y, 0, 480, 0, tft.width());
    uint16_t y = map(p.x, 0, 320, tft.height(), 0);

    // Music top-bar volume swipe (zone X>130, Y<54 — avoids Back button)
    bool swipeBlocked = false;
    if (menu == 2 && y < 54) {
      if (x > 130) {
        if (!wasTouching) musicSwipeStartX = x;
        musicSwipeLastX = x;
      }
      if (musicSwipeStartX != -1) {
        swipeBlocked = true;
        if (musicSwipeDir == 0) {
          int delta = (int)musicSwipeLastX - (int)musicSwipeStartX;
          if (abs(delta) > 40) {
            musicSwipeDir = (delta > 0) ? 1 : -1;
            bool up = (musicSwipeDir > 0);
            float newVol = sendVolumeRequest(up);
            if (newVol >= 0.0f) musicVolLevel = newVol;
            else musicVolLevel = constrain(musicVolLevel + (up ? 0.05f : -0.05f), 0.0f, 1.0f);
            drawMusicVolIndicator(musicVolLevel);
            musicVolShowUntil  = millis() + 1500;
            musicSwipeRepeatAt = millis() + 250;
          }
        } else {
          if (millis() >= musicSwipeRepeatAt) {
            bool up = (musicSwipeDir > 0);
            float newVol = sendVolumeRequest(up);
            if (newVol >= 0.0f) musicVolLevel = newVol;
            else musicVolLevel = constrain(musicVolLevel + (up ? 0.05f : -0.05f), 0.0f, 1.0f);
            drawMusicVolIndicator(musicVolLevel);
            musicVolShowUntil  = millis() + 1500;
            musicSwipeRepeatAt = millis() + 250;
          }
        }
      }
    }

    if (!swipeBlocked) {
      switch (menu) {
        case 0: mainMenu(x, y);       break;
        case 1: wledMenu(x, y);       break;
        case 2: musicMenu(x, y);      break;
        case 3: settingsMenu(x, y);   break;
        case 4: racingMenu(x, y);     break;
        case 5: pcStatsMenu(x, y);    break;
        case 6: appDrawerMenu(x, y);  break;
        case 7: weatherMenu(x, y);    break;
      }
    }
    wasTouching = true;

  } else {
    // Finger lifted — reset swipe state (commands were already sent during hold)
    if (wasTouching && menu == 2 && musicSwipeStartX != -1) {
      musicSwipeStartX   = -1;
      musicSwipeLastX    = -1;
      musicSwipeDir      = 0;
      musicSwipeRepeatAt = 0;
    }
    // Flush any pending WLED colour change
    if (wasTouching && menu == 1 && wledNeedsSync) {
      uint8_t r, g, b;
      getWledRGB(r, g, b);
      sendWLEDColor(r, g, b);
      sendWLEDBrightness(brightness);
      wledNeedsSync = false;
    }
    wasTouching = false;
  }
}
