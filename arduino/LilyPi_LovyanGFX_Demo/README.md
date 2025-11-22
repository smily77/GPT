# Lily Pi LovyanGFX Demo (Arduino)

Beispielsketch für den LilyGO **Lily Pi** (ILI9481 + GT911) mit der Arduino IDE.
Lege den kompletten Ordner `LilyPi_LovyanGFX_Demo` in deinen Arduino-Sketchbook-Pfad,
so dass die `LilyPi_LovyanGFX_Demo.ino` im gleichnamigen Verzeichnis liegt.

## Voraussetzungen
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX) installiert.
- ESP32-Board-Unterstützung in der Arduino IDE.
- Board-Auswahl: ESP32 (Lily Pi basiert auf einem ESP32 mit externem PSRAM).

## Pinbelegung (anpassbar)
Die Standardpins sind im Sketch dokumentiert und lassen sich bei Bedarf ändern:
- Display (SPI): SCLK=18, MOSI=19, DC=27, CS=5, RST=33, BL=32
- Touch (I2C): SDA=21, SCL=22, INT=39, RST=38

Wenn dein Lily Pi oder dein Panel abweicht, passe die Pins im Konstruktor der
`LGFX_LilyPi` Klasse entsprechend an. Reduziere ggf. `freq_write`, falls das Display
Artefakte zeigt.

## Verhalten
- Beim Start wird ein Farbverlauf gezeichnet und ein Hinweis zum Antippen angezeigt.
- Jeder Touchpunkt wird mit einem gelben Kreis markiert und in einer Liste mit
  Koordinaten ausgegeben.
- Das Backlight wird über PWM eingeschaltet (`setBrightness(220)`).

Viel Spaß beim Experimentieren!
