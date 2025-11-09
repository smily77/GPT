#include <M5CoreInk.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>
#include "Credentials.h"

// Uncomment to enable accelerated testing mode.
//#define Test

static const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char *NTP_SERVER_1 = "pool.ntp.org";
static const char *NTP_SERVER_2 = "time.nist.gov";

RTC_DATA_ATTR bool g_timeSynced = false;

Ink_Sprite InkPage(&M5.M5Ink);

struct ReminderState {
    bool showYes;
    uint32_t secondsUntilNext;
};

static bool waitForTime(struct tm *timeInfo, int maxRetries) {
    while (maxRetries-- > 0) {
        if (getLocalTime(timeInfo, 1000)) {
            return true;
        }
        delay(1000);
    }
    return false;
}

static void synchronizeTime() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    const unsigned long wifiTimeout = millis() + 30000; // 30 seconds
    while (WiFi.status() != WL_CONNECTED && millis() < wifiTimeout) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
        struct tm timeInfo;
        if (waitForTime(&timeInfo, 20)) {
            g_timeSynced = true;
        }
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

static long dayNumberFromDate(const struct tm &timeInfo) {
    int year = timeInfo.tm_year + 1900;
    int month = timeInfo.tm_mon + 1;
    int day = timeInfo.tm_mday;

    if (month <= 2) {
        year -= 1;
        month += 12;
    }

    long era = year / 400;
    int yoe = year - era * 400;
    int doy = (153 * (month - 3) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}

static ReminderState buildReminderState(const struct tm &timeInfo) {
    ReminderState state{};
    const int secondsSinceMidnight = timeInfo.tm_hour * 3600 + timeInfo.tm_min * 60 + timeInfo.tm_sec;

#ifdef Test
    const int periodSeconds = 120; // every two minutes
    const int periodIndex = secondsSinceMidnight / periodSeconds;
    state.showYes = (periodIndex % 2) == 0;
    const int nextPeriodStart = (periodIndex + 1) * periodSeconds;
    state.secondsUntilNext = nextPeriodStart - secondsSinceMidnight;
    if (state.secondsUntilNext <= 0) {
        state.secondsUntilNext = periodSeconds;
    }
#else
    const long dayNumber = dayNumberFromDate(timeInfo);
    state.showYes = (dayNumber % 2) == 0;
    state.secondsUntilNext = 86400 - secondsSinceMidnight;
    if (state.secondsUntilNext <= 0) {
        state.secondsUntilNext = 86400;
    }
#endif
    return state;
}

static void drawYesIcon(int centerX, int centerY, int radius) {
    InkPage.drawCircle(centerX, centerY, radius, BLACK);
    InkPage.fillCircle(centerX, centerY, radius - 4, WHITE);
    const int checkStartX = centerX - radius / 2;
    const int checkStartY = centerY;
    const int checkMidX = centerX - radius / 8;
    const int checkMidY = centerY + radius / 3;
    const int checkEndX = centerX + radius / 2;
    const int checkEndY = centerY - radius / 2;
    InkPage.drawLine(checkStartX, checkStartY, checkMidX, checkMidY, BLACK);
    InkPage.drawLine(checkMidX, checkMidY, checkEndX, checkEndY, BLACK);
}

static void drawNoIcon(int centerX, int centerY, int radius) {
    InkPage.drawCircle(centerX, centerY, radius, BLACK);
    InkPage.fillCircle(centerX, centerY, radius - 4, WHITE);
    const int offset = radius / 2;
    InkPage.drawLine(centerX - offset, centerY - offset, centerX + offset, centerY + offset, BLACK);
    InkPage.drawLine(centerX - offset, centerY + offset, centerX + offset, centerY - offset, BLACK);
}

static void renderDisplay(const struct tm &timeInfo, const ReminderState &state) {
    InkPage.fillCanvas(WHITE);

    const int iconCenterX = 100;
    const int iconCenterY = 80;
    const int iconRadius = 60;

    if (state.showYes) {
        drawYesIcon(iconCenterX, iconCenterY, iconRadius);
    } else {
        drawNoIcon(iconCenterX, iconCenterY, iconRadius);
    }

    InkPage.setTextSize(2);
    InkPage.setTextColor(BLACK);
    InkPage.setTextDatum(BC_DATUM);
    InkPage.drawString(state.showYes ? "YES" : "NO", iconCenterX, iconCenterY + iconRadius + 10);

    char dateBuffer[32];
    strftime(dateBuffer, sizeof(dateBuffer), "%d.%m.%Y", &timeInfo);
    InkPage.setTextDatum(MC_DATUM);
    InkPage.setTextSize(2);
    InkPage.drawString(dateBuffer, 100, 160);

    const int battery = M5.Power.getBatteryLevel();
    char batteryBuffer[24];
    snprintf(batteryBuffer, sizeof(batteryBuffer), "Battery: %d%%", battery);
    InkPage.setTextDatum(MC_DATUM);
    InkPage.setTextSize(1);
    InkPage.drawString(batteryBuffer, 100, 185);

    InkPage.pushSprite(0, 0);
}

void setup() {
    M5.begin();
    M5.M5Ink.begin();
    M5.M5Ink.clear();

    if (!InkPage.createSprite(200, 200)) {
        while (true) {
            delay(1000);
        }
    }

    setenv("TZ", TZ_INFO, 1);
    tzset();

    if (!g_timeSynced) {
        synchronizeTime();
    }

    struct tm timeInfo;
    if (!waitForTime(&timeInfo, 5)) {
        InkPage.fillCanvas(WHITE);
        InkPage.setTextColor(BLACK);
        InkPage.setTextDatum(MC_DATUM);
        InkPage.drawString("Zeitfehler", 100, 100);
        InkPage.pushSprite(0, 0);
        esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    const ReminderState state = buildReminderState(timeInfo);
    renderDisplay(timeInfo, state);

    const uint64_t sleepDuration = static_cast<uint64_t>(state.secondsUntilNext) * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleepDuration);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    esp_deep_sleep_start();
}

void loop() {
    // Deep sleep is entered at the end of setup().
}
