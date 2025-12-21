#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Credentials.h>
#include <doorLockData.h>

#define DEBUG false

// ====== PINS ======
constexpr uint8_t PIN_RELAY = 10;
constexpr uint8_t PIN_SENSE = 4; // digital input (voltage divider)
constexpr uint8_t PIN_RELAY_LED = 8;

// ====== ESP-NOW ======
// MAC addresses come from doorLockData.h so the controller and actor share the
// same channel/identity configuration as the DoorSender/DoorReceiver system.
uint8_t controllerMac[6];

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
unsigned long lastControllerMsg = 0;

// ====== OTA STATE ======
bool otaMode = false;
bool otaReady = false;
bool otaSetupAttempted = false;
constexpr unsigned long OTA_WIFI_TIMEOUT_MS = 7000;

// ====== TIMING ======
constexpr unsigned long STATUS_INTERVAL_MS = 1000;
unsigned long lastStatusSentMs = 0;

constexpr unsigned long CONTROLLER_TIMEOUT_MS = 5000;

void updatePower() {
  powerOk = digitalRead(PIN_SENSE) == HIGH;
}

void applyRelay(bool on) {
  relayOn = on && powerOk;
  digitalWrite(PIN_RELAY, relayOn ? HIGH : LOW);
  digitalWrite(PIN_RELAY_LED, relayOn ? HIGH : LOW);
}

void sendStatus() {
  StatusMessage msg{MSG_STATUS, relayOn, powerOk};
  esp_now_send(controllerMac, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
  lastStatusSentMs = millis();
}

void sendOtaAck() {
  OtaMessage msg{MSG_OTA_ACK};
  if (DEBUG) Serial.println("Sending OTA ACK to Controller");
  esp_now_send(controllerMac, reinterpret_cast<uint8_t *>(&msg), sizeof(msg));
}

void onSend(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // callback reserved for debugging
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 1) return;

  uint8_t msgType = data[0];

  if (msgType == MSG_COMMAND && len == sizeof(CommandMessage)) {
    CommandMessage incoming;
    memcpy(&incoming, data, sizeof(CommandMessage));
    lastControllerMsg = millis();

    updatePower();
    applyRelay(incoming.desiredRelay);
    sendStatus();
  } else if (msgType == MSG_OTA_REQUEST && len == sizeof(OtaMessage)) {
    if (DEBUG) Serial.println("OTA Request received from Controller");
    // Acknowledge OTA request
    sendOtaAck();
    // Enter OTA mode
    otaMode = true;
    otaSetupAttempted = false;
  }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_mac(WIFI_IF_STA, ACTOR_MAC);

  if (esp_now_init() != ESP_OK) {
    return;
  }

  esp_now_register_send_cb(onSend);
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, controllerMac, 6);
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

void setupOTA() {
  // Disconnect ESP-NOW and connect to WiFi
  esp_now_deinit();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  if (DEBUG) Serial.println("Connecting to WiFi...");

  unsigned long startConnect = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startConnect) < OTA_WIFI_TIMEOUT_MS) {
    delay(250);
    if (DEBUG) Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (DEBUG) {
      Serial.println("\nConnected to WiFi");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }

    ArduinoOTA.setHostname("RemoteSwitch-Actor");

    ArduinoOTA.onStart([]() {
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      if (DEBUG) {
        Serial.println("Start updating " + type);
      }
      // Turn off relay during update for safety
      applyRelay(false);
    });

    ArduinoOTA.onEnd([]() {
      if (DEBUG) Serial.println("\nEnd");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      if (DEBUG) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      }
    });

    ArduinoOTA.onError([](ota_error_t error) {
      if (DEBUG) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
      }
    });

    ArduinoOTA.begin();
    otaReady = true;
    if (DEBUG) Serial.println("OTA Ready");
  } else {
    // WiFi connection failed - return to normal operation
    if (DEBUG) Serial.println("\nWiFi connection failed - returning to normal mode");

    // Reset OTA state
    otaMode = false;
    otaReady = false;
    otaSetupAttempted = false;

    // Re-initialize ESP-NOW
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(50);
    setupEspNow();
  }
}

void setup() {
  if (DEBUG) {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Actor starting...");
  }

  const SenderSecret *controllerSecret = findSenderSecret(CONTROLLER_SENDER_ID);
  if (!controllerSecret) {
    if (DEBUG) Serial.println("Controller MAC not found in doorLockData.h");
    while (true) delay(1000);
  }
  memcpy(controllerMac, controllerSecret->sender_mac, sizeof(controllerMac));

  pinMode(PIN_RELAY, OUTPUT);
  applyRelay(false);

  pinMode(PIN_SENSE, INPUT);

  pinMode(PIN_RELAY_LED, OUTPUT);
  digitalWrite(PIN_RELAY_LED, LOW);

  setupEspNow();
}

void loop() {
  if (otaMode) {
    // OTA Mode handling
    if (!otaReady && !otaSetupAttempted) {
      // Setup OTA if not already done
      otaSetupAttempted = true;
      setupOTA();
    } else if (otaReady) {
      // Handle OTA updates
      ArduinoOTA.handle();
    }
    // If otaSetupAttempted but not otaReady, setupOTA() failed and reset everything
    // We'll return to normal operation automatically
    return;
  }

  // Normal operation mode
  updatePower();

  if (!powerOk) {
    applyRelay(false);
  }

  unsigned long now = millis();
  if (now - lastStatusSentMs > STATUS_INTERVAL_MS) {
    sendStatus();
  }

  if (lastControllerMsg != 0 && (now - lastControllerMsg) > CONTROLLER_TIMEOUT_MS) {
    applyRelay(false);
  }
}
