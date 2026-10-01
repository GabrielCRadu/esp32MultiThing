# USB Display - ecranul ESP32 ca monitor secundar

`usb_display.py` captureaza un monitor Windows, il trimite ca JPEG pe portul serial USB
catre aplicatia **USB DISPLAY** de pe ESP32 si transforma atingerile de pe ecran in
click-uri / drag cu mouse-ul pe acel monitor.

## 1. Monitor virtual (Virtual Display Driver, IDD)

Ca sa ai un monitor "in plus" pe care sa-l trimiti la ESP32:

1. Descarca ultima versiune de la <https://github.com/VirtualDrivers/Virtual-Display-Driver/releases>
   si ruleaza installerul (instaleaza driverul si aplicatia de control).
2. Adauga rezolutia **480x320** (exact rezolutia ecranului, fara scalare). In versiunile
   recente lista de rezolutii e in `C:\VirtualDisplayDriver\vdd_settings.xml` (sau din
   aplicatia de control); versiunile vechi foloseau `C:\IddSampleDriver\option.txt`.
   Dupa modificare reporneste driverul (Device Manager > Display adapters >
   Virtual Display Driver > Disable / Enable).
3. Windows > Settings > System > Display: alege monitorul virtual, rezolutie 480x320,
   Scale 100%, "Extend these displays", apoi aseaza-l unde vrei fata de monitorul principal.

Daca Windows nu accepta 480x320, foloseste 960x640 (exact 2x) si porneste scriptul cu
`--monitor N`; imaginea se micsoreaza, deci textul va fi mic.

## 2. Instalare

```bash
cd tools/usb_display
pip install -r requirements.txt
```

## 3. Pornire

1. Pe ESP32: **APPS > USB DISPLAY**. Apare "Astept conexiunea...".
2. Pe PC (inchide inainte Serial Monitor din Arduino IDE, portul poate fi folosit de un
   singur program):

```bash
python usb_display.py                        # port si monitor 480x320 detectate automat
python usb_display.py --port COM3 --monitor 2
python usb_display.py --list                 # monitoare si porturi disponibile
```

| Optiune | Implicit | Ce face |
|---|---|---|
| `--port` | auto (CP210x/CH34x/FTDI) | portul serial al ESP32 |
| `--baud` | 921600 | trebuie sa fie egal cu `USBD_BAUD` din `esp32MultiThing.ino` |
| `--monitor` | monitorul de 480x320 | indexul din `--list` |
| `--width`, `--height` | din ESP32 (480x320) | marimea cadrului trimis |
| `--fps` | 5 | maxim de cadre pe secunda |
| `--quality` | 70 | calitate JPEG 20-95 (scade automat daca un cadru nu incape) |
| `--rotate` | 0 | roteste captura in sensul acelor de ceas: 0/90/180/270 |

Scriptul se reconecteaza singur daca portul dispare (cablu scos, ESP32 resetat).

## 4. Utilizare

- Atingere = click stanga, atingere + miscare = drag.
- **Iesire:** tine apasat 2 s coltul stanga-sus (apare "Iesire: tine apasat"), apoi ridica
  degetul. O atingere scurta in colt ajunge normal la PC. Pe ecranul de asteptare merge si
  "< Inapoi".
- Cat timp aplicatia e deschisa, portul serial e folosit pentru imagine: nu apar loguri de
  debug, iar meteo si WLED Music Cover sunt puse pe pauza (revin la iesire).

## Performanta

Dupa primul cadru complet se trimit doar blocurile 16x16 care s-au schimbat, grupate in
cateva dreptunghiuri. Pentru fiecare dreptunghi PC-ul alege formatul care ajunge mai
repede pe ecran:

- **nativ** (fara pierderi): pixelii exact in formatul ILI9488, comprimati simplu
  (repetari, copiere din randul de sus, 64 de culori recente). ESP32 doar copiaza octeti
  pe SPI, deci textul si interfetele apar clare si repede;
- **JPEG**: pentru poze si video, unde formatul nativ ar fi prea mare pentru cablu.

Cand ecranul sta neschimbat, zonele trimise ca JPEG sunt retrimise in format nativ, ca
sa devina clare. Cursorul mouse-ului e desenat de script peste imagine (captura Windows
nu il contine).

Placa are un CP2102 (maxim ~921600 baud, ~92 KB/s), deci cablul ramane limita la
schimbarile mari. `--fps 10` reduce intarzierea la schimbarile mici.

## Probleme

- **"Date invalide (baud diferit?)"** pe ESP32: `--baud` nu e egal cu `USBD_BAUD`.
- **"cannot open COMx"**: portul e deschis in alt program (Serial Monitor).
- **Click-urile nimeresc langa tinta**: monitorul virtual trebuie sa aiba Scale 100% si
  aceeasi orientare ca imaginea; verifica si `--rotate`.

## Protocol (pentru referinta)

- PC -> ESP32: `"FRM1"` + lungime uint32 little-endian + JPEG baseline (lungime 0 = ping);
  `"FRM2"` + lungime uint32 + x, y uint16 little-endian + JPEG = dreptunghi desenat la x,y;
  `"FRM3"` + lungime uint32 + x, y, w, h uint16 + date native (formatul e descris in
  sectiunea USB Display din `esp32MultiThing.ino`).
- ESP32 -> PC, cate o linie: `READY,<w>,<h>,<max>,<proto>` (proto 3 = accepta `FRM2` si `FRM3`),
  `ACK`, `REFRESH`, `ERR,...`, `T,<x>,<y>,<D|M|U>`, `BYE`. PC-ul asteapta `ACK` dupa
  fiecare mesaj.
