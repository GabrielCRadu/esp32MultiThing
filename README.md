# 🎨 TFT Touch WLED Controller (ESP32 TouchDown v1.1)

**TFT Touch WLED Controller** este un controller IoT cu ecran tactil, bazat pe **ESP32 TouchDown v1.1**, care permite controlul unui dispozitiv **WLED** prin rețea WiFi. Interfața grafică rulează direct pe ecranul TFT integrat și oferă control rapid asupra **culorii** și **luminozității** LED-urilor RGB.

> Proiect educațional orientat pe UI embedded, comunicație HTTP și integrare cu ecosistemul smart home (WLED), folosind o placă all-in-one cu display și touch.

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

| Componentă                  | Descriere                                                                 |
|----------------------------|---------------------------------------------------------------------------|
| **ESP32 TouchDown v1.1**   | Placă ESP32 all-in-one cu WiFi, TFT și touchscreen integrat              |
| **TFT 480×320**            | Ecran color integrat, compatibil `TFT_eSPI`                               |
| **FT6206**                 | Controller touchscreen capacitiv (I2C), integrat pe placă                |
| **Server WLED**            | Controller LED RGB compatibil HTTP API                                    |
| **Rețea WiFi**             | Comunicație locală între ESP32 TouchDown și WLED                          |

---

## 💻 Software și Librării

- **Limbaj:** C++ (Arduino)
- **Platformă:** Arduino IDE
- **Librării utilizate:**
  - `TFT_eSPI` – randare interfață grafică pe TFT
  - `Adafruit_FT6206` – citire touch capacitiv
  - `WiFi.h` – conectare la rețea WiFi
  - `HTTPClient.h` – trimitere comenzi HTTP
- **Fișier configurare:** `secrets.h`  
  (conține SSID-ul și parola rețelei WiFi)

---

## ☁️ Integrare WLED (HTTP API)

Controllerul trimite comenzi HTTP direct către instanța WLED din rețeaua locală:

- 🎨 Setare culoare RGB
- 🔆 Setare luminozitate globală

Toate comenzile sunt trimise **local**, fără cloud extern, pentru latență minimă și control complet.

---

## 🖥️ Interfață grafică

- Butoane colorate predefinite (RED, GREEN, BLUE, AQUA, etc.)
- Buton WHITE pentru reset rapid al culorii
- Două butoane dedicate pentru controlul luminozității
- Layout optimizat pentru touch (butoane mari, colțuri rotunjite)
- Corecție software pentru rotația și maparea coordonatelor touch

---

## 🔮 Dezvoltări viitoare

- 🌈 Color picker RGB / HSV dinamic
- 🏠 Meniu principal cu mai multe module (WLED, Media Controller, Smart Home)
- 💾 Salvare și recall preset-uri de culoare
- 🎵 Integrare WLED Sound Reactive
- 🌐 Control WLED prin MQTT
- 📱 Integrare cu Home Assistant

---

## 👨‍💻 Autor

- **Radu Gabriel Claudiu** – arhitectură software, UI TFT, logică touch, comunicație HTTP, integrare WLED
