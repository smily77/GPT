#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Einfaches LovyanGFX-Demoprogramm für den LilyGO Lily Pi
// - Display: ILI9481 (SPI)
// - Touch:   GT911 (I2C)
// Passe die Pinbelegung bei Bedarf an die eigene Verdrahtung an.
class LGFX_LilyPi : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus_instance;
  lgfx::Panel_ILI9481 _panel_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_GT911 _touch_instance;

public:
  LGFX_LilyPi() {
    { // SPI-Bus für das ILI9481 konfigurieren
      auto cfg = _bus_instance.config();
      cfg.spi_host   = VSPI_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;  // ggf. reduzieren, falls Bildfehler auftreten
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = true;      // ILI9481 nutzt nur MOSI, kein MISO
      cfg.use_lock   = true;
      cfg.dma_channel = 1;

      cfg.pin_sclk = 18;
      cfg.pin_mosi = 19;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 27;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    { // Panel-spezifische Pins/Funktionen einstellen
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = 5;
      cfg.pin_rst  = 33;
      cfg.pin_busy = -1;

      cfg.memory_width  = 320;
      cfg.memory_height = 480;
      cfg.panel_width   = 320;
      cfg.panel_height  = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.rgb_order = false;
      cfg.bus_shared = true; // teilt sich den Bus mit dem Touch-Reset

      _panel_instance.config(cfg);
    }

    { // PWM-Backlight
      auto cfg = _light_instance.config();
      cfg.pin_bl = 32;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      cfg.invert = false;
      cfg.brightness = 255;

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    { // GT911 Touch-Controller per I2C einbinden
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;  cfg.x_max = 319;
      cfg.y_min = 0;  cfg.y_max = 479;
      cfg.pin_int = 39;
      cfg.pin_rst = 38;
      cfg.bus_shared = true;
      cfg.offset_rotation = 1; // passend zur Displayausrichtung

      cfg.i2c_port = I2C_NUM_0;
      cfg.pin_sda  = 21;
      cfg.pin_scl  = 22;
      cfg.freq = 400000;

      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};

LGFX_LilyPi gfx;

static void drawGradientBackground() {
  const int w = gfx.width();
  const int h = gfx.height();
  for (int y = 0; y < h; ++y) {
    uint8_t c = (255 * y) / h;
    gfx.drawFastHLine(0, y, w, gfx.color565(c, 180 - c / 2, 255 - c));
  }
}

static void drawHeader() {
  gfx.setTextDatum(textdatum_t::middle_left);
  gfx.setTextSize(2);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.fillRect(0, 0, gfx.width(), 26, TFT_BLACK);
  gfx.drawString("Lily Pi + LovyanGFX Demo", 6, 13);
  gfx.setTextSize(1);
  gfx.drawString("ILI9481 + GT911", 6, 13 + 16);
}

void setup() {
  gfx.init();
  gfx.setRotation(1);
  gfx.setBrightness(220);

  drawGradientBackground();
  drawHeader();

  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setTextSize(2);
  gfx.fillRoundRect(12, 60, 220, 50, 8, TFT_BLACK);
  gfx.drawRoundRect(12, 60, 220, 50, 8, TFT_CYAN);
  gfx.drawCentreString("Tippe den Screen!", 12 + 110, 60 + 25);

  gfx.setTextSize(1);
  gfx.drawString("Touch-Punkte werden mit Koordinaten angezeigt.", 12, 120);
  gfx.drawString("Passe Pins oben an dein Lily Pi-Board an.", 12, 140);
}

void loop() {
  lgfx::touch_point_t points[5];
  uint8_t count = gfx.getTouch(points, 5);

  // Bereich für Touch-Anzeige
  const int infoY = 180;
  const int infoH = gfx.height() - infoY - 20;
  gfx.fillRoundRect(10, infoY, gfx.width() - 20, infoH, 6, TFT_BLACK);
  gfx.drawRoundRect(10, infoY, gfx.width() - 20, infoH, 6, TFT_CYAN);

  gfx.setTextColor(TFT_CYAN);
  gfx.setTextSize(2);
  gfx.drawCentreString(String(count) + " Touch", gfx.width() / 2, infoY + 12);

  gfx.setTextSize(1);
  gfx.setTextColor(TFT_WHITE);

  if (count == 0) {
    gfx.drawString("Keine Beruhrung erkannt.", 20, infoY + 40);
  } else {
    for (uint8_t i = 0; i < count; ++i) {
      const auto &p = points[i];
      gfx.fillCircle(p.x, p.y, 8, TFT_YELLOW);
      gfx.drawString(String(i) + ": x=" + String(p.x) + ", y=" + String(p.y), 20, infoY + 40 + i * 20);
    }
  }

  delay(16); // ~60 fps Refresh
}
