#pragma once
// ════════════════════════════════════════════════════════════════════════════
// sensors/gps.h — L76K receiver, read for time and for our own position
//
// Far simpler than camillia-mt's driver, which probes baud rates and UART pin
// pairs because it has to run on seven boards. This one runs on exactly one,
// where the pins and baud are known, so it opens the port and reads.
//
// The monitor wants the GPS for two things: a clock when there is no network
// (see net/time_sync.h), and its own coordinates, so observations can be tied
// to where they were heard.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>

// Powers the module, releases reset, and opens the UART.
void gpsBegin();

// Feeds NMEA into the parser. Call from loop().
void gpsLoop();

bool gpsHasFix();
bool gpsHasPosition();

// Last known position, 1e-7 degrees, valid only when gpsHasPosition().
int32_t gpsLatitudeI();
int32_t gpsLongitudeI();

uint8_t gpsSatellites();
