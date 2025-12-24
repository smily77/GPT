#define PROTOCOL_VERSION 1
#define MSG_HELLO 1
#define MSG_CHALLENGE 2
#define MSG_OPEN 3
#define MSG_OPEN_ACK 4
#define MSG_DENY 5

struct DoorMessage {
  uint8_t version;
  uint8_t type;
  uint8_t sender_id;
  uint8_t reserved;
  uint32_t session_id;
  uint8_t nonce[16];
  uint8_t tag[16];
} __attribute__((packed));

#include <Streaming.h>

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Credentials.h>
#include <mbedtls/md.h>
#include <doorLockData.h>
#include <M5Unified.h>

#define DEBUG false

// ====== COLORS/HELPERS ======
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t COLOR_BG        = rgb565(10, 10, 10);
const uint16_t COLOR_BLUE      = rgb565(0, 160, 255);
const uint16_t COLOR_YELLOW    = rgb565(255, 210, 0);
const uint16_t COLOR_BLACK     = rgb565(0, 0, 0);
const uint16_t COLOR_WHITE     = rgb565(255, 255, 255);
const uint16_t COLOR_RED       = rgb565(220, 40, 40);

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
unsigned long buttonPressStartTime = 0;
bool buttonLongPressHandled = false;
constexpr unsigned long LONG_PRESS_MS = 2000;

// ====== DISPLAY HELPERS ======
M5GFX &gfx = M5.Display;

void drawCenteredText(const String &text, uint16_t fg, uint16_t bg) {
  gfx.fillScreen(bg);
  gfx.setTextDatum(MC_DATUM);
  gfx.setTextColor(fg, bg);
  gfx.setTextSize(2);
  gfx.drawString(text, gfx.width() / 2, gfx.height() / 2);
}

void drawFogIcon(uint16_t bg, uint16_t fg) {
  gfx.fillScreen(bg);
  int cx = gfx.width() / 2;
  int cy = gfx.height() / 2;
  int radius = 36;

  // Lamp body
  gfx.fillCircle(cx - 16, cy, radius, fg);
  gfx.fillRect(cx - 16, cy - radius, radius * 2, radius * 2, bg);
  gfx.fillRoundRect(cx - 10, cy - 18, 64, 36, 10, fg);

  // Vertical bar and horizontal line
  gfx.fillRect(cx + 22, cy - 24, 6, 48, fg);
  gfx.fillRect(cx - 30, cy - 4, 80, 8, fg);

  // Fog beam lines
  for (int i = -16; i <= 16; i += 16) {
    gfx.drawLine(cx + 30, cy + i, cx + 52, cy + i, fg);
  }
}

void drawGarageRemoteIcon(uint16_t fg, uint16_t bg) {
  gfx.fillScreen(bg);
  int w = gfx.width();
  int h = gfx.height();

  // Door frame
  gfx.fillRoundRect(w / 2 - 42, h / 2 - 36, 84, 72, 10, fg);
  gfx.fillRoundRect(w / 2 - 34, h / 2 - 28, 68, 56, 8, bg);
  gfx.fillRect(w / 2 - 28, h / 2 - 8, 56, 12, fg);

  // Remote waves
  int cx = w / 2 + 26;
  int cy = h / 2 - 26;
  gfx.drawCircle(cx, cy, 12, fg);
  gfx.drawCircle(cx, cy, 18, fg);
  gfx.fillCircle(cx, cy, 6, fg);
}

void updateDisplayReady() {
  drawFogIcon(COLOR_BG, COLOR_BLUE);
}

void updateDisplayOn() {
  drawFogIcon(COLOR_YELLOW, COLOR_BLACK);
}

void updateDisplayGarage(bool deny) {
  if (deny) {
    drawCenteredText("DENY", COLOR_WHITE, COLOR_RED);
  } else {
    drawGarageRemoteIcon(COLOR_WHITE, COLOR_BG);
  }
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
  updateDisplayGarage(true);
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

    ArduinoOTA.setHostname("RemoteSwitch-Controller-M5");

    ArduinoOTA.onStart([]() {
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      if (DEBUG) Serial << "Start updating " << type << endl;
      drawCenteredText("Updating", COLOR_WHITE, COLOR_BG);
    });

    ArduinoOTA.onEnd([]() {
      if (DEBUG) Serial << "\nEnd" << endl;
      drawCenteredText("Done", COLOR_WHITE, COLOR_BG);
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
      drawCenteredText("OTA Error", COLOR_WHITE, COLOR_RED);
    });

    ArduinoOTA.begin();
    otaReady = true;
    drawCenteredText("OTA", COLOR_WHITE, COLOR_BG);
    if (DEBUG) Serial << "OTA Ready" << endl;
  } else {
    // WiFi connection failed - return to normal operation
    if (DEBUG) Serial << "WiFi connection failed - returning to normal mode" << endl;
    drawCenteredText("WiFi Fail", COLOR_WHITE, COLOR_RED);
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
    updateDisplayReady();
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
  auto cfg = M5.config();
  M5.begin(cfg);
  gfx.setRotation(0);
  gfx.setBrightness(180);
  gfx.setTextFont(2);

  Serial.begin(115200);
  delay(500);

  selfSecret = findSenderSecret(SENDER_ID);
  if (!selfSecret) {
    if (DEBUG) Serial << "Sender secret missing" << endl;
    drawCenteredText("No secret", COLOR_WHITE, COLOR_RED);
    while (true) delay(1000);
  }
  memcpy(controllerMac, selfSecret->sender_mac, sizeof(controllerMac));

  memcpy(actorPeerMac, ACTOR_MAC, sizeof(actorPeerMac));
  actorMacKnown = true;

  updateDisplayReady();
  setupEspNow();
}

void handleButton(bool doorLink, bool denyActive) {
  if (M5.BtnA.wasPressed()) {
    buttonPressStartTime = millis();
    buttonLongPressHandled = false;
  }

  if (M5.BtnA.isPressed() && !buttonLongPressHandled && !otaMode) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (pressDuration >= LONG_PRESS_MS) {
      buttonLongPressHandled = true;
      if (DEBUG) Serial << "Long press detected - initiating OTA mode" << endl;

      // Start OTA sequence
      otaMode = true;
      otaRequested = true;
      otaAckReceived = false;
      drawCenteredText("OTA req.", COLOR_WHITE, COLOR_BG);
      sendOtaRequest();
    }
  }

  if (M5.BtnA.wasReleased()) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (!buttonLongPressHandled && pressDuration < LONG_PRESS_MS) {
      if (!otaMode && doorLink && !denyActive) {
        sendOpen();
      } else if (!otaMode && !doorLink && linkOk && powerOk) {
        desiredRelayState = !relayOn;
        sendCommand(desiredRelayState);
      }
    }
  }
}

void loop() {
  M5.update();
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
    updateDisplayGarage(true);
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
      static unsigned long lastOtaRequestMs = 0;
      if (millis() - lastOtaRequestMs > 1000) {
        sendOtaRequest();
        lastOtaRequestMs = millis();
      }
    } else if (otaAckReceived && !otaReady) {
      // Actor acknowledged, show "go OTA" and start WiFi connection
      drawCenteredText("go OTA", COLOR_WHITE, COLOR_BG);
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
    updateDisplayGarage(denyUntil);
  } else if (linkOk && powerOk) {
    bool needResend = (desiredRelayState != relayOn) || (now - lastCommandSentMs > COMMAND_INTERVAL_MS) || (lastSentCommandState != desiredRelayState);
    if (needResend) {
      sendCommand(desiredRelayState);
    }

    if (relayOn) {
      updateDisplayOn();
    } else {
      updateDisplayReady();
    }
  } else {
    if (!linkOk) {
      drawCenteredText("No Link", COLOR_WHITE, COLOR_BG);
    } else if (!powerOk) {
      drawCenteredText("No Power", COLOR_WHITE, COLOR_BG);
    }
  }
}
