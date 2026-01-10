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

---

## Safety Extension (Optional) - DoorRECEIVER_SAFETY / DoorSLAVE_SAFETY

### Übersicht
Die Safety Extension ist eine **optionale** Hardware-Sicherheitsschicht, die das Risiko unbeabsichtigter Türöffnungen durch MCU-Abstürze, undefinierte GPIO-Zustände oder Software-Hänger reduziert.

**WICHTIG:** Dies ist eine **SAFETY**-Schicht, KEINE **SECURITY**-Schicht. Die Sicherheit wird weiterhin durch das Challenge-Response-Protokoll garantiert.

### Hardware-Installation

#### Relais-Verkabelung (in Serie)
Die Relais müssen **in Serie** geschaltet werden:

```
[Stromquelle] → [DoorRECEIVER_SAFETY Relais] → [DoorSLAVE_SAFETY Relais] → [Türöffner]
```

**Beide Relais müssen aktiv sein**, damit Strom zum Türöffner fließt.

#### Benötigte Hardware
- 2× ESP32-C3 (oder kompatibel)
- 2× Relais-Module (identische Hardware wie bei DoorRECEIVER)
- Optional: 2× WS2812 Status-LEDs (nur für DoorRECEIVER_SAFETY)

### Konfiguration

#### Schritt 1: MACs ermitteln
Flashe beide ESP32 mit einem minimalen Sketch, der die MAC-Adresse ausgibt:
```cpp
#include <WiFi.h>
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
void loop() {}
```

Notiere die MACs:
- **Master-MAC**: MAC des DoorRECEIVER_SAFETY
- **Slave-MAC**: MAC des DoorSLAVE_SAFETY

#### Schritt 2: doorLockData.h anpassen
Öffne `doorLockData.h` und aktiviere die Safety-MACs:

```cpp
// Beispiel:
#define DOORSAFETY_SLAVE_MAC ((const uint8_t[]){0x24, 0x6F, 0x28, 0xBB, 0xCC, 0xDD})
#define DOORSAFETY_MASTER_MAC ((const uint8_t[]){0x50, 0x78, 0x7D, 0x52, 0xD8, 0xA8})
```

- `DOORSAFETY_SLAVE_MAC`: Trage die MAC des DoorSLAVE_SAFETY ein (wird von DoorRECEIVER_SAFETY verwendet)
- `DOORSAFETY_MASTER_MAC`: Trage die MAC des DoorRECEIVER_SAFETY ein (wird von DoorSLAVE_SAFETY verwendet)

**Optionalität:** Wenn diese Defines **nicht gesetzt** sind, verhält sich DoorRECEIVER_SAFETY exakt wie DoorRECEIVER.

#### Schritt 3: Flashen
1. Flashe **DoorRECEIVER_SAFETY** auf den Master-ESP32
2. Flashe **DoorSLAVE_SAFETY** auf den Slave-ESP32
3. Stelle sicher, dass beide Geräte denselben `WIFI_CHANNEL` verwenden (in `doorLockData.h`)

### Betrieb

#### Normaler Betrieb
1. Sender (z.B. ControllerOTA_DC_GEN_II) sendet Türöffnungs-Anfrage
2. DoorRECEIVER_SAFETY authentifiziert mit Challenge-Response
3. Bei Erfolg:
   - DoorRECEIVER_SAFETY sendet **PERMIT1** an DoorSLAVE_SAFETY
   - DoorRECEIVER_SAFETY aktiviert sein Relais (~350 ms)
   - DoorRECEIVER_SAFETY sendet **PERMIT2** an DoorSLAVE_SAFETY
   - DoorSLAVE_SAFETY prüft Nonce und Timing-Fenster
   - Bei Erfolg: DoorSLAVE_SAFETY aktiviert sein Relais (~500 ms)
   - Beide Relais aktiv → Türöffner wird mit Strom versorgt

#### Verhalten ohne Slave
- Wenn `DOORSAFETY_SLAVE_MAC` **nicht definiert** ist: DoorRECEIVER_SAFETY arbeitet wie ein normaler DoorRECEIVER
- Wenn Slave-MAC definiert, aber Slave **nicht vorhanden** oder **nicht erreichbar**:
  - DoorRECEIVER_SAFETY öffnet trotzdem sein Relais
  - **WICHTIG:** In diesem Fall funktioniert die Türöffnung NICHT (Relais in Serie!)
  - Überprüfe die Verkabelung und Stromversorgung des Slave

### Fehlersuche

#### Tür öffnet nicht (mit Safety Extension)
1. **Prüfe serielle Logs:**
   - DoorRECEIVER_SAFETY: Sollte "Sent PERMIT1" und "Sent PERMIT2" zeigen
   - DoorSLAVE_SAFETY: Sollte "PERMIT1 received" und "RELAY ACTIVATED" zeigen

2. **Prüfe Verkabelung:**
   - Sind beide Relais in Serie geschaltet?
   - Haben beide ESP32 Strom?
   - Sind die Relais-Pins korrekt konfiguriert (Standard: GPIO 2)?

3. **Prüfe Timing:**
   - PERMIT1 und PERMIT2 müssen innerhalb von ≤200 ms beim Slave ankommen
   - Bei schlechter WiFi-Verbindung kann das Timing-Fenster überschritten werden
   - Teste im selben Raum mit geringer Entfernung

4. **Prüfe MACs:**
   - DoorSLAVE_SAFETY akzeptiert Permits NUR von der konfigurierten Master-MAC
   - Serielle Logs zeigen "Permit from unknown MAC" bei Mismatch

#### Watchdog-Resets
- Watchdog-Resets sind normal bei MCU-Abstürzen (das ist der Zweck!)
- Nach Watchdog-Reset: Beide Relais defaulten auf OFF (sicherer Zustand)
- Wenn häufige Watchdog-Resets auftreten: Prüfe Stromversorgung und serielle Logs

### Sicherheitshinweise
- **Safety ≠ Security:** Permit-Nachrichten sind unverschlüsselt und bieten KEINEN Schutz gegen Angreifer
- **Zweck:** Reduzierung von Hardware-Fehlfunktionen, NICHT Schutz gegen Replay oder Spoofing
- **Hauptsicherheit:** Bleibt unverändert im Challenge-Response-Protokoll zwischen Sender und DoorRECEIVER_SAFETY
- Die Safety Extension erschwert absichtliche Angriffe NICHT – sie reduziert nur unbeabsichtigte Aktivierungen
