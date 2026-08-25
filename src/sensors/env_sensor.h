#pragma once
// ════════════════════════════════════════════════════════════════════════════
// sensors/env_sensor.h — onboard BME280-class environment sensor
//
// camillia-mt's driver sweeps a list of candidate I2C routes because it runs on
// seven boards. This one knows exactly where the part is (ENV_SDA/ENV_SCL, see
// hal/hardware.h and the note there about those two pins being swapped relative
// to the board's generic pins_arduino.h) so it opens that bus and probes the
// two addresses a BME280 can answer on.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>

struct EnvReading {
    float temperatureC;
    float humidityPct;
    float pressureHpa;
    bool  hasHumidity;   // false on a BMP280, which has no humidity element
};

// Brings up the I2C bus and detects the sensor. Safe to call when none is
// fitted; envHasSensor() then stays false and nothing else here does anything.
bool envBegin();

const char *envSensorName();   // e.g. "BME280@0x76", or "none"

// Latest sample. Re-reads at most every few seconds; in between it returns the
// cached values, so callers can poll this from a UI timer without hammering the
// bus. False when no sensor is present.
bool envRead(EnvReading &out);
