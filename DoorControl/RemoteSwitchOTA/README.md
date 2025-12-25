# RemoteSwitchOTA - ESP32-C3 Remote Relay Control with OTA Support

## Architektur-Übersicht

RemoteSwitchOTA ist ein verteiltes System bestehend aus zwei ESP32-C3 Geräten:

1. **Controller**: Benutzer-Interface mit Display, Button und LED
2. **Actor**: Steuert ein Relais basierend auf Befehlen vom Controller

Zusätzlich gibt es die DoorControl-Edition **ControllerOTA_DC**/**ActorOTA_DC**:

- **ControllerOTA_DC** kombiniert den DoorSender (Garagentor-Öffner) mit dem
  RemoteSwitch-Controller. Sobald eine Session mit dem DoorReceiver besteht,
  verhält sich das Gerät wie der DoorSender (kurzer Tastendruck öffnet das Tor);
  ohne Door-Link arbeitet es wie der RemoteSwitch-Controller. Ein langer
  Tastendruck (>2s) startet immer den OTA-Ablauf.
- **ActorOTA_DC** ist der zum ControllerOTA_DC passende Aktor. Beide nutzen die
  MAC-Adressen aus `doorLockData.h` (Sender 1 für den Controller, `ACTOR_MAC`
  für den Aktor) und den dort konfigurierten WiFi-Kanal.
- **ControllerOTA_DC_M5** ist eine Variante von ControllerOTA_DC für den
  **M5Stack Atom S3** (128×128 TFT, interner Button, <M5Unified.h>). Das Verhalten
  bleibt identisch: Ist der DoorReceiver sichtbar, agiert das Gerät als
  Garagentoröffner und zeigt ein Garagen-Fernbedienungs-Symbol. Ohne Door-Link
  steuert es den Actor; der Status wird mit einem Nebelschlussleuchten-Symbol
  visualisiert (blau bei „Ready“, schwarzes Symbol vor gelbem Hintergrund bei
  „On“). Ein langer Button-Press (>2s) aktiviert wie gehabt den OTA-Modus.
- **ControllerOTA_DC_GEN** vereinigt die beiden Controller-Varianten in einer
  generischen Fassung. Über `#define Atom3` (M5Stack Atom S3) oder
  `#define Original` (ESP32-C3 + SSD1306) in `doorLockData.h` wird zur
  gewünschten Plattform kompiliert, die restliche Logik bleibt identisch.
- **ActorOTA_DC** ist der zum ControllerOTA_DC passende Aktor. Beide nutzen die
  MAC-Adressen aus `doorLockData.h` (Sender 1 für den Controller, `ACTOR_MAC`
  für den Aktor) und den dort konfigurierten WiFi-Kanal.

Die Kommunikation erfolgt primär über **ESP-NOW** (WiFi-unabhängiges Protokoll). Für OTA-Updates können beide Geräte temporär in den **WiFi-Modus** wechseln und danach automatisch zu ESP-NOW zurückkehren.

## Hardware-Anforderungen

### Controller
- **MCU**: ESP32-C3 (Arduino IDE)
- **Display**: OLED SSD1306 128x32 (I2C, Adresse 0x3C)
- **Input**: Taster (aktiv LOW)
- **Output**: LED
- **I2C**: SDA/SCL für Display

### Actor
- **MCU**: ESP32-C3 (Arduino IDE)
- **Output**: Relais (HIGH = EIN)
- **Input**: Power-Sense (Digital Input, HIGH = Power OK)
- **Keine Display/UI-Komponenten**

## Pin-Belegung

### Controller (Controller.ino)
```cpp
PIN_LED    = 6   // Status-LED
PIN_BUTTON = 7   // Taster (aktiv LOW, mit internem PULLUP)
PIN_SDA    = 8   // I2C Data für Display
PIN_SCL    = 9   // I2C Clock für Display
```

### Actor (Actor.ino)
```cpp
PIN_RELAY  = 10  // Relais-Steuerung (HIGH = EIN)
PIN_SENSE  = 4   // Power-Sense Input (Digital, HIGH = Power vorhanden)
```

## MAC-Adressen

Die MAC-Adressen sind **hardcoded** und müssen vor dem Upload angepasst werden:

```cpp
// Controller
CONTROLLER_MAC[6] = {0x24, 0x6F, 0x28, 0xAA, 0xAA, 0x01}

// Actor
ACTOR_MAC[6] = {0x24, 0x6F, 0x28, 0xAA, 0xAA, 0x02}
```

**WICHTIG**: Beide Geräte kennen die MAC des jeweils anderen. Der Controller lernt zusätzlich dynamisch die Actor-MAC aus eingehenden Status-Nachrichten (für Robustheit bei MAC-Änderungen).

## ESP-NOW Kommunikationsprotokoll

### Nachrichtentypen

```cpp
MSG_STATUS      = 1  // Actor → Controller (periodisch)
MSG_COMMAND     = 2  // Controller → Actor (on demand)
MSG_OTA_REQUEST = 3  // Controller → Actor (bei langem Tastendruck)
MSG_OTA_ACK     = 4  // Actor → Controller (Bestätigung für OTA)
```

### Nachrichten-Strukturen

#### StatusMessage (Actor → Controller)
```cpp
struct StatusMessage {
  uint8_t msgType;    // = MSG_STATUS (1)
  bool relayOn;       // Aktueller Relais-Zustand
  bool powerOk;       // Externe Power verfügbar?
};
```
- **Sendeintervall**: 1000ms (STATUS_INTERVAL_MS)
- **Zweck**: Controller über aktuellen Zustand informieren

#### CommandMessage (Controller → Actor)
```cpp
struct CommandMessage {
  uint8_t msgType;      // = MSG_COMMAND (2)
  bool desiredRelay;    // Gewünschter Relais-Zustand
};
```
- **Sendeintervall**: Periodisch alle 1000ms ODER bei Zustandsänderung
- **Zweck**: Relais-Zustand setzen

#### OtaMessage (Bidirektional)
```cpp
struct OtaMessage {
  uint8_t msgType;  // MSG_OTA_REQUEST (3) oder MSG_OTA_ACK (4)
};
```
- **MSG_OTA_REQUEST**: Controller → Actor (OTA-Modus initiieren)
- **MSG_OTA_ACK**: Actor → Controller (Bereitschaft bestätigen)

## Controller State Machine

### Normale Betriebszustände

```
┌─────────────────────────────────────────────────────────────┐
│ NORMAL MODE (otaMode = false)                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│ linkOk && powerOk?                                          │
│   ├─ JA: Display zeigt "On" oder "Ready"                   │
│   │       LED folgt Relais-Zustand                          │
│   │       Sende Commands periodisch                         │
│   │                                                         │
│   └─ NEIN: LED aus                                          │
│           Button gedrückt? → Display "No Link"/"No Power"   │
│           Button losgelassen? → Display aus                 │
│                                                             │
│ Timeout: linkOk = false nach 5000ms ohne Status            │
└─────────────────────────────────────────────────────────────┘
```

### Button-Handling

```cpp
Debouncing: 40ms (DEBOUNCE_DELAY_MS)
Long Press: 2000ms (LONG_PRESS_MS)

Zustandsübergänge:
1. Button gedrückt (HIGH → LOW):
   - Starte Timer (buttonPressStartTime)
   - Setze buttonLongPressHandled = false

2. Während Button gehalten (LOW):
   - Wenn Dauer >= 2000ms UND !buttonLongPressHandled:
     → OTA-Modus aktivieren
     → buttonLongPressHandled = true

3. Button losgelassen (LOW → HIGH):
   - Wenn Dauer < 2000ms UND !buttonLongPressHandled:
     → Toggle Relais (normaler Schaltvorgang)
```

### OTA State Machine (Controller)

```
┌──────────────────────────────────────────────────────────────┐
│ OTA MODE SEQUENCE                                            │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│ 1. TRIGGER (Button >= 2 Sekunden)                           │
│    otaMode = true                                            │
│    otaRequested = true                                       │
│    otaAckReceived = false                                    │
│    Display: "OTA req."                                       │
│    → Sende MSG_OTA_REQUEST (periodisch alle 1s)             │
│                                                              │
│ 2. WAIT FOR ACK                                              │
│    Empfange MSG_OTA_ACK → otaAckReceived = true             │
│    Display: "go OTA"                                         │
│    → Warte 1 Sekunde                                         │
│                                                              │
│ 3. WIFI SETUP                                                │
│    → esp_now_deinit()                                        │
│    → WiFi.begin(ssid, password)                             │
│    → Warte max. 15 Sekunden (30 × 500ms)                    │
│                                                              │
│    ├─ ERFOLG:                                                │
│    │  ArduinoOTA.begin()                                     │
│    │  otaReady = true                                        │
│    │  Display: "OTA"                                         │
│    │  → loop() ruft ArduinoOTA.handle()                     │
│    │                                                         │
│    └─ FEHLER (WiFi nicht verfügbar):                         │
│       Display: "WiFi Fail" (2 Sekunden)                      │
│       otaMode = false                                        │
│       otaRequested = false                                   │
│       otaAckReceived = false                                 │
│       otaReady = false                                       │
│       WiFi.disconnect()                                      │
│       → setupEspNow() (Re-Initialisierung)                  │
│       Display: "Ready"                                       │
│       → Zurück zu NORMAL MODE                                │
└──────────────────────────────────────────────────────────────┘
```

## Actor State Machine

### Normale Betriebszustände

```
┌─────────────────────────────────────────────────────────────┐
│ NORMAL MODE (otaMode = false)                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│ 1. Empfange MSG_COMMAND:                                    │
│    → updatePower()                                          │
│    → applyRelay(incoming.desiredRelay)                      │
│    → sendStatus()                                           │
│    → lastControllerMsg = millis()                           │
│                                                             │
│ 2. Periodisches Status-Senden:                              │
│    → sendStatus() alle 1000ms                               │
│                                                             │
│ 3. Controller-Timeout:                                       │
│    → Nach 5000ms ohne MSG_COMMAND:                          │
│      applyRelay(false)  // Sicherheitsabschaltung          │
│                                                             │
│ 4. Power-Überwachung:                                        │
│    → updatePower() in jedem Loop                            │
│    → !powerOk? → applyRelay(false)                          │
└─────────────────────────────────────────────────────────────┘
```

### Relais-Logik

```cpp
void applyRelay(bool on) {
  relayOn = on && powerOk;  // Relais NUR wenn Power OK
  digitalWrite(PIN_RELAY, relayOn ? HIGH : LOW);
}
```

**WICHTIG**: Das Relais wird IMMER ausgeschaltet wenn:
- `powerOk == false` (keine externe Power)
- 5000ms ohne Controller-Nachricht
- OTA-Update startet

### OTA State Machine (Actor)

```
┌──────────────────────────────────────────────────────────────┐
│ OTA MODE SEQUENCE                                            │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│ 1. TRIGGER (Empfange MSG_OTA_REQUEST)                       │
│    → sendOtaAck()                                            │
│    otaMode = true                                            │
│    otaSetupAttempted = false                                 │
│                                                              │
│ 2. WIFI SETUP (in loop(), EINMALIG)                         │
│    if (!otaReady && !otaSetupAttempted):                    │
│      otaSetupAttempted = true                                │
│      → esp_now_deinit()                                      │
│      → WiFi.begin(ssid, password)                           │
│      → Warte max. 15 Sekunden                               │
│                                                              │
│      ├─ ERFOLG:                                              │
│      │  ArduinoOTA.begin()                                   │
│      │  otaReady = true                                      │
│      │  → loop() ruft ArduinoOTA.handle()                   │
│      │                                                       │
│      └─ FEHLER:                                              │
│         otaMode = false                                      │
│         otaReady = false                                     │
│         otaSetupAttempted = false                            │
│         WiFi.disconnect()                                    │
│         → setupEspNow()                                      │
│         → Zurück zu NORMAL MODE                              │
│                                                              │
│ 3. OTA READY                                                 │
│    → ArduinoOTA.handle() in jedem Loop                      │
│    → Relais IMMER aus während Update                         │
└──────────────────────────────────────────────────────────────┘
```

## Wichtige Code-Besonderheiten

### 1. Controller lernt Actor-MAC dynamisch

```cpp
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // Bei jeder Status-Nachricht:
  if (info && info->src_addr) {
    bool macChanged = memcmp(actorPeerMac, info->src_addr, 6) != 0;
    if (macChanged) {
      // Alten Peer entfernen, neuen hinzufügen
      esp_now_del_peer(actorPeerMac);
      memcpy(actorPeerMac, info->src_addr, 6);
      esp_now_add_peer(&peerInfo);
    }
  }
}
```

**Zweck**: Robustheit bei MAC-Änderungen (z.B. nach Firmware-Update)

### 2. Command Resend-Logik (Controller)

```cpp
bool needResend =
  (desiredRelayState != relayOn) ||                    // Zustand passt nicht
  (millis() - lastCommandSentMs > COMMAND_INTERVAL_MS) ||  // Periodisch
  (lastSentCommandState != desiredRelayState);         // Desired geändert

if (needResend) {
  sendCommand(desiredRelayState);
}
```

**Zweck**:
- Robustheit bei Paketverlust
- Synchronisation halten
- Schnelle Reaktion bei Zustandsänderung

### 3. WiFi-Fallback (beide Geräte)

Wenn WiFi-Verbindung fehlschlägt:
1. Display "WiFi Fail" zeigen (nur Controller, 2 Sekunden)
2. Alle OTA-Flags zurücksetzen
3. `WiFi.disconnect()`
4. `setupEspNow()` aufrufen (Re-Initialisierung)
5. Zurück zu normalem Betrieb

**KRITISCH**: Ohne diese Logik würde das System im OTA-Modus hängen bleiben!

### 4. OTA-Safety (Actor)

```cpp
ArduinoOTA.onStart([]() {
  applyRelay(false);  // Sicherheitsabschaltung!
});
```

**Zweck**: Verhindern dass Relais während Firmware-Update in undefiniertem Zustand ist.

### 5. Display Sleep (Controller)

```cpp
void updateDisplay(const String &message, bool keepOn) {
  if (!keepOn) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    return;
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
  // ... render text ...
}
```

Im Fehlerfall (No Link/Power) schaltet Display aus wenn Button nicht gedrückt ist.

## Externe Abhängigkeiten

### Credentials.h

Beide Programme erwarten:
```cpp
#include <Credentials.h>

// Muss definieren:
const char* ssid = "your-wifi-ssid";
const char* password = "your-wifi-password";
```

Diese Datei ist **NICHT** im Repository und muss lokal erstellt werden.

### Bibliotheken

**Controller**:
- `Streaming.h` (für Serial << Syntax)
- `esp_now.h`, `esp_wifi.h`, `WiFi.h`
- `Wire.h` (I2C)
- `Adafruit_GFX.h`, `Adafruit_SSD1306.h`
- `Fonts/FreeSansBold12pt7b.h`
- `ArduinoOTA.h`

**Actor**:
- `esp_now.h`, `esp_wifi.h`, `WiFi.h`
- `ArduinoOTA.h`

## Timing-Konstanten

### Controller
```cpp
STATUS_TIMEOUT_MS    = 5000  // Max Zeit ohne Actor-Status
COMMAND_INTERVAL_MS  = 1000  // Periodisches Command-Resend
DEBOUNCE_DELAY_MS    = 40    // Button Debouncing
LONG_PRESS_MS        = 2000  // Button für OTA-Modus
```

### Actor
```cpp
STATUS_INTERVAL_MS       = 1000  // Periodisches Status-Senden
CONTROLLER_TIMEOUT_MS    = 5000  // Max Zeit ohne Controller-Command
```

## WiFi-Kanal

Beide Geräte nutzen:
```cpp
esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
```

**WICHTIG**: Kanal 1 ist hardcoded. Bei Änderung müssen BEIDE Geräte angepasst werden!

## Debug-Modus

```cpp
#define DEBUG false  // Auf true setzen für Serial-Output
```

Bei `DEBUG = true`:
- Detaillierte Serial-Ausgaben
- Hilfreich für Entwicklung und Fehlersuche
- **ACHTUNG**: Erhöht Code-Größe und kann Timing beeinflussen

## Typische Ablaufszenarien

### Szenario 1: Normaler Schaltvorgang

```
1. Benutzer drückt Button kurz (<2 Sek)
2. Controller: desiredRelayState = !relayOn
3. Controller → Actor: MSG_COMMAND(desiredRelay = true)
4. Actor: applyRelay(true), digitalWrite(PIN_RELAY, HIGH)
5. Actor → Controller: MSG_STATUS(relayOn = true, powerOk = true)
6. Controller: Display "On", LED HIGH
```

### Szenario 2: OTA-Update (mit WLAN)

```
1. Benutzer hält Button >= 2 Sekunden
2. Controller: Display "OTA req."
3. Controller → Actor: MSG_OTA_REQUEST (periodisch)
4. Actor → Controller: MSG_OTA_ACK
5. Controller: Display "go OTA"
6. Beide: WiFi.begin(ssid, password)
7. Beide: ArduinoOTA.begin()
8. Controller: Display "OTA"
9. Benutzer führt OTA-Update durch (z.B. mit Arduino IDE oder platformio)
10. Nach Update: ESP rebootet automatisch
```

### Szenario 3: OTA-Update fehlgeschlagen (kein WLAN)

```
1-4. Wie Szenario 2
5. Controller: Display "go OTA"
6. Beide: WiFi.begin(ssid, password) → TIMEOUT
7. Controller: Display "WiFi Fail" (2 Sek)
8. Beide: setupEspNow() → Zurück zu ESP-NOW
9. Controller: Display "Ready"
10. Normale Funktion wiederhergestellt
```

### Szenario 4: Power-Verlust am Actor

```
1. Actor: digitalRead(PIN_SENSE) == LOW
2. Actor: powerOk = false
3. Actor: applyRelay(false) → Relais AUS
4. Actor → Controller: MSG_STATUS(relayOn = false, powerOk = false)
5. Controller: Display "No Power" (wenn Button gedrückt)
6. Wenn Power zurückkehrt: Relais bleibt AUS bis neuer Command
```

### Szenario 5: Link-Verlust

```
1. Actor sendet keine Status-Nachrichten mehr
2. Controller: Nach 5000ms → linkOk = false
3. Controller: Display "No Link" (wenn Button gedrückt)
4. Controller: LED aus
5. Wenn Status zurückkehrt: Synchronisation erfolgt automatisch
```

## Erweiterungsmöglichkeiten für AI-Agenten

### Mögliche Verbesserungen

1. **Multi-Actor Support**
   - Controller könnte mehrere Actors verwalten
   - Array von Actor-MACs
   - Actor-Auswahl über Button-Kombinationen

2. **Persistenter Zustand**
   - EEPROM/Preferences für letzten Relais-Zustand
   - Automatische Wiederherstellung nach Reboot

3. **Erweiterte Display-Funktionen**
   - Menü-System
   - WiFi-Konfiguration über Display
   - OTA-Progress-Anzeige

4. **Erweiterte Sicherheit**
   - ESP-NOW Verschlüsselung aktivieren
   - Challenge-Response für Commands
   - OTA mit Passwort

5. **Energie-Management**
   - Deep-Sleep Modi
   - Wake-on-Button
   - Battery-Monitoring

6. **Alternative WiFi-Fallback**
   - Nach mehreren OTA-Fehlversuchen → Access Point starten
   - Web-Interface für Konfiguration

7. **Telemetrie**
   - MQTT-Integration
   - Cloud-Logging
   - Statistiken (Schaltvorgänge, Uptime, etc.)

8. **Multi-Channel Support**
   - Dynamische Kanal-Auswahl
   - Kanal-Scanning bei Verbindungsverlust

### Code-Struktur für Erweiterungen

Die aktuelle Architektur ist modular aufgebaut:
- Klare Trennung von Kommunikation (ESP-NOW/WiFi)
- State Machines für verschiedene Modi
- Callback-basierte Nachrichten-Verarbeitung

Neue Funktionen sollten:
- Eigene Message-Types definieren (5+)
- State-Flags hinzufügen (ähnlich wie `otaMode`)
- Nicht-blockierende Implementierung nutzen
- Failsafe-Mechanismen einbauen

## Fehlersuche

### Problem: Display bleibt dunkel
- I2C-Adresse prüfen (default: 0x3C)
- Pin-Belegung SDA/SCL prüfen
- I2C-Scanner nutzen

### Problem: ESP-NOW funktioniert nicht
- MAC-Adressen korrekt gesetzt?
- WiFi-Kanal identisch?
- `esp_now_init()` erfolgreich?

### Problem: OTA hängt
- Credentials.h vorhanden und korrekt?
- WiFi-Reichweite OK?
- Firewall blockiert OTA-Port (8266)?

### Problem: Relais schaltet nicht
- `powerOk` Status prüfen (PIN_SENSE)
- Actor empfängt Commands? (Serial DEBUG)
- Relais-Hardware defekt?

### Problem: System blockiert nach OTA-Versuch
- **FIX BEREITS IMPLEMENTIERT** (siehe WiFi-Fallback)
- Falls dennoch: setupOTA() Logik prüfen

## Lizenz und Verwendung

Diese Programme sind für ESP32-C3 mit Arduino IDE entwickelt.
Anpassungen für andere ESP32-Varianten erfordern Pin-Änderungen.

**MAC-Adressen müssen vor Upload angepasst werden!**

---

**Version**: RemoteSwitchOTA v1.1
**Letzte Änderung**: 2025-12-20
**Autor**: AI-generiert für smily77/AI_Test
