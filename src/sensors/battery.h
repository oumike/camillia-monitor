#pragma once
// ════════════════════════════════════════════════════════════════════════════
// sensors/battery.h — pack voltage and state of charge
//
// Trimmed from camillia-mt's battery_util, minus the per-unit calibration trim
// and the gauge backends other boards use. What is kept is the part that is not
// obvious: this board gates its divider behind a sense-enable pin whose active
// level is not reliably documented, so the level is determined by measurement
// rather than assumed.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>

void batteryBegin();

// Smoothed pack voltage, or 0 when nothing is connected to the divider.
float batteryVoltage();

// State of charge from a Li-ion OCV curve, 0-100. Returns 0 when the divider
// reads nothing, which batteryVoltage() reporting 0 distinguishes from a flat pack.
uint8_t batteryPercent();
