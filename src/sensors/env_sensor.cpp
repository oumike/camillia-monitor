// ════════════════════════════════════════════════════════════════════════════
// sensors/env_sensor.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "env_sensor.h"
#include "hardware.h"

#include <Wire.h>
#include <Adafruit_BME280.h>

namespace {

Adafruit_BME280 sBme;
bool  sPresent = false;
char  sName[16] = "none";

EnvReading sLast = {};
uint32_t   sLastReadMs = 0;
bool       sHasSample = false;

// The part is slow to convert and the values move slowly; polling it faster
// than this only costs bus time and self-heating.
constexpr uint32_t kReadIntervalMs = 5000;

}  // namespace

bool envBegin() {
    Wire.begin(ENV_SDA, ENV_SCL);
    Wire.setClock(100000);

    // 0x76 is the usual strapping on these carriers; 0x77 is the alternative.
    for (uint8_t addr : {0x76, 0x77}) {
        if (sBme.begin(addr, &Wire)) {
            sPresent = true;
            snprintf(sName, sizeof(sName), "BME280@0x%02X", addr);
            // Weather-station preset: oversample modestly and read on demand
            // rather than free-running, which keeps self-heating out of the
            // temperature reading. A monitor sampling every few seconds has no
            // use for the higher-rate modes.
            sBme.setSampling(Adafruit_BME280::MODE_FORCED,
                             Adafruit_BME280::SAMPLING_X1,   // temperature
                             Adafruit_BME280::SAMPLING_X1,   // pressure
                             Adafruit_BME280::SAMPLING_X1,   // humidity
                             Adafruit_BME280::FILTER_OFF);
            Serial.printf("[env] %s on sda=%d scl=%d\n", sName, ENV_SDA, ENV_SCL);
            return true;
        }
    }

    Serial.printf("[env] no sensor found on sda=%d scl=%d\n", ENV_SDA, ENV_SCL);
    return false;
}

const char *envSensorName() { return sName; }

bool envRead(EnvReading &out) {
    if (!sPresent) return false;

    if (!sHasSample || (millis() - sLastReadMs) >= kReadIntervalMs) {
        sLastReadMs = millis();
        // Forced mode: each sample has to be triggered, and takes a few ms.
        if (sBme.takeForcedMeasurement()) {
            sLast.temperatureC = sBme.readTemperature();
            sLast.pressureHpa  = sBme.readPressure() / 100.0f;
            sLast.humidityPct  = sBme.readHumidity();
            // A BMP280 answers the same driver but has no humidity element and
            // reports a flat 0; treat that as absent rather than as 0% RH.
            sLast.hasHumidity  = (sLast.humidityPct > 0.0f);
            sHasSample = true;
        }
    }

    if (!sHasSample) return false;
    out = sLast;
    return true;
}
