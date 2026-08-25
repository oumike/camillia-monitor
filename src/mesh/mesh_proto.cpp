// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_proto.cpp — Meshtastic packet decode + channel decryption
//
// Trimmed from camillia-mt's mesh_proto.cpp. This build is a passive listener:
// every encoder, and the whole PKI (Curve25519 / AES-CCM) path, is gone. What
// remains is the receive half — protobuf readers, the AES-CTR channel cipher,
// and the ServiceEnvelope reader used by the MQTT source.
//
// PKI is dropped rather than stubbed because it could never work here: a
// PKI-encrypted DM is sealed to its recipient's key, and a monitor is not the
// recipient. Those packets are counted and attributed to a sender, never read.
// ════════════════════════════════════════════════════════════════════════════
#include "mesh_proto.h"
#include "mbedtls/aes.h"

// ── PSK expansion ─────────────────────────────────────────────
// Meshtastic DEFAULT_KEY = kDkBase[0..14] + PSK_byte.
// PSK 0x01 → DEFAULT_KEY unchanged (base64 "AQ==").
static const uint8_t kDkBase[15] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69
};

static void expandPsk(uint8_t psk, uint8_t out[16]) {
    memcpy(out, kDkBase, 15);
    out[15] = psk;
}

static void resolveMeshKey(const uint8_t *key, uint8_t keyLen,
                           uint8_t expanded[16],
                           const uint8_t *&outKey, uint8_t &outLen) {
    outKey = key;
    outLen = keyLen;

    // Meshtastic PSK index 0 (AA==) disables channel encryption.
    if (keyLen == 1 && key && key[0] == 0x00) {
        outLen = 0;
        return;
    }

    if (keyLen == 1 && key) {
        expandPsk(key[0], expanded);
        outKey = expanded;
        outLen = 16;
    }
}

uint8_t computeChannelHash(const char *name, const uint8_t *key, uint8_t keyLen) {
    uint8_t exp[16];
    const uint8_t *k = key;
    uint8_t kl = keyLen;
    resolveMeshKey(key, keyLen, exp, k, kl);
    uint8_t h = 0;
    for (const char *p = name; *p; p++) h ^= (uint8_t)*p;
    for (int i = 0; i < kl; i++) h ^= k[i];
    return h;
}

// ── Channel key table ─────────────────────────────────────────
// Decryption candidates, nothing more. camillia-mt carries per-channel role,
// uplink/downlink, mute and hop-budget flags on this struct because it decides
// what to *send* on each channel; a listener only ever asks "does this key turn
// the payload into a valid Data protobuf?", so those fields are gone, along
// with the virtual DM and ANN channels that exist to originate traffic.
//
// Slot 0 is Meshtastic's default public channel. The rest start empty and are
// filled in from config (channel URL import) so the monitor can also read
// private channels the operator has keys for.
//
// 1-byte PSK keys are stored as a single byte and expanded at runtime via
// expandPsk(); hash is the on-air channel hash, precomputed for slot 0.
ChannelKey CHANNEL_KEYS[MAX_CHANNELS] = {
    //  name         PSK        len  hash  name_buf
    { "LongFast",  { 0x01 },    1,  0x08,  {} },   // Meshtastic default (AQ==)
    { "",          { 0x01 },    1,  0x08,  {} },
    { "",          { 0x01 },    1,  0x08,  {} },
    { "",          { 0x01 },    1,  0x08,  {} },
    { "",          { 0x01 },    1,  0x08,  {} },
    { "",          { 0x01 },    1,  0x08,  {} },
    { "",          { 0x01 },    1,  0x08,  {} },
    { "",          { 0x01 },    1,  0x08,  {} },
};

// ── Protobuf helpers ──────────────────────────────────────────
static size_t pbReadVarint(const uint8_t *buf, size_t len, size_t off, uint64_t &val) {
    val = 0;
    int shift = 0;
    while (off < len) {
        uint8_t b = buf[off++];
        val |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) return off;
    }
    return 0;
}

static size_t pbSkip(const uint8_t *buf, size_t len, size_t i, int wtype) {
    if (wtype == 0) { uint64_t v; return pbReadVarint(buf, len, i, v); }
    if (wtype == 1) return i + 8;
    if (wtype == 5) return i + 4;
    if (wtype == 2) {
        uint64_t sz; size_t j = pbReadVarint(buf, len, i, sz);
        return j ? j + sz : 0;
    }
    return 0;
}

bool decodeData(const uint8_t *buf, size_t len,
                uint32_t &portnum, const uint8_t *&payPtr, size_t &payLen,
                uint32_t &requestId, bool &wantResponse,
                uint32_t *destNode, bool *hasDestNode,
                uint32_t *sourceNode, bool *hasSourceNode) {
    portnum = 0; payPtr = nullptr; payLen = 0; requestId = 0; wantResponse = false;
    if (destNode) *destNode = 0;
    if (sourceNode) *sourceNode = 0;
    if (hasDestNode) *hasDestNode = false;
    if (hasSourceNode) *hasSourceNode = false;
    size_t i = 0;
    while (i < len) {
        uint64_t tag; i = pbReadVarint(buf, len, i, tag); if (!i) break;
        uint32_t field = tag >> 3, wtype = tag & 7;
        if (wtype == 0) {
            uint64_t v; i = pbReadVarint(buf, len, i, v); if (!i) break;
            if (field == 1) portnum = (uint32_t)v;
            else if (field == 3) wantResponse = (v != 0);
            else if (field == 6) requestId = (uint32_t)v;   // request_id varint (Meshtastic standard)
            else if (field == 4 && destNode) {
                *destNode = (uint32_t)v;
                if (hasDestNode) *hasDestNode = true;
            } else if (field == 5 && sourceNode) {
                *sourceNode = (uint32_t)v;
                if (hasSourceNode) *hasSourceNode = true;
            }
        } else if (wtype == 2) {
            uint64_t sz; i = pbReadVarint(buf, len, i, sz); if (!i) break;
            if (field == 2) { payPtr = buf + i; payLen = (size_t)sz; }
            i += sz;
        } else if (wtype == 5) {
            // fixed32 — fields 4=dest, 5=source, 6=request_id, 7=reply_id, 8=emoji
            if (i + 4 <= len) {
                uint32_t v; memcpy(&v, buf + i, 4);
                if (field == 6) requestId = v;
                else if (field == 4 && destNode) {
                    *destNode = v;
                    if (hasDestNode) *hasDestNode = true;
                } else if (field == 5 && sourceNode) {
                    *sourceNode = v;
                    if (hasSourceNode) *hasSourceNode = true;
                }
            }
            i += 4;
        } else { i = pbSkip(buf, len, i, wtype); if (!i) break; }
    }
    return true;
}

bool decodeUser(const uint8_t *buf, size_t len, UserInfo &out) {
    out.id[0] = '\0';
    out.hasId = false;
    out.longName[0] = out.shortName[0] = '\0';
    out.hwModel = 0; out.hasHwModel = false;
    out.role    = 0; out.hasRole    = false;
    size_t i = 0;
    while (i < len) {
        uint64_t tag; i = pbReadVarint(buf, len, i, tag); if (!i) break;
        uint32_t field = tag >> 3, wtype = tag & 7;
        if (wtype == 2) {
            uint64_t sz; i = pbReadVarint(buf, len, i, sz); if (!i) break;
            if (i + sz > len) break;
            if (field == 1 && sz > 0) {
                const size_t copy = min((size_t)sz, sizeof(out.id) - 1);
                memcpy(out.id, buf + i, copy);
                out.id[copy] = '\0';
                out.hasId = true;
            } else if (field == 2 && sz > 0) {
                size_t copy = min((size_t)sz, sizeof(out.longName) - 1);
                while (copy > 0 && ((buf[i + copy] & 0xC0u) == 0x80u)) copy--;
                memcpy(out.longName, buf + i, copy);
                out.longName[copy] = '\0';
            } else if (field == 3 && sz > 0) {
                size_t copy = min((size_t)sz, sizeof(out.shortName) - 1);
                while (copy > 0 && ((buf[i + copy] & 0xC0u) == 0x80u)) copy--;
                memcpy(out.shortName, buf + i, copy);
                out.shortName[copy] = '\0';
            }
            i += sz;
        } else if (wtype == 0) {
            uint64_t v; i = pbReadVarint(buf, len, i, v); if (!i) break;
            if (field == 5)      { out.hwModel = (uint8_t)v; out.hasHwModel = true; }
            else if (field == 7) { out.role    = (uint8_t)v; out.hasRole    = true; }
        } else { i = pbSkip(buf, len, i, wtype); if (!i) break; }
    }
    return true;
}

bool decodePosition(const uint8_t *buf, size_t len, PositionInfo &out) {
    out.latI = out.lonI = out.alt = 0;
    out.precisionBits = 0;
    out.hasPrecisionBits = false;
    out.hasLatLon = false;
    // Legacy compatibility: older builds encoded lat/lon as sint32 varints.
    auto unzz = [](uint32_t v) -> int32_t {
        return (int32_t)((v >> 1) ^ (uint32_t)-(int32_t)(v & 1));
    };
    size_t i = 0;
    while (i < len) {
        uint64_t tag; i = pbReadVarint(buf, len, i, tag); if (!i) break;
        uint32_t field = tag >> 3, wtype = tag & 7;
        if (wtype == 5) {
            // Current Meshtastic Position.latitude_i/longitude_i are sfixed32.
            if (i + 4 > len) break;
            uint32_t v = (uint32_t)buf[i]
                       | ((uint32_t)buf[i + 1] << 8)
                       | ((uint32_t)buf[i + 2] << 16)
                       | ((uint32_t)buf[i + 3] << 24);
            if (field == 1)      { out.latI = (int32_t)v; out.hasLatLon = true; }
            else if (field == 2)   out.lonI = (int32_t)v;
            else if (field == 3)   out.alt  = (int32_t)v;
            i += 4;
        } else if (wtype == 0) {
            uint64_t v; i = pbReadVarint(buf, len, i, v); if (!i) break;
            if (field == 1)       { out.latI = unzz((uint32_t)v); out.hasLatLon = true; }
            else if (field == 2)    out.lonI = unzz((uint32_t)v);
            else if (field == 3)    out.alt  = (int32_t)(uint32_t)v;
            else if (field == 22)  { out.precisionBits = (uint8_t)v;
                                     out.hasPrecisionBits = true; }
        } else { i = pbSkip(buf, len, i, wtype); if (!i) break; }
    }
    return true;
}

bool decodeTelemetry(const uint8_t *buf, size_t len, TelemetryInfo &out) {
    // Telemetry.device_metrics = field 2 (legacy senders may use 1)
    // Telemetry.environment_metrics = field 3
    out = {0, 0, 0, 0, 0, 0, 0, false, false, false};
    size_t i = 0;
    while (i < len) {
        uint64_t tag; i = pbReadVarint(buf, len, i, tag); if (!i) break;
        uint32_t field = tag >> 3, wtype = tag & 7;
        if (wtype == 2 && (field == 1 || field == 2)) {
            // DeviceMetrics submessage
            uint64_t sz; i = pbReadVarint(buf, len, i, sz); if (!i) break;
            const uint8_t *dm = buf + i; size_t dmLen = sz; i += sz;
            size_t j = 0;
            while (j < dmLen) {
                uint64_t t2; j = pbReadVarint(dm, dmLen, j, t2); if (!j) break;
                uint32_t f2 = t2 >> 3, w2 = t2 & 7;
                if (w2 == 0) {
                    uint64_t v; j = pbReadVarint(dm, dmLen, j, v); if (!j) break;
                    if (f2 == 1) out.battPct = (float)v;
                } else if (w2 == 5) {
                    if (j + 4 <= dmLen) {
                        float fv; memcpy(&fv, dm + j, 4);
                        if (f2 == 2) out.voltage = fv;
                        else if (f2 == 3) out.chUtil  = fv;
                        else if (f2 == 4) out.airUtil = fv;
                    }
                    j += 4;
                } else { j = pbSkip(dm, dmLen, j, w2); if (!j) break; }
            }
            out.hasDeviceMetrics = true;
        } else if (wtype == 2 && field == 3) {
            // EnvironmentMetrics submessage
            uint64_t sz; i = pbReadVarint(buf, len, i, sz); if (!i) break;
            const uint8_t *em = buf + i; size_t emLen = sz; i += sz;
            size_t j = 0;
            while (j < emLen) {
                uint64_t t2; j = pbReadVarint(em, emLen, j, t2); if (!j) break;
                uint32_t f2 = t2 >> 3, w2 = t2 & 7;
                if (w2 == 5) {
                    if (j + 4 <= emLen) {
                        float fv; memcpy(&fv, em + j, 4);
                        if (f2 == 1) out.temperatureC = fv;
                        else if (f2 == 2) out.humidityPct = fv;
                        else if (f2 == 3) out.pressureHpa = fv;
                    }
                    j += 4;
                } else {
                    j = pbSkip(em, emLen, j, w2); if (!j) break;
                }
            }
            out.hasEnvironmentMetrics = true;
        } else { i = pbSkip(buf, len, i, wtype); if (!i) break; }
    }
    out.valid = out.hasDeviceMetrics || out.hasEnvironmentMetrics;
    return true;
}

// ── AES-CTR core ─────────────────────────────────────────────
static bool aesCtr(const uint8_t *key, uint8_t keyLen,
                   uint32_t packetId, uint32_t fromNode,
                   const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t nonce[16] = {0};
    memcpy(nonce,     &packetId, 4);
    memcpy(nonce + 8, &fromNode, 4);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int bits = (keyLen == 32) ? 256 : 128;
    if (mbedtls_aes_setkey_enc(&ctx, key, bits) != 0) {
        mbedtls_aes_free(&ctx); return false;
    }
    size_t nc_off = 0; uint8_t stream[16] = {0};
    bool ok = (mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream, in, out) == 0);
    mbedtls_aes_free(&ctx);
    return ok;
}

// Returns true if plain looks like a valid Meshtastic Data protobuf.
static bool looksLikeData(const uint8_t *plain, size_t len) {
    if (len < 2) return false;
    // Expect field 1 or 2 as first tag; wire types 0 (varint) or 2 (len-delim)
    uint8_t tag = plain[0];
    if (tag == 0 || tag == 0xFF) return false;
    int wtype = tag & 0x07;
    if (wtype > 5) return false;
    // Try to decode portnum (field 1, varint) to verify it's a known port
    uint32_t portnum = 0; const uint8_t *payPtr = nullptr; size_t payLen = 0;
    uint32_t reqId = 0; bool wantResp = false;
    decodeData(plain, len, portnum, payPtr, payLen, reqId, wantResp);
    // Accept known ports or any non-zero port up to 1024
    return portnum > 0 && portnum <= 1024;
}

int decryptPacket(const MeshHdr &hdr, const uint8_t *cipher,
                  uint8_t *plain, size_t len) {
    auto tryDecrypt = [&](int i) -> bool {
        // An unconfigured slot is not a decryption candidate. Without this it
        // is: keyLen 0 means "this channel is not encrypted", so the ciphertext
        // is passed through verbatim and handed to looksLikeData(), which is a
        // weak enough check that random ciphertext clears it every so often —
        // inventing a portnum and a payload out of noise. camillia-mt never hit
        // this because its spare slots all carried the default PSK; here they
        // are genuinely empty.
        if (CHANNEL_KEYS[i].keyLen == 0 && CHANNEL_KEYS[i].name[0] == '\0') return false;

        uint8_t exp[16];
        const uint8_t *keyPtr = CHANNEL_KEYS[i].key;
        uint8_t keyLen = CHANNEL_KEYS[i].keyLen;
        resolveMeshKey(CHANNEL_KEYS[i].key, CHANNEL_KEYS[i].keyLen, exp, keyPtr, keyLen);
        if (keyLen == 0) {
            memcpy(plain, cipher, len);
        } else {
            if (!aesCtr(keyPtr, keyLen, hdr.id, hdr.from, cipher, plain, len)) return false;
        }
        return looksLikeData(plain, len);
    };

    // Pass 1: try the channel whose hash matches hdr.channel
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (CHANNEL_KEYS[i].hash != hdr.channel) continue;
        if (tryDecrypt(i)) return i;
    }
    // Pass 2: fall back — try all keys (handles unknown/unregistered channels)
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (CHANNEL_KEYS[i].hash == hdr.channel) continue; // already tried
        if (tryDecrypt(i)) return i;
    }
    return -1;
}


const char *portnumName(uint32_t p) {
    switch (p) {
        case TEXT_MESSAGE_APP:  return "TEXT";
        case POSITION_APP:      return "POSITION";
        case NODEINFO_APP:      return "NODEINFO";
        case ROUTING_APP:       return "ROUTING";
        case TELEMETRY_APP:     return "TELEMETRY";
        case TRACEROUTE_APP:    return "TRACEROUTE";
        case NEIGHBORINFO_APP:  return "NEIGHBORINFO";
        default:                return "UNKNOWN";
    }
}
