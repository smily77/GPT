# ESP32 BNO055 Hand Compass

Dieses Projekt implementiert einen Touch-Handkompass für ESP32/ESP32-S3 Boards. Ein Bosch BNO055 IMU-Sensor wird per I²C ausgelesen und die Kompassanzeige erfolgt über ein TFT-Display, das mit [LovyanGFX](https://github.com/lovyan03/LovyanGFX) angesteuert wird.

## Struktur
```
esp32-bno055-hand-compass/
├─ src/
│  ├─ HandCompass_BNO055.ino
│  ├─ CompassUI.h / .cpp
│  ├─ BNO055Manager.h / .cpp
│  └─ CYD_Display_Config.h   // Wrapper, eigentliche Datei liefert der Nutzer
├─ lib/        // leer, für optionale Libraries
├─ README.md
└─ .gitignore
```

## Voraussetzungen
- Arduino IDE mit ESP32-Board-Unterstützung (ESP32 oder ESP32-S3).
- Bibliotheken:
  - `Adafruit BNO055`
  - `Adafruit Unified Sensor`
  - `Preferences` (Teil der ESP32-Platform)
  - `LovyanGFX`
- Eine benutzerdefinierte `CYD_Display_Config.h`, die die passende `LGFX`-Displayklasse bereitstellt.

## Verwendung
1. Repository klonen oder als ZIP laden.
2. `CYD_Display_Config.h` durch die eigene Display-Konfiguration ersetzen bzw. ergänzen.
3. Projekt in der Arduino IDE öffnen (`src/HandCompass_BNO055.ino`).
4. Board auswählen (ESP32 oder ESP32-S3) und die gewünschten I²C-Pins setzen. Standard ist SDA=21, SCL=22; bei Bedarf können `extSDA`/`extSCL` definiert werden.
5. Kompilieren und flashen.

## Funktionsumfang
- Windrose mit hervorgehobener Nordmarkierung sowie digitale Anzeige aus Grad und Richtungstext.
- Kalibrierstatusanzeige (System/Gyro/Accel/Mag) und Warnhinweis, wenn die Kalibrierung im Normalbetrieb für länger als 5 Sekunden schlecht ist.
- Sanfter Heading-Filter (Exponentialsmoothing mit Wrap-Around).
- Automatische Kalibrierung beim ersten Start: Sobald stabile Vollkalibrierung erreicht ist, werden Offsets im NVS (`bno055cal`) gespeichert und bei künftigen Starts geladen.
- Manuelle Kalibrierung: Über den Touch-Button **CALIBRATE** kann eine erneute Kalibrierung ausgelöst werden.

## Kalibrierung
- **Automatisch**: Wenn keine Offsets im NVS liegen, wird automatisch kalibriert. Bewegen Sie das Gerät in alle Achsen, bis alle Werte `3` erreichen; bleiben Sie anschließend kurz ruhig, damit die Offsets gespeichert werden.
- **Manuell**: Tippen Sie auf **CALIBRATE**, um erneut zu kalibrieren. Die UI zeigt Hinweise, wann die Offsets gespeichert werden.

## Hinweise
- Die Datei `CYD_Display_Config.h` im Repo ist nur ein Include-Wrapper; bringen Sie Ihre eigene Display-Implementierung mit.
- Das Projekt ist für Arduino ausgelegt, lässt sich aber auch in PlatformIO nutzen, solange die abhängigen Bibliotheken installiert sind.
