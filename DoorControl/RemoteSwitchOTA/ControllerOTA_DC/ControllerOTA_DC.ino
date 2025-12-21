#include <Streaming.h>

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <ArduinoOTA.h>
#include <Credentials.h>
#include <mbedtls/md.h>
#include <doorLockData.h>

#define DEBUG false

// ====== PINS ======
constexpr uint8_t PIN_LED = 6;
constexpr uint8_t PIN_BUTTON = 7; // active LOW
constexpr uint8_t PIN_SDA = 8;
constexpr uint8_t PIN_SCL = 9;

// ====== LED PWM ======
constexpr uint32_t LED_PWM_FREQ = 5000;
constexpr uint8_t LED_PWM_RESOLUTION = 8;  // 8-bit: 0-255
constexpr uint8_t LED_BRIGHTNESS_PERCENT = 20;  // LED brightness in %
constexpr uint8_t LED_BRIGHTNESS = (LED_BRIGHTNESS_PERCENT * 255) / 100;  // PWM value (20% = 51)

// ====== DISPLAY ======
constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 32;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ====== ESP-NOW ======
// The Controller uses the MAC of Sender 1 from doorLockData.h
constexpr uint8_t SENDER_ID = CONTROLLER_SENDER_ID;
uint8_t controllerMac[6] = {0};
uint8_t actorPeerMac[6] = {0};
bool actorMacKnown = false;

// RemoteSwitch messages
constexpr uint8_t MSG_STATUS = 1;
constexpr uint8_t MSG_COMMAND = 2;
constexpr uint8_t MSG_OTA_REQUEST = 3;
constexpr uint8_t MSG_OTA_ACK = 4;

struct StatusMessage {
  uint8_t msgType;
  bool relayOn;
  bool powerOk;
};

struct CommandMessage {
  uint8_t msgType;
  bool desiredRelay;
};

struct OtaMessage {
  uint8_t msgType;
};

// DoorSender <-> DoorReceiver handshake
#define PROTOCOL_VERSION 1
#define MSG_HELLO 1
#define MSG_CHALLENGE 2
#define MSG_OPEN 3
#define MSG_OPEN_ACK 4
#define MSG_DENY 5

struct __attribute__((packed)) DoorMessage {
  uint8_t version;
  uint8_t type;
  uint8_t sender_id;
  uint8_t reserved;
  uint32_t session_id;
  uint8_t nonce[16];
  uint8_t tag[16];
};

const SenderSecret *findSenderSecret(uint8_t id) {
  for (size_t i = 0; i < SENDER_SECRETS_COUNT; ++i) {
    if (SENDER_SECRETS[i].sender_id == id) {
      return &SENDER_SECRETS[i];
    }
  }
  return nullptr;
}

// ====== STATE ======
bool relayOn = false;
bool powerOk = false;
bool linkOk = false;
unsigned long lastStatusMs = 0;
bool desiredRelayState = false;
bool lastSentCommandState = false;

// DoorSender state
const SenderSecret *selfSecret = nullptr;
uint32_t currentSessionId = 0;
uint8_t receiverNonce[16];
uint32_t sessionStartMs = 0;
bool sessionValid = false;
bool denyUntil = false;
uint32_t denyUntilMs = 0;
bool openPending = false;
uint32_t openSentMs = 0;

// ====== OTA STATE ======
bool otaMode = false;
bool otaRequested = false;
bool otaAckReceived = false;
bool otaReady = false;
constexpr unsigned long OTA_WIFI_TIMEOUT_MS = 7000;

// ====== TIMING ======
constexpr unsigned long STATUS_TIMEOUT_MS = 5000;
constexpr unsigned long COMMAND_INTERVAL_MS = 1000;
unsigned long lastCommandSentMs = 0;

// Door timing (copied from DoorSender)
constexpr unsigned long IN_RANGE_TIMEOUT_MS = 5000;
constexpr unsigned long SESSION_TTL_MS = 10000;
constexpr unsigned long DENY_DISPLAY_MS = 3000;
constexpr unsigned long OPEN_TIMEOUT_MS = 1000;
constexpr unsigned long HELLO_INTERVAL_MS = 300;

// ====== BUTTON ======
bool lastReading   = HIGH;  // Rohwert letztes Mal
bool buttonState   = HIGH;  // stabiler, entprellter Zustand
unsigned long lastDebounceTime = 0;
unsigned long buttonPressStartTime = 0;
bool buttonLongPressHandled = false;
constexpr unsigned long DEBOUNCE_DELAY_MS = 40;
constexpr unsigned long LONG_PRESS_MS = 2000;

void updateDisplay(const String &message, bool keepOn) {
  if (!keepOn) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    return;
  }

  display.ssd1306_command(SSD1306_DISPLAYON);
  display.clearDisplay();
  display.setFont(&FreeSansBold12pt7b);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(message, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (SCREEN_WIDTH - w) / 2;
  int16_t y = (SCREEN_HEIGHT + h) / 2 - 2; // visually centered
  display.setCursor(x, y);
  display.print(message);
  display.display();
}

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

void hmacTrunc16(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t dataLen, uint8_t out[16]) {
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, data, dataLen);
  uint8_t full[32];
  mbedtls_md_hmac_finish(&ctx, full);
  memcpy(out, full, 16);
  mbedtls_md_free(&ctx);
}

void computeChallengeTag(uint8_t out[16], uint32_t session_id, const uint8_t nonce[16]) {
  const char prefix[] = "CHAL";
  uint8_t buf[4 + 1 + 4 + 16 + 6];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = SENDER_ID;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  memcpy(buf + idx, nonce, 16); idx += 16;
  memcpy(buf + idx, RECEIVER_MAC, 6); idx += 6;
  hmacTrunc16(selfSecret->key, sizeof(selfSecret->key), buf, idx, out);
}

void computeOpenTag(uint8_t out[16], uint32_t session_id, const uint8_t nonce[16]) {
  const char prefix[] = "OPEN";
  uint8_t buf[4 + 1 + 4 + 16 + 6];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = SENDER_ID;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  memcpy(buf + idx, nonce, 16); idx += 16;
  memcpy(buf + idx, RECEIVER_MAC, 6); idx += 6;
  hmacTrunc16(selfSecret->key, sizeof(selfSecret->key), buf, idx, out);
}

void computeAckTag(uint8_t out[16], uint32_t session_id, uint8_t result_code) {
  const char prefix[] = "ACK";
  uint8_t buf[3 + 1 + 4 + 1];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 3); idx += 3;
  buf[idx++] = SENDER_ID;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  buf[idx++] = result_code;
  hmacTrunc16(selfSecret->key, sizeof(selfSecret->key), buf, idx, out);
}

void computeDenyTag(uint8_t out[16], uint32_t session_id, uint8_t reason_code) {
  const char prefix[] = "DENY";
  uint8_t buf[4 + 1 + 4 + 1];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = SENDER_ID;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  buf[idx++] = reason_code;
  hmacTrunc16(selfSecret->key, sizeof(selfSecret->key), buf, idx, out);
}

void sendHello() {
  if (!esp_now_is_peer_exist(RECEIVER_MAC)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, RECEIVER_MAC, 6);
    peer.channel = WIFI_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  DoorMessage msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_HELLO;
  msg.sender_id = SENDER_ID;
  esp_now_send(RECEIVER_MAC, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
}

void sendOpen() {
  if (!sessionValid) return;
  if (!esp_now_is_peer_exist(RECEIVER_MAC)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, RECEIVER_MAC, 6);
    peer.channel = WIFI_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  DoorMessage msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_OPEN;
  msg.sender_id = SENDER_ID;
  msg.session_id = currentSessionId;
  memcpy(msg.nonce, receiverNonce, 16);
  computeOpenTag(msg.tag, currentSessionId, receiverNonce);
  esp_err_t res = esp_now_send(RECEIVER_MAC, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
  if (res == ESP_OK) {
    openPending = true;
    openSentMs = millis();
  }
}

void handleChallenge(const DoorMessage &msg) {
  if (msg.version != PROTOCOL_VERSION || msg.sender_id != SENDER_ID) return;
  uint8_t expected[16];
  computeChallengeTag(expected, msg.session_id, msg.nonce);
  if (!constantTimeEqual(expected, msg.tag, 16)) {
    return;
  }
  currentSessionId = msg.session_id;
  memcpy(receiverNonce, msg.nonce, 16);
  sessionStartMs = millis();
  sessionValid = true;
}

void handleAck(const DoorMessage &msg) {
  if (!sessionValid || msg.session_id != currentSessionId) return;
  uint8_t expected[16];
  computeAckTag(expected, msg.session_id, msg.reserved);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  denyUntil = false;
  openPending = false;
}

void handleDeny(const DoorMessage &msg) {
  if (!sessionValid || msg.session_id != currentSessionId) return;
  uint8_t expected[16];
  computeDenyTag(expected, msg.session_id, msg.reserved);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  openPending = false;
  denyUntil = true;
  denyUntilMs = millis() + DENY_DISPLAY_MS;
  updateDisplay("Deny", true);
}

void onSend(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // No action required, but keeps callback registered for completeness.
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == (int)sizeof(DoorMessage)) {
    DoorMessage msg;
    memcpy(&msg, data, sizeof(DoorMessage));
    switch (msg.type) {
      case MSG_CHALLENGE:
        handleChallenge(msg);
        break;
      case MSG_OPEN_ACK:
        handleAck(msg);
        break;
      case MSG_DENY:
        handleDeny(msg);
        break;
      default:
        break;
    }
    return;
  }

  if (len < 1) return;
  uint8_t msgType = data[0];

  if (msgType == MSG_STATUS && len == sizeof(StatusMessage)) {
    StatusMessage incoming;
    memcpy(&incoming, data, sizeof(StatusMessage));

    // Learn/update the actor MAC from the status source so commands go back reliably.
    if (info && info->src_addr) {
      bool macChanged = memcmp(actorPeerMac, info->src_addr, 6) != 0 || !actorMacKnown;
      if (macChanged) {
        if (actorMacKnown) {
          esp_now_del_peer(actorPeerMac);
        }
        memcpy(actorPeerMac, info->src_addr, 6);

        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, actorPeerMac, 6);
        peerInfo.ifidx = WIFI_IF_STA;
        peerInfo.channel = WIFI_CHANNEL;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
        actorMacKnown = true;
      }
    }

    relayOn = incoming.relayOn;
    powerOk = incoming.powerOk;
    linkOk = true;
    lastStatusMs = millis();

    // Synchronize desired state with actual when status arrives.
    if (!desiredRelayState && relayOn) {
      desiredRelayState = relayOn;
    }
  } else if (msgType == MSG_OTA_ACK && len == sizeof(OtaMessage)) {
    if (DEBUG) Serial << "OTA ACK received from Actor" << endl;
    otaAckReceived = true;
  }
}

void setupDisplay() {
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false)) {
    // If display fails to initialize, continue without blocking.
    return;
  }
  display.clearDisplay();
  updateDisplay("Ready", true);
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_mac(WIFI_IF_STA, selfSecret->sender_mac);

  if (esp_now_init() != ESP_OK) {
    return;
  }

  esp_now_register_send_cb(onSend);
  esp_now_register_recv_cb(onReceive);

  // Door receiver peer
  esp_now_peer_info_t rxPeer = {};
  memcpy(rxPeer.peer_addr, RECEIVER_MAC, 6);
  rxPeer.ifidx = WIFI_IF_STA;
  rxPeer.channel = WIFI_CHANNEL;
  rxPeer.encrypt = false;
  esp_now_add_peer(&rxPeer);

  // Actor peer (if known)
  if (actorMacKnown) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, actorPeerMac, 6);
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.channel = WIFI_CHANNEL;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

void setupOTA() {
  // Disconnect ESP-NOW and connect to WiFi
  esp_now_deinit();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  if (DEBUG) Serial << "Connecting to WiFi..." << endl;

  unsigned long startConnect = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startConnect) < OTA_WIFI_TIMEOUT_MS) {
    delay(250);
    if (DEBUG) Serial << "." << endl;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (DEBUG) {
      Serial << "Connected to WiFi" << endl;
      Serial << "IP: " << WiFi.localIP() << endl;
    }

    ArduinoOTA.setHostname("RemoteSwitch-Controller");

    ArduinoOTA.onStart([]() {
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      if (DEBUG) Serial << "Start updating " << type << endl;
      updateDisplay("Updating", true);
    });

    ArduinoOTA.onEnd([]() {
      if (DEBUG) Serial << "\nEnd" << endl;
      updateDisplay("Done", true);
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      if (DEBUG) Serial << "Progress: " << (progress / (total / 100)) << "%" << endl;
    });

    ArduinoOTA.onError([](ota_error_t error) {
      if (DEBUG) {
        Serial << "Error[" << error << "]: ";
        if (error == OTA_AUTH_ERROR) Serial << "Auth Failed" << endl;
        else if (error == OTA_BEGIN_ERROR) Serial << "Begin Failed" << endl;
        else if (error == OTA_CONNECT_ERROR) Serial << "Connect Failed" << endl;
        else if (error == OTA_RECEIVE_ERROR) Serial << "Receive Failed" << endl;
        else if (error == OTA_END_ERROR) Serial << "End Failed" << endl;
      }
      updateDisplay("OTA Error", true);
    });

    ArduinoOTA.begin();
    otaReady = true;
    updateDisplay("OTA", true);
    if (DEBUG) Serial << "OTA Ready" << endl;
  } else {
    // WiFi connection failed - return to normal operation
    if (DEBUG) Serial << "WiFi connection failed - returning to normal mode" << endl;
    updateDisplay("WiFi Fail", true);
    delay(2000);

    // Reset OTA state
    otaMode = false;
    otaRequested = false;
    otaAckReceived = false;
    otaReady = false;

    // Re-initialize ESP-NOW
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(50);
    setupEspNow();
    updateDisplay("Ready", true);
  }
}

void sendCommand(bool state) {
  if (DEBUG) Serial << "Send Command Stage 1:   " << state << endl;
  if (!actorMacKnown) return; // wait until we know where to send
  CommandMessage msg{MSG_COMMAND, state};
  if (DEBUG) Serial << "Send Command Stage 2:   " << msg.desiredRelay << endl;
  esp_now_send(actorPeerMac, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
  lastCommandSentMs = millis();
  lastSentCommandState = state;
}

void sendOtaRequest() {
  if (!actorMacKnown) return;
  OtaMessage msg{MSG_OTA_REQUEST};
  if (DEBUG) Serial << "Sending OTA Request to Actor" << endl;
  esp_now_send(actorPeerMac, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  if(DEBUG) Serial << "Serial Ready" << endl;

  selfSecret = findSenderSecret(SENDER_ID);
  if (!selfSecret) {
    if (DEBUG) Serial << "Sender secret missing" << endl;
    while (true) delay(1000);
  }
  memcpy(controllerMac, selfSecret->sender_mac, sizeof(controllerMac));

  memcpy(actorPeerMac, ACTOR_MAC, sizeof(actorPeerMac));
  actorMacKnown = true;

  // Setup LED PWM (using new ESP32 Arduino Core 3.x API)
  ledcAttach(PIN_LED, LED_PWM_FREQ, LED_PWM_RESOLUTION);
  ledcWrite(PIN_LED, 0);  // Start with LED off

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  setupDisplay();
  setupEspNow();
}


void handleButton(bool doorLink, bool denyActive) {
  int reading = digitalRead(PIN_BUTTON);

  // 1) Rohwert-Änderung? -> Timer neu starten
  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  // 2) Wenn lange genug stabil: stabilen Zustand übernehmen
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY_MS) {

    // hat sich der stabile Zustand wirklich geändert?
    if (reading != buttonState) {
      buttonState = reading;

      // 3) Aktion bei Zustandsänderung
      if (buttonState == LOW) {
        // Button wurde gedrückt
        buttonPressStartTime = millis();
        buttonLongPressHandled = false;
        if (DEBUG) Serial << "Button press (debounced)" << endl;
      } else {
        // Button wurde losgelassen
        unsigned long pressDuration = millis() - buttonPressStartTime;

        if (!buttonLongPressHandled && pressDuration < LONG_PRESS_MS) {
          // Kurzer Druck
          if (!otaMode && doorLink && !denyActive) {
            sendOpen();
          } else if (!otaMode && !doorLink && linkOk && powerOk) {
            desiredRelayState = !relayOn;
            sendCommand(desiredRelayState);
          }
        }
      }
    }
  }

  // 4) Check for long press while button is held
  if (buttonState == LOW && !buttonLongPressHandled && !otaMode) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (pressDuration >= LONG_PRESS_MS) {
      buttonLongPressHandled = true;
      if (DEBUG) Serial << "Long press detected - initiating OTA mode" << endl;

      // Start OTA sequence
      otaMode = true;
      otaRequested = true;
      otaAckReceived = false;
      updateDisplay("OTA req.", true);
      sendOtaRequest();
    }
  }

  // 4) Rohwert merken
  lastReading = reading;
}

void loop() {
  unsigned long now = millis();

  // Maintain door link session
  static unsigned long lastHello = 0;
  if (!otaMode && (now - lastHello > HELLO_INTERVAL_MS)) {
    sendHello();
    lastHello = now;
  }

  if (sessionValid && (now - sessionStartMs > SESSION_TTL_MS)) {
    sessionValid = false;
    openPending = false;
  }

  bool doorLink = sessionValid && (now - sessionStartMs < IN_RANGE_TIMEOUT_MS);

  if (openPending && (now - openSentMs > OPEN_TIMEOUT_MS)) {
    openPending = false;
    denyUntil = true;
    denyUntilMs = now + DENY_DISPLAY_MS;
    updateDisplay("Deny", true);
  }
  if (denyUntil && now > denyUntilMs) {
    denyUntil = false;
  }

  if (otaMode) {
    // OTA Mode handling
    if (otaReady) {
      // Already in OTA mode, handle OTA updates
      ArduinoOTA.handle();
    } else if (otaRequested && !otaAckReceived) {
      // Waiting for actor acknowledgment
      // Keep displaying "OTA req." and resend request periodically
      static unsigned long lastOtaRequestMs = 0;
      if (millis() - lastOtaRequestMs > 1000) {
        sendOtaRequest();
        lastOtaRequestMs = millis();
      }
    } else if (otaAckReceived && !otaReady) {
      // Actor acknowledged, show "go OTA" and start WiFi connection
      updateDisplay("go OTA", true);
      delay(1000);
      setupOTA();
    }
    return;
  }

  // Normal operation mode
  if (linkOk && now - lastStatusMs > STATUS_TIMEOUT_MS) {
    linkOk = false;
    powerOk = false;
  }

  handleButton(doorLink, denyUntil);

  if (doorLink) {
    ledcWrite(PIN_LED, 0);
    if (denyUntil) {
      updateDisplay("Deny", true);
    } else {
      updateDisplay("Link", true);
    }
  } else if (linkOk && powerOk) {
    bool needResend = (desiredRelayState != relayOn) || (now - lastCommandSentMs > COMMAND_INTERVAL_MS) || (lastSentCommandState != desiredRelayState);
    if (needResend) {
      sendCommand(desiredRelayState);
    }

    ledcWrite(PIN_LED, relayOn ? LED_BRIGHTNESS : 0);
    updateDisplay(relayOn ? "On" : "Ready", true);
  } else {
    ledcWrite(PIN_LED, 0);
    bool showText = digitalRead(PIN_BUTTON) == LOW;
    if (showText) {
      if (!linkOk) {
        updateDisplay("No Link", true);
      } else if (!powerOk) {
        updateDisplay("No Power", true);
      }
    } else {
      updateDisplay("", false);
    }
  }
}
