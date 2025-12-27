// Display state enum (must precede includes so Arduino prototypes see it)
enum class DisplayMode {
  FogReady,
  FogOn,
  Garage,
  GarageDeny,
  NoLink,
  NoPower,
  OTAReq,
  GoOTA,
  OTA,
  OTAError,
  Updating,
  Done,
  WiFiFail,
  None
};

#include <Arduino.h>
#include <Streaming.h>

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Credentials.h>
#include <mbedtls/md.h>
#include <doorLockData.h>

#if defined(Atom3)
  #define PLATFORM_ATOM3 1
#elif defined(Original)
  #define PLATFORM_ORIGINAL 1
#elif defined(Switch_Light)
  #define PLATFORM_SWITCH_LIGHT 1
#else
  #error "Define Atom3, Original, or Switch_Light in doorLockData.h for ControllerOTA_DC_GEN"
#endif

#if PLATFORM_ATOM3
  #include <M5Unified.h>
#elif PLATFORM_ORIGINAL
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  #include <Fonts/FreeSansBold12pt7b.h>
#elif PLATFORM_SWITCH_LIGHT
  #include <Adafruit_NeoPixel.h>
#endif

#define DEBUG false

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
constexpr uint8_t SENDER_ID = CONTROLLER_SENDER_ID;
uint8_t controllerMac[6] = {0};
uint8_t actorPeerMac[6] = {0};
bool actorMacKnown = false;

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
constexpr unsigned long STATUS_TIMEOUT_MS = 12000;
constexpr unsigned long COMMAND_INTERVAL_MS = 1000;
unsigned long lastCommandSentMs = 0;

constexpr unsigned long IN_RANGE_TIMEOUT_MS = 8000;
constexpr unsigned long SESSION_TTL_MS = 15000;
constexpr unsigned long DENY_DISPLAY_MS = 3000;
constexpr unsigned long OPEN_TIMEOUT_MS = 1000;
constexpr unsigned long HELLO_INTERVAL_MS = 300;

// ====== BUTTON ======
unsigned long buttonPressStartTime = 0;
bool buttonLongPressHandled = false;
constexpr unsigned long LONG_PRESS_MS = 2000;

#if PLATFORM_ATOM3
// ====== DISPLAY / INPUT (Atom S3) ======
M5GFX &gfx = M5.Display;
DisplayMode currentDisplay = DisplayMode::OTA;

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

  gfx.fillCircle(cx - 16, cy, radius, fg);
  gfx.fillRect(cx - 16, cy - radius, radius * 2, radius * 2, bg);
  gfx.fillRoundRect(cx - 10, cy - 18, 64, 36, 10, fg);
  gfx.fillRect(cx + 22, cy - 24, 6, 48, fg);
  gfx.fillRect(cx - 30, cy - 4, 80, 8, fg);
  for (int i = -16; i <= 16; i += 16) {
    gfx.drawLine(cx + 30, cy + i, cx + 52, cy + i, fg);
  }
}

void drawGarageRemoteIcon(uint16_t fg, uint16_t bg) {
  gfx.fillScreen(bg);
  int w = gfx.width();
  int h = gfx.height();
  gfx.fillRoundRect(w / 2 - 42, h / 2 - 36, 84, 72, 10, fg);
  gfx.fillRoundRect(w / 2 - 34, h / 2 - 28, 68, 56, 8, bg);
  gfx.fillRect(w / 2 - 28, h / 2 - 8, 56, 12, fg);
  int cx = w / 2 + 26;
  int cy = h / 2 - 26;
  gfx.drawCircle(cx, cy, 12, fg);
  gfx.drawCircle(cx, cy, 18, fg);
  gfx.fillCircle(cx, cy, 6, fg);
}

void setDisplay(DisplayMode mode) {
  if (mode == currentDisplay) return;
  currentDisplay = mode;
  switch (mode) {
    case DisplayMode::FogReady:   drawFogIcon(COLOR_BG, COLOR_BLUE); break;
    case DisplayMode::FogOn:      drawFogIcon(COLOR_YELLOW, COLOR_BLACK); break;
    case DisplayMode::Garage:     drawGarageRemoteIcon(COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::GarageDeny: drawCenteredText("DENY", COLOR_WHITE, COLOR_RED); break;
    case DisplayMode::NoLink:     drawCenteredText("No Link", COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::NoPower:    drawCenteredText("No Power", COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::OTAReq:     drawCenteredText("OTA req.", COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::GoOTA:      drawCenteredText("go OTA", COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::OTA:        drawCenteredText("OTA", COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::OTAError:   drawCenteredText("OTA Error", COLOR_WHITE, COLOR_RED); break;
    case DisplayMode::Updating:   drawCenteredText("Updating", COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::Done:       drawCenteredText("Done", COLOR_WHITE, COLOR_BG); break;
    case DisplayMode::WiFiFail:   drawCenteredText("WiFi Fail", COLOR_WHITE, COLOR_RED); break;
    case DisplayMode::None:
    default:
      gfx.fillScreen(COLOR_BG);
      break;
  }
}

// Convenience wrappers
void showReady() { setDisplay(DisplayMode::FogReady); }
void showOn() { setDisplay(DisplayMode::FogOn); }
void showGarage(bool deny) { setDisplay(deny ? DisplayMode::GarageDeny : DisplayMode::Garage); }
void showNoLink() { setDisplay(DisplayMode::NoLink); }
void showNoPower() { setDisplay(DisplayMode::NoPower); }
void showOtaReq() { setDisplay(DisplayMode::OTAReq); }
void showGoOta() { setDisplay(DisplayMode::GoOTA); }
void showOta() { setDisplay(DisplayMode::OTA); }
void showOtaError() { setDisplay(DisplayMode::OTAError); }
void showUpdating() { setDisplay(DisplayMode::Updating); }
void showDone() { setDisplay(DisplayMode::Done); }
void showWifiFail() { setDisplay(DisplayMode::WiFiFail); }

#elif PLATFORM_ORIGINAL
// ====== DISPLAY / INPUT (Original ESP32-C3 + SSD1306) ======
constexpr uint8_t PIN_LED = 6;
constexpr uint8_t PIN_BUTTON = 7; // active LOW
constexpr uint8_t PIN_SDA = 8;
constexpr uint8_t PIN_SCL = 9;

constexpr uint32_t LED_PWM_FREQ = 5000;
constexpr uint8_t LED_PWM_RESOLUTION = 8;
constexpr uint8_t LED_BRIGHTNESS_PERCENT = 20;
constexpr uint8_t LED_BRIGHTNESS = (LED_BRIGHTNESS_PERCENT * 255) / 100;

constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 32;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool displayReady = false;
DisplayMode currentDisplay = DisplayMode::OTA;

bool lastReading = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
constexpr unsigned long DEBOUNCE_DELAY_MS = 40;

void drawTextCenter(const String &message, bool keepOn) {
  if (!displayReady) return;
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
  int16_t y = (SCREEN_HEIGHT + h) / 2 - 2;
  display.setCursor(x, y);
  display.print(message);
  display.display();
}

void setDisplay(DisplayMode mode) {
  if (mode == currentDisplay) return;
  currentDisplay = mode;
  switch (mode) {
    case DisplayMode::Garage:      drawTextCenter("Link", true); break;
    case DisplayMode::GarageDeny:  drawTextCenter("Deny", true); break;
    case DisplayMode::FogOn:       drawTextCenter("On", true); break;
    case DisplayMode::FogReady:    drawTextCenter("Ready", true); break;
    case DisplayMode::NoLink:      drawTextCenter("No Link", true); break;
    case DisplayMode::NoPower:     drawTextCenter("No Power", true); break;
    case DisplayMode::OTAReq:      drawTextCenter("OTA req.", true); break;
    case DisplayMode::GoOTA:       drawTextCenter("go OTA", true); break;
    case DisplayMode::OTA:         drawTextCenter("OTA", true); break;
    case DisplayMode::OTAError:    drawTextCenter("OTA Error", true); break;
    case DisplayMode::Updating:    drawTextCenter("Updating", true); break;
    case DisplayMode::Done:        drawTextCenter("Done", true); break;
    case DisplayMode::WiFiFail:    drawTextCenter("WiFi Fail", true); break;
    case DisplayMode::None:
    default:
      drawTextCenter("", false);
      break;
  }
}

// Convenience wrappers
void showReady() { setDisplay(DisplayMode::FogReady); }
void showOn() { setDisplay(DisplayMode::FogOn); }
void showGarage(bool deny) { setDisplay(deny ? DisplayMode::GarageDeny : DisplayMode::Garage); }
void showNoLink() { setDisplay(DisplayMode::NoLink); }
void showNoPower() { setDisplay(DisplayMode::NoPower); }
void showOtaReq() { setDisplay(DisplayMode::OTAReq); }
void showGoOta() { setDisplay(DisplayMode::GoOTA); }
void showOta() { setDisplay(DisplayMode::OTA); }
void showOtaError() { setDisplay(DisplayMode::OTAError); }
void showUpdating() { setDisplay(DisplayMode::Updating); }
void showDone() { setDisplay(DisplayMode::Done); }
void showWifiFail() { setDisplay(DisplayMode::WiFiFail); }

#elif PLATFORM_SWITCH_LIGHT
// ====== DISPLAY / INPUT (Switch_Light: GPIO LEDs + single pixel) ======
constexpr uint8_t PIN_BLUE = 6;
constexpr uint8_t PIN_YELLOW = 5;
constexpr uint8_t PIN_BUTTON = 7; // active LOW
constexpr uint8_t PIN_BUTTON_ALT = 9; // active LOW (test harness)
constexpr uint8_t PIN_PIXEL = 8;

constexpr uint16_t BLINK_FAST_MS = 150;
constexpr uint16_t BLINK_SLOW_MS = 500;

Adafruit_NeoPixel indicator(1, PIN_PIXEL, NEO_GRB + NEO_KHZ800);
DisplayMode currentDisplay = DisplayMode::OTA;

bool lastBlueState = false;
bool lastYellowState = false;
bool lastReadingSwitch = HIGH;
bool buttonStateSwitch = HIGH;
unsigned long lastDebounceSwitch = 0;
constexpr unsigned long DEBOUNCE_SWITCH_MS = 40;
constexpr uint8_t PIXEL_BRIGHTNESS_PCT = 30; // 30% brightness

void applySwitchLightOutputs(bool blue, bool yellow) {
  if (blue != lastBlueState) {
    digitalWrite(PIN_BLUE, blue ? HIGH : LOW);
    lastBlueState = blue;
  }
  if (yellow != lastYellowState) {
    digitalWrite(PIN_YELLOW, yellow ? HIGH : LOW);
    lastYellowState = yellow;
  }

  uint8_t r = yellow ? 255 : 0;
  uint8_t g = yellow ? 200 : 0;
  uint8_t b = blue ? 255 : 0;
  uint8_t scale = (PIXEL_BRIGHTNESS_PCT * 255) / 100;
  r = (uint16_t(r) * scale) / 255;
  g = (uint16_t(g) * scale) / 255;
  b = (uint16_t(b) * scale) / 255;
  indicator.setPixelColor(0, indicator.Color(r, g, b));
  indicator.show();
}

void updateSwitchLightOutputs(unsigned long now, bool force = false) {
  static unsigned long lastUpdateMs = 0;
  if (!force && (now - lastUpdateMs < 20)) return;
  lastUpdateMs = now;

  bool blueOn = false;
  bool yellowOn = false;
  bool blinkBlue = false;
  bool blinkYellow = false;
  bool blinkOpposite = false;
  uint16_t period = BLINK_SLOW_MS;

  switch (currentDisplay) {
    case DisplayMode::FogReady:
      blueOn = true; yellowOn = false; break;
    case DisplayMode::FogOn:
      blueOn = true; yellowOn = true; break;
    case DisplayMode::Garage:
      blueOn = false; blinkYellow = true; period = BLINK_SLOW_MS; break;
    case DisplayMode::GarageDeny:
      blueOn = false; blinkYellow = true; period = BLINK_FAST_MS; break;
    case DisplayMode::NoLink:
    case DisplayMode::NoPower:
    case DisplayMode::Done:
    case DisplayMode::None:
      blueOn = false; yellowOn = false; break;
    case DisplayMode::OTAReq:
      blinkBlue = true; blinkYellow = true; blinkOpposite = true; period = BLINK_SLOW_MS; break;
    case DisplayMode::GoOTA:
      blinkBlue = true; blinkYellow = true; blinkOpposite = false; period = BLINK_SLOW_MS; break;
    case DisplayMode::OTA:
      blinkBlue = true; blinkYellow = false; period = BLINK_SLOW_MS; yellowOn = true; break;
    case DisplayMode::OTAError:
      blinkBlue = true; blinkYellow = true; blinkOpposite = true; period = BLINK_FAST_MS; break;
    case DisplayMode::Updating:
      blinkBlue = true; blinkYellow = false; period = BLINK_SLOW_MS; yellowOn = true; break;
    case DisplayMode::WiFiFail:
      blinkBlue = true; blinkYellow = true; blinkOpposite = false; period = BLINK_FAST_MS; break;
  }

  if (blinkBlue || blinkYellow) {
    bool phase = ((now / period) % 2) != 0;
    if (blinkBlue) blueOn = phase;
    if (blinkYellow) yellowOn = blinkOpposite ? !phase : phase;
  }

  applySwitchLightOutputs(blueOn, yellowOn);
}

void setDisplay(DisplayMode mode) {
  if (mode == currentDisplay) return;
  currentDisplay = mode;
  updateSwitchLightOutputs(millis(), true);
}

// Convenience wrappers
void showReady() { setDisplay(DisplayMode::FogReady); }
void showOn() { setDisplay(DisplayMode::FogOn); }
void showGarage(bool deny) { setDisplay(deny ? DisplayMode::GarageDeny : DisplayMode::Garage); }
void showNoLink() { setDisplay(DisplayMode::NoLink); }
void showNoPower() { setDisplay(DisplayMode::NoPower); }
void showOtaReq() { setDisplay(DisplayMode::OTAReq); }
void showGoOta() { setDisplay(DisplayMode::GoOTA); }
void showOta() { setDisplay(DisplayMode::OTA); }
void showOtaError() { setDisplay(DisplayMode::OTAError); }
void showUpdating() { setDisplay(DisplayMode::Updating); }
void showDone() { setDisplay(DisplayMode::Done); }
void showWifiFail() { setDisplay(DisplayMode::WiFiFail); }
#endif

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
  if (!constantTimeEqual(expected, msg.tag, 16)) return;
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
  showGarage(true);
}

void onSend(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // No action required, but keeps callback registered for completeness.
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == (int)sizeof(DoorMessage)) {
    DoorMessage msg;
    memcpy(&msg, data, sizeof(DoorMessage));
    switch (msg.type) {
      case MSG_CHALLENGE: handleChallenge(msg); break;
      case MSG_OPEN_ACK:  handleAck(msg); break;
      case MSG_DENY:      handleDeny(msg); break;
      default: break;
    }
    return;
  }

  if (len < 1) return;
  uint8_t msgType = data[0];

  if (msgType == MSG_STATUS && len == sizeof(StatusMessage)) {
    StatusMessage incoming;
    memcpy(&incoming, data, sizeof(StatusMessage));

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

  esp_now_peer_info_t rxPeer = {};
  memcpy(rxPeer.peer_addr, RECEIVER_MAC, 6);
  rxPeer.ifidx = WIFI_IF_STA;
  rxPeer.channel = WIFI_CHANNEL;
  rxPeer.encrypt = false;
  esp_now_add_peer(&rxPeer);

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
      showUpdating();
    });

    ArduinoOTA.onEnd([]() {
      if (DEBUG) Serial << "\nEnd" << endl;
      showDone();
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
      showOtaError();
    });

    ArduinoOTA.begin();
    otaReady = true;
    showOta();
    if (DEBUG) Serial << "OTA Ready" << endl;
  } else {
    if (DEBUG) Serial << "WiFi connection failed - returning to normal mode" << endl;
    showWifiFail();
    delay(2000);

    otaMode = false;
    otaRequested = false;
    otaAckReceived = false;
    otaReady = false;

    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(50);
    setupEspNow();
    showReady();
  }
}

void sendCommand(bool state) {
  if (!actorMacKnown) return;
  CommandMessage msg{MSG_COMMAND, state};
  esp_now_send(actorPeerMac, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
  lastCommandSentMs = millis();
  lastSentCommandState = state;
}

void sendOtaRequest() {
  if (!actorMacKnown) return;
  OtaMessage msg{MSG_OTA_REQUEST};
  esp_now_send(actorPeerMac, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
}

void setup() {
#if defined(Atom3)
  auto cfg = M5.config();
  M5.begin(cfg);
  gfx.setRotation(0);
  gfx.setBrightness(180);
  gfx.setTextFont(2);
#elif PLATFORM_ORIGINAL
  Serial.begin(115200);
  delay(1000);

  ledcAttach(PIN_LED, LED_PWM_FREQ, LED_PWM_RESOLUTION);
  ledcWrite(PIN_LED, 0);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false);
  if (displayReady) {
    display.clearDisplay();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
#elif PLATFORM_SWITCH_LIGHT
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUTTON_ALT, INPUT_PULLUP);
  digitalWrite(PIN_BLUE, LOW);
  digitalWrite(PIN_YELLOW, LOW);
  indicator.begin();
  indicator.clear();
  indicator.show();
#endif

#if PLATFORM_ATOM3
  Serial.begin(115200);
  delay(500);
#endif

  selfSecret = findSenderSecret(SENDER_ID);
  if (!selfSecret) {
    if (DEBUG) Serial << "Sender secret missing" << endl;
    showOtaError();
    while (true) delay(1000);
  }
  memcpy(controllerMac, selfSecret->sender_mac, sizeof(controllerMac));

  memcpy(actorPeerMac, ACTOR_MAC, sizeof(actorPeerMac));
  actorMacKnown = true;

  setDisplay(DisplayMode::None);
  setupEspNow();
}

#if PLATFORM_ATOM3
void handleButton(bool doorLink, bool denyActive) {
  if (M5.BtnA.wasPressed()) {
    buttonPressStartTime = millis();
    buttonLongPressHandled = false;
  }

  if (M5.BtnA.isPressed() && !buttonLongPressHandled && !otaMode) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (pressDuration >= LONG_PRESS_MS) {
      buttonLongPressHandled = true;
      otaMode = true;
      otaRequested = true;
      otaAckReceived = false;
      showOtaReq();
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
#elif PLATFORM_ORIGINAL
void handleButton(bool doorLink, bool denyActive) {
  int reading = digitalRead(PIN_BUTTON);
  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY_MS) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        buttonPressStartTime = millis();
        buttonLongPressHandled = false;
      } else {
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
  }

  if (buttonState == LOW && !buttonLongPressHandled && !otaMode) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (pressDuration >= LONG_PRESS_MS) {
      buttonLongPressHandled = true;
      otaMode = true;
      otaRequested = true;
      otaAckReceived = false;
      showOtaReq();
      sendOtaRequest();
    }
  }

  lastReading = reading;
}
#elif PLATFORM_SWITCH_LIGHT
void handleButton(bool doorLink, bool denyActive) {
  int readingMain = digitalRead(PIN_BUTTON);
  int readingAlt = digitalRead(PIN_BUTTON_ALT);
  int reading = (readingMain == LOW || readingAlt == LOW) ? LOW : HIGH;
  if (reading != lastReadingSwitch) {
    lastDebounceSwitch = millis();
  }

  if ((millis() - lastDebounceSwitch) > DEBOUNCE_SWITCH_MS) {
    if (reading != buttonStateSwitch) {
      buttonStateSwitch = reading;
      if (buttonStateSwitch == LOW) {
        buttonPressStartTime = millis();
        buttonLongPressHandled = false;
      } else {
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
  }

  if (buttonStateSwitch == LOW && !buttonLongPressHandled && !otaMode) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (pressDuration >= LONG_PRESS_MS) {
      buttonLongPressHandled = true;
      otaMode = true;
      otaRequested = true;
      otaAckReceived = false;
      showOtaReq();
      sendOtaRequest();
    }
  }

  lastReadingSwitch = reading;
}
#endif

void loop() {
#if PLATFORM_ATOM3
  M5.update();
#endif
  unsigned long now = millis();

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
    showGarage(true);
  }
  if (denyUntil && now > denyUntilMs) {
    denyUntil = false;
  }

  if (otaMode) {
    if (otaReady) {
      ArduinoOTA.handle();
    } else if (otaRequested && !otaAckReceived) {
      static unsigned long lastOtaRequestMs = 0;
      if (millis() - lastOtaRequestMs > 1000) {
        sendOtaRequest();
        lastOtaRequestMs = millis();
      }
    } else if (otaAckReceived && !otaReady) {
      showGoOta();
      delay(1000);
      setupOTA();
    }
#if PLATFORM_SWITCH_LIGHT
    updateSwitchLightOutputs(now);
#endif
    return;
  }

  if (linkOk && now - lastStatusMs > STATUS_TIMEOUT_MS) {
    linkOk = false;
    powerOk = false;
  }

  handleButton(doorLink, denyUntil);

  if (doorLink) {
    showGarage(denyUntil);
#if PLATFORM_ORIGINAL
    ledcWrite(PIN_LED, 0);
#endif
  } else if (linkOk && powerOk) {
    bool needResend = (desiredRelayState != relayOn) || (now - lastCommandSentMs > COMMAND_INTERVAL_MS) || (lastSentCommandState != desiredRelayState);
    if (needResend) {
      sendCommand(desiredRelayState);
    }

#if PLATFORM_ORIGINAL
    ledcWrite(PIN_LED, relayOn ? LED_BRIGHTNESS : 0);
#endif

    if (relayOn) {
      showOn();
    } else {
      showReady();
    }
  } else {
    bool buttonHeld =
#if PLATFORM_ATOM3
      M5.BtnA.isPressed();
#elif PLATFORM_ORIGINAL
      digitalRead(PIN_BUTTON) == LOW;
#elif PLATFORM_SWITCH_LIGHT
      digitalRead(PIN_BUTTON) == LOW;
#endif

    if (!linkOk) {
      if (buttonHeld) {
        showNoLink();
      } else {
        setDisplay(DisplayMode::None);
      }
    } else if (!powerOk) {
      if (buttonHeld) {
        showNoPower();
      } else {
        setDisplay(DisplayMode::None);
      }
    }

#if PLATFORM_ORIGINAL
    ledcWrite(PIN_LED, 0);
#endif
  }

#if PLATFORM_SWITCH_LIGHT
  updateSwitchLightOutputs(now);
#endif
}
