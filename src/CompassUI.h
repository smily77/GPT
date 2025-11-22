#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <functional>

class CompassUI {
public:
  using CalibrationCallback = std::function<void()>;

  void begin(LGFX* display);
  void setHeading(float headingDegrees, const char* directionText);
  void setCalibrationStatus(uint8_t sys, uint8_t gyro, uint8_t accel, uint8_t mag, bool warn);
  void setCalibrationMode(const char* modeText);
  void showInstruction(const char* message);
  void showError(const char* message);
  void loop();
  void onCalibrationRequested(CalibrationCallback cb);

private:
  void drawStaticElements();
  void drawCompass();
  void drawCalibrationPanel();
  void drawButton();
  bool handleTouch();

  LGFX* lcd = nullptr;
  CalibrationCallback calibrateCallback;

  float heading = 0.0f;
  const char* direction = "N";
  uint8_t calSys = 0, calGyro = 0, calAccel = 0, calMag = 0;
  bool warningActive = false;
  const char* calibrationMode = "";
  const char* instruction = "";

  bool buttonPressed = false;
  uint32_t lastTouchMs = 0;
};
