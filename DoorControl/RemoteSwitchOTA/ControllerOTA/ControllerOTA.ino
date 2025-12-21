#include <Streaming.h>

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <ArduinoOTA.h>
#include <Credentials.h>

#define DEBUG false

// ====== PINS ======
constexpr uint8_t PIN_LED = 6;
constexpr uint8_t PIN_BUTTON = 7; // active LOW
constexpr uint8_t PIN_SDA = 8;
constexpr uint8_t PIN_SCL = 9;

// ====== LED PWM ======
constexpr uint32_t LED_PWM_FREQ = 5000;
constexpr uint8_t LED_PWM_RESOLUTION = 8;  // 8-bit: 0-255
constexpr uint8_t LED_BRIGHTNESS_PERCENT = 20;  // LED brightness in %
constexpr uint8_t LED_BRIGHTNESS = (LED_BRIGHTNESS_PERCENT * 255) / 100;  // PWM value (20% = 51)

// ====== DISPLAY ======
constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 32;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ====== ESP-NOW ======
// Replace the MAC addresses with the real hardware addresses before uploading.
// The controller MAC is fixed; the actor MAC is learned from incoming packets.
const uint8_t CONTROLLER_MAC[6] = {0x24, 0x6F, 0x28, 0xAA, 0xAA, 0x01};
uint8_t actorPeerMac[6] = {0x24, 0x6F, 0x28, 0xAA, 0xAA, 0x02};
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

// ====== STATE ======
bool relayOn = false;
bool powerOk = false;
bool linkOk = false;
unsigned long lastStatusMs = 0;
bool desiredRelayState = false;
bool lastSentCommandState = false;

// ====== OTA STATE ======
bool otaMode = false;
bool otaRequested = false;
bool otaAckReceived = false;
bool otaReady = false;

// ====== TIMING ======
constexpr unsigned long STATUS_TIMEOUT_MS = 5000;
constexpr unsigned long COMMAND_INTERVAL_MS = 1000;
unsigned long lastCommandSentMs = 0;

// ====== BUTTON ======
bool lastReading   = HIGH;  // Rohwert letztes Mal
bool buttonState   = HIGH;  // stabiler, entprellter Zustand
unsigned long lastDebounceTime = 0;
unsigned long buttonPressStartTime = 0;
bool buttonLongPressHandled = false;
constexpr unsigned long DEBOUNCE_DELAY_MS = 40;
constexpr unsigned long LONG_PRESS_MS = 2000;

void updateDisplay(const String &message, bool keepOn) {
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
  int16_t y = (SCREEN_HEIGHT + h) / 2 - 2; // visually centered
  display.setCursor(x, y);
  display.print(message);
  display.display();
}

void onSend(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // No action required, but keeps callback registered for completeness.
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
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
        peerInfo.channel = 1;
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

void setupDisplay() {
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false)) {
    // If display fails to initialize, continue without blocking.
    return;
  }
  display.clearDisplay();
  updateDisplay("Ready", true);
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_mac(WIFI_IF_STA, CONTROLLER_MAC);

  if (esp_now_init() != ESP_OK) {
    return;
  }

  esp_now_register_send_cb(onSend);
  esp_now_register_recv_cb(onReceive);

  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

void setupOTA() {
  // Disconnect ESP-NOW and connect to WiFi
  esp_now_deinit();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  if (DEBUG) Serial << "Connecting to WiFi..." << endl;

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    if (DEBUG) Serial << "." << endl;
    attempts++;
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
      updateDisplay("Updating", true);
    });

    ArduinoOTA.onEnd([]() {
      if (DEBUG) Serial << "\nEnd" << endl;
      updateDisplay("Done", true);
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
      updateDisplay("OTA Error", true);
    });

    ArduinoOTA.begin();
    otaReady = true;
    updateDisplay("OTA", true);
    if (DEBUG) Serial << "OTA Ready" << endl;
  } else {
    // WiFi connection failed - return to normal operation
    if (DEBUG) Serial << "WiFi connection failed - returning to normal mode" << endl;
    updateDisplay("WiFi Fail", true);
    delay(2000);

    // Reset OTA state
    otaMode = false;
    otaRequested = false;
    otaAckReceived = false;
    otaReady = false;

    // Re-initialize ESP-NOW
    WiFi.disconnect();
    setupEspNow();
    updateDisplay("Ready", true);
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
  Serial.begin(115200);
  delay(1000);
  if(DEBUG) Serial << "Serial Ready" << endl;

  // Setup LED PWM (using new ESP32 Arduino Core 3.x API)
  ledcAttach(PIN_LED, LED_PWM_FREQ, LED_PWM_RESOLUTION);
  ledcWrite(PIN_LED, 0);  // Start with LED off

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  setupDisplay();
  setupEspNow();
}


void handleButton() {
  int reading = digitalRead(PIN_BUTTON);

  // 1) Rohwert-Änderung? -> Timer neu starten
  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  // 2) Wenn lange genug stabil: stabilen Zustand übernehmen
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY_MS) {

    // hat sich der stabile Zustand wirklich geändert?
    if (reading != buttonState) {
      buttonState = reading;

      // 3) Aktion bei Zustandsänderung
      if (buttonState == LOW) {
        // Button wurde gedrückt
        buttonPressStartTime = millis();
        buttonLongPressHandled = false;
        if (DEBUG) Serial << "Button press (debounced)" << endl;
      } else {
        // Button wurde losgelassen
        unsigned long pressDuration = millis() - buttonPressStartTime;

        if (!buttonLongPressHandled && pressDuration < LONG_PRESS_MS) {
          // Kurzer Druck: Normal relay toggle
          if (!otaMode && linkOk && powerOk) {
            desiredRelayState = !relayOn;
            sendCommand(desiredRelayState);
          }
        }
      }
    }
  }

  // 4) Check for long press while button is held
  if (buttonState == LOW && !buttonLongPressHandled && !otaMode) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (pressDuration >= LONG_PRESS_MS) {
      buttonLongPressHandled = true;
      if (DEBUG) Serial << "Long press detected - initiating OTA mode" << endl;

      // Start OTA sequence
      otaMode = true;
      otaRequested = true;
      otaAckReceived = false;
      updateDisplay("OTA req.", true);
      sendOtaRequest();
    }
  }

  // 4) Rohwert merken
  lastReading = reading;
}

void loop() {
  if (otaMode) {
    // OTA Mode handling
    if (otaReady) {
      // Already in OTA mode, handle OTA updates
      ArduinoOTA.handle();
    } else if (otaRequested && !otaAckReceived) {
      // Waiting for actor acknowledgment
      // Keep displaying "OTA req." and resend request periodically
      static unsigned long lastOtaRequestMs = 0;
      if (millis() - lastOtaRequestMs > 1000) {
        sendOtaRequest();
        lastOtaRequestMs = millis();
      }
    } else if (otaAckReceived && !otaReady) {
      // Actor acknowledged, show "go OTA" and start WiFi connection
      updateDisplay("go OTA", true);
      delay(1000);
      setupOTA();
    }
    return;
  }

  // Normal operation mode
  if (linkOk && millis() - lastStatusMs > STATUS_TIMEOUT_MS) {
    linkOk = false;
    powerOk = false;
  }

  handleButton();

  if (linkOk && powerOk) {
    bool needResend = (desiredRelayState != relayOn) || (millis() - lastCommandSentMs > COMMAND_INTERVAL_MS) || (lastSentCommandState != desiredRelayState);
    if (needResend) {
      sendCommand(desiredRelayState);
    }

    ledcWrite(PIN_LED, relayOn ? LED_BRIGHTNESS : 0);
    updateDisplay(relayOn ? "On" : "Ready", true);
  } else {
    ledcWrite(PIN_LED, 0);
    bool showText = digitalRead(PIN_BUTTON) == LOW;
    if (showText) {
      if (!linkOk) {
        updateDisplay("No Link", true);
      } else if (!powerOk) {
        updateDisplay("No Power", true);
      }
    } else {
      updateDisplay("", false);
    }
  }
}
