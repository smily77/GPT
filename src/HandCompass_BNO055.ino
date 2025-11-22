#include "CYD_Display_Config.h"
#include "CompassUI.h"
#include "BNO055Manager.h"

static LGFX lcd;
static CompassUI ui;
static BNO055Manager compass;

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setBrightness(200);
  lcd.setColorDepth(16);
  lcd.fillScreen(TFT_BLACK);

  ui.begin(&lcd);
  ui.onCalibrationRequested([]() { compass.requestManualCalibration(); });

  if (!compass.begin()) {
    ui.showError("BNO055 not found!");
    while (true) delay(1000);
  }

  compass.attachUI(&ui);
}

void loop() {
  compass.update();
  ui.loop();

  float heading = compass.getFilteredHeadingDegrees();
  const char* dir = compass.getDirectionText();
  uint8_t sys, gyro, accel, mag;
  compass.getCalibrationStatus(sys, gyro, accel, mag);
  bool warn = compass.isWarningActive();

  ui.setHeading(heading, dir);
  ui.setCalibrationStatus(sys, gyro, accel, mag, warn);

  delay(20);
}
