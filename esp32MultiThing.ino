#include <TFT_eSPI.h>
#include <Adafruit_FT6206.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include "secrets.h"
#include "images.h"

TFT_eSPI tft = TFT_eSPI();
Adafruit_FT6206 ctp = Adafruit_FT6206();

// --- WLED server ---
const char* serverName = "http://192.168.1.135"; // pentru WLED

// --- Media server ---
const char* mediaServerUrl = "http://192.168.1.136:8000"; // pentru FastAPI / JSON

unsigned long lastTime = 0;
unsigned long timerDelay = 5000;

#define TFT_BACKGROUND TFT_BLACK
#define MAX_BRIGHTNESS 255

struct ColorButton {
  int x, y, w, h, radius;
  uint16_t color;
  const char* label;
  uint8_t r, g, b;
};

struct Button {
  int x, y, w, h, radius;
  uint16_t color;
  const char* label;
};

uint8_t brightness = MAX_BRIGHTNESS;
uint8_t menu = 0;
int ledMode = 0;

const char* mode0 = "Solid";
const char* mode1 = "AmbiWLED";

Button toWledMenu  = { 128, 242, 80, 70, 12, tft.color565(186,186,186), "WLED"};
Button toMusicMenu = { 216, 242, 80, 70, 12, tft.color565(186,186,186), "Music"};

Button brightnessButtons[] = {
  { 216, 242, 80, 70, 12, tft.color565(186,186,186), "BRIGHT-"},
  { 304, 242, 80, 70, 12, tft.color565(186,186,186), "BRIGHT+"}
};

Button toggleLEDButton  = { 128, 242, 80,  70, 12, tft.color565(186,186,186), "ON/OFF"};
Button modeSwitchButton = { 8,   242, 100, 70, 12, tft.color565(186,186,186), "MODE"};
Button backButton       = { 8,   8,   100, 70, 12, tft.color565(186,186,186), "BACK"};

Button previousButton = { 123, 245, 50, 50};
Button pauseButton    = { 215, 245, 50, 50};
Button nextButton     = { 307, 245, 50, 50};

ColorButton colorButtons[] = {
  { 216,  8,  80, 70, 12, tft.color565(255,0,0), "", 255,0,0 },
  { 304,  8,  80, 70, 12, tft.color565(255,128,0), "", 255,128,0 },
  { 392,  8,  80, 70, 12, tft.color565(255,217,0), "", 255,217,0 },
  { 216, 86,  80, 70, 12, tft.color565(0,170,255), "", 0,170,255 },
  { 304, 86,  80, 70, 12, tft.color565(0,255,183), "", 0,255,183 },
  { 392, 86,  80, 70, 12, tft.color565(0,255,30), "", 0,255,0 },
  { 216, 164, 80, 70, 12, tft.color565(38,0,255), "", 0,0,255 },
  { 304, 164, 80, 70, 12, tft.color565(153,0,255), "", 153,0,255 },
  { 392, 164, 80, 70, 12, tft.color565(255,0,191), "", 255,0,191 },
  { 392, 242, 80, 70, 12, tft.color565(255,255,255), "", 255,255,255 }
};

const int BRIGHT_BTN_COUNT = sizeof(brightnessButtons) / sizeof(brightnessButtons[0]);
const int BTN_COUNT = sizeof(colorButtons) / sizeof(colorButtons[0]);

// --- MediaInfo struct si variabila globala ---
struct MediaInfo {
  bool playing;
  String title;
  String artist;
  String album;
  int duration;
  int position;
  String status;
};

MediaInfo mediaData;

// --- Callback pentru TJpg_Decoder ---
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

// --- Functia care descarca si afiseaza thumbnail ---
void downloadAndDisplayThumbnail(int x, int y) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    tft.fillRect(x, y, 128, 128, TFT_BLACK);
    return;
  }

  HTTPClient http;
  String url = String(mediaServerUrl) + "/media/thumbnail";
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    int len = http.getSize();
    
    if (len > 0) {
      uint8_t* buffer = (uint8_t*)malloc(len);
      if (buffer) {
        int bytesRead = 0;
        while (http.connected() && bytesRead < len) {
          size_t available = stream->available();
          if (available) {
            int c = stream->readBytes(buffer + bytesRead, available);
            bytesRead += c;
          }
          yield();
        }
        
        TJpgDec.setJpgScale(1);
        TJpgDec.setCallback(tft_output);
        TJpgDec.drawJpg(x, y, buffer, bytesRead);
        
        free(buffer);
      } else {
        Serial.println("Nu s-a putut aloca memoria pentru thumbnail");
        tft.fillRect(x, y, 128, 128, TFT_BLACK);
      }
    }
  } else {
    Serial.print("HTTP error pentru thumbnail: ");
    Serial.println(httpCode);
    tft.fillRect(x, y, 128, 128, TFT_BLACK);
  }

  http.end();
}

// --- Functia care citeste media ---
MediaInfo getMediaInfo() {
  MediaInfo info;
  info.playing = false;
  info.title = "";
  info.artist = "";
  info.album = "";
  info.duration = 0;
  info.position = 0;
  info.status = "";

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = String(mediaServerUrl) + "/media";
    http.begin(url);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      String payload = http.getString();
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        info.playing = doc["playing"];
        info.title = String((const char*)doc["title"]);
        info.artist = String((const char*)doc["artist"]);
        info.album = String((const char*)doc["album"]);
        info.duration = doc["duration"];
        info.position = doc["position"];
        info.status = String((const char*)doc["status"]);
      } else {
        Serial.print("Eroare JSON: "); Serial.println(error.c_str());
      }
    } else {
      Serial.print("HTTP error: "); Serial.println(httpResponseCode);
    }

    http.end();
  }

  return info;
}

// --- Functii HTTP ---
void sendHTTPRequest(const String& params) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }

  HTTPClient http;
  String url = String(serverName) + params;
  http.begin(url);
  int code = http.GET();

  if (code > 0) {
    Serial.println(http.getString());
  } else {
    Serial.print("HTTP error: "); Serial.println(code);
  }
  http.end();
}

void sendHTTPPOSTRequest(const String& url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }

  HTTPClient http;
  http.begin(url);
  int code = http.POST("");

  if (code > 0) {
    Serial.println(http.getString());
  } else {
    Serial.print("HTTP error: "); Serial.println(code);
  }
  http.end();
}

void sendWLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  sendHTTPRequest("/win&R=" + String(r) + "&G=" + String(g) + "&B=" + String(b));
}

void sendWLEDBrightness(uint8_t brightness) {
  sendHTTPRequest("/win&A=" + String(brightness));
}

void toggleLEDState() {
  sendHTTPRequest("/win&T=2");
}

void updateLedMode(int ledMode) {
  if (ledMode == 0) {
    sendHTTPRequest("/win&PL=1");
  } else if (ledMode == 1) {
    sendHTTPRequest("/win&PL=2");
  }
}

// --- Functii de desenare meniuri ---
void drawButton(const Button& btn, const char* customLabel = nullptr) {
  tft.fillRoundRect(btn.x, btn.y, btn.w, btn.h, btn.radius, btn.color);
  tft.setTextColor(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(customLabel ? customLabel : btn.label,
                 btn.x + btn.w / 2,
                 btn.y + btn.h / 2);
}

void drawColorButton(const ColorButton& btn) {
  tft.fillRoundRect(btn.x, btn.y, btn.w, btn.h, btn.radius, btn.color);
  tft.setTextColor(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(btn.label, btn.x + btn.w / 2, btn.y + btn.h / 2);
}

void drawBitmapTransparent(int x, int y, int w, int h, const uint16_t *bitmap, uint16_t transparentColor) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint16_t color = bitmap[j * w + i];
            if (color != transparentColor) {
                tft.drawPixel(x + i, y + j, color);
            }
        }
    }
}


void drawMainMenu() {
  tft.fillScreen(TFT_BACKGROUND);
  tft.setTextFont(1);

  drawButton(toWledMenu);
  drawButton(toMusicMenu);
}

void drawWLEDMenu() {
  tft.fillScreen(TFT_BACKGROUND);
  tft.setTextFont(1);
  
  drawButton(backButton);
  drawButton(toggleLEDButton);
  drawButton(modeSwitchButton, ledMode == 0 ? mode0 : mode1);
  
  for (int i = 0; i < BRIGHT_BTN_COUNT; i++) {
    drawButton(brightnessButtons[i]);
  }
  
  for (int i = 0; i < BTN_COUNT; i++) {
    drawColorButton(colorButtons[i]);
  }
}


String oldTitle;
String oldArtist;

void drawMusicMenu(bool refresh = false) {

  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(TL_DATUM);
  mediaData = getMediaInfo();
  
  if (refresh == false){

    tft.fillRect(0, 0, 480, 50, TFT_WHITE);
    tft.fillRect(0, 50, 480, 170, tft.color565(49,59,61));
    tft.fillRect(0, 220, 480, 100, tft.color565(33,44,47));

    drawBitmapTransparent(123, 245, 50, 50, previousIcon, 0x0000);
    drawBitmapTransparent(215, 245, 50, 50, pauseIcon,    0x0000);
    drawBitmapTransparent(307, 245, 50, 50, nextIcon,     0x0000);
    downloadAndDisplayThumbnail(21, 71);
  }

  if(oldTitle != mediaData.title || oldArtist != mediaData.artist) {
    tft.fillRect(149, 71, 331, 128, tft.color565(49,59,61));
    downloadAndDisplayThumbnail(21, 71);
  }

  tft.setTextFont(4);
  tft.drawString(mediaData.title, 170, 100);
  tft.drawString(mediaData.artist, 170, 142);

  oldTitle = mediaData.title;
  oldArtist = mediaData.artist;
}

void drawManager(int menuIndex) {
  switch (menuIndex) {
    case 0:
      drawMainMenu();
      break;
    case 1:
      drawWLEDMenu();
      break;
    case 2:
      drawMusicMenu();
      break;
  }
}

// --- Functii touch ---
bool isButtonPressed(const Button& btn, int touchX, int touchY) {
  return (touchX > btn.x && touchX < btn.x + btn.w &&
          touchY > btn.y && touchY < btn.y + btn.h);
}

bool isColorButtonPressed(const ColorButton& btn, int touchX, int touchY) {
  return (touchX > btn.x && touchX < btn.x + btn.w &&
          touchY > btn.y && touchY < btn.y + btn.h);
}

void mainMenu(int touchX, int touchY) {
  if (isButtonPressed(toWledMenu, touchX, touchY)) {
    menu = 1;
    drawManager(menu);
    delay(150);
    return;
  }
  if (isButtonPressed(toMusicMenu, touchX, touchY)) {
    menu = 2;
    drawManager(menu);
    delay(150);
    return;
  }
}

void wledMenu(int touchX, int touchY) {
  for (int i = 0; i < BTN_COUNT; i++) {
    if (isColorButtonPressed(colorButtons[i], touchX, touchY)) {
      sendWLEDColor(colorButtons[i].r, colorButtons[i].g, colorButtons[i].b);
      delay(150);
      return;
    }
  }

  if (isButtonPressed(brightnessButtons[0], touchX, touchY)) {
    brightness = (brightness <= 25) ? 0 : brightness - 25;
    sendWLEDBrightness(brightness);
    delay(150);
    return;
  }

  if (isButtonPressed(brightnessButtons[1], touchX, touchY)) {
    brightness = (brightness >= MAX_BRIGHTNESS - 25) ? MAX_BRIGHTNESS : brightness + 25;
    sendWLEDBrightness(brightness);
    delay(150);
    return;
  }

  if (isButtonPressed(backButton, touchX, touchY)) {
    menu = 0;
    drawManager(menu);
    delay(150);
    return;
  }

  if (isButtonPressed(modeSwitchButton, touchX, touchY)) {
    ledMode = 1 - ledMode;
    updateLedMode(ledMode);
    drawButton(modeSwitchButton, ledMode == 0 ? mode0 : mode1);
    delay(150);
    return;
  }

  if (isButtonPressed(toggleLEDButton, touchX, touchY)) {
    toggleLEDState();
    delay(150);
    return;
  }
}

void musicMenu(int touchX, int touchY) {
  if (isButtonPressed(backButton, touchX, touchY)) {
    menu = 0;
    drawManager(menu);
    delay(150);
    return;
  }

  if(isButtonPressed(previousButton, touchX, touchY)){
    sendHTTPPOSTRequest(String(mediaServerUrl) + "/control/previous");
    delay(150);
    return;
  }

  if(isButtonPressed(pauseButton, touchX, touchY)){
    sendHTTPPOSTRequest(String(mediaServerUrl) + "/control/toggle");
    delay(150);
    return;
  }

  if(isButtonPressed(nextButton, touchX, touchY)){
    sendHTTPPOSTRequest(String(mediaServerUrl) + "/control/next");
    delay(150);
    return;
  }
}

// --- Setup & Loop ---
void setup() {
  Serial.begin(115200);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to WiFi network with IP Address: ");
  Serial.println(WiFi.localIP());

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BACKGROUND);

  TJpgDec.setSwapBytes(true);

  if (!ctp.begin(40)) {
    Serial.println("FT6206 touchscreen nu a fost gasit!");
    while (1);
  }

  drawManager(menu);
}

unsigned long lastUpdate = 0;
const unsigned long refreshInterval = 1000; // 1000 ms = 1 sec

void loop() {

  unsigned long now = millis();

  if (now - lastUpdate >= refreshInterval) {
    lastUpdate = now;
    if(menu == 2)drawMusicMenu(true);
  }

  if (ctp.touched()) {
    TS_Point p = ctp.getPoint();

    uint16_t x = map(p.y, 0, 480, 0, tft.width());
    uint16_t y = map(p.x, 0, 320, tft.height(), 0);

    switch (menu) {
      case 0:
        mainMenu(x, y);
        break;
      case 1:
        wledMenu(x, y);
        break;
      case 2:
        musicMenu(x, y);
        break;
    }
  }
}