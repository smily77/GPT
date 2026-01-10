# DoorControl Technische Übersicht

Dieses Dokument beschreibt die Funktionsweise der DoorSENDER- und DoorRECEIVER-Sketches, das Nachrichtenformat und die Sicherheitslogik, um eine spätere Weiterentwicklung zu erleichtern.

## Architektur
- **Transport**: ESP-NOW im STA-Modus auf festem `WIFI_CHANNEL` aus `doorLockData.h` (beide Seiten identisch). Peers werden mit vordefinierten MACs hinzugefügt; keine ESP-NOW-Verschlüsselung, stattdessen anwendungsspezifisches HMAC.
- **Rolle Sender**: OLED-UI (nur „-“/„Link“/„Denay“), Taster (aktiv low, Pull-up). Sendet zyklisch HELLO und bei Bedarf OPEN. Hält aktuelle Session (session_id + receiver_nonce) im RAM.
- **Rolle Receiver**: Verwaltet pro Sender-ID eine RAM-Session (session_id, receiver_nonce, Ablaufzeit, used-Flag). Steuert Relais (350 ms Puls) und WS2812-Statuspixel (aus/grün/blau).

## Nachrichtenformat
`Message` ist gepackt und 42 Bytes lang:
- `uint8_t version`
- `uint8_t type` (HELLO=1, CHALLENGE=2, OPEN=3, OPEN_ACK=4, DENY=5)
- `uint8_t sender_id`
- `uint8_t reserved` (z.B. result/reason codes)
- `uint32_t session_id`
- `uint8_t nonce[16]` (für CHALLENGE; bei anderen Messages leer oder mit receiver_nonce beim OPEN)
- `uint8_t tag[16]` (Trunc128 von HMAC-SHA256)

## Kryptografie
- HMAC-SHA256 über mbedTLS, Ergebnis auf 16 Bytes gekürzt.
- Konstante Zeitvergleichsfunktion für Tags.
- Schlüssel pro Sender (32 Byte). Receiver speichert pro Allowlist-Eintrag den Schlüssel.

## Challenge-Response-Ablauf
1. **HELLO** (Sender → Receiver): enthält nur Version/Typ/Sender-ID.
2. **CHALLENGE** (Receiver → Sender): enthält `session_id`, `receiver_nonce`, Tag = HMAC("CHAL" || sender_id || session_id || receiver_nonce || receiver_mac).
3. **OPEN** (Sender → Receiver): nur bei gültiger Session; enthält `session_id`, `receiver_nonce`, Tag = HMAC("OPEN" || sender_id || session_id || receiver_nonce || receiver_mac).
4. **OPEN_ACK** oder **DENY** (Receiver → Sender): Tag = HMAC("ACK"/"DENY" || sender_id || session_id || code).

## Anti-Replay
- `session_id` und `receiver_nonce` werden per `esp_random()` neu generiert je HELLO/CHALLENGE.
- Receiver speichert pro Sender-ID eine Session mit Ablauf (`SESSION_TTL_MS`) und `used`-Flag.
- OPEN ist nur einmal gültig: nach Erfolg wird `used=true`. Abgelaufene Sessions werden verworfen.
- Keine NVS-Verwendung; alles im RAM.

## Statuslogik
### Sender
- Zustand „Link“, wenn eine gültige, nicht abgelaufene Session vorliegt (`SESSION_TTL_MS`, `IN_RANGE_TIMEOUT_MS`).
- Button: bei „Link“ → OPEN senden; sonst UI auf „-“ setzen.
- OPEN-Timeout setzt Anzeige auf „Denay“ für `DENY_DISPLAY_MS`.
- HELLO-Intervall verkürzt, um Link-Stabilität zu verbessern.

### Receiver
- Führt `last_contact_ms` pro Sender, aktualisiert bei CHALLENGE/OPEN.
- Statuspixel: Aus, wenn seit `IN_RANGE_TIMEOUT_MS` kein Kontakt; Grün bei Kontakt; Blau während Relais-Puls.
- Relais: 350 ms HIGH-Puls auf `RELAY_PIN` bei gültigem OPEN.

## Erweiterungspunkte
- **Weitere Sender**: `SENDER_SECRETS[]` in `doorLockData.h` erweitern (MAC, sender_id, key); Sender erhält passenden Key/MAC und setzt `SENDER_ID` entsprechend.
- **Mehr Fehlergründe**: `reserved`-Byte in DENY/ACK kann differenzierte Codes tragen.
- **Multi-Pixel-Status**: WS2812 kann auf mehrere Pixel erweitert werden; aktuell nur Index 0 genutzt.
- **Logging/Telemetry**: Serielle Logs enthalten MACs und Sendestatus; kann auf Remote-Logging erweitert werden.
- **Energieoptimierung**: HELLO-Intervall und OLED-Schlaf anpassbar, falls Stromverbrauch kritisch wird.

## Build-Hinweise
- Arduino-ESP32 Core (aktuelle ESP-NOW-API mit `esp_now_recv_info`).
- Bibliotheken: Adafruit_SSD1306, Adafruit_GFX, FreeSansBold12pt7b (Sender), mbedTLS (HMAC), Adafruit_NeoPixel (Receiver).
- Beide Sketche setzen `WiFi.mode(WIFI_STA)`; definierte MACs werden vor ESP-NOW-Init gesetzt.

---

## Safety Extension (DoorRECEIVER_SAFETY / DoorSLAVE_SAFETY)

Die Safety Extension ist eine **OPTIONALE** Hardware-Sicherheitsschicht, die das Risiko unbeabsichtigter Türöffnungen durch MCU-Abstürze, undefinierte GPIO-Zustände oder Software-Hänger reduziert. Diese Erweiterung **schwächt die Sicherheit des Hauptsystems NICHT** – die Challenge-Response-Authentifizierung bleibt vollständig intakt.

### Architektur
- **DoorRECEIVER_SAFETY**: Erweiterte Version des Receivers mit Safety-Permit-Protokoll
- **DoorSLAVE_SAFETY**: Minimales Safety-Gate mit zweitem Relais (in Serie geschaltet)
- Beide Geräte nutzen denselben `WIFI_CHANNEL`
- Permit-Nachrichten sind **unverschlüsselt** und **nicht authentifiziert** (keine Kryptografie)
- Keine NVS-Nutzung, keine persistenten Counter

### Safety Permit Nachrichtenformat
`PermitMessage` ist gepackt und 10 Bytes lang:
- `uint8_t type` (PERMIT1=0x01, PERMIT2=0x02)
- `uint32_t magic` (0x53414645 = "SAFE" in ASCII)
- `uint16_t nonce` (kleine Nonce zum Abgleich von PERMIT1 und PERMIT2)
- `uint8_t reserved[3]` (Padding)

### Permit-Protokoll-Ablauf
1. **Normale Authentifizierung**: Sender → Receiver (Challenge-Response wie bisher)
2. **PERMIT1**: Nach erfolgreicher Authentifizierung generiert DoorRECEIVER_SAFETY eine frische 16-Bit-Nonce und sendet PERMIT1(nonce) an DoorSLAVE_SAFETY (3× zur Robustheit)
3. **Relais-Puls (Master)**: DoorRECEIVER_SAFETY aktiviert sein lokales Relais (500 ms)
4. **PERMIT2**: DoorRECEIVER_SAFETY sendet PERMIT2(nonce) an DoorSLAVE_SAFETY (3×)
5. **Optional**: Backup-Sendungen von PERMIT1 und PERMIT2 nach kurzer Verzögerung

### DoorSLAVE_SAFETY State Machine
**Zustände:**
- `IDLE`: Wartet auf PERMIT1
- `GOT_PERMIT1`: PERMIT1 empfangen, wartet auf PERMIT2
- `DONE`: Relais wurde aktiviert, Cooldown-Phase

**Übergänge:**
- `IDLE → GOT_PERMIT1`: Beim Empfang von PERMIT1 mit gültiger Magic; Nonce und Timestamp werden gespeichert
- `GOT_PERMIT1 → DONE`: Beim Empfang von PERMIT2 mit **übereinstimmender Nonce** und **innerhalb des Zeitfensters** (≤200 ms); Relais-Puls (~500 ms) wird ausgelöst
- `DONE → IDLE`: Nach Cooldown-Periode (~1000 ms)
- `GOT_PERMIT1 → IDLE`: Wenn PERMIT_WINDOW_MS überschritten oder Nonce-Mismatch

**Validierung:**
- PERMIT2-Nonce muss mit gespeicherter PERMIT1-Nonce übereinstimmen
- Zeitdifferenz zwischen PERMIT1 und PERMIT2 muss ≤ PERMIT_WINDOW_MS sein (Standard: 200 ms)
- Alle weiteren Permits werden während `DONE` ignoriert

### MAC-Adressen (Hardware-Austauschbarkeit)
Alle Receiver-Programme setzen ihre MAC-Adresse aus doorLockData.h, um Hardware-Austauschbarkeit zu ermöglichen:

**DoorRECEIVER:**
- Setzt `RECEIVER_MAC` (static const uint8_t array)
- Ermöglicht Austausch ohne Neuflashen der Controller

**DoorRECEIVER_SAFETY:**
- Setzt `RECEIVER_SAFETY_MAC` (#define)
- Sender müssen diese MAC als Ziel kennen

**DoorSLAVE_SAFETY:**
- Setzt `SLAVE_SAFETY_MAC` (#define)
- Empfängt nur von `RECEIVER_SAFETY_MAC`

**Sequenz für alle:**
1. `WiFi.mode(WIFI_STA)` - WiFi initialisieren
2. `esp_wifi_stop()` - WiFi stoppen (muss initialisiert aber gestoppt sein)
3. `esp_wifi_set_mac(WIFI_IF_STA, MAC)` - MAC setzen
4. `esp_wifi_start()` - WiFi mit neuer MAC starten

### Optionalität (MANDATORY)
- Wenn `SLAVE_SAFETY_MAC` **nicht definiert** ist: DoorRECEIVER_SAFETY verhält sich exakt wie DoorRECEIVER
- Wenn Slave-MAC konfiguriert ist, aber Slave fehlt: Türöffnung funktioniert trotzdem (nur Master-Relais)
- Slave ist NIEMALS erforderlich für normale Türöffnung
- Bestehende Projekte bleiben voll kompatibel

### Hardware
- **WS2812 Status-LED**: Beide DoorRECEIVER_SAFETY und DoorSLAVE_SAFETY haben Status-LED auf GPIO 8
  - BLAU: Relais aktiv
  - AUS: Relais inaktiv (DoorSLAVE_SAFETY)
  - GRÜN: Sender in Reichweite (DoorRECEIVER_SAFETY)
- **Relais-Dauer**: DoorRECEIVER_SAFETY (500ms), DoorSLAVE_SAFETY (500ms) - synchron

### Sicherheitshinweise
- **Safety ≠ Security**: Permit-Protokoll ist unverschlüsselt und bietet KEINEN Schutz gegen Angreifer
- **Zweck**: Reduzierung von Hardwarefehlern (Crashes, GPIO-Glitches), NICHT Schutz gegen Replay oder Spoofing
- **Hauptsicherheit**: Bleibt unverändert im Challenge-Response-Protokoll
- **Relais in Serie**: Beide Relais müssen aktiv sein, damit die Tür öffnet
