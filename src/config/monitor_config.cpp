// ════════════════════════════════════════════════════════════════════════════
// config/monitor_config.cpp — defaults, NVS persistence, derived radio settings
// ════════════════════════════════════════════════════════════════════════════
#include "monitor_config.h"
#include "mesh_channel_plan.h"
#include "mesh_proto.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

MonitorConfig gCfg;

namespace {
constexpr char kNvsNamespace[] = "cam-monitor";
constexpr char kBlobKey[]      = "cfg";

const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    // Accept the URL-safe alphabet too: Meshtastic channel URLs are base64url,
    // and a PSK pasted out of one arrives with - and _ rather than + and /.
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
}  // namespace

void configDefaults(MonitorConfig &cfg) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = MONITOR_CFG_VERSION;

    strncpy(cfg.monitorName, "camillia-monitor", sizeof(cfg.monitorName) - 1);

    cfg.ingestEnabled   = false;
    cfg.ingestIntervalS = 60;

    strncpy(cfg.region, "US", sizeof(cfg.region) - 1);
    cfg.usePreset   = true;
    cfg.modemPreset = PRESET_LONG_FAST;
    cfg.bwCode      = 250;
    cfg.loraSf      = MESH_SF;
    cfg.loraCr      = MESH_CR;
    cfg.pinSlot     = false;
    cfg.freqSlot    = 0;

    // Slot 0: the public Meshtastic channel every stock node uses. A monitor
    // that boots hearing nothing would look broken, so the default is the
    // channel most likely to have traffic on it.
    strncpy(cfg.channels[0].name, "LongFast", sizeof(cfg.channels[0].name) - 1);
    cfg.channels[0].key[0] = 0x01;   // packed PSK index 1 == the default key
    cfg.channels[0].keyLen = 1;

    // Defaults match the public Meshtastic broker, so the screen has something
    // to show the moment it is switched on.
    cfg.mqttEnabled = false;
    strncpy(cfg.mqttServer, "mqtt.meshtastic.org", sizeof(cfg.mqttServer) - 1);
    cfg.mqttPort = 1883;
    strncpy(cfg.mqttUser, "meshdev", sizeof(cfg.mqttUser) - 1);
    strncpy(cfg.mqttPass, "large4cats", sizeof(cfg.mqttPass) - 1);
    strncpy(cfg.mqttRoot, "msh/US", sizeof(cfg.mqttRoot) - 1);
    cfg.mqttTls = false;

    cfg.useMetric = false;      // imperial
    strncpy(cfg.tz, "EST5EDT,M3.2.0,M11.1.0", sizeof(cfg.tz) - 1);
    cfg.webAuthEnabled = false;
}

// Reports what NVS thinks of itself. A save that fails is almost always a
// storage-level problem rather than anything about the settings, and without
// this the only evidence is a boolean.
void configNvsReport(const char *why) {
    nvs_stats_t stats;
    const esp_err_t err = nvs_get_stats(nullptr, &stats);
    if (err != ESP_OK) {
        Serial.printf("[cfg] nvs stats (%s): unavailable — %s\n", why, esp_err_to_name(err));
        return;
    }
    Serial.printf("[cfg] nvs stats (%s): used=%u free=%u total=%u namespaces=%u\n",
                  why, (unsigned)stats.used_entries, (unsigned)stats.free_entries,
                  (unsigned)stats.total_entries, (unsigned)stats.namespace_count);
}

// Opens the config namespace, bringing NVS up first if it is not initialised.
//
// Arduino's startup calls nvs_flash_init() and re-inits after erasing on the two
// errors it knows how to recover from. It does not retry anything else, and a
// device flashed over a different partition layout can land exactly there: the
// region now labelled "nvs" still holds whatever the previous layout put at that
// offset, and every open fails for the life of the boot.
static bool openNvs(nvs_open_mode_t mode, nvs_handle_t &out) {
    esp_err_t err = nvs_open(kNvsNamespace, mode, &out);
    if (err == ESP_OK) return true;

    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        Serial.println("[cfg] NVS not initialised — initialising");
        esp_err_t initErr = nvs_flash_init();
        if (initErr == ESP_ERR_NVS_NO_FREE_PAGES || initErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            Serial.printf("[cfg] NVS unusable (%s) — erasing the partition\n",
                          esp_err_to_name(initErr));
            // Only reached when the partition cannot be mounted at all, so there
            // are no settings here to lose: whatever is in it is not readable.
            if (nvs_flash_erase() == ESP_OK) initErr = nvs_flash_init();
        }
        if (initErr != ESP_OK) {
            Serial.printf("[cfg] nvs_flash_init failed: %s\n", esp_err_to_name(initErr));
            return false;
        }
        err = nvs_open(kNvsNamespace, mode, &out);
        if (err == ESP_OK) return true;
    }

    // A namespace that has never been written does not exist yet; that is the
    // expected state on a first boot, not a fault.
    if (err == ESP_ERR_NVS_NOT_FOUND && mode == NVS_READONLY) return false;

    Serial.printf("[cfg] nvs_open(%s, %s) failed: %s\n", kNvsNamespace,
                  mode == NVS_READONLY ? "ro" : "rw", esp_err_to_name(err));
    return false;
}

void configLoad() {
    configDefaults(gCfg);

    nvs_handle_t h;
    if (!openNvs(NVS_READONLY, h)) {
        Serial.println("[cfg] no stored settings — using defaults");
        return;
    }

    MonitorConfig stored{};
    size_t len = sizeof(stored);
    const esp_err_t err = nvs_get_blob(h, kBlobKey, &stored, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[cfg] no stored settings — using defaults");
        return;
    }
    if (err != ESP_OK) {
        Serial.printf("[cfg] read failed: %s — using defaults\n", esp_err_to_name(err));
        return;
    }
    if (len != sizeof(stored)) {
        Serial.printf("[cfg] stored blob is %u bytes, expected %u — using defaults\n",
                      (unsigned)len, (unsigned)sizeof(stored));
        return;
    }
    if (stored.version != MONITOR_CFG_VERSION) {
        // Deliberately not migrated. Every setting here is re-enterable in under
        // a minute, and silently reinterpreting a blob whose layout changed is a
        // good way to tune the radio to something nobody chose.
        Serial.printf("[cfg] stored version %lu != %d — using defaults\n",
                      (unsigned long)stored.version, MONITOR_CFG_VERSION);
        return;
    }

    gCfg = stored;
    // Migrates a value written by a build that stored the full report URL.
    configNormalizeIngestUrl(gCfg);
    Serial.printf("[cfg] loaded  wifi=\"%s\"  ingest=%s  region=%s\n",
                  gCfg.wifiSsid, gCfg.ingestEnabled ? "on" : "off", gCfg.region);
}

bool configSave() {
    gCfg.version = MONITOR_CFG_VERSION;

    nvs_handle_t h;
    if (!openNvs(NVS_READWRITE, h)) {
        configNvsReport("open failed");
        return false;
    }

    esp_err_t err = nvs_set_blob(h, kBlobKey, &gCfg, sizeof(gCfg));
    if (err != ESP_OK) {
        Serial.printf("[cfg] nvs_set_blob(%u bytes) failed: %s\n",
                      (unsigned)sizeof(gCfg), esp_err_to_name(err));
        configNvsReport("write failed");
        nvs_close(h);
        return false;
    }

    // Without the commit the write lives only in the cache and is lost on
    // reboot — which is exactly the symptom the save message warns about.
    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        Serial.printf("[cfg] nvs_commit failed: %s\n", esp_err_to_name(err));
        configNvsReport("commit failed");
        return false;
    }

    Serial.printf("[cfg] saved (%u bytes)\n", (unsigned)sizeof(gCfg));
    return true;
}

void configFactoryReset() {
    nvs_handle_t h;
    if (openNvs(NVS_READWRITE, h)) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    configDefaults(gCfg);
    Serial.println("[cfg] factory reset");
}

ResolvedModem configResolveModem(const MonitorConfig &cfg) {
    ResolvedModem r{};

    const char *hashName;
    if (cfg.usePreset) {
        const uint8_t idx = (cfg.modemPreset < PRESET_COUNT) ? cfg.modemPreset : PRESET_LONG_FAST;
        r.bw = kPresets[idx].bw;
        r.sf = kPresets[idx].sf;
        r.cr = kPresets[idx].cr;
        // With a preset in play Meshtastic hashes the preset's own channel name
        // ("LongFast"), not whatever the operator called the channel locally.
        hashName = kPresets[idx].channelName;
    } else {
        r.bw = loraBwFromCode(loraCoerceBwCode(cfg.bwCode));
        r.sf = (cfg.loraSf >= LORA_SF_MIN && cfg.loraSf <= LORA_SF_MAX) ? cfg.loraSf : MESH_SF;
        r.cr = (cfg.loraCr >= LORA_CR_MIN && cfg.loraCr <= LORA_CR_MAX) ? cfg.loraCr : MESH_CR;
        // Custom settings hash the primary channel's own name, matching
        // Meshtastic's Channels::getName() once use_preset is false. An unnamed
        // channel hashes the same literal it does there.
        hashName = cfg.channels[0].name[0] ? cfg.channels[0].name : CUSTOM_CHANNEL_NAME;
    }
    if (r.bw <= 0.0f) r.bw = MESH_BW;

    r.slotCount = regionSlotCount(cfg.region, r.bw);
    if (cfg.pinSlot && r.slotCount > 0) {
        r.slot = (cfg.freqSlot < r.slotCount) ? cfg.freqSlot : (r.slotCount - 1);
        r.freq = regionSlotFreqNum(cfg.region, r.bw, r.slot);
    } else {
        r.freq = regionSlotFreq(cfg.region, r.bw, hashName);
        // Recover which slot that landed on, so the config page can show it.
        if (r.slotCount > 0) {
            const float bwMhz = r.bw / 1000.0f;
            const float first = regionSlotFreqNum(cfg.region, r.bw, 0);
            r.slot = (uint32_t)((r.freq - first) / bwMhz + 0.5f);
        }
    }
    return r;
}

void configApplyChannels(const MonitorConfig &cfg) {
    for (int i = 0; i < MESH_CHANNELS; i++) {
        ChannelKey &dst = CHANNEL_KEYS[i];
        const ChannelConfig &src = cfg.channels[i];

        memset(dst.name_buf, 0, sizeof(dst.name_buf));
        strncpy(dst.name_buf, src.name, sizeof(dst.name_buf) - 1);
        dst.name = dst.name_buf;

        memset(dst.key, 0, sizeof(dst.key));
        dst.keyLen = src.keyLen;
        if (src.keyLen > 0 && src.keyLen <= sizeof(dst.key)) {
            memcpy(dst.key, src.key, src.keyLen);
        } else {
            dst.keyLen = 0;
        }

        // An unused slot must not collide with a real channel hash. 0xFF is not
        // reserved, but decryptPacket() also verifies the plaintext, so a stray
        // match on an empty slot cannot produce a decoded packet — the hash only
        // decides which key is tried first.
        dst.hash = (dst.keyLen == 0 && dst.name_buf[0] == '\0')
                       ? 0xFF
                       : computeChannelHash(dst.name, dst.key, dst.keyLen);
    }
}

bool configParsePsk(const char *b64, uint8_t *out, uint8_t &outLen) {
    outLen = 0;
    if (!b64) return false;

    uint8_t buf[32];
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (const char *p = b64; *p; p++) {
        if (*p == '=' || *p == ' ' || *p == '\r' || *p == '\n') continue;
        const int v = b64Value(*p);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= sizeof(buf)) return false;   // longer than any valid key
            buf[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }

    // 1 = Meshtastic's packed PSK index, 16 = AES-128, 32 = AES-256. Anything
    // else is a typo rather than a key, and accepting it would produce a
    // channel that silently never decrypts.
    if (n != 1 && n != 16 && n != 32) return false;

    memcpy(out, buf, n);
    outLen = (uint8_t)n;
    return true;
}

void configFormatPsk(const uint8_t *key, uint8_t keyLen, char *out, size_t outCap) {
    if (!out || outCap == 0) return;
    out[0] = '\0';
    if (!key || keyLen == 0) return;

    // 4 chars per 3 bytes, rounded up, plus the terminator.
    const size_t need = ((size_t)keyLen + 2) / 3 * 4 + 1;
    if (need > outCap) return;

    size_t o = 0;
    for (size_t i = 0; i < keyLen; i += 3) {
        const uint32_t b0 = key[i];
        const uint32_t b1 = (i + 1 < keyLen) ? key[i + 1] : 0;
        const uint32_t b2 = (i + 2 < keyLen) ? key[i + 2] : 0;
        const uint32_t trio = (b0 << 16) | (b1 << 8) | b2;

        out[o++] = kB64Alphabet[(trio >> 18) & 0x3F];
        out[o++] = kB64Alphabet[(trio >> 12) & 0x3F];
        out[o++] = (i + 1 < keyLen) ? kB64Alphabet[(trio >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < keyLen) ? kB64Alphabet[trio & 0x3F] : '=';
    }
    out[o] = '\0';
}

// Carried over from camillia-mt. Deliberately a short curated list rather than
// the full zone database: the strings are the interesting part (each carries its
// own DST rule), and a 400-entry dropdown would be worse to use, not better.
const TzOption kTzOptions[] = {
    { "UTC",                               "UTC0"                          },
    { "US - Hawaii (UTC-10)",              "HST10"                         },
    { "US - Alaska (UTC-9/-8)",            "AKST9AKDT,M3.2.0,M11.1.0"      },
    { "US - Pacific (UTC-8/-7)",           "PST8PDT,M3.2.0,M11.1.0"        },
    { "US - Mountain (UTC-7/-6)",          "MST7MDT,M3.2.0,M11.1.0"        },
    { "US - Arizona, no DST (UTC-7)",      "MST7"                          },
    { "US - Central (UTC-6/-5)",           "CST6CDT,M3.2.0,M11.1.0"        },
    { "US - Eastern (UTC-5/-4)",           "EST5EDT,M3.2.0,M11.1.0"        },
    { "Canada - Atlantic (UTC-4/-3)",      "AST4ADT,M3.2.0/0,M11.1.0/0"    },
    { "Brazil - Brasilia (UTC-3)",         "BRT3BRST,M10.3.0/0,M2.3.0/0"   },
    { "Argentina (UTC-3)",                 "ART3"                          },
    { "UK (UTC+0/+1)",                     "GMT0BST,M3.5.0/1,M10.5.0"      },
    { "Western Europe - CET (UTC+1/+2)",   "CET-1CEST,M3.5.0,M10.5.0/3"    },
    { "Eastern Europe - EET (UTC+2/+3)",   "EET-2EEST,M3.5.0/3,M10.5.0/4"  },
    { "Russia - Moscow (UTC+3)",           "MSK-3"                         },
    { "India (UTC+5:30)",                  "IST-5:30"                      },
    { "China / Singapore (UTC+8)",         "CST-8"                         },
    { "Japan / Korea (UTC+9)",             "JST-9"                         },
    { "Australia - Perth (UTC+8)",         "AWST-8"                        },
    { "Australia - Eastern (UTC+10/+11)",  "AEST-10AEDT,M10.1.0,M4.1.0/3"  },
    { "New Zealand (UTC+12/+13)",          "NZST-12NZDT,M9.5.0,M4.1.0/3"   },
};
const uint8_t kTzOptionCount = (uint8_t)(sizeof(kTzOptions) / sizeof(kTzOptions[0]));

void configApplyTimezone(const MonitorConfig &cfg) {
    const char *tz = cfg.tz[0] ? cfg.tz : "UTC0";
    setenv("TZ", tz, 1);
    tzset();
    Serial.printf("[cfg] timezone %s\n", tz);
}

void configNormalizeIngestUrl(MonitorConfig &cfg) {
    char *u = cfg.ingestUrl;

    // Leading/trailing whitespace, which a paste into a form field collects.
    size_t start = 0;
    while (u[start] == ' ' || u[start] == '\t') start++;
    if (start) memmove(u, u + start, strlen(u + start) + 1);

    size_t len = strlen(u);
    while (len > 0 && (u[len - 1] == ' ' || u[len - 1] == '\t' ||
                       u[len - 1] == '\r' || u[len - 1] == '\n')) {
        u[--len] = '\0';
    }

    // A full report URL is accepted and reduced to its root, so an endpoint
    // copied out of the API docs works as typed.
    static const char kReportRoute[] = "/nodes/heard";
    const size_t routeLen = sizeof(kReportRoute) - 1;
    if (len > routeLen && strcmp(u + len - routeLen, kReportRoute) == 0) {
        len -= routeLen;
        u[len] = '\0';
    }

    while (len > 0 && u[len - 1] == '/') u[--len] = '\0';
}
