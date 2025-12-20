# DoorControl Benutzeranleitung

## Überblick
Dieses Projekt besteht aus einem ESP-NOW-Sender (DoorSENDER) mit OLED und Taster sowie einem ESP-NOW-Empfänger (DoorRECEIVER) mit Relais und Status-LED-Pixel. Beide Geräte nutzen ein Challenge-Response-Verfahren mit HMAC-SHA256, ohne persistenten Speicher, um Garage-Öffnen-Befehle abzusichern.

## Sicherheitshinweise
- **Geheime Schlüssel und MACs gehören in `doorLockData.h`, nicht in Git.** Kopiere `doorLockDataExample.h` zu `doorLockData.h`, trage eigene MACs und 32-Byte-Schlüssel ein und halte die Datei lokal.
- Jeder Sender hat einen eigenen Schlüssel; kompromittiert ein Gerät, kompromittiert nur diesen Sender.
- Bewahre die Geräte physisch sicher auf; ein Angreifer mit Firmwarezugriff kann den Schlüssel extrahieren.

## Konfiguration vor dem Flashen
1. **WLAN-Kanal**: Setze `WIFI_CHANNEL` in `doorLockData.h` und nutze denselben Wert für Sender und Empfänger.
2. **MACs/Schlüssel eintragen**: Fülle `RECEIVER_MAC` und alle Einträge in `SENDER_SECRETS[]` in `doorLockData.h` (MAC + 32-Byte-Key pro Sender).
3. **Sender-ID**: Weise jedem Sender eine eindeutige `sender_id` (0–255) zu und setze `SENDER_ID` nur im DoorSENDER-Sketch auf den passenden Eintrag.
4. **Pins prüfen**:
   - Sender: Button an GPIO7 (INPUT_PULLUP, aktiv low), SDA=GPIO8, SCL=GPIO9, optional LED an GPIO6 (wird aktuell low gesetzt).
   - Receiver: Relais an `RELAY_PIN`, WS2812-Pixel an GPIO8.

## Einen weiteren Sender hinzufügen
1. Wähle eine neue `sender_id` und 32-Byte-Schlüssel.
2. Ergänze in `doorLockData.h` einen neuen Eintrag in `SENDER_SECRETS[]` mit `sender_id`, Sender-MAC und Schlüssel.
3. Setze im neuen Sender-Sketch dieselbe `sender_id` (Konstante `SENDER_ID`).
4. Kompiliere und flashe beide Geräte. Stelle sicher, dass alle Sender denselben `WIFI_CHANNEL` verwenden.

## Bedienung
- **OLED-Anzeige (Sender)**: zeigt „-“, „Link“ oder „Denay“.
- **Button**: Bei „Link“ wird ein OPEN gesendet; sonst zeigt der Sender „-“.
- **Status-Pixel (Receiver)**: Aus = kein Kontakt, Grün = gültiger Kontakt in letzter Zeit, Blau = Relais aktiv.

## Fehlersuche
- Prüfe, dass die MAC-Adressen exakt mit der Allowlist übereinstimmen und der Kanal korrekt ist.
- Wenn „-“ nicht zu „Link“ wird, Receiver einschalten, Reichweite prüfen und Schlüssel/MAC/ID vergleichen.
- Serielle Logs zeigen MACs, Sendestatus und abgelehnte Pakete.

## Wartungsempfehlungen
- Schlüssel regelmäßig rotieren (alle 6–12 Monate oder bei Verdacht auf Kompromittierung).
- Firmware-Updates offline vorbereiten, prüfen, und erst danach auf Geräte flashen.
- Bei Verlust eines Senders: dessen Eintrag und Schlüssel im Receiver entfernen oder Schlüssel auf allen Geräten wechseln.
