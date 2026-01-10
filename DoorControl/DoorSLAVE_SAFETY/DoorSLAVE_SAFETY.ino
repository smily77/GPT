// DoorSLAVE_SAFETY.ino - ESP32-C3 safety relay gate for series-connected door relay

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <doorLockData.h>

// === CONFIGURABLE PARAMETERS ===
#define RELAY_PIN 2
#define RELAY_PULSE_MS 500
#define PERMIT_WINDOW_MS 200
#define DONE_HOLD_MS 1000
#define WDT_TIMEOUT_MS 5000
#define DEBUG 1
// ================================

#define PERMIT_MAGIC 0x53414645u
#define PERMIT_TYPE_1 1
#define PERMIT_TYPE_2 2

#ifndef DOORSAFETY_MASTER_MAC
#error "Define DOORSAFETY_MASTER_MAC in doorLockData.h for DoorSLAVE_SAFETY"
#endif

struct __attribute__((packed)) PermitMessage {
  uint8_t type;
  uint32_t magic;
  uint16_t nonce;
};

enum PermitState {
  STATE_IDLE = 0,
  STATE_GOT_PERMIT1 = 1,
  STATE_DONE = 2
};

static const uint8_t SAFETY_MASTER_MAC[6] = DOORSAFETY_MASTER_MAC;
PermitState permitState = STATE_IDLE;
uint16_t lastNonce = 0;
uint32_t permit1At = 0;
uint32_t doneUntil = 0;

void feedWatchdog() {
  esp_task_wdt_reset();
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

void pulseRelay() {
  feedWatchdog();
  digitalWrite(RELAY_PIN, HIGH);
  delay(RELAY_PULSE_MS);
  feedWatchdog();
  digitalWrite(RELAY_PIN, LOW);
  feedWatchdog();
}

void ensurePeer(const uint8_t *mac) {
  esp_now_peer_info_t peer = {};
  if (esp_now_is_peer_exist(mac)) return;
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_err_t res = esp_now_add_peer(&peer);
  logDebug("Add peer res=%d", res);
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  feedWatchdog();
  if (!info) return;
  const uint8_t *mac = info->src_addr;
  logPeer("RX from ", mac);
  if (memcmp(mac, SAFETY_MASTER_MAC, 6) != 0) {
    logDebug("Ignoring non-master permit");
    return;
  }
  if (len != (int)sizeof(PermitMessage)) return;
  PermitMessage msg = {};
  memcpy(&msg, incomingData, sizeof(msg));
  if (msg.magic != PERMIT_MAGIC) return;

  uint32_t now = millis();
  if (permitState == STATE_DONE && now < doneUntil) {
    return;
  }

  if (msg.type == PERMIT_TYPE_1) {
    permitState = STATE_GOT_PERMIT1;
    lastNonce = msg.nonce;
    permit1At = now;
    logDebug("PERMIT1 nonce=%u", lastNonce);
    return;
  }

  if (msg.type == PERMIT_TYPE_2) {
    if (permitState != STATE_GOT_PERMIT1) {
      return;
    }
    if (msg.nonce != lastNonce) {
      logDebug("PERMIT2 nonce mismatch");
      permitState = STATE_IDLE;
      return;
    }
    if (now - permit1At > PERMIT_WINDOW_MS) {
      logDebug("PERMIT2 timeout");
      permitState = STATE_IDLE;
      return;
    }
    pulseRelay();
    permitState = STATE_DONE;
    doneUntil = now + DONE_HOLD_MS;
    logDebug("Permit sequence done");
  }
}

void onDataSent(const uint8_t *, esp_now_send_status_t status) {
  feedWatchdog();
  logDebug("Send status=%d", status);
}

void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  esp_task_wdt_config_t wdt_config = {};
  wdt_config.timeout_ms = WDT_TIMEOUT_MS;
  wdt_config.idle_core_mask = 1 << 0;
  wdt_config.trigger_panic = true;
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  ensurePeer(SAFETY_MASTER_MAC);

  if (esp_now_init() != ESP_OK) {
    logDebug("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
}

void loop() {
  feedWatchdog();
  uint32_t now = millis();
  if (permitState == STATE_GOT_PERMIT1 && now - permit1At > PERMIT_WINDOW_MS) {
    permitState = STATE_IDLE;
  }
  if (permitState == STATE_DONE && now >= doneUntil) {
    permitState = STATE_IDLE;
  }
}
