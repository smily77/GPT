// DoorRECEIVER_SAFETY.ino - ESP32-C3 garage door receiver with relay and OPTIONAL safety permit protocol
// This version adds a safety permit mechanism for an optional secondary relay box (DoorSLAVE_SAFETY).
// The safety extension is OPTIONAL and the system works correctly even if no slave is present.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <mbedtls/md.h>
#include <Adafruit_NeoPixel.h>
#include <doorLockData.h>

// === CONFIGURABLE PARAMETERS ===
#define RELAY_PIN 2
#define STATUS_PIXEL_PIN 8
#define SESSION_TTL_MS 10000
#define IN_RANGE_TIMEOUT_MS 3000
#define RELAY_PULSE_MS 350
#define DEBUG 1
// ================================

#define PROTOCOL_VERSION 1
#define MSG_HELLO 1
#define MSG_CHALLENGE 2
#define MSG_OPEN 3
#define MSG_OPEN_ACK 4
#define MSG_DENY 5

// Safety permit message types (separate from secure door protocol)
#define PERMIT_MSG_PERMIT1 0x01
#define PERMIT_MSG_PERMIT2 0x02
#define PERMIT_MAGIC 0x53414645UL  // "SAFE" in ASCII

struct __attribute__((packed)) Message {
  uint8_t version;
  uint8_t type;
  uint8_t sender_id;
  uint8_t reserved;
  uint32_t session_id;
  uint8_t nonce[16];
  uint8_t tag[16];
};

struct __attribute__((packed)) PermitMessage {
  uint8_t type;           // PERMIT_MSG_PERMIT1 or PERMIT_MSG_PERMIT2
  uint32_t magic;         // PERMIT_MAGIC
  uint16_t nonce;         // Small nonce for matching PERMIT1 and PERMIT2
  uint8_t reserved[3];    // Padding to 10 bytes
};

struct SessionState {
  uint32_t session_id = 0;
  uint8_t nonce[16] = {0};
  uint32_t expires_at = 0;
  bool used = false;
};

SessionState sessions[8];
uint8_t selfMac[6];
Adafruit_NeoPixel statusPixel(1, STATUS_PIXEL_PIN, NEO_GRB + NEO_KHZ800);
bool relayPulse = false;
uint32_t relayUntil = 0;
uint32_t lastContactMs[8] = {0};

// Safety permit state
bool safetySlaveConfigured = false;
uint16_t currentPermitNonce = 0;

void setStatusColor(uint32_t color) {
  statusPixel.setPixelColor(0, color);
  statusPixel.show();
}

uint32_t colorOff() { return statusPixel.Color(0, 0, 0); }
uint32_t colorGreen() { return statusPixel.Color(0, 255, 0); }
uint32_t colorBlue() { return statusPixel.Color(0, 0, 255); }

bool hasActiveSession(uint32_t now) {
  for (size_t i = 0; i < 8; i++) {
    if (lastContactMs[i] != 0 && (now - lastContactMs[i]) <= IN_RANGE_TIMEOUT_MS) {
      return true;
    }
  }
  return false;
}

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
  return diff == 0;
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

void logDebug(const char *fmt, ...) {
#if DEBUG
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
#endif
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

const SenderSecret *findSender(uint8_t sender_id, const uint8_t mac[6]) {
  for (size_t i = 0; i < SENDER_SECRETS_COUNT; i++) {
    if (SENDER_SECRETS[i].sender_id == sender_id && memcmp(SENDER_SECRETS[i].sender_mac, mac, 6) == 0) {
      return &SENDER_SECRETS[i];
    }
  }
  return nullptr;
}

void computeChallengeTag(uint8_t out[16], const SenderSecret &sc, uint32_t session_id, const uint8_t nonce[16]) {
  const char prefix[] = "CHAL";
  uint8_t buf[4 + 1 + 4 + 16 + 6];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = sc.sender_id;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  memcpy(buf + idx, nonce, 16); idx += 16;
  memcpy(buf + idx, selfMac, 6); idx += 6;
  hmacTrunc16(sc.key, sizeof(sc.key), buf, idx, out);
}

void computeOpenTag(uint8_t out[16], const SenderSecret &sc, uint32_t session_id, const uint8_t nonce[16]) {
  const char prefix[] = "OPEN";
  uint8_t buf[4 + 1 + 4 + 16 + 6];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = sc.sender_id;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  memcpy(buf + idx, nonce, 16); idx += 16;
  memcpy(buf + idx, selfMac, 6); idx += 6;
  hmacTrunc16(sc.key, sizeof(sc.key), buf, idx, out);
}

void computeAckTag(uint8_t out[16], const SenderSecret &sc, uint32_t session_id, uint8_t result_code) {
  const char prefix[] = "ACK";
  uint8_t buf[3 + 1 + 4 + 1];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 3); idx += 3;
  buf[idx++] = sc.sender_id;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  buf[idx++] = result_code;
  hmacTrunc16(sc.key, sizeof(sc.key), buf, idx, out);
}

void computeDenyTag(uint8_t out[16], const SenderSecret &sc, uint32_t session_id, uint8_t reason_code) {
  const char prefix[] = "DENY";
  uint8_t buf[4 + 1 + 4 + 1];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = sc.sender_id;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  buf[idx++] = reason_code;
  hmacTrunc16(sc.key, sizeof(sc.key), buf, idx, out);
}

uint32_t rand32() { return esp_random(); }

void fillRandom(uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i += 4) {
    uint32_t r = esp_random();
    size_t chunk = min((size_t)4, len - i);
    memcpy(buf + i, &r, chunk);
  }
}

void sendMessage(const uint8_t *mac, const Message &msg) {
  esp_now_send(mac, (const uint8_t *)&msg, sizeof(Message));
}

void sendPermit(uint8_t permitType, uint16_t nonce) {
#ifdef DOORSAFETY_SLAVE_MAC
  if (!safetySlaveConfigured) return;

  PermitMessage pm = {};
  pm.type = permitType;
  pm.magic = PERMIT_MAGIC;
  pm.nonce = nonce;

  // Send multiple times for robustness
  for (int i = 0; i < 3; i++) {
    esp_err_t result = esp_now_send(DOORSAFETY_SLAVE_MAC, (const uint8_t *)&pm, sizeof(PermitMessage));
    logDebug("  Permit send attempt %d, result=%d, size=%d", i+1, result, sizeof(PermitMessage));
    delayMicroseconds(500);
  }

  logDebug("Sent PERMIT%d nonce=%u", permitType, nonce);
#endif
}

void sendChallenge(const SenderSecret &sc, const uint8_t *mac) {
  SessionState &ss = sessions[sc.sender_id % 8];
  ss.session_id = rand32();
  fillRandom(ss.nonce, 16);
  ss.expires_at = millis() + SESSION_TTL_MS;
  ss.used = false;
  lastContactMs[sc.sender_id % 8] = millis();

  Message msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_CHALLENGE;
  msg.sender_id = sc.sender_id;
  msg.session_id = ss.session_id;
  memcpy(msg.nonce, ss.nonce, 16);
  computeChallengeTag(msg.tag, sc, ss.session_id, ss.nonce);
  sendMessage(mac, msg);
  logDebug("Challenge to sender %d", sc.sender_id);
  setStatusColor(colorGreen());
}

void sendAck(const SenderSecret &sc, uint32_t session_id, const uint8_t *mac, uint8_t code) {
  Message msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_OPEN_ACK;
  msg.sender_id = sc.sender_id;
  msg.session_id = session_id;
  msg.reserved = code;
  computeAckTag(msg.tag, sc, session_id, code);
  sendMessage(mac, msg);
}

void sendDeny(const SenderSecret &sc, uint32_t session_id, const uint8_t *mac, uint8_t code) {
  Message msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_DENY;
  msg.sender_id = sc.sender_id;
  msg.session_id = session_id;
  msg.reserved = code;
  computeDenyTag(msg.tag, sc, session_id, code);
  sendMessage(mac, msg);
}

void handleHello(const SenderSecret &sc, const uint8_t *mac) {
  lastContactMs[sc.sender_id % 8] = millis();
  sendChallenge(sc, mac);
}

void handleOpen(const SenderSecret &sc, const uint8_t *mac, const Message &msg) {
  lastContactMs[sc.sender_id % 8] = millis();
  SessionState &ss = sessions[sc.sender_id % 8];
  if (msg.session_id != ss.session_id || ss.used || millis() > ss.expires_at) {
    sendDeny(sc, msg.session_id, mac, 1);
      return;
  }
  uint8_t expected[16];
  computeOpenTag(expected, sc, msg.session_id, ss.nonce);
  if (!constantTimeEqual(expected, msg.tag, 16)) {
    sendDeny(sc, msg.session_id, mac, 2);
      return;
  }
  ss.used = true;

  // Generate fresh permit nonce
  currentPermitNonce = (uint16_t)(esp_random() & 0xFFFF);
  logDebug("=== DOOR OPEN - Starting permit sequence ===");

  // SAFETY PERMIT PROTOCOL:
  // 1) Send PERMIT1 to slave (if configured)
  sendPermit(PERMIT_MSG_PERMIT1, currentPermitNonce);

  // 2) Pulse local relay
  relayPulse = true;
  relayUntil = millis() + RELAY_PULSE_MS;
  setStatusColor(colorBlue());
  logDebug("Local relay activated");

  // 3) Send PERMIT2 to slave (if configured)
  sendPermit(PERMIT_MSG_PERMIT2, currentPermitNonce);

  // 4) Optional late backup sends
  delay(10);
  sendPermit(PERMIT_MSG_PERMIT1, currentPermitNonce);
  sendPermit(PERMIT_MSG_PERMIT2, currentPermitNonce);

  sendAck(sc, msg.session_id, mac, 0);
  logDebug("Open accepted sender=%d", sc.sender_id);
}

void ensurePeer(const uint8_t *mac) {
  esp_now_peer_info_t peer = {};
  if (esp_now_is_peer_exist(mac)) {
    logDebug("Peer already exists");
    return;
  }
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_err_t res = esp_now_add_peer(&peer);
  if (res == ESP_OK) {
    logDebug("Peer added successfully");
  } else {
    logDebug("Add peer FAILED, error=%d", res);
  }
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  if (!info) return;
  const uint8_t *mac = info->src_addr;
  logPeer("RX from ", mac);
  if (len != (int)sizeof(Message)) return;
  Message msg;
  memcpy(&msg, incomingData, sizeof(Message));
  const SenderSecret *sc = findSender(msg.sender_id, mac);
  if (!sc) {
    logDebug("Unknown sender or MAC mismatch");
      return;
  }
  if (msg.version != PROTOCOL_VERSION) {
      return;
  }
  ensurePeer(mac);
  switch (msg.type) {
    case MSG_HELLO:
      handleHello(*sc, mac);
      break;
    case MSG_OPEN:
      handleOpen(*sc, mac, msg);
      break;
    default:
      break;
  }
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    logPeer("Send FAILED to ", mac);
    logDebug("Send status=%d (0=success, 1=fail)", status);
  }
}

void setup() {
#if DEBUG
  Serial.begin(115200);
#endif

  // CRITICAL: Relay MUST default to OFF on boot (including watchdog reset)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  statusPixel.begin();
  statusPixel.clear();
  statusPixel.show();

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  logPeer("Receiver MAC ", selfMac);
  logDebug("WiFi Channel: %d", WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    logDebug("ESP-NOW init failed");
    while (true) {
          delay(1000);
    }
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  for (size_t i = 0; i < SENDER_SECRETS_COUNT; i++) {
    ensurePeer(SENDER_SECRETS[i].sender_mac);
  }

  // Check if safety slave is configured
#ifdef DOORSAFETY_SLAVE_MAC
  safetySlaveConfigured = true;
  logDebug("=== SAFETY SLAVE CONFIGURATION ===");
  logPeer("Slave MAC: ", DOORSAFETY_SLAVE_MAC);
  logDebug("Slave channel: %d", WIFI_CHANNEL);
  ensurePeer(DOORSAFETY_SLAVE_MAC);
  logDebug("Safety slave configured and peer added");
#else
  safetySlaveConfigured = false;
  logDebug("Safety slave NOT configured - operating standalone");
#endif

  logDebug("=== DoorRECEIVER_SAFETY ready ===");
}

void loop() {

  uint32_t now = millis();
  if (relayPulse && now > relayUntil) {
    digitalWrite(RELAY_PIN, LOW);
    relayPulse = false;
  }
  if (relayPulse) {
      digitalWrite(RELAY_PIN, HIGH);
    setStatusColor(colorBlue());
  } else {
    digitalWrite(RELAY_PIN, LOW);
  }

  for (size_t i = 0; i < 8; i++) {
    if (sessions[i].session_id != 0 && now > sessions[i].expires_at) {
      sessions[i].session_id = 0;
      sessions[i].used = false;
      lastContactMs[i] = 0;
    }
  }

  if (!relayPulse) {
    setStatusColor(hasActiveSession(now) ? colorGreen() : colorOff());
  }

  delay(10);
}
