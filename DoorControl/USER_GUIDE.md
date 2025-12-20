# DoorControl Benutzeranleitung

## Überblick
Dieses Projekt besteht aus einem ESP-NOW-Sender (DoorSENDER) mit OLED und Taster sowie einem ESP-NOW-Empfänger (DoorRECEIVER) mit Relais und Status-LED-Pixel. Beide Geräte nutzen ein Challenge-Response-Verfahren mit HMAC-SHA256, ohne persistenten Speicher, um Garage-Öffnen-Befehle abzusichern.

## Sicherheitshinweise
- **Geheime Schlüssel gehören nicht in Git.** Die 32-Byte-Schlüssel pro Sender (`K_SENDER` im Sender, `key` in der Receiver-Whitelist) müssen lokal eingefügt werden, bevor kompiliert/geflasht wird. Checke sie niemals ein und teile sie nicht.
- Jeder Sender hat einen eigenen Schlüssel; kompromittiert ein Gerät, kompromittiert nur diesen Sender.
- Bewahre die Geräte physisch sicher auf; ein Angreifer mit Firmwarezugriff kann den Schlüssel extrahieren.

## Konfiguration vor dem Flashen
1. **WLAN-Kanal**: Stelle `WIFI_CHANNEL` in beiden Sketches identisch ein.
2. **MAC-Adressen festlegen**:
   - Sender: Definiere `SENDER_MAC` im DoorSENDER-Sketch, damit er mit der Receiver-Allowlist übereinstimmt.
   - Receiver: Trage die erlaubten Sender-MACs in der `senders[]`-Liste im DoorRECEIVER-Sketch ein.
3. **Schlüssel einsetzen**:
   - Ersetze die Platzhalter in `K_SENDER` (Sender) und `key` (Receiver) durch 32 zufällige Bytes pro Sender.
   - Nutze einen sicheren Zufallszahlengenerator und verwalte die Schlüssel offline.
4. **Sender-ID**: Weise jedem Sender eine eindeutige `sender_id` (0–255) zu und trage sie im Sender sowie in der Receiver-Allowlist ein.
5. **Pins prüfen**:
   - Sender: Button an GPIO7 (INPUT_PULLUP, aktiv low), SDA=GPIO8, SCL=GPIO9, optional LED an GPIO6.
   - Receiver: Relais an `RELAY_PIN`, WS2812-Pixel an GPIO8.

## Einen weiteren Sender hinzufügen
1. Wähle eine neue `sender_id` und 32-Byte-Schlüssel.
2. Aktualisiere `senders[]` im Receiver mit `sender_id`, Sender-MAC und Schlüssel.
3. Setze im neuen Sender-Sketch dieselbe `sender_id`, seinen eigenen Schlüssel (`K_SENDER`) und seine MAC (`SENDER_MAC`).
4. Kompiliere und flashe beide Geräte. Stelle sicher, dass alle Sender denselben `WIFI_CHANNEL` verwenden.

## Bedienung
- **OLED-Anzeige (Sender)**: zeigt exakt „Wait“, „Link“ oder „Denay“.
- **Button**: Bei „Link“ wird ein OPEN gesendet; sonst zeigt der Sender „Wait“.
- **Status-Pixel (Receiver)**: Aus = kein Kontakt, Grün = gültiger Kontakt in letzter Zeit, Blau = Relais aktiv.

## Fehlersuche
- Prüfe, dass die MAC-Adressen exakt mit der Allowlist übereinstimmen und der Kanal korrekt ist.
- Wenn „Wait“ nicht zu „Link“ wird, Receiver einschalten, Reichweite prüfen und Schlüssel/MAC/ID vergleichen.
- Serielle Logs zeigen MACs, Sendestatus und abgelehnte Pakete.

## Wartungsempfehlungen
- Schlüssel regelmäßig rotieren (alle 6–12 Monate oder bei Verdacht auf Kompromittierung).
- Firmware-Updates offline vorbereiten, prüfen, und erst danach auf Geräte flashen.
- Bei Verlust eines Senders: dessen Eintrag und Schlüssel im Receiver entfernen oder Schlüssel auf allen Geräten wechseln.
