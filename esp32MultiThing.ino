#include <TFT_eSPI.h>
#include <Adafruit_FT6206.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

TFT_eSPI tft = TFT_eSPI();                // Ecran TFT
Adafruit_FT6206 ctp = Adafruit_FT6206();  // Touch I2C

String serverName = "http://192.168.1.135";  //Your Domain name with URL path or IP address with path

unsigned long lastTime = 0;
unsigned long timerDelay = 5000; // 5 sec

#define TFT_BACKGROUND TFT_BLACK

struct ColorButton {
  int x, y, w, h, radius;
  uint16_t color;
  const char* label;
  uint8_t r, g, b;
};

struct Button{
  int x, y, w, h, radius;
  uint16_t color;
  const char* label;
};



#define MAX_BRIGHTNESS 255
uint8_t brightness = MAX_BRIGHTNESS;

Button brightnessButtons[] = {
  { 216, 242, 80, 70, 12, tft.color565(186,186,186), "BRIGHT-"},
  { 304, 242, 80, 70, 12, tft.color565(186,186,186), "BRIGHT+"}
};

Button toggleLEDButton = { 128, 242, 80, 70, 12, tft.color565(186,186,186), "ON/OFF"};

Button backButton = { 8, 8, 100, 70, 12, tft.color565(186,186,186), "BACK"};

Button modeSwitchButton = { 8, 242, 100, 70, 12, tft.color565(186,186,186), "Placeholder, check the code below"};

const char* mode0 = "Solid";
const char* mode1 = "AmbiWLED";

int ledMode = 0; //(0-normal, 1=abilight)

ColorButton colorButtons[] = {
  { 216,  8,  80, 70, 12, tft.color565(255,0,0), "RED", 255,0,0 },
  { 304,  8,  80, 70, 12, tft.color565(255,128,0), "ORANGE", 255,128,0 },
  { 392,  8,  80, 70, 12, tft.color565(255,217,0), "YELLOW", 255,217,0 },
  { 216, 86,  80, 70, 12, tft.color565(0,170,255), "LIGHTBLUE", 0,170,255 },
  { 304, 86,  80, 70, 12, tft.color565(0,255,183), "AQUA", 0,255,183 },
  { 392, 86,  80, 70, 12, tft.color565(0,255,30), "GREEN", 0,255,0 },
  { 216, 164, 80, 70, 12, tft.color565(38,0,255), "BLUE", 0,0,255 },
  { 304, 164, 80, 70, 12, tft.color565(153,0,255), "VIOLET", 153,0,255 },
  { 392, 164, 80, 70, 12, tft.color565(255,0,191), "PINK", 255,0,191 },
  { 392, 242, 80, 70, 12, tft.color565(255,255,255), "WHITE", 255,255,255 }
};

const int BRIGHT_BTN_COUNT = sizeof(brightnessButtons) / sizeof(brightnessButtons[0]);
const int BTN_COUNT = sizeof(colorButtons) / sizeof(colorButtons[0]);


//==============================================================================================0. MAIN ==============================================================================================
void drawMainMenu() {
  
}

void mainMenu(int touchX, int touchY) {

}
//==============================================================================================0. MAIN ==============================================================================================

//==============================================================================================1. WLED ==============================================================================================
void sendWLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }

  HTTPClient http;
  String url = serverName + "/win&R=" + r + "&G=" + g + "&B=" + b;

  http.begin(url);
  int code = http.GET();

  if (code > 0) {
    Serial.println(http.getString());
  } else {
    Serial.print("HTTP error: ");
    Serial.println(code);
  }

  http.end();
}

void sendWLEDBrightness(uint8_t brightness) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }

  HTTPClient http;
  String url = serverName + "/win&A=" + brightness;

  http.begin(url);
  int code = http.GET();

  if (code > 0) {
    Serial.println(http.getString());
  } else {
    Serial.print("HTTP error: ");
    Serial.println(code);
  }

  http.end();
}

void toggleLEDState(){
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }

  HTTPClient http;
  String url = serverName + "/win&T=2";

  http.begin(url);
  int code = http.GET();

  if (code > 0) {
    Serial.println(http.getString());
  } else {
    Serial.print("HTTP error: ");
    Serial.println(code);
  }

  http.end();
}

void updateLedMode(int ledMode){

  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi Disconnected");
      return;
  }

  if(ledMode==0){

    HTTPClient http;
    String url = serverName + "/win&PL=1";

    http.begin(url);
    int code = http.GET();

    if (code > 0) {
      Serial.println(http.getString());
    } else {
     Serial.print("HTTP error: ");
      Serial.println(code);
    }

    http.end();
  }

  if(ledMode==1){
    
    HTTPClient http;
    String url = serverName + "/win&PL=2";

    http.begin(url);
    int code = http.GET();

    if (code > 0) {
      Serial.println(http.getString());
    } else {
     Serial.print("HTTP error: ");
      Serial.println(code);
    }

    http.end();
  }
}

void drawWLEDMenu() {

  tft.fillScreen(TFT_BACKGROUND);
  tft.setTextDatum(MC_DATUM);
  
  //Back button
    tft.fillRoundRect(
      backButton.x,
      backButton.y,
      backButton.w,
      backButton.h,
      backButton.radius,
      backButton.color);

    tft.setTextColor(TFT_BLACK);
    tft.drawString(
      backButton.label,
      backButton.x + backButton.w / 2,
      backButton.y + backButton.h / 2);
  //Back button

  //ON/OFF button
    tft.fillRoundRect(
      toggleLEDButton.x,
      toggleLEDButton.y,
      toggleLEDButton.w,
      toggleLEDButton.h,
      toggleLEDButton.radius,
      toggleLEDButton.color);

    tft.setTextColor(TFT_BLACK);
    tft.drawString(
      toggleLEDButton.label,
      toggleLEDButton.x + toggleLEDButton.w / 2,
      toggleLEDButton.y + toggleLEDButton.h / 2);
  //ON/OFF button

  //Mode switch button
    tft.fillRoundRect(
      modeSwitchButton.x,
      modeSwitchButton.y,
      modeSwitchButton.w,
      modeSwitchButton.h,
      modeSwitchButton.radius,
      modeSwitchButton.color);

    tft.setTextColor(TFT_BLACK);

    if(ledMode == 0){
      tft.drawString(
        mode0,
        modeSwitchButton.x + modeSwitchButton.w / 2,
        modeSwitchButton.y + modeSwitchButton.h / 2);

    }

    if(ledMode == 1){
      tft.drawString(
        mode1,
        modeSwitchButton.x + modeSwitchButton.w / 2,
        modeSwitchButton.y + modeSwitchButton.h / 2);

    }

  //Mode switch button


  for (int i = 0; i < BRIGHT_BTN_COUNT; i++) {
    tft.fillRoundRect(
      brightnessButtons[i].x,
      brightnessButtons[i].y,
      brightnessButtons[i].w,
      brightnessButtons[i].h,
      brightnessButtons[i].radius,
      brightnessButtons[i].color);

    tft.setTextColor(TFT_BLACK);
    tft.drawString(
      brightnessButtons[i].label,
      brightnessButtons[i].x + brightnessButtons[i].w / 2,
      brightnessButtons[i].y + brightnessButtons[i].h / 2);
  }


  for (int i = 0; i < BTN_COUNT; i++) {
    tft.fillRoundRect(
      colorButtons[i].x,
      colorButtons[i].y,
      colorButtons[i].w,
      colorButtons[i].h,
      colorButtons[i].radius,
      colorButtons[i].color);

    tft.setTextColor(TFT_BLACK);
    tft.drawString(
      colorButtons[i].label,
      colorButtons[i].x + colorButtons[i].w / 2,
      colorButtons[i].y + colorButtons[i].h / 2);
  }
}

void wledMenu(int touchX, int touchY) {//================================================================TOUCH=========================================================

  //butoane culori
  for (int i = 0; i < BTN_COUNT; i++) {
    if (touchX > colorButtons[i].x && touchX < colorButtons[i].x + colorButtons[i].w &&
        touchY > colorButtons[i].y && touchY < colorButtons[i].y + colorButtons[i].h) {
          sendWLEDColor(colorButtons[i].r, colorButtons[i].g, colorButtons[i].b);
          delay(150);  // debounce
          return;
    }
  }

  //brightness -
  if (touchX > brightnessButtons[0].x && touchX < brightnessButtons[0].x + brightnessButtons[0].w &&
      touchY > brightnessButtons[0].y && touchY < brightnessButtons[0].y + brightnessButtons[0].h) {
          if(brightness <= 25)brightness = 0;
          else brightness -= 25;

          sendWLEDBrightness(brightness);
        delay(150);  // debounce
        return;
  }

  //brightness +
  if (touchX > brightnessButtons[1].x && touchX < brightnessButtons[1].x + brightnessButtons[1].w &&
      touchY > brightnessButtons[1].y && touchY < brightnessButtons[1].y + brightnessButtons[1].h) {
          if(brightness >= MAX_BRIGHTNESS-25)brightness = MAX_BRIGHTNESS;
          else brightness += 25;

          sendWLEDBrightness(brightness);
        delay(150);  // debounce
        return;
  }

  //back
  if (touchX > backButton.x && touchX < backButton.x + backButton.w &&
      touchY > backButton.y && touchY < backButton.y + backButton.h) {
        //menu=0;
        delay(150);  // debounce
        return;
  }

  //mode switch
  if (touchX > modeSwitchButton.x && touchX < modeSwitchButton.x + modeSwitchButton.w &&
      touchY > modeSwitchButton.y && touchY < modeSwitchButton.y + modeSwitchButton.h) {
        if(ledMode == 1){
          ledMode = 0;
          updateLedMode(ledMode);
        }
        if(ledMode == 0){
          updateLedMode(ledMode);
          ledMode = 1;
        }
        delay(150);  // debounce
        return;
  }


  //on/off
  if (touchX > toggleLEDButton.x && touchX < toggleLEDButton.x + toggleLEDButton.w &&
      touchY > toggleLEDButton.y && touchY < toggleLEDButton.y + toggleLEDButton.h) {
        toggleLEDState();
        delay(150);  // debounce
        return;
  }
}

//==============================================================================================1. WLED ==============================================================================================
uint8_t menu = 1;

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

  if (!ctp.begin(40)) {  // sensibilitate
    Serial.println("FT6206 touchscreen nu a fost gasit!");
    while (1);
  }

  drawWLEDMenu();
}

void loop() {
  if (ctp.touched()) {
    TS_Point p = ctp.getPoint();

    // Corectie flip pentru setRotation(1)
    uint16_t x = map(p.y, 0, 480, 0, tft.width());   // swap X/Y
    uint16_t y = map(p.x, 0, 320, tft.height(), 0);  // inversare Y

    switch (menu) {
      case 0:
        mainMenu(x, y);
        break;
      case 1:
        wledMenu(x, y);
        break;
    }
  }
}
