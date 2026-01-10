// DoorSLAVE_SAFETY_WD.ino - Minimal safety gate for secondary relay with WATCHDOG
// This is a SAFETY layer, NOT a security layer.
// Purpose: Reduce risk of unintended door opening from MCU crashes, undefined GPIO states, or software hangs.
// Requires both PERMIT1 and PERMIT2 with matching nonce within a time window to activate relay.
// WD suffix: Includes ESP32 Task Watchdog for liveness monitoring.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <Adafruit_NeoPixel.h>
#include <doorLockData.h>

// === CONFIGURABLE PARAMETERS ===
#define RELAY_PIN 2
#define STATUS_PIXEL_PIN 8
#define PERMIT_WINDOW_MS 200          // Maximum time between PERMIT1 and PERMIT2
#define RELAY_PULSE_MS 500             // Relay pulse duration (slightly longer than master)
#define COOLDOWN_MS 1000               // Time to stay in DONE state before returning to IDLE
#define WATCHDOG_TIMEOUT_MS 10000      // 10 seconds
#define DEBUG 1
// ================================

// Safety permit message types
#define PERMIT_MSG_PERMIT1 0x01
#define PERMIT_MSG_PERMIT2 0x02
#define PERMIT_MAGIC 0x53414645UL  // "SAFE" in ASCII

struct __attribute__((packed)) PermitMessage {
  uint8_t type;           // PERMIT_MSG_PERMIT1 or PERMIT_MSG_PERMIT2
  uint32_t magic;         // PERMIT_MAGIC
  uint16_t nonce;         // Small nonce for matching PERMIT1 and PERMIT2
  uint8_t reserved[3];    // Padding to 10 bytes
};

// State machine states
enum State {
  STATE_IDLE,
  STATE_GOT_PERMIT1,
  STATE_DONE
};

State currentState = STATE_IDLE;
uint16_t storedNonce = 0;
uint32_t permit1Timestamp = 0;
uint32_t doneTimestamp = 0;
bool relayPulse = false;
uint32_t relayUntil = 0;
uint32_t lastHeartbeat = 0;
uint32_t packetCount = 0;

Adafruit_NeoPixel statusPixel(1, STATUS_PIXEL_PIN, NEO_GRB + NEO_KHZ800);

void feedWatchdog() {
  esp_task_wdt_reset();
}

void setStatusColor(uint32_t color) {
  statusPixel.setPixelColor(0, color);
  statusPixel.show();
}

uint32_t colorOff() { return statusPixel.Color(0, 0, 0); }
uint32_t colorBlue() { return statusPixel.Color(0, 0, 255); }

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

void logPeer(const char *label, const uint8_t mac[6]) {
#if DEBUG
  char buf[32];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print(label);
  Serial.println(buf);
#endif
}

void pulseRelay() {
  logDebug("RELAY ACTIVATED");
  relayPulse = true;
  relayUntil = millis() + RELAY_PULSE_MS;
  digitalWrite(RELAY_PIN, HIGH);
  setStatusColor(colorBlue());
}

void handlePermit1(uint16_t nonce) {
  if (currentState != STATE_IDLE) {
    logDebug("PERMIT1 ignored (not in IDLE)");
      return;
  }

  // Transition to GOT_PERMIT1
  currentState = STATE_GOT_PERMIT1;
  storedNonce = nonce;
  permit1Timestamp = millis();
  logDebug("PERMIT1 received, nonce=%u", nonce);
}

void handlePermit2(uint16_t nonce) {
  if (currentState != STATE_GOT_PERMIT1) {
    logDebug("PERMIT2 ignored (not in GOT_PERMIT1)");
      return;
  }

  // Check nonce match
  if (nonce != storedNonce) {
    logDebug("PERMIT2 nonce mismatch (expected=%u, got=%u)", storedNonce, nonce);
    // Reset to IDLE
    currentState = STATE_IDLE;
    storedNonce = 0;
      return;
  }

  // Check time window
  uint32_t now = millis();
  uint32_t elapsed = now - permit1Timestamp;
  if (elapsed > PERMIT_WINDOW_MS) {
    logDebug("PERMIT2 too late (elapsed=%u ms)", elapsed);
    // Reset to IDLE
    currentState = STATE_IDLE;
    storedNonce = 0;
      return;
  }

  // Valid PERMIT2 received within window - activate relay
  logDebug("PERMIT2 valid, nonce=%u, elapsed=%u ms", nonce, elapsed);
  pulseRelay();

  // Transition to DONE
  currentState = STATE_DONE;
  doneTimestamp = now;
  storedNonce = 0;
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  feedWatchdog();  // Feed watchdog on ESP-NOW activity
  if (!info) return;

  packetCount++;
  logDebug("Packet #%u received, len=%d", packetCount, len);

#ifdef RECEIVER_SAFETY_MAC
  // Verify sender is the configured master
  if (memcmp(info->src_addr, RECEIVER_SAFETY_MAC, 6) != 0) {
    logPeer("Permit from unknown MAC: ", info->src_addr);
    logDebug("Expected master MAC differs, ignoring");
    return;
  }
#else
#error "RECEIVER_SAFETY_MAC must be defined in doorLockData.h for DoorSLAVE_SAFETY"
#endif

  if (len != (int)sizeof(PermitMessage)) {
    logDebug("Wrong packet size (expected %d, got %d)", sizeof(PermitMessage), len);
    return;
  }

  PermitMessage pm;
  memcpy(&pm, incomingData, sizeof(PermitMessage));

  // Verify magic
  if (pm.magic != PERMIT_MAGIC) {
    logDebug("Invalid permit magic");
      return;
  }

  // Handle based on permit type
  switch (pm.type) {
    case PERMIT_MSG_PERMIT1:
      handlePermit1(pm.nonce);
      break;
    case PERMIT_MSG_PERMIT2:
      handlePermit2(pm.nonce);
      break;
    default:
      logDebug("Unknown permit type=%u", pm.type);
      break;
  }
}

void onDataSent(const uint8_t *, esp_now_send_status_t status) {
  feedWatchdog();  // Feed watchdog on ESP-NOW activity
  // Slave doesn't send messages, but callback must be registered
}

void setup() {
#if DEBUG
  Serial.begin(115200);
  delay(100);  // Allow serial to stabilize
#endif

  // Initialize watchdog (LIVENESS safeguard)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = 0,  // Don't watch idle tasks
    .trigger_panic = true  // Trigger reset on timeout
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);  // Add current task
  feedWatchdog();
  logDebug("Watchdog initialized (timeout=%dms)", WATCHDOG_TIMEOUT_MS);

  // CRITICAL: Relay MUST default to OFF on boot (including watchdog reset)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  statusPixel.begin();
  statusPixel.clear();
  statusPixel.show();

  // Initialize WiFi first (required for esp_wifi_set_mac)
  WiFi.mode(WIFI_STA);

#ifdef SLAVE_SAFETY_MAC
  // CRITICAL: Set MAC address from configuration
  // This allows hardware replacement without reflashing all devices

  // Stop WiFi (must be initialized but stopped to set MAC)
  esp_wifi_stop();

  // Now set the MAC address
  esp_err_t result = esp_wifi_set_mac(WIFI_IF_STA, SLAVE_SAFETY_MAC);
  if (result == ESP_OK) {
    logDebug("MAC set from doorLockData.h");
  } else {
    logDebug("MAC set FAILED, error=%d (0x%X)", result, result);
  }

  // Start WiFi again
  esp_wifi_start();
#else
#error "SLAVE_SAFETY_MAC must be defined in doorLockData.h for DoorSLAVE_SAFETY"
#endif

  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  uint8_t selfMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  logPeer("Slave MAC ", selfMac);
  logDebug("WiFi Channel: %d", WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    logDebug("ESP-NOW init failed");
    while (true) {
          delay(1000);
    }
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

#ifdef RECEIVER_SAFETY_MAC
  // Add master as peer
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_SAFETY_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_err_t res = esp_now_add_peer(&peer);
  logPeer("Master MAC ", RECEIVER_SAFETY_MAC);
  if (res == ESP_OK) {
    logDebug("Master peer added successfully");
  } else {
    logDebug("Master peer add FAILED, error=%d", res);
  }
#else
#error "RECEIVER_SAFETY_MAC must be defined in doorLockData.h for DoorSLAVE_SAFETY"
#endif

  logDebug("DoorSLAVE_SAFETY_WD ready");
  logDebug("Listening on channel %d", WIFI_CHANNEL);
}

void loop() {
  feedWatchdog();  // Feed watchdog every loop iteration

  uint32_t now = millis();

  // Heartbeat every 10 seconds
  if (now - lastHeartbeat > 10000) {
    logDebug("Alive | State: %d | Packets: %u", currentState, packetCount);
    lastHeartbeat = now;
  }

  // Handle relay pulse
  if (relayPulse && now > relayUntil) {
    digitalWrite(RELAY_PIN, LOW);
    relayPulse = false;
    setStatusColor(colorOff());
    logDebug("Relay OFF");
  }

  // Keep status LED in sync with relay
  if (relayPulse) {
    setStatusColor(colorBlue());
  } else {
    setStatusColor(colorOff());
  }

  // Handle state machine timeouts
  switch (currentState) {
    case STATE_GOT_PERMIT1:
      // Check if PERMIT1 has expired
      if ((now - permit1Timestamp) > PERMIT_WINDOW_MS) {
        logDebug("PERMIT1 expired, back to IDLE");
        currentState = STATE_IDLE;
        storedNonce = 0;
      }
      break;

    case STATE_DONE:
      // Stay in DONE for cooldown period, then return to IDLE
      if ((now - doneTimestamp) > COOLDOWN_MS) {
        logDebug("Cooldown complete, back to IDLE");
        currentState = STATE_IDLE;
      }
      break;

    case STATE_IDLE:
    default:
      // Nothing to do
      break;
  }

  delay(10);
}
