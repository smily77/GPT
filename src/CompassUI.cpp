#include <Arduino.h>
#include "CompassUI.h"

static const uint32_t BUTTON_COLOR = 0x0066CC;
static const uint32_t BUTTON_TEXT = 0xFFFFFF;
static const int BUTTON_X = 60;
static const int BUTTON_Y = 250;
static const int BUTTON_W = 200;
static const int BUTTON_H = 50;

void CompassUI::begin(LGFX* display) {
  lcd = display;
  drawStaticElements();
}

void CompassUI::setHeading(float headingDegrees, const char* directionText) {
  heading = headingDegrees;
  direction = directionText;
  drawCompass();
}

void CompassUI::setCalibrationStatus(uint8_t sys, uint8_t gyro, uint8_t accel, uint8_t mag, bool warn) {
  calSys = sys;
  calGyro = gyro;
  calAccel = accel;
  calMag = mag;
  warningActive = warn;
  drawCalibrationPanel();
}

void CompassUI::setCalibrationMode(const char* modeText) {
  calibrationMode = modeText;
  drawCalibrationPanel();
}

void CompassUI::showInstruction(const char* message) {
  instruction = message;
  drawCalibrationPanel();
}

void CompassUI::showError(const char* message) {
  if (!lcd) return;
  lcd->fillScreen(TFT_BLACK);
  lcd->setTextColor(TFT_RED, TFT_BLACK);
  lcd->setFont(&fonts::Font4);
  lcd->setTextDatum(middle_center);
  lcd->drawString(message, lcd->width() / 2, lcd->height() / 2);
}

void CompassUI::onCalibrationRequested(CalibrationCallback cb) {
  calibrateCallback = cb;
}

void CompassUI::loop() {
  handleTouch();
}

void CompassUI::drawStaticElements() {
  if (!lcd) return;
  lcd->fillScreen(TFT_BLACK);

  lcd->setTextDatum(middle_center);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setFont(&fonts::Orbitron_Light_32);
  lcd->drawString("Hand Compass", lcd->width() / 2, 20);

  drawCompass();
  drawCalibrationPanel();
  drawButton();
}

void CompassUI::drawCompass() {
  if (!lcd) return;

  int centerX = lcd->width() / 2;
  int centerY = 140;
  int radius = 100;

  lcd->fillCircle(centerX, centerY, radius + 4, TFT_DARKGREY);
  lcd->fillCircle(centerX, centerY, radius, TFT_BLACK);
  lcd->drawCircle(centerX, centerY, radius, TFT_WHITE);

  // Draw cardinal markers
  const char* labels[4] = {"N", "E", "S", "W"};
  int offsets[4][2] = {{0, -radius + 15}, {radius - 15, 0}, {0, radius - 15}, {-radius + 15, 0}};
  for (int i = 0; i < 4; ++i) {
    lcd->setTextColor(i == 0 ? TFT_RED : TFT_WHITE, TFT_BLACK);
    lcd->setFont(&fonts::Orbitron_Light_24);
    lcd->drawString(labels[i], centerX + offsets[i][0], centerY + offsets[i][1]);
  }

  // Draw needle
  float rad = heading * DEG_TO_RAD;
  int needleX = centerX + static_cast<int>(radius * 0.9f * sin(rad));
  int needleY = centerY - static_cast<int>(radius * 0.9f * cos(rad));
  lcd->drawLine(centerX, centerY, needleX, needleY, TFT_CYAN);
  lcd->fillCircle(centerX, centerY, 4, TFT_WHITE);

  // Digital readout
  lcd->fillRect(0, 200, lcd->width(), 40, TFT_BLACK);
  lcd->setFont(&fonts::Orbitron_Light_32);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%03d° %s", static_cast<int>(heading + 0.5f), direction);
  lcd->drawString(buffer, lcd->width() / 2, 220);
}

void CompassUI::drawCalibrationPanel() {
  if (!lcd) return;
  lcd->fillRect(0, 240, lcd->width(), 40, TFT_BLACK);
  lcd->setFont(&fonts::Font2);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  char status[64];
  snprintf(status, sizeof(status), "CAL sys:%d g:%d a:%d m:%d", calSys, calGyro, calAccel, calMag);
  lcd->drawString(status, 10, 245);

  lcd->setTextColor(warningActive ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
  lcd->drawString(warningActive ? "WARN" : "OK", lcd->width() - 40, 245);

  lcd->fillRect(0, 280, lcd->width(), 40, TFT_BLACK);
  lcd->setTextColor(TFT_SKYBLUE, TFT_BLACK);
  lcd->drawString(calibrationMode, 10, 285);

  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->drawString(instruction, 10, 305);
}

void CompassUI::drawButton() {
  if (!lcd) return;
  lcd->fillRoundRect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, 8, BUTTON_COLOR);
  lcd->setTextColor(BUTTON_TEXT, BUTTON_COLOR);
  lcd->setFont(&fonts::Font4);
  lcd->setTextDatum(middle_center);
  lcd->drawString("CALIBRATE", BUTTON_X + BUTTON_W / 2, BUTTON_Y + BUTTON_H / 2);
}

bool CompassUI::handleTouch() {
  if (!lcd) return false;
  uint16_t x, y;
  if (lcd->getTouch(&x, &y)) {
    uint32_t now = millis();
    if (!buttonPressed && now - lastTouchMs > 300) {
      if (x >= BUTTON_X && x <= BUTTON_X + BUTTON_W && y >= BUTTON_Y && y <= BUTTON_Y + BUTTON_H) {
        buttonPressed = true;
        lastTouchMs = now;
        if (calibrateCallback) calibrateCallback();
        return true;
      }
    }
  } else {
    buttonPressed = false;
  }
  return false;
}
