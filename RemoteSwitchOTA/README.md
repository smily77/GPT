# RemoteSwitchOTA_DC

Kombination aus Garagentor-Sender (DoorSENDER) und ESP-NOW-Schalter (ControllerOTA/ActorOTA) mit OTA-Update-Modus. Beide neuen Sketche nutzen dieselbe `doorLockData.h`, die **nicht** eingecheckt wird; als Vorlage dient `DoorControl/doorLockDataExample.h`.

## Struktur
```
RemoteSwitchOTA/
├─ ControllerOTA_DC/ControllerOTA_DC.ino   // Kombinierter Tür-Sender + Schalter-Controller
├─ ActorOTA_DC/ActorOTA_DC.ino             // Schalt-Aktor mit HMAC-gesicherter Steuerung
└─ README.md                               // Dieses Dokument
```

## Geheimnisse
- Kopiere `DoorControl/doorLockDataExample.h` als `doorLockData.h` in **beide** Sketch-Ordner und ersetze die MACs/Keys/OTA-Zugangsdaten durch deine echten Werte.
- `CONTROLLER_COMBINED_MAC` muss dem Sender 1 in `SENDER_SECRETS` entsprechen.
- `ACTOR_MAC` und `ACTOR_KEY` sichern die Schaltbefehle zwischen Controller und Actor.
- `RECEIVER_MAC` sowie `SENDER_SECRETS` dienen weiter dem Garagentor-Handshake.

## ControllerOTA_DC Verhalten
- Standardanzeige: `"-"` im Idle, `"Link"` bei validierter Tür-Session, `"Denay"` bei Ablehnung.
- Kurz-Tastendruck (<2 s):
  - Mit Tür-Link: sendet OPEN an den DoorRECEIVER.
  - Ohne Tür-Link: sendet einen HMAC-gesicherten TOGGLE-Befehl an den Actor.
- Lang-Tastendruck (>=2 s): wechselt in den OTA-Modus (WLAN STA, `ArduinoOTA`).
- MAC-Adresse wird aus `CONTROLLER_COMBINED_MAC` gesetzt, der WiFi-Kanal aus `WIFI_CHANNEL`.

## ActorOTA_DC Verhalten
- Erwartet Befehle ausschließlich von `CONTROLLER_COMBINED_MAC`.
- Prüft HMAC (`ACTOR_KEY`) und Nonce, toggelt dann `RELAY_PIN` und bestätigt mit ACK.
- Optionaler OTA-Start über `OTA_TRIGGER_PIN` (default -1/deaktiviert).
- Relais-Status kann mit `STATUS_LED_PIN` gespiegelt werden (hier einfacher GPIO-Ausgang).

## OTA-Hinweise
- Hinterlege `OTA_WIFI_SSID` und `OTA_WIFI_PASSWORD` in `doorLockData.h`, falls OTA genutzt wird.
- Während OTA wird ESP-NOW beendet; nach Neustart ist die Funksteuerung wieder aktiv.

## Sonstiges
- Der vorhandene DoorRECEIVER-Sketch bleibt unverändert nutzbar; er erkennt den kombinierten Controller wie den bisherigen DoorSENDER.
- Achte auf identischen `WIFI_CHANNEL` in allen beteiligten Geräten.
