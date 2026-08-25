// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_channel_plan.cpp — preset and region tables
// ════════════════════════════════════════════════════════════════════════════
#include "mesh_channel_plan.h"
#include "mesh_config.h"
#include "mesh_proto.h"
#include <string.h>
#include <stdlib.h>

const PresetParams kPresets[PRESET_COUNT] = {
    //  human label      channel name    bw      sf  cr
    { "Long Fast",     "LongFast",    250.0f, 11, 5 },
    { "Long Moderate", "LongMod",     125.0f, 11, 8 },
    { "Long Slow",     "LongSlow",    125.0f, 12, 8 },
    { "Long Turbo",    "LongTurbo",   500.0f, 11, 8 },
    { "Medium Fast",   "MediumFast",  250.0f,  9, 5 },
    { "Medium Slow",   "MediumSlow",  250.0f, 10, 5 },
    { "Short Fast",    "ShortFast",   250.0f,  7, 5 },
    { "Short Slow",    "ShortSlow",   250.0f,  8, 5 },
    { "Short Turbo",   "ShortTurbo",  500.0f,  7, 5 },
};

// Band edges mirror Meshtastic's RegionInfo table. camillia-mt also carries a
// per-region TX power ceiling here; a listener has no use for it.
const RegionPlan kRegions[] = {
    //  code       freqStart  freqEnd
    { "US",      902.0f,   928.0f   },
    { "EU_433",  433.0f,   434.0f   },
    { "EU_868",  869.4f,   869.65f  },
    { "CN",      470.0f,   510.0f   },
    { "JP",      920.5f,   923.5f   },
    { "ANZ",     915.0f,   928.0f   },
    { "ANZ_433", 433.05f,  434.79f  },
    { "RU",      868.7f,   869.2f   },
    { "KR",      920.0f,   923.0f   },
    { "TW",      920.0f,   925.0f   },
    { "IN",      865.0f,   867.0f   },
    { "NZ_865",  864.0f,   868.0f   },
    { "TH",      920.0f,   925.0f   },
    { "UA_433",  433.0f,   434.7f   },
    { "UA_868",  868.0f,   868.6f   },
    { "MY_433",  433.0f,   435.0f   },
    { "MY_919",  919.0f,   924.0f   },
    { "SG_923",  917.0f,   925.0f   },
    { "PH_433",  433.0f,   434.7f   },
    { "PH_868",  868.0f,   869.4f   },
    { "PH_915",  915.0f,   918.0f   },
    { "KZ_433",  433.075f, 434.775f },
    { "KZ_863",  863.0f,   868.0f   },
    { "NP_865",  865.0f,   868.0f   },
    { "BR_902",  902.0f,   907.5f   },
    { "LORA_24", 2400.0f,  2483.5f  },
};
const uint8_t kRegionCount = (uint8_t)(sizeof(kRegions) / sizeof(kRegions[0]));

// Meshtastic bandwidth codes this board's SX1262 can produce.
const uint16_t kBwCodes[] = { 31, 62, 125, 250, 500 };
const uint8_t  kBwCodeCount = (uint8_t)(sizeof(kBwCodes) / sizeof(kBwCodes[0]));

float loraBwFromCode(uint16_t code) {
    for (uint8_t i = 0; i < kBwCodeCount; i++) {
        if (kBwCodes[i] != code) continue;
        // The two shorthand codes; the rest are already the literal kHz.
        if (code == 31) return 31.25f;
        if (code == 62) return 62.5f;
        return (float)code;
    }
    return 0.0f;
}

uint16_t loraCoerceBwCode(uint16_t code) {
    for (uint8_t i = 0; i < kBwCodeCount; i++) {
        if (kBwCodes[i] == code) return code;
    }
    // Unsupported: snap to the nearest supported code rather than silently
    // falling back to the default, so a value copied from a mesh running
    // hardware we do not have lands somewhere close instead of jumping to 250.
    uint16_t best = kBwCodes[0];
    uint32_t bestDist = (uint32_t)abs((int32_t)best - (int32_t)code);
    for (uint8_t i = 1; i < kBwCodeCount; i++) {
        uint32_t d = (uint32_t)abs((int32_t)kBwCodes[i] - (int32_t)code);
        if (d < bestDist) { best = kBwCodes[i]; bestDist = d; }
    }
    return best;
}

// djb2 string hash — the function Meshtastic uses to pick a frequency slot.
static uint32_t meshNameHash(const char *s) {
    uint32_t h = 5381;
    for (; s && *s; s++) h = (h * 33u) + (uint8_t)*s;
    return h;
}

static const RegionPlan *regionLookup(const char *code) {
    if (code) {
        for (uint8_t i = 0; i < kRegionCount; i++) {
            if (strcmp(kRegions[i].code, code) == 0) return &kRegions[i];
        }
    }
    return nullptr;
}

uint32_t regionSlotCount(const char *code, float bwKhz) {
    const RegionPlan *r = regionLookup(code);
    if (!r || bwKhz <= 0.0f) return 0;
    uint32_t numChannels = (uint32_t)((r->freqEnd - r->freqStart) / (bwKhz / 1000.0f));
    return numChannels == 0 ? 1 : numChannels;
}

float regionSlotFreqNum(const char *code, float bwKhz, uint32_t slot) {
    const RegionPlan *r = regionLookup(code);
    uint32_t numChannels = regionSlotCount(code, bwKhz);
    if (!r || numChannels == 0) return MESH_FREQ;
    if (slot >= numChannels) slot = numChannels - 1;
    float bwMhz = bwKhz / 1000.0f;
    return r->freqStart + (bwMhz / 2.0f) + (slot * bwMhz);
}

float regionSlotFreq(const char *code, float bwKhz, const char *channelName) {
    uint32_t numChannels = regionSlotCount(code, bwKhz);
    if (numChannels == 0) return MESH_FREQ;
    return regionSlotFreqNum(code, bwKhz, meshNameHash(channelName) % numChannels);
}

