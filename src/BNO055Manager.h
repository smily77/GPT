#pragma once

#include <Arduino.h>
#include <Adafruit_BNO055.h>
#include <Preferences.h>
#include <Wire.h>
#include "CompassUI.h"

class BNO055Manager {
public:
  bool begin();
  void update();
  float getFilteredHeadingDegrees() const { return filteredHeading; }
  void getCalibrationStatus(uint8_t& sys, uint8_t& gyro, uint8_t& accel, uint8_t& mag);
  bool isFullyCalibrated() const { return fullyCalibrated; }
  bool hasValidCalibrationLoaded() const { return calibrationLoaded; }
  void requestManualCalibration();
  const char* getDirectionText() const;
  bool isWarningActive() const { return warningActive; }
  void attachUI(CompassUI* ui);

private:
  enum State { STATE_NORMAL, STATE_AUTO_CALIBRATING, STATE_MANUAL_CALIBRATING };

  bool loadCalibrationFromNVS();
  void saveCalibrationToNVS(const adafruit_bno055_offsets_t& offsets);
  void applyOffsets(const adafruit_bno055_offsets_t& offsets);
  void updateFilteredHeading(float rawHeading);
  float angleDiff(float a, float b);
  void handleCalibrationState();
  void updateWarning();

  Adafruit_BNO055 bno{55};
  Preferences prefs;
  CompassUI* compassUI = nullptr;

  State state = STATE_NORMAL;
  bool calibrationLoaded = false;
  bool fullyCalibrated = false;
  bool warningActive = false;
  uint32_t calibratedSince = 0;
  uint32_t lastWarningCheck = 0;

  float filteredHeading = 0.0f;
  const float warningDurationMs = 5000;
};
