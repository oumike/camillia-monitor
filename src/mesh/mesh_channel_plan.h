#pragma once
// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_channel_plan.h — Meshtastic modem presets and regional band plans
//
// Trimmed from camillia-mt. The TX-side pieces are gone: regionPower() capped
// transmit power, and presetToMeshtastic()/regionCodeToMeshtastic() existed to
// describe this node's own settings in an MQTT map report. A listener neither
// transmits nor advertises itself.
// ════════════════════════════════════════════════════════════════════════════
#include <stdint.h>

// Meshtastic modem presets (Config_LoRaConfig_ModemPreset ordering).
enum ModemPreset : uint8_t {
    PRESET_LONG_FAST     = 0,
    PRESET_LONG_MODERATE = 1,
    PRESET_LONG_SLOW     = 2,
    PRESET_LONG_TURBO    = 3,
    PRESET_MEDIUM_FAST   = 4,
    PRESET_MEDIUM_SLOW   = 5,
    PRESET_SHORT_FAST    = 6,
    PRESET_SHORT_SLOW    = 7,
    PRESET_SHORT_TURBO   = 8,
    PRESET_COUNT         = 9
};

struct PresetParams {
    const char *name;         // human label, e.g. "Long Fast"
    const char *channelName;  // Meshtastic on-air channel name, e.g. "LongFast"
    float       bw;           // kHz
    uint8_t     sf;           // 7-12
    uint8_t     cr;           // coding-rate denominator (5 = 4/5 ... 8 = 4/8)
};

// Per-region band edges (MHz). Meshtastic derives the operating frequency from
// these via a name-hashed channel slot, so the band is stored rather than a
// single fixed frequency.
struct RegionPlan {
    const char *code;
    float       freqStart;  // MHz
    float       freqEnd;    // MHz
};

extern const PresetParams kPresets[PRESET_COUNT];
extern const RegionPlan   kRegions[];
extern const uint8_t      kRegionCount;

// ── Custom (non-preset) modem settings ──────────────────────────────────────
// Bandwidth is stored the way Meshtastic stores it: an integer count of kHz,
// where two values are shorthand for a fractional bandwidth the radio actually
// produces — 31 is 31.25 kHz and 62 is 62.5 kHz. Keeping the code rather than
// the float means a number copied off a working node in the local mesh ("we run
// 62") means the same thing here as it does there.
extern const uint16_t kBwCodes[];
extern const uint8_t  kBwCodeCount;

#define LORA_SF_MIN 7
#define LORA_SF_MAX 12
#define LORA_CR_MIN 5
#define LORA_CR_MAX 8

// Name an unnamed primary channel takes while custom settings are active, and
// therefore what the frequency slot hashes. Meshtastic's Channels::getName()
// substitutes this same literal when use_preset is false, so a mesh running
// custom settings on an unnamed channel lands on the same slot as we do.
#define CUSTOM_CHANNEL_NAME "Custom"

float    loraBwFromCode(uint16_t code);
uint16_t loraCoerceBwCode(uint16_t code);

// Meshtastic frequency-slot operating frequency (MHz) for a region code,
// bandwidth (kHz) and primary-channel name. The slot is
// hash(channelName) % numChannels, matching stock Meshtastic.
float    regionSlotFreq(const char *code, float bwKhz, const char *channelName);

// How many frequency slots a region's band holds at the given bandwidth, or 0
// if the region is unknown. Narrow bandwidths give many more slots than the
// preset ones do (62.5 kHz over the US band is 416), which is why a custom
// setup usually pins the slot number instead of relying on the name hash.
uint32_t regionSlotCount(const char *code, float bwKhz);

// Center frequency (MHz) of a 0-based slot, clamped to the region's slot count.
float    regionSlotFreqNum(const char *code, float bwKhz, uint32_t slot);

