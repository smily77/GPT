// DoorSENDER.ino - ESP32-C3 sender with OLED + button

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <mbedtls/md.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold12pt7b.h>

// === CONFIGURABLE PARAMETERS ===
#define WIFI_CHANNEL 6
#define SENDER_ID 1
static const uint8_t RECEIVER_MAC[6] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};
static const uint8_t K_SENDER[32] = {
  0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
  0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,
  0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
  0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20
};
#define BUTTON_PIN 0
#define SDA_PIN 8
#define SCL_PIN 10
#define I2C_FREQ 400000
#define IN_RANGE_TIMEOUT_MS 3000
#define SESSION_TTL_MS 10000
#define DENY_DISPLAY_MS 3000
#define OPEN_TIMEOUT_MS 1000
#define DEBOUNCE_MS 60
#define DEBUG 1
// ================================

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

Adafruit_SSD1306 display(128, 32, &Wire, -1);

uint32_t currentSessionId = 0;
uint8_t receiverNonce[16];
uint32_t sessionStartMs = 0;
bool sessionValid = false;
bool denyUntil = false;
uint32_t denyUntilMs = 0;
bool openPending = false;
uint32_t openSentMs = 0;

volatile bool sendOpenRequest = false;

String currentText = "";

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
  hmacTrunc16(K_SENDER, sizeof(K_SENDER), buf, idx, out);
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
  hmacTrunc16(K_SENDER, sizeof(K_SENDER), buf, idx, out);
}

void computeAckTag(uint8_t out[16], uint32_t session_id, uint8_t result_code) {
  const char prefix[] = "ACK";
  uint8_t buf[3 + 1 + 4 + 1];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 3); idx += 3;
  buf[idx++] = SENDER_ID;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  buf[idx++] = result_code;
  hmacTrunc16(K_SENDER, sizeof(K_SENDER), buf, idx, out);
}

void computeDenyTag(uint8_t out[16], uint32_t session_id, uint8_t reason_code) {
  const char prefix[] = "DENY";
  uint8_t buf[4 + 1 + 4 + 1];
  size_t idx = 0;
  memcpy(buf + idx, prefix, 4); idx += 4;
  buf[idx++] = SENDER_ID;
  memcpy(buf + idx, &session_id, sizeof(session_id)); idx += sizeof(session_id);
  buf[idx++] = reason_code;
  hmacTrunc16(K_SENDER, sizeof(K_SENDER), buf, idx, out);
}

void sendHello() {
  Message msg = {};
  msg.version = PROTOCOL_VERSION;
  msg.type = MSG_HELLO;
  msg.sender_id = SENDER_ID;
  esp_now_send(RECEIVER_MAC, (uint8_t *)&msg, sizeof(msg));
}

void sendOpen() {
  if (!sessionValid) return;
  Message msg = {};
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

void handleChallenge(const Message &msg) {
  if (msg.version != PROTOCOL_VERSION || msg.sender_id != SENDER_ID) return;
  uint8_t expected[16];
  computeChallengeTag(expected, msg.session_id, msg.nonce);
  if (!constantTimeEqual(expected, msg.tag, 16)) {
    logDebug("Challenge tag fail");
    return;
  }
  currentSessionId = msg.session_id;
  memcpy(receiverNonce, msg.nonce, 16);
  sessionStartMs = millis();
  sessionValid = true;
  logDebug("Challenge ok session=%lu", (unsigned long)currentSessionId);
}

void handleAck(const Message &msg) {
  if (!sessionValid || msg.session_id != currentSessionId) return;
  uint8_t expected[16];
  computeAckTag(expected, msg.session_id, msg.reserved);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  logDebug("Open ACK received");
  denyUntil = false;
   openPending = false;
}

void handleDeny(const Message &msg) {
  if (!sessionValid || msg.session_id != currentSessionId) return;
  uint8_t expected[16];
  computeDenyTag(expected, msg.session_id, msg.reserved);
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
  openPending = false;
  denyUntil = true;
  denyUntilMs = millis() + DENY_DISPLAY_MS;
  drawCentered("Denay");
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  (void)info;
  if (len != (int)sizeof(Message)) return;
  Message msg;
  memcpy(&msg, incomingData, sizeof(Message));
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
}

void onDataSent(const uint8_t *, esp_now_send_status_t status) {
  logDebug("Send status=%d", status);
}

void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(SDA_PIN, SCL_PIN, I2C_FREQ);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(&FreeSansBold12pt7b);
  drawCentered("Wait");

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    logDebug("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void loop() {
  static uint32_t lastHello = 0;
  uint32_t now = millis();
  if (now - lastHello > 500) {
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
  if (denyUntil && millis() > denyUntilMs) {
    denyUntil = false;
  }

  static uint32_t lastButtonChange = 0;
  static bool lastButtonState = HIGH;
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastButtonChange = now;
    lastButtonState = reading;
  }
  if ((now - lastButtonChange) > DEBOUNCE_MS && reading == LOW) {
    if (link && !denyUntil) {
      sendOpen();
    } else {
      drawCentered("Wait");
      openPending = false;
    }
    while (digitalRead(BUTTON_PIN) == LOW) {
      delay(10);
    }
  }

  if (denyUntil) {
    drawCentered("Denay");
  } else if (link) {
    drawCentered("Link");
  } else {
    drawCentered("Wait");
  }
}
