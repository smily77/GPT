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

### Safety-Permit-Format (optional)
Die Safety-Permit-Schicht ist **getrennt** vom Door-Sicherheitsprotokoll und dient nur als Liveness-/Sicherheitsbarriere.

`PermitMessage` ist gepackt und 7 Bytes lang:
- `uint8_t type` (PERMIT1=1, PERMIT2=2)
- `uint32_t magic` (fester 32-bit Magic-Wert zur Erkennung)
- `uint16_t nonce` (kleiner Nonce, frisch pro Öffnung)

Permits werden ohne Kryptografie übertragen und verwenden keine persistenten Zähler oder NVS.

## Kryptografie
- HMAC-SHA256 über mbedTLS, Ergebnis auf 16 Bytes gekürzt.
- Konstante Zeitvergleichsfunktion für Tags.
- Schlüssel pro Sender (32 Byte). Receiver speichert pro Allowlist-Eintrag den Schlüssel.

## Challenge-Response-Ablauf
1. **HELLO** (Sender → Receiver): enthält nur Version/Typ/Sender-ID.
2. **CHALLENGE** (Receiver → Sender): enthält `session_id`, `receiver_nonce`, Tag = HMAC("CHAL" || sender_id || session_id || receiver_nonce || receiver_mac).
3. **OPEN** (Sender → Receiver): nur bei gültiger Session; enthält `session_id`, `receiver_nonce`, Tag = HMAC("OPEN" || sender_id || session_id || receiver_nonce || receiver_mac).
4. **OPEN_ACK** oder **DENY** (Receiver → Sender): Tag = HMAC("ACK"/"DENY" || sender_id || session_id || code).

## Safety-Permit-Ablauf (optional, DoorRECEIVER_SAFETY ↔ DoorSLAVE_SAFETY)
1. **PERMIT1** (Receiver → SafetyBox): nach erfolgreicher Authentifizierung gesendet, enthält `magic` + `nonce`.
2. **Relais-Puls**: der primäre Receiver pulst sein Relais lokal.
3. **PERMIT2** (Receiver → SafetyBox): erneut `magic` + `nonce`, gleiche Nonce wie PERMIT1.
4. Optional kann PERMIT1/PERMIT2 einmal verzögert erneut gesendet werden.

Diese Schicht ist **keine** Sicherheitsverbesserung des Door-Protokolls (README §3.1), sondern nur eine optionale Safety-Barriere.

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

### SafetyBox (DoorSLAVE_SAFETY)
- Zustände: `IDLE` → `GOT_PERMIT1` → `DONE`.
- `IDLE` → `GOT_PERMIT1`: bei gültigem PERMIT1, speichert Nonce + Zeitstempel.
- `GOT_PERMIT1` → `DONE`: bei PERMIT2 mit gleicher Nonce innerhalb eines kurzen Fensters (≤200 ms); pulst Relais ~400–600 ms.
- `DONE`: ignoriert weitere Permits für ~1 s, danach zurück zu `IDLE`.

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
