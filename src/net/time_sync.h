#pragma once
// ════════════════════════════════════════════════════════════════════════════
// net/time_sync.h — wall-clock time for the monitor
//
// The monitor has no RTC. Until something sets the clock, every timestamp it
// could produce is a lie, which is why node reports omitted rxTime entirely
// before this existed.
//
// Two sources, in priority order:
//   NTP — available the moment WiFi associates, accurate to well under a second.
//   GPS — slower to first fix and needs sky, but works with no network at all
//         and is the only source on a monitor deployed somewhere remote.
//
// NTP wins while both are present: it is authoritative, immediate, and does not
// drift between fixes. GPS is what keeps the clock true when the network is
// gone — which, for a device that exists to watch a mesh in the field, is the
// case that matters.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>

// Starts an SNTP client. Safe to call before the network is up; the ESP-IDF
// client retries on its own and picks the time up when association completes.
void timeSyncBegin();

// Polls for a first successful sync and re-arms SNTP after a reconnect.
void timeSyncLoop();

// Hands a UTC fix from the GPS to the clock. Ignored while NTP holds it.
void timeSyncNoteGpsFix(uint32_t epochSeconds);

// True once the clock has been set by any source. Everything that stamps a
// time must check this first — an unset ESP32 clock reads as 1970, and the
// ingestor rejects implausible timestamps rather than storing them.
bool timeIsValid();

// Epoch seconds at a past millis() stamp, for timestamping a packet that was
// received before the report is built. Returns 0 when the clock is unset, or
// when the moment predates the clock being set — a packet heard before first
// sync cannot be given a real time, and guessing one would be worse than
// letting the server stamp its own arrival time.
uint32_t timeEpochAtMillis(uint32_t whenMs);

const char *timeSourceName();

// Human-readable UTC, e.g. "2026-08-23 23:34:50Z", or "not set".
void timeFormatUtc(char *out, size_t cap);

// Just the wall clock, "HH:MM", or "--:--" before the clock has been set.
// UTC, like everything else here — this device has no timezone setting.
void timeFormatClock(char *out, size_t cap);
