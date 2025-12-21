// ActorOTA_DC.ino - Remote switch actor with OTA, secured via HMAC over ESP-NOW
// Requires user-supplied doorLockData.h alongside this sketch.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <mbedtls/md.h>
#include <ArduinoOTA.h>
#include "doorLockData.h"

#define ACTOR_PROTO_VERSION 1
#define ACTOR_MSG_CMD 1
#define ACTOR_MSG_ACK 2
#define ACTOR_CMD_TOGGLE 1

// ==== CONFIGURATION ====
#define RELAY_PIN 2
#define STATUS_LED_PIN 8   // WS2812 is unavailable here; use GPIO LED if attached
#define OTA_TRIGGER_PIN -1 // set to a GPIO (active low) to force OTA on boot
#define DEBUG 1
// =======================

struct __attribute__((packed)) ActorMessage {
  uint8_t version;
  uint8_t type;
  uint8_t command;
  uint8_t reserved;
  uint32_t nonce;
  uint8_t tag[16];
};

uint32_t lastNonce = 0;
bool relayState = false;
bool otaMode = false;

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

void sendAck(const uint8_t *mac, uint32_t nonce, uint8_t command) {
  ActorMessage msg = {};
  msg.version = ACTOR_PROTO_VERSION;
  msg.type = ACTOR_MSG_ACK;
  msg.command = command;
  msg.nonce = nonce;
  computeActorTag(msg.tag, msg.type, msg.command, msg.nonce);
  esp_now_send(mac, (uint8_t *)&msg, sizeof(msg));
}

void setRelay(bool on) {
  relayState = on;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
}

void toggleRelay() {
  setRelay(!relayState);
}

void maybeUpdateLed(bool on) {
#if STATUS_LED_PIN >= 0
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
}

void handleCommand(const uint8_t *mac, const ActorMessage &msg) {
  if (msg.command != ACTOR_CMD_TOGGLE) return;
  uint8_t expected[16];
  computeActorTag(expected, msg.type, msg.command, msg.nonce);
  if (!constantTimeEqual(expected, msg.tag, 16)) {
    logDebug("Tag mismatch");
    return;
  }
  if (msg.nonce == lastNonce) {
    logDebug("Replay nonce");
    return;
  }
  lastNonce = msg.nonce;
  toggleRelay();
  maybeUpdateLed(relayState);
  sendAck(mac, msg.nonce, msg.command);
  logDebug("Toggle -> %d", relayState);
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  if (len != sizeof(ActorMessage)) return;
  ActorMessage msg;
  memcpy(&msg, incomingData, sizeof(ActorMessage));
  if (msg.version != ACTOR_PROTO_VERSION || msg.type != ACTOR_MSG_CMD) return;
  if (memcmp(info->src_addr, CONTROLLER_COMBINED_MAC, 6) != 0) {
    logDebug("Unknown src");
    return;
  }
  handleCommand(info->src_addr, msg);
}

void onDataSent(const uint8_t *, esp_now_send_status_t status) {
  logDebug("Send status=%d", status);
}

void enterOtaMode() {
  otaMode = true;
  esp_now_deinit();
  WiFi.mode(WIFI_STA);
#ifdef OTA_WIFI_SSID
  WiFi.begin(OTA_WIFI_SSID, OTA_WIFI_PASSWORD);
#endif
  ArduinoOTA.setHostname("ActorOTA_DC");
  ArduinoOTA.begin();
  logDebug("OTA mode");
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_mac(WIFI_IF_STA, ACTOR_MAC);
  if (esp_now_init() != ESP_OK) {
    logDebug("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, CONTROLLER_COMBINED_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif
  if (OTA_TRIGGER_PIN >= 0) {
    pinMode(OTA_TRIGGER_PIN, INPUT_PULLUP);
  }

  if (OTA_TRIGGER_PIN >= 0 && digitalRead(OTA_TRIGGER_PIN) == LOW) {
    enterOtaMode();
  } else {
    setupEspNow();
  }
}

void loop() {
  if (otaMode) {
    ArduinoOTA.handle();
    return;
  }
}
