# 🎨 TFT Touch WLED Controller

**TFT Touch WLED Controller** este un controller IoT cu ecran tactil, bazat pe ESP32, care permite controlul unui dispozitiv **WLED** prin rețea WiFi. Interfața grafică rulează pe un ecran TFT touchscreen și oferă control rapid asupra **culorii** și **luminozității** LED-urilor RGB.

> Proiect educațional orientat pe UI embedded, comunicație HTTP și integrare cu ecosistemul smart home (WLED).

---

## 📦 Funcționalități principale

- 🎨 Selectare culoare RGB prin butoane tactile
- 🔆 Control al luminozității LED-urilor (BRIGHT+ / BRIGHT−)
- 📡 Comunicare prin HTTP cu server WLED local
- 🖥️ Interfață grafică pe ecran TFT 480×320 px
- 👆 Control complet touch (FT6206 capacitive touchscreen)
- ⚡ Răspuns rapid, fără aplicații externe
- 🌐 Integrare ușoară în rețea locală (LAN)

---

## 🛠️ Componente Hardware

| Componentă          | Descriere                                                                 |
|--------------------|---------------------------------------------------------------------------|
| **ESP32**          | Microcontroller principal cu WiFi integrat                               |
| **TFT 480×320**    | Ecran color cu driver TFT_eSPI                                            |
| **FT6206**         | Controller touchscreen capacitiv (I2C)                                   |
| **Server WLED**    | Controller LED RGB compatibil HTTP API                                   |
| **Rețea WiFi**     | Comunicație între ESP32 și WLED                                           |

---

## 💻 Software și Librării

- **Limbaj:** C++ (Arduino)
- **Platformă:** Arduino IDE
- **Librării utilizate:**
  - `TFT_eSPI` – randare grafică pe TFT
  - `Adafruit_FT6206` – citire touch capacitiv
  - `WiFi.h` – conectare WiFi
  - `HTTPClient.h` – request-uri HTTP
- **Fișier configurare:** `secrets.h` (SSID + parolă WiFi)

---

## ☁️ Integrare WLED (HTTP API)

Controllerul trimite comenzi HTTP direct către WLED:

- 🎨 Setare culoare:
```

/win&R=<r>&G=<g>&B=<b>

```
- 🔆 Setare luminozitate:
```

/win&A=<brightness>

````

Toate comenzile sunt trimise în rețeaua locală, fără cloud extern.

---

## 🖥️ Interfață grafică

* Butoane colorate predefinite (RED, GREEN, BLUE, AQUA, etc.)
* Buton WHITE pentru reset rapid
* Două butoane dedicate pentru control luminozitate
* Layout optimizat pentru touch (butoane mari, colțuri rotunjite)
* Corecție software pentru rotația ecranului

---

## 🔮 Dezvoltări viitoare

* 🌈 Color picker RGB/HSV dinamic
* 🏠 Meniu principal cu mai multe module (WLED, Media, Smart Home)
* 💾 Salvare preset-uri de culoare
* 🎵 Sincronizare cu muzica (WLED Sound Reactive)
* 🌐 Control WLED prin MQTT
* 📱 Integrare Home Assistant

---

## 👨‍💻 Autor

* **Radu Gabriel Claudiu** – arhitectură software, UI TFT, comunicație HTTP, integrare WLED
