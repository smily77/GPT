// DoorRECEIVER.ino - ESP32-C3 garage door receiver with relay
// === CONFIGURABLE PARAMETERS ===
#define WIFI_CHANNEL 6
#define RELAY_PIN 2
#define SESSION_TTL_MS 10000
#define IN_RANGE_TIMEOUT_MS 3000
#define RELAY_PULSE_MS 350
#define DEBUG 1

// Allowlisted senders (update MACs/keys)
struct SenderConfig {
  uint8_t sender_id;
  uint8_t mac[6];
  uint8_t key[32];
};

static SenderConfig senders[] = {
  {1, {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33}, {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20}}
};
const size_t NUM_SENDERS = sizeof(senders) / sizeof(senders[0]);
// ================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <mbedtls/md.h>

#define PROTOCOL_VERSION 1
#define MSG_HELLO 1
#define MSG_CHALLENGE 2
#define MSG_OPEN 3
#define MSG_OPEN_ACK 4
#define MSG_DENY 5

struct __attribute__((packed)) Message {
  uint8_t version;
  uint8_t type;
  uint8_t sender_id;
  uint8_t reserved;
  uint32_t session_id;
  uint8_t nonce[16];
  uint8_t tag[16];
};

struct SessionState {
  uint32_t session_id = 0;
  uint8_t nonce[16] = {0};
  uint32_t expires_at = 0;
  bool used = false;
};

SessionState sessions[8];
uint8_t selfMac[6];

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
  return diff == 0;
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

const SenderConfig *findSender(uint8_t sender_id, const uint8_t mac[6]) {
  for (size_t i = 0; i < NUM_SENDERS; i++) {
    if (senders[i].sender_id == sender_id && memcmp(senders[i].mac, mac, 6) == 0) {
      return &senders[i];
    }
  }
  return nullptr;
}

void computeChallengeTag(uint8_t out[16], const SenderConfig &sc, uint32_t session_id, const uint8_t nonce[16]) {
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

void computeOpenTag(uint8_t out[16], const SenderConfig &sc, uint32_t session_id, const uint8_t nonce[16]) {
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

void computeAckTag(uint8_t out[16], const SenderConfig &sc, uint32_t session_id, uint8_t result_code) {
  const char prefix[] = "ACK";
  uint8_t buf[3 + 1 + 4 + 1];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 3); idx += 3;
  buf[idx++] = sc.sender_id;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  buf[idx++] = result_code;
  hmacTrunc16(sc.key, sizeof(sc.key), buf, idx, out);
}

void computeDenyTag(uint8_t out[16], const SenderConfig &sc, uint32_t session_id, uint8_t reason_code) {
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

void sendChallenge(const SenderConfig &sc, const uint8_t *mac) {
  SessionState &ss = sessions[sc.sender_id % 8];
  ss.session_id = rand32();
  fillRandom(ss.nonce, 16);
  ss.expires_at = millis() + SESSION_TTL_MS;
  ss.used = false;

  Message msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_CHALLENGE;
  msg.sender_id = sc.sender_id;
  msg.session_id = ss.session_id;
  memcpy(msg.nonce, ss.nonce, 16);
  computeChallengeTag(msg.tag, sc, ss.session_id, ss.nonce);
  sendMessage(mac, msg);
  logDebug("Challenge to sender %d", sc.sender_id);
}

void sendAck(const SenderConfig &sc, uint32_t session_id, const uint8_t *mac, uint8_t code) {
  Message msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_OPEN_ACK;
  msg.sender_id = sc.sender_id;
  msg.session_id = session_id;
  msg.reserved = code;
  computeAckTag(msg.tag, sc, session_id, code);
  sendMessage(mac, msg);
}

void sendDeny(const SenderConfig &sc, uint32_t session_id, const uint8_t *mac, uint8_t code) {
  Message msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_DENY;
  msg.sender_id = sc.sender_id;
  msg.session_id = session_id;
  msg.reserved = code;
  computeDenyTag(msg.tag, sc, session_id, code);
  sendMessage(mac, msg);
}

bool relayPulse = false;
uint32_t relayUntil = 0;

void handleHello(const SenderConfig &sc, const uint8_t *mac) {
  sendChallenge(sc, mac);
}

void handleOpen(const SenderConfig &sc, const uint8_t *mac, const Message &msg) {
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
  relayPulse = true;
  relayUntil = millis() + RELAY_PULSE_MS;
  sendAck(sc, msg.session_id, mac, 0);
  logDebug("Open accepted sender=%d", sc.sender_id);
}

void ensurePeer(const uint8_t *mac) {
  esp_now_peer_info_t peer = {};
  if (esp_now_is_peer_exist(mac)) return;
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len != sizeof(Message)) return;
  Message msg;
  memcpy(&msg, incomingData, sizeof(Message));
  const SenderConfig *sc = findSender(msg.sender_id, mac);
  if (!sc) {
    logDebug("Unknown sender or MAC mismatch");
    return;
  }
  if (msg.version != PROTOCOL_VERSION) return;
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

void onDataSent(const uint8_t *, esp_now_send_status_t status) {
  logDebug("Send status=%d", status);
}

void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, selfMac);

  if (esp_now_init() != ESP_OK) {
    logDebug("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  for (size_t i = 0; i < NUM_SENDERS; i++) {
    ensurePeer(senders[i].mac);
  }
}

void loop() {
  uint32_t now = millis();
  if (relayPulse && now > relayUntil) {
    digitalWrite(RELAY_PIN, LOW);
    relayPulse = false;
  }
  if (relayPulse) {
    digitalWrite(RELAY_PIN, HIGH);
  }

  for (size_t i = 0; i < 8; i++) {
    if (sessions[i].session_id != 0 && now > sessions[i].expires_at) {
      sessions[i].session_id = 0;
      sessions[i].used = false;
    }
  }
}
