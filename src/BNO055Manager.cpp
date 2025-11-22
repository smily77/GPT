#include <Arduino.h>
#include "BNO055Manager.h"

static const char* NAMESPACE = "bno055cal";
static const char* KEY_OFFSETS = "offsets";

bool BNO055Manager::begin() {
  // Configure I2C pins
#ifdef extSDA
  const int SDA_PIN = extSDA;
#else
  const int SDA_PIN = 21;
#endif

#ifdef extSCL
  const int SCL_PIN = extSCL;
#else
  const int SCL_PIN = 22;
#endif

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!bno.begin()) {
    return false;
  }

  bno.setExtCrystalUse(true);
  prefs.begin(NAMESPACE, false);

  calibrationLoaded = loadCalibrationFromNVS();
  if (!calibrationLoaded) {
    state = STATE_AUTO_CALIBRATING;
    if (compassUI) {
      compassUI->setCalibrationMode("AUTO-CAL");
      compassUI->showInstruction("Move sensor on all axes");
    }
  }

  return true;
}

void BNO055Manager::update() {
  sensors_event_t orientationData;

  bno.getEvent(&orientationData, Adafruit_BNO055::VECTOR_EULER);
  float rawHeading = orientationData.orientation.x;
  if (rawHeading < 0) rawHeading += 360.0f;
  updateFilteredHeading(rawHeading);

  uint8_t sys, gyro, accel, mag;
  bno.getCalibration(&sys, &gyro, &accel, &mag);
  fullyCalibrated = (sys >= 3 && gyro >= 3 && accel >= 3 && mag >= 3);

  handleCalibrationState();
  updateWarning();
}

void BNO055Manager::attachUI(CompassUI* ui) {
  compassUI = ui;
  if (!compassUI) return;

  if (state == STATE_AUTO_CALIBRATING) {
    compassUI->setCalibrationMode("AUTO-CAL");
    compassUI->showInstruction("Move sensor on all axes");
  } else if (state == STATE_MANUAL_CALIBRATING) {
    compassUI->setCalibrationMode("MANUAL");
    compassUI->showInstruction("Move sensor until CAL=3");
  } else if (calibrationLoaded) {
    compassUI->setCalibrationMode("NVS LOADED");
    compassUI->showInstruction("Calibration restored");
  }
}

void BNO055Manager::getCalibrationStatus(uint8_t& sys, uint8_t& gyro, uint8_t& accel, uint8_t& mag) {
  bno.getCalibration(&sys, &gyro, &accel, &mag);
}

void BNO055Manager::requestManualCalibration() {
  state = STATE_MANUAL_CALIBRATING;
  if (compassUI) {
    compassUI->setCalibrationMode("MANUAL");
    compassUI->showInstruction("Move sensor until CAL=3");
  }
}

const char* BNO055Manager::getDirectionText() const {
  static const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int idx = static_cast<int>((filteredHeading + 22.5f) / 45.0f) % 8;
  return dirs[idx];
}

float BNO055Manager::angleDiff(float a, float b) {
  float d = a - b;
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

void BNO055Manager::updateFilteredHeading(float rawHeading) {
  const float alpha = 0.10f;
  float diff = angleDiff(rawHeading, filteredHeading);
  filteredHeading += alpha * diff;
  if (filteredHeading < 0) filteredHeading += 360;
  if (filteredHeading >= 360) filteredHeading -= 360;
}

bool BNO055Manager::loadCalibrationFromNVS() {
  adafruit_bno055_offsets_t offsets;
  size_t read = prefs.getBytes(KEY_OFFSETS, &offsets, sizeof(offsets));
  if (read != sizeof(offsets)) {
    return false;
  }

  applyOffsets(offsets);
  if (compassUI) {
    compassUI->setCalibrationMode("NVS LOADED");
    compassUI->showInstruction("Calibration restored");
  }
  return true;
}

void BNO055Manager::applyOffsets(const adafruit_bno055_offsets_t& offsets) {
  bno.setMode(Adafruit_BNO055::OPERATION_MODE_CONFIG);
  delay(25);
  bno.setSensorOffsets(offsets);
  delay(10);
  bno.setMode(Adafruit_BNO055::OPERATION_MODE_NDOF);
  delay(20);
}

void BNO055Manager::saveCalibrationToNVS(const adafruit_bno055_offsets_t& offsets) {
  prefs.putBytes(KEY_OFFSETS, &offsets, sizeof(offsets));
  if (compassUI) {
    compassUI->setCalibrationMode("SAVED");
    compassUI->showInstruction("Calibration stored");
  }
}

void BNO055Manager::handleCalibrationState() {
  uint8_t sys, gyro, accel, mag;
  bno.getCalibration(&sys, &gyro, &accel, &mag);
  bool calibratedNow = (sys >= 3 && gyro >= 3 && accel >= 3 && mag >= 3);

  if (calibratedNow) {
    if (calibratedSince == 0) {
      calibratedSince = millis();
    }
  } else {
    calibratedSince = 0;
  }

  // Save calibration after stability period
  if (calibratedSince != 0 && millis() - calibratedSince > 3000) {
    adafruit_bno055_offsets_t offsets;
    bno.getSensorOffsets(offsets);
    saveCalibrationToNVS(offsets);
    state = STATE_NORMAL;
    calibratedSince = 0;
  }

  if (state == STATE_AUTO_CALIBRATING && calibratedNow) {
    if (compassUI) {
      compassUI->setCalibrationMode("AUTO DONE");
      compassUI->showInstruction("Ready");
    }
  } else if (state == STATE_AUTO_CALIBRATING) {
    if (compassUI) {
      compassUI->showInstruction("Rotate on all axes");
    }
  }

  if (state == STATE_MANUAL_CALIBRATING) {
    if (compassUI) {
      compassUI->showInstruction(calibratedNow ? "Hold still to save" : "Keep moving device");
    }
  }
}

void BNO055Manager::updateWarning() {
  uint32_t now = millis();
  if (now - lastWarningCheck < 500) return;
  lastWarningCheck = now;

  uint8_t sys, gyro, accel, mag;
  bno.getCalibration(&sys, &gyro, &accel, &mag);
  bool low = (sys < 2 || mag < 2);

  static uint32_t lowSince = 0;

  if (state == STATE_NORMAL && low) {
    if (lowSince == 0) lowSince = now;
    if (now - lowSince > warningDurationMs) {
      warningActive = true;
    }
  } else {
    warningActive = false;
    lowSince = 0;
  }
}
