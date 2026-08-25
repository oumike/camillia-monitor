// ════════════════════════════════════════════════════════════════════════════
// sensors/battery.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "battery.h"
#include "hardware.h"

namespace {

// Below this the divider is reading noise, not a pack.
constexpr float kPresentThresholdV = 2.5f;

constexpr uint32_t kSampleIntervalMs = 5000;
constexpr int      kSamplesPerRead   = 16;

// Exponential smoothing. The ADC on this part is noisy and the radio's TX
// bursts pull the rail around; without this the displayed voltage flickers.
constexpr float kSmoothing = 0.25f;

int      sSenseLevel  = BATT_SENSE_ENABLE_LEVEL;
bool     sSenseLocked = false;
float    sFiltered    = 0.0f;
bool     sHasSample   = false;
uint32_t sLastSampleMs = 0;

float sampleAtLevel(int level) {
    digitalWrite(BATT_SENSE_ENABLE_PIN, level ? HIGH : LOW);
    delayMicroseconds(200);   // let the divider settle after switching

    int64_t mvSum = 0;
    int valid = 0;
    for (int i = 0; i < kSamplesPerRead; i++) {
        const uint32_t mv = analogReadMilliVolts(BATT_ADC_PIN);
        if (mv > 0) { mvSum += mv; valid++; }
    }
    if (valid <= 0) return 0.0f;
    return ((float)mvSum / valid) / 1000.0f * BATT_DIV;
}

// Which level actually enables the divider is decided by trying both and
// keeping whichever reads higher. camillia-mt does the same, because the
// documented polarity for this pin has not proven reliable across units and
// guessing wrong reports a flat battery on a full one.
float sampleAuto() {
    if (!sSenseLocked) {
        const int preferred = (BATT_SENSE_ENABLE_LEVEL == LOW) ? LOW : HIGH;
        const int opposite  = (preferred == LOW) ? HIGH : LOW;

        const float vPreferred = sampleAtLevel(preferred);
        const float vOpposite  = sampleAtLevel(opposite);

        sSenseLevel  = (vOpposite > vPreferred + 0.03f) ? opposite : preferred;
        sSenseLocked = true;
        Serial.printf("[batt] sense-enable active %s (%.2fV vs %.2fV)\n",
                      sSenseLevel ? "HIGH" : "LOW",
                      sSenseLevel == preferred ? vPreferred : vOpposite,
                      sSenseLevel == preferred ? vOpposite : vPreferred);
    }
    return sampleAtLevel(sSenseLevel);
}

// Li-ion open-circuit-voltage curve rather than a linear 3.0-4.2 V ramp, so the
// percentage tracks real runtime instead of collapsing through the middle of
// the discharge where the curve is nearly flat.
uint8_t voltageToPct(float v) {
    struct Pt { float v; uint8_t pct; };
    static const Pt kCurve[] = {
        {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85},
        {4.02f, 75},  {3.98f, 65}, {3.95f, 55}, {3.91f, 45},
        {3.87f, 35},  {3.85f, 30}, {3.84f, 25}, {3.82f, 20},
        {3.80f, 15},  {3.79f, 10}, {3.77f, 5},  {3.73f, 2},
        {3.70f, 0},
    };
    const int last = (int)(sizeof(kCurve) / sizeof(kCurve[0])) - 1;
    if (v <= 0.0f) return 0;
    if (v >= kCurve[0].v) return 100;
    if (v <= kCurve[last].v) return 0;

    for (int i = 0; i < last; i++) {
        const Pt &hi = kCurve[i];
        const Pt &lo = kCurve[i + 1];
        if (v <= hi.v && v >= lo.v) {
            const float span = hi.v - lo.v;
            if (span <= 0.0001f) return lo.pct;
            const float t = (v - lo.v) / span;
            return (uint8_t)(lo.pct + (hi.pct - lo.pct) * t + 0.5f);
        }
    }
    return 0;
}

void refresh() {
    if (sHasSample && (millis() - sLastSampleMs) < kSampleIntervalMs) return;
    sLastSampleMs = millis();

    const float v = sampleAuto();
    if (v <= 0.0f) return;   // unknown stays unknown; the filter holds

    sFiltered = sHasSample ? (sFiltered + kSmoothing * (v - sFiltered)) : v;
    sHasSample = true;
}

// Internal now: the display distinguishes "no pack" from "flat pack" by reading
// batteryVoltage() directly, so nothing outside needs this.
bool batteryPresent() {
    refresh();
    return sHasSample && sFiltered >= kPresentThresholdV;
}

}  // namespace

void batteryBegin() {
    pinMode(BATT_SENSE_ENABLE_PIN, OUTPUT);
    digitalWrite(BATT_SENSE_ENABLE_PIN, BATT_SENSE_ENABLE_LEVEL);
    analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);   // 0-3.3 V input range
    refresh();
}

float batteryVoltage() {
    refresh();
    return sHasSample ? sFiltered : 0.0f;
}


uint8_t batteryPercent() {
    refresh();
    if (!batteryPresent()) return 0;
    return voltageToPct(sFiltered);
}
