// ════════════════════════════════════════════════════════════════════════════
// sensors/gps.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "gps.h"
#include "hardware.h"
#include "time_sync.h"

#include <TinyGPSPlus.h>

namespace {

TinyGPSPlus  sGps;
HardwareSerial sSerial(1);   // UART1

bool     sPositionValid = false;
int32_t  sLatI = 0, sLonI = 0;
uint32_t sLastTimePushMs = 0;

// The GPS re-reports time in every sentence. Pushing all of them at the clock
// would be a settimeofday() several times a second for no accuracy gain, so
// fixes are handed over at a walking pace once the clock is already set.
constexpr uint32_t kTimePushIntervalMs = 60000;

// Days since epoch for a civil date, by Howard Hinnant's days_from_civil. Used
// instead of mktime() because mktime interprets its input in the system
// timezone; NMEA dates are UTC, and routing them through local time would apply
// an offset that this device has no reason to have.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void publishTime() {
    if (!sGps.date.isValid() || !sGps.time.isValid()) return;
    if (sGps.date.year() < 2020) return;         // receiver has no almanac yet

    // isUpdated() would also be true for a stale sentence replayed by the
    // parser; age is the check that actually says "this arrived just now".
    if (sGps.date.age() > 2000 || sGps.time.age() > 2000) return;

    const uint32_t now = millis();
    if (timeIsValid() && (now - sLastTimePushMs) < kTimePushIntervalMs) return;
    sLastTimePushMs = now;

    const int64_t days = daysFromCivil(sGps.date.year(), sGps.date.month(), sGps.date.day());
    const int64_t epoch = days * 86400LL
                        + sGps.time.hour() * 3600LL
                        + sGps.time.minute() * 60LL
                        + sGps.time.second();
    if (epoch <= 0) return;

    timeSyncNoteGpsFix((uint32_t)epoch);
}

}  // namespace

void gpsBegin() {
    pinMode(GPS_ENABLE_PIN, OUTPUT);
    digitalWrite(GPS_ENABLE_PIN, GPS_ENABLE_ON_LEVEL);

    // Pulse reset so the module comes up in a known state rather than whatever
    // a previous boot left it in.
    pinMode(GPS_RESET_PIN, OUTPUT);
    digitalWrite(GPS_RESET_PIN, GPS_RESET_ACTIVE_LEVEL);
    delay(20);
    digitalWrite(GPS_RESET_PIN, !GPS_RESET_ACTIVE_LEVEL);

    sSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.printf("[gps] UART1 rx=%d tx=%d @ %d baud\n", GPS_RX, GPS_TX, GPS_BAUD);
}

void gpsLoop() {
    // Bounded per pass. A saturated UART could otherwise hold the loop here
    // long enough to miss a LoRa packet, which is the one thing this device
    // must not do for the sake of a convenience feature.
    int budget = 512;
    while (sSerial.available() && budget-- > 0) {
        sGps.encode((char)sSerial.read());
    }

    if (sGps.location.isValid() && sGps.location.age() < 10000) {
        sLatI = (int32_t)(sGps.location.lat() * 1e7);
        sLonI = (int32_t)(sGps.location.lng() * 1e7);
        sPositionValid = true;
    }

    publishTime();
}

bool gpsHasFix()      { return sGps.location.isValid() && sGps.location.age() < 10000; }
bool gpsHasPosition() { return sPositionValid; }

int32_t gpsLatitudeI()  { return sLatI; }
int32_t gpsLongitudeI() { return sLonI; }

uint8_t gpsSatellites() { return sGps.satellites.isValid() ? (uint8_t)sGps.satellites.value() : 0; }
