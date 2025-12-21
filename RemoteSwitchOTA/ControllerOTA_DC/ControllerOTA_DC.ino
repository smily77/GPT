// ControllerOTA_DC.ino - Combined garage door sender + remote switch controller with OTA
// Requires a user-supplied doorLockData.h alongside this sketch.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <mbedtls/md.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <ArduinoOTA.h>
#include "doorLockData.h"

// ===== CONFIGURABLE PINS / TIMING =====
#define SENDER_ID 1
#define BUTTON_PIN 7          // Active low, INPUT_PULLUP
#define SDA_PIN 8
#define SCL_PIN 9
#define I2C_FREQ 400000
#define AUX_LED_PIN 6
#define IN_RANGE_TIMEOUT_MS 5000
#define SESSION_TTL_MS 10000
#define DENY_DISPLAY_MS 3000
#define OPEN_TIMEOUT_MS 1000
#define DEBOUNCE_MS 60
#define HELLO_INTERVAL_MS 500
#define OTA_HOLD_MS 2000
#define ACTOR_CMD_TIMEOUT_MS 750
#define DEBUG 1
// =====================================

// Door control protocol
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

// Actor control protocol
#define ACTOR_PROTO_VERSION 1
#define ACTOR_MSG_CMD 1
#define ACTOR_MSG_ACK 2
#define ACTOR_CMD_TOGGLE 1

struct __attribute__((packed)) ActorMessage {
  uint8_t version;
  uint8_t type;
  uint8_t command;
  uint8_t reserved;
  uint32_t nonce;
  uint8_t tag[16];
};

Adafruit_SSD1306 display(128, 32, &Wire, -1);

uint8_t selfMac[6];
const SenderSecret *selfSecret = nullptr;
uint32_t currentSessionId = 0;
uint8_t receiverNonce[16];
uint32_t sessionStartMs = 0;
bool sessionValid = false;
bool denyUntil = false;
uint32_t denyUntilMs = 0;
bool openPending = false;
uint32_t openSentMs = 0;
bool otaMode = false;

bool actorPending = false;
uint32_t actorSentMs = 0;
uint32_t lastActorNonce = 0;

String currentText = "";

void logDebug(const char *fmt, ...) {
#if DEBUG
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
#endif
}

void logPeer(const char *label, const uint8_t mac[6]) {
#if DEBUG
  char buf[32];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print(label);
  Serial.println(buf);
#endif
}

void drawCentered(const String &text) {
  if (text == currentText) return;
  currentText = text;
  display.clearDisplay();
  int16_t x1, y1;
  uint16_t w, h;
  display.setFont(&FreeSansBold12pt7b);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (display.width() - w) / 2 - x1;
  int16_t y = (display.height() + h) / 2;
  display.setCursor(x, y);
  display.print(text);
  display.display();
}

const SenderSecret *findSelfSecret(uint8_t senderId) {
  for (size_t i = 0; i < SENDER_SECRETS_COUNT; i++) {
    if (SENDER_SECRETS[i].sender_id == senderId) {
      return &SENDER_SECRETS[i];
    }
  }
  return nullptr;
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

void computeActorTag(uint8_t out[16], uint8_t type, uint8_t command, uint32_t nonce) {
  const char prefix[] = "ACTR";
  uint8_t buf[4 + 1 + 1 + 4 + 6 + 6];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = type;
  buf[idx++] = command;
  memcpy(buf + idx, &nonce, sizeof(nonce)); idx += sizeof(nonce);
  memcpy(buf + idx, CONTROLLER_COMBINED_MAC, 6); idx += 6;
  memcpy(buf + idx, ACTOR_MAC, 6); idx += 6;
  hmacTrunc16(ACTOR_KEY, sizeof(ACTOR_KEY), buf, idx, out);
}

void ensurePeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_err_t res = esp_now_add_peer(&peer);
  logDebug("Add peer res=%d", res);
}

void sendHello() {
  ensurePeer(RECEIVER_MAC);
  DoorMessage msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_HELLO;
  msg.sender_id = SENDER_ID;
  esp_now_send(RECEIVER_MAC, (uint8_t *)&msg, sizeof(msg));
}

void sendOpen() {
  if (!sessionValid) return;
  ensurePeer(RECEIVER_MAC);
  DoorMessage msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_OPEN;
  msg.sender_id = SENDER_ID;
  msg.session_id = currentSessionId;
  memcpy(msg.nonce, receiverNonce, 16);
  computeOpenTag(msg.tag, currentSessionId, receiverNonce);
  esp_err_t res = esp_now_send(RECEIVER_MAC, (uint8_t *)&msg, sizeof(msg));
  logDebug("OPEN send res=%d", res);
  if (res == ESP_OK) {
    openPending = true;
    openSentMs = millis();
  }
}

void sendActorToggle() {
  ensurePeer(ACTOR_MAC);
  ActorMessage msg = {};
  msg.version = ACTOR_PROTO_VERSION;
  msg.type = ACTOR_MSG_CMD;
  msg.command = ACTOR_CMD_TOGGLE;
  msg.nonce = esp_random();
  computeActorTag(msg.tag, msg.type, msg.command, msg.nonce);
  esp_err_t res = esp_now_send(ACTOR_MAC, (uint8_t *)&msg, sizeof(msg));
  logDebug("Actor toggle send res=%d", res);
  if (res == ESP_OK) {
    actorPending = true;
    actorSentMs = millis();
    lastActorNonce = msg.nonce;
  }
}

void handleChallenge(const DoorMessage &msg) {
  if (msg.version != PROTOCOL_VERSION || msg.sender_id != SENDER_ID) return;
  uint8_t expected[16];
  computeChallengeTag(expected, msg.session_id, msg.nonce);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  currentSessionId = msg.session_id;
  memcpy(receiverNonce, msg.nonce, 16);
  sessionStartMs = millis();
  sessionValid = true;
  logDebug("Challenge ok session=%lu", (unsigned long)currentSessionId);
}

void handleAck(const DoorMessage &msg) {
  if (!sessionValid || msg.session_id != currentSessionId) return;
  uint8_t expected[16];
  computeAckTag(expected, msg.session_id, msg.reserved);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  denyUntil = false;
  openPending = false;
  logDebug("Open ACK");
}

void handleDeny(const DoorMessage &msg) {
  if (!sessionValid || msg.session_id != currentSessionId) return;
  uint8_t expected[16];
  computeDenyTag(expected, msg.session_id, msg.reserved);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  openPending = false;
  denyUntil = true;
  denyUntilMs = millis() + DENY_DISPLAY_MS;
  drawCentered("Denay");
  logDebug("Open DENY code=%d", msg.reserved);
}

void handleActorAck(const ActorMessage &msg) {
  if (!actorPending) return;
  uint8_t expected[16];
  computeActorTag(expected, msg.type, msg.command, msg.nonce);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  if (msg.nonce != lastActorNonce) return;
  actorPending = false;
  logDebug("Actor ACK");
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  if (len == sizeof(DoorMessage)) {
    DoorMessage msg;
    memcpy(&msg, incomingData, sizeof(DoorMessage));
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
  } else if (len == sizeof(ActorMessage)) {
    ActorMessage msg;
    memcpy(&msg, incomingData, sizeof(ActorMessage));
    if (msg.version != ACTOR_PROTO_VERSION) return;
    if (msg.type == ACTOR_MSG_ACK) {
      handleActorAck(msg);
    }
  }
}

void onDataSent(const uint8_t *, esp_now_send_status_t status) {
  logDebug("Send status=%d", status);
}

void enterOtaMode() {
  otaMode = true;
  drawCentered("OTA");
  esp_now_deinit();
  WiFi.mode(WIFI_STA);
#ifdef OTA_WIFI_SSID
  WiFi.begin(OTA_WIFI_SSID, OTA_WIFI_PASSWORD);
#endif
  ArduinoOTA.setHostname("ControllerOTA_DC");
  ArduinoOTA.begin();
}

void setupDisplay() {
  Wire.begin(SDA_PIN, SCL_PIN, I2C_FREQ);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(&FreeSansBold12pt7b);
  drawCentered("-");
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_mac(WIFI_IF_STA, CONTROLLER_COMBINED_MAC);
  esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  logPeer("Self MAC=", selfMac);
  if (esp_now_init() != ESP_OK) {
    logDebug("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  ensurePeer(RECEIVER_MAC);
  ensurePeer(ACTOR_MAC);
}

void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(AUX_LED_PIN, OUTPUT);
  digitalWrite(AUX_LED_PIN, LOW);
  setupDisplay();

  selfSecret = findSelfSecret(SENDER_ID);
  if (!selfSecret) {
    logDebug("Sender secret missing");
    while (true) delay(1000);
  }

  setupEspNow();
}

void loop() {
  static uint32_t lastHello = 0;
  uint32_t now = millis();

  if (otaMode) {
    ArduinoOTA.handle();
    return;
  }

  if (now - lastHello > HELLO_INTERVAL_MS) {
    sendHello();
    lastHello = now;
  }

  if (sessionValid && (now - sessionStartMs > SESSION_TTL_MS)) {
    sessionValid = false;
    openPending = false;
  }

  bool link = sessionValid && (now - sessionStartMs < IN_RANGE_TIMEOUT_MS);
  if (openPending && (now - openSentMs > OPEN_TIMEOUT_MS)) {
    openPending = false;
    denyUntil = true;
    denyUntilMs = now + DENY_DISPLAY_MS;
    drawCentered("Denay");
  }
  if (denyUntil && now > denyUntilMs) {
    denyUntil = false;
  }
  if (actorPending && (now - actorSentMs > ACTOR_CMD_TIMEOUT_MS)) {
    actorPending = false;
  }

  static uint32_t lastButtonChange = 0;
  static bool lastButtonState = HIGH;
  static uint32_t pressStart = 0;
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastButtonChange = now;
    lastButtonState = reading;
    if (reading == LOW) pressStart = now;
  }

  if ((now - lastButtonChange) > DEBOUNCE_MS && reading == LOW) {
    // hold detection happens on release
  }

  if ((now - lastButtonChange) > DEBOUNCE_MS && reading == HIGH && pressStart != 0) {
    uint32_t held = now - pressStart;
    pressStart = 0;
    if (held >= OTA_HOLD_MS) {
      enterOtaMode();
    } else {
      if (link && !denyUntil) {
        sendOpen();
      } else {
        sendActorToggle();
        drawCentered("-");
      }
    }
  }

  if (denyUntil) {
    drawCentered("Denay");
  } else if (link) {
    drawCentered("Link");
  } else {
    drawCentered("-");
  }
}
