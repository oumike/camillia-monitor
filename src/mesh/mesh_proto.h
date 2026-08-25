#pragma once
// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_proto.h — Meshtastic packet structures and receive-side decode
//
// Trimmed from camillia-mt. Decode only: no encoders, no PKI. See mesh_proto.cpp
// for why the PKI path is absent rather than stubbed.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include "mesh_config.h"

// ── Channel key table ─────────────────────────────────────────
// A decryption candidate. The TX-side fields camillia-mt carries here (role,
// uplink/downlink, mute, per-channel hop budget) have no meaning for a node
// that never originates traffic; see the table in mesh_proto.cpp.
struct ChannelKey {
    const char *name;         // points to literal at init; redirected to name_buf after import
    uint8_t     key[32];
    uint8_t     keyLen;       // 1 = packed PSK index, 16 = AES-128, 32 = AES-256
    uint8_t     hash;         // XOR(name_bytes) ^ XOR(expanded_key_bytes)
    char        name_buf[16]; // mutable storage for imported names (zero at static init)
};

// Inline definitions so the table lives in mesh_proto.cpp (extern declared below)
extern ChannelKey CHANNEL_KEYS[MAX_CHANNELS];

// ── Meshtastic raw packet header (16 bytes, little-endian) ────
struct __attribute__((packed)) MeshHdr {
    uint32_t to;
    uint32_t from;
    uint32_t id;
    uint8_t  flags;    // [2:0]=hop_limit [3]=want_ack [4]=via_mqtt [7:5]=hop_start
    uint8_t  channel;    // channel hash
    uint8_t  next_hop;   // low byte of next-hop node (0 = no preference)
    uint8_t  relay_node; // low byte of node that relayed this packet
};

// ── Meshtastic port numbers ───────────────────────────────────
enum PortNum : uint32_t {
    UNKNOWN_APP      = 0,
    TEXT_MESSAGE_APP = 1,
    POSITION_APP     = 3,
    NODEINFO_APP     = 4,
    ROUTING_APP      = 5,    // ACK/NAK packets (Meshtastic PortNum_ROUTING_APP)
    STORE_FORWARD_APP = 65,  // Store and Forward module (replayed messages)
    TELEMETRY_APP    = 67,
    NEIGHBORINFO_APP = 71,
    TRACEROUTE_APP   = 70,   // traceroute (not ACK)
    MAP_REPORT_APP   = 73,   // MQTT-only self-description; never sent over LoRa
    MESH_BEACON_APP  = 37,   // Meshtastic 2.7 MeshBeacon: cross-mesh advertisement
};

// ── Decoded incoming packet ───────────────────────────────────
struct MeshPacket {
    MeshHdr  hdr;
    uint32_t portnum;
    float    rssi;
    float    snr;
    uint32_t rxMs;            // millis() at receipt
    // Meshtastic's Data.payload maximum. Was 220, which is enough for any text
    // message sent directly but not for one arriving through Store and Forward:
    // a replay wraps the original text in a StoreAndForward message, and the
    // ~4-5 bytes of extra framing pushed full-length messages over the old cap
    // — where they were dropped in silence.
    uint8_t  payload[237];    // decrypted inner payload (after Data wrapper)
    size_t   payloadLen;
    uint32_t requestId;       // non-zero for ROUTING_APP ACK/NAK
    uint32_t dataDest;        // Data.dest (field 4), when present
    uint32_t dataSource;      // Data.source (field 5), when present
    bool     hasDataDest;
    bool     hasDataSource;
    bool     wantResponse;    // Data.want_response (observed, never acted on)
    bool     decrypted;       // false for PKI DMs and unknown-key channels
    int      chanIdx;         // which channel key was used (-1 = none)
};

// ── Decoded app-layer payloads ────────────────────────────────
struct UserInfo {
    // User.id (field 1): the node's own canonical "!abcdef12" string. Normally
    // the hex of its node number, but it is the node's own claim about its
    // identity, so it is decoded rather than assumed.
    char    id[16];
    bool    hasId;
    char    longName[40];
    char    shortName[5];
    uint8_t hwModel;      // User.hw_model (field 5), HardwareModel enum value
    bool    hasHwModel;
    uint8_t role;         // User.role (field 7), Config.DeviceConfig.Role value
    bool    hasRole;
};

struct PositionInfo {
    int32_t  latI;   // degrees * 1e7
    int32_t  lonI;
    int32_t  alt;    // meters
    // Position.precision_bits (field 22): how much of the coordinate the sender
    // kept. Low values are a deliberately blurred fix, not a bad one, and
    // anything consuming the position needs to be able to tell the difference.
    uint8_t  precisionBits;
    bool     hasPrecisionBits;
    bool     hasLatLon;   // false when the packet carried no coordinate at all
};

struct TelemetryInfo {
    float battPct;
    float voltage;
    float chUtil;
    float airUtil;
    float temperatureC;
    float humidityPct;
    float pressureHpa;
    bool  hasDeviceMetrics;
    bool  hasEnvironmentMetrics;
    bool  valid;
};

// Decode Data message: fills portnum, payload slice, requestId, wantResponse
bool decodeData(const uint8_t *buf, size_t len,
                uint32_t &portnum, const uint8_t *&payPtr, size_t &payLen,
                uint32_t &requestId, bool &wantResponse,
                uint32_t *destNode = nullptr, bool *hasDestNode = nullptr,
                uint32_t *sourceNode = nullptr, bool *hasSourceNode = nullptr);

bool decodeUser(const uint8_t *buf, size_t len, UserInfo &out);
bool decodePosition(const uint8_t *buf, size_t len, PositionInfo &out);
bool decodeTelemetry(const uint8_t *buf, size_t len, TelemetryInfo &out);

// Compute the on-air channel hash (XOR of name bytes ^ XOR of expanded key bytes).
uint8_t computeChannelHash(const char *name, const uint8_t *key, uint8_t keyLen);


// ── Channel decryption ────────────────────────────────────────
// Try all known channel keys against a received payload; returns the channel
// index that produced a plausible Data protobuf, or -1 if none did.
int  decryptPacket(const MeshHdr &hdr, const uint8_t *cipher,
                   uint8_t *plain, size_t len);


// ── Port name helper ──────────────────────────────────────────
const char *portnumName(uint32_t p);
