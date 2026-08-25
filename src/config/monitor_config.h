#pragma once
// ════════════════════════════════════════════════════════════════════════════
// config/monitor_config.h — Persistent settings for the Camillia Monitor
//
// Everything the operator can change, in one struct, persisted to NVS as a
// single versioned blob. There is no filesystem partition on this board (see
// partitions.csv), so NVS is the only durable store.
//
// This is deliberately not camillia-mt's RhinoConfig: that struct is dominated
// by settings for a device that transmits, chats and renders a UI for a person
// holding it. A monitor's configuration is four questions — what network am I
// on, where do I ship observations, what should I listen to, and which channels
// can I decrypt.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include "mesh_config.h"

// Bumped when the layout of MonitorConfig changes incompatibly. loadConfig()
// discards a blob written by a different version and starts from defaults,
// which is the honest behaviour for a device whose settings are all re-enterable
// in a minute — the alternative is migrating fields that may never have existed.
#define MONITOR_CFG_VERSION 4

// One decryptable channel. Stored as the operator entered it: a name plus the
// raw PSK bytes. A single byte is Meshtastic's packed PSK index (1 = the well
// known default key, 0 = no encryption); 16 or 32 bytes is a literal AES key.
struct ChannelConfig {
    char    name[16];
    uint8_t key[32];
    uint8_t keyLen;    // 0 = slot unused
};

struct MonitorConfig {
    uint32_t version;

    // ── Identity ────────────────────────────────────────────────────────────
    // How this monitor labels itself to the ingestor. Not a Meshtastic node
    // name: the device never announces itself on the mesh.
    char     monitorName[32];

    // ── WiFi station ────────────────────────────────────────────────────────
    // Empty ssid means "never configured", which is what puts the device into
    // SoftAP onboarding on boot.
    char     wifiSsid[33];
    char     wifiPass[64];

    // ── Ingestor uplink ─────────────────────────────────────────────────────
    bool     ingestEnabled;
    char     ingestUrl[160];     // full URL, e.g. http://10.0.0.5:3000/api/nodes
    char     ingestToken[80];    // optional; sent as "Authorization: Bearer ..."
    uint32_t ingestIntervalS;    // how often the queue is flushed

    // ── Radio ───────────────────────────────────────────────────────────────
    char     region[12];         // "US", "EU_868", ...
    bool     usePreset;          // false = the custom bw/sf/cr below
    uint8_t  modemPreset;        // ModemPreset index, when usePreset
    uint16_t bwCode;             // Meshtastic bandwidth code, when custom
    uint8_t  loraSf;             // 7..12, when custom
    uint8_t  loraCr;             // 5..8,  when custom

    // Frequency slot. Meshtastic normally derives the slot from a hash of the
    // primary channel name; pinning it matters for custom setups, where a
    // narrow bandwidth gives hundreds of slots and the name hash is unlikely to
    // land on the one the local mesh actually uses.
    bool     pinSlot;
    uint32_t freqSlot;

    // ── Channels the monitor can decrypt ────────────────────────────────────
    // Slot 0 is the primary; its name is also what the frequency slot hashes
    // when pinSlot is false.
    ChannelConfig channels[MESH_CHANNELS];

    // ── MQTT ────────────────────────────────────────────────────────────────
    // Subscribe-only. camillia-mt also carries mqttEncryption and mqttMapReport;
    // both describe what a node *publishes* — payload encryption on uplink, and
    // announcing itself to an MQTT-fed map — and this device publishes nothing,
    // so they would be settings with no effect.
    bool     mqttEnabled;
    char     mqttServer[64];
    uint16_t mqttPort;
    char     mqttUser[32];
    char     mqttPass[48];
    char     mqttRoot[48];       // topic root, e.g. "msh/US"
    bool     mqttTls;

    // ── Display ─────────────────────────────────────────────────────────────
    // Imperial by default. Affects only what is shown on the screen and in the
    // portal; everything sent to the ingestor stays in the units Meshtastic
    // defines (1e-7 degrees, metres, celsius), because a consumer of that data
    // must not have to know how this device happened to be configured.
    bool     useMetric;

    // POSIX TZ string, e.g. "EST5EDT,M3.2.0,M11.1.0". Stored rather than an
    // offset because newlib applies the DST rules embedded in it, so the clock
    // changes on its own twice a year instead of drifting an hour until someone
    // notices. Applied with setenv("TZ")+tzset(); everything reported upstream
    // stays UTC.
    char     tz[48];

    // ── Config portal ───────────────────────────────────────────────────────
    bool     webAuthEnabled;
    char     webPass[64];
};

// The live config. Populated by configLoad() before anything reads it.
extern MonitorConfig gCfg;

// Fills cfg with defaults: US LongFast, no network, uplink off, the public
// Meshtastic channel in slot 0.
void configDefaults(MonitorConfig &cfg);

// Reads the NVS blob into gCfg, falling back to defaults when absent or written
// by a different MONITOR_CFG_VERSION. Always leaves gCfg usable.
void configLoad();

// Persists gCfg. Returns false if NVS rejected the write.
bool configSave();

// Wipes the stored blob and resets gCfg to defaults.
void configFactoryReset();

// Logs NVS used/free entry counts. Called at boot and on any save failure.
void configNvsReport(const char *why);

// Normalises the ingest URL to a bare API root: trims whitespace and trailing
// slashes, and accepts a full report URL (".../api/nodes/heard") by stripping
// the route off it. That last part is what lets a value stored by an earlier
// build, or one pasted straight out of Swagger, keep working untouched.
void configNormalizeIngestUrl(MonitorConfig &cfg);

// Pushes cfg.tz into the C library so localtime_r() resolves against it.
// Call after configLoad() and after any save that changed the zone.
void configApplyTimezone(const MonitorConfig &cfg);

// The timezone presets offered by the config portal.
struct TzOption { const char *label; const char *posix; };
extern const TzOption kTzOptions[];
extern const uint8_t  kTzOptionCount;

// ── Derived radio settings ──────────────────────────────────────────────────
// Resolves config (region + preset, or region + custom bw/sf/cr, plus the slot
// rule) into the four numbers the radio actually needs. Kept as one function so
// the config page's preview and the radio's tuning cannot disagree.
struct ResolvedModem {
    float   freq;   // MHz
    float   bw;     // kHz
    uint8_t sf;
    uint8_t cr;
    uint32_t slot;      // the slot that was used
    uint32_t slotCount; // how many the band holds at this bandwidth
};
ResolvedModem configResolveModem(const MonitorConfig &cfg);

// Copies the configured channels into the mesh layer's CHANNEL_KEYS table and
// recomputes each on-air hash. Call after any change to cfg.channels.
void configApplyChannels(const MonitorConfig &cfg);

// Parses a base64 PSK ("AQ==", or a full 16/32-byte key) into out/outLen.
// Returns false on malformed input or an unusable length.
bool configParsePsk(const char *b64, uint8_t *out, uint8_t &outLen);

// Renders a channel's key back to base64 for display in the config form.
void configFormatPsk(const uint8_t *key, uint8_t keyLen, char *out, size_t outCap);
