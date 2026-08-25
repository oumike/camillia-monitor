// ════════════════════════════════════════════════════════════════════════════
// net/node_uplink.cpp — heard-node reporting
// ════════════════════════════════════════════════════════════════════════════
#include "node_uplink.h"
#include "monitor_config.h"
#include "mesh_enum_names.h"
#include "time_sync.h"
#include "mqtt_monitor.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace {

// Meshtastic reserves both ends of the range: 0 is unassigned and 0xFFFFFFFF is
// the broadcast address. The endpoint rejects them, and a packet addressed
// *from* either is malformed rather than a node worth recording.
constexpr uint32_t kBroadcastNode = 0xFFFFFFFFu;

// Minimum gap between POSTs while a backlog is draining. Each POST blocks, and
// the radio can only hold one packet, so the backlog is drained at a trickle
// rather than in a burst. At 250 ms a cold start that discovers 40 nodes is
// caught up in ten seconds and never misses more than one packet at a time.
constexpr uint32_t kPostGapMs = 250;

constexpr uint32_t kHttpTimeoutMs = 4000;

struct NodeObs {
    uint32_t nodeNum;        // 0 marks a free slot
    uint32_t lastReportMs;

    char     longName[41];
    char     shortName[9];
    bool     hasLongName;
    bool     hasShortName;

    char     nodeId[16];   // the node's own User.id, when it has told us
    bool     hasNodeId;

    uint8_t  hwModel;  bool hasHwModel;
    uint8_t  role;     bool hasRole;

    // millis() when the packet behind the pending report arrived, so the report
    // can carry the reception time rather than the send time. Those differ by
    // however long the node waited in the send queue.
    uint32_t lastHeardMs;

    float    snr;
    int16_t  rssi;
    bool     hasSignal;

    uint8_t  hopsAway; bool hasHops;
    bool     viaMqtt;

    int32_t  latI, lonI;  bool hasLatLon;
    int32_t  alt;         bool hasAlt;
    uint8_t  precisionBits; bool hasPrecision;

    uint8_t  battLevel; bool hasBatt;
    float    voltage;   bool hasVoltage;

    bool     reported;   // has been accepted by the ingestor at least once
    bool     pending;    // has unsent changes
};

NodeObs  sNodes[MAX_NODES];
uint16_t sKnown   = 0;
uint16_t sDropped = 0;
uint32_t sSent    = 0;
uint32_t sFailed  = 0;

uint32_t sOurNodeNum = 0;
char     sOurNodeId[12] = "";

uint32_t sLastPostMs = 0;
uint16_t sScanCursor = 0;

// Outcome of the most recent attempt, which is what the status icon reflects.
bool sHasAttempted = false;
bool sLastPostOk   = false;

// Node count held by the ingestor when this device booted, and how many nodes
// our reports have added to it since.
uint32_t sIngestorBaseline    = 0;
bool     sBaselineFetched     = false;
uint32_t sBaselineNextTryMs   = 0;
uint32_t sCreatedSinceBoot    = 0;

// ── Messages ────────────────────────────────────────────────────────────────
// A packet is identified by sender plus packet id; the id alone is only unique
// per sender. A mesh rebroadcasts heavily, so the same packet arrives several
// times by different routes — this ring remembers what has already been
// reported so the endpoint is not told the same thing five times.
constexpr uint16_t kSeenRing  = 128;
constexpr uint8_t  kMsgQueue  = 16;

uint64_t sSeen[kSeenRing] = {};
uint16_t sSeenCursor = 0;

struct MessageObs {
    bool     used;
    uint32_t packetId;
    uint32_t fromNum, toNum;
    uint8_t  channel;
    uint32_t portnum;
    bool     hasPort;
    bool     encrypted;
    float    snr;
    int16_t  rssi;
    uint8_t  hopsAway;
    bool     hasHops;
    bool     viaMqtt;
    uint32_t rxMs;
    bool     hasText;
    char     text[MESH_TEXT_MAX_LEN + 1];
    bool     hasTelemetry;
    TelemetryInfo telemetry;
};

MessageObs sMsgQueue[kMsgQueue] = {};
uint8_t    sMsgHead = 0;      // next slot to fill
uint32_t   sMessagesBaseline = 0;
bool       sMessagesBaselineFetched = false;
uint32_t   sMessagesCreated = 0;
uint16_t   sMessagesDropped = 0;

// ── MQTT topic batching ─────────────────────────────────────────────────────
// Topics go up in batches on a slow cadence rather than one at a time: a topic
// appearing is not perishable the way a packet is, and a busy broker would
// otherwise turn the trickle into a steady stream of near-empty POSTs.
// Two cadences. The slow one is the steady state, once topic discovery has
// plateaued and new ones trickle in. The fast one applies while the pending
// queue is more than half full, which is what a cold start on a busy broker
// looks like — without it the queue overflows between batches and topics are
// lost to a timer rather than to any real limit.
constexpr uint32_t kMqttBatchIdleMs  = 60000;
constexpr uint32_t kMqttBatchBusyMs  = 8000;
constexpr int      kMqttBatchMax     = 32;
uint32_t sNextMqttBatchMs = 0;
uint32_t sMqttChannels = 0;
bool     sMqttSeedRequested = false;
bool     sMqttChannelsKnown = false;

bool seenBefore(uint32_t from, uint32_t packetId) {
    const uint64_t key = ((uint64_t)from << 32) | packetId;
    for (uint16_t i = 0; i < kSeenRing; i++) {
        if (sSeen[i] == key) return true;
    }
    sSeen[sSeenCursor] = key;
    sSeenCursor = (uint16_t)((sSeenCursor + 1) % kSeenRing);
    return false;
}

NodeObs *find(uint32_t nodeNum) {
    for (uint16_t i = 0; i < MAX_NODES; i++) {
        if (sNodes[i].nodeNum == nodeNum) return &sNodes[i];
    }
    return nullptr;
}

NodeObs *findOrCreate(uint32_t nodeNum, bool &created) {
    created = false;
    if (NodeObs *n = find(nodeNum)) return n;

    for (uint16_t i = 0; i < MAX_NODES; i++) {
        if (sNodes[i].nodeNum != 0) continue;
        NodeObs &n = sNodes[i];
        n = NodeObs{};
        n.nodeNum = nodeNum;
        sKnown++;
        created = true;
        return &n;
    }

    // Table full. Deliberately not evicting the oldest: this device is meant to
    // be watching a mesh that fits, and silently rotating nodes out would make
    // the reports oscillate as evicted nodes get "discovered" again. Counted
    // instead, so a full table is visible rather than inferred.
    sDropped++;
    return nullptr;
}

// Copies src into dst when it differs, reporting whether anything changed.
bool setStr(char *dst, size_t cap, const char *src, bool &has) {
    if (!src || !src[0]) return false;
    if (has && strncmp(dst, src, cap - 1) == 0) return false;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    has = true;
    return true;
}

// ── Report body ─────────────────────────────────────────────────────────────
void buildReport(const NodeObs &n, String &out) {
    JsonDocument doc;

    doc["nodeNum"] = n.nodeNum;
    // The node's own id when it has announced one, so the ingestor keys on what
    // the node calls itself rather than on a value derived here.
    if (n.hasNodeId) doc["nodeId"] = n.nodeId;
    if (sOurNodeId[0]) doc["heardBy"] = sOurNodeId;

    // Only once a clock has been set, and only for a packet heard after it was
    // set — timeEpochAtMillis() returns 0 otherwise, and the ingestor's own
    // arrival time is then the more accurate of the two.
    const uint32_t rxTime = timeEpochAtMillis(n.lastHeardMs);
    if (rxTime != 0) doc["rxTime"] = rxTime;

    if (n.hasLongName)  doc["longName"]  = n.longName;
    if (n.hasShortName) doc["shortName"] = n.shortName;

    // Enum names, not numbers, and only when this firmware recognises the value
    // — see mesh_enum_names.h for why an unknown one is omitted rather than
    // labelled.
    if (n.hasHwModel) {
        // The raw enum value always goes up, even when this firmware has no
        // name for it. The number is the part that cannot be recovered later —
        // the ingestor can resolve a name for it whenever it learns one, but
        // only if the value survived the trip.
        doc["hwModelNum"] = n.hwModel;
        if (const char *hw = meshHwModelName(n.hwModel)) doc["hwModel"] = hw;
    }
    if (n.hasRole) {
        if (const char *r = meshRoleName(n.role)) doc["role"] = r;
    }

    if (n.hasSignal) {
        doc["snr"]  = n.snr;
        doc["rssi"] = n.rssi;   // the endpoint types this as an integer
    }
    if (n.hasHops) doc["hopsAway"] = n.hopsAway;
    doc["viaMqtt"] = n.viaMqtt;

    if (n.hasLatLon) {
        doc["latitudeI"]  = n.latI;
        doc["longitudeI"] = n.lonI;
    }
    if (n.hasAlt)       doc["altitude"]      = n.alt;
    if (n.hasPrecision) doc["precisionBits"] = n.precisionBits;

    if (n.hasBatt)    doc["batteryLevel"] = n.battLevel;
    if (n.hasVoltage) doc["voltage"]      = n.voltage;

    // rxTime is deliberately absent: this device has no RTC and does not sync a
    // clock, so anything it put there would be wrong. The endpoint documents
    // that it falls back to its own clock, which — given a report is sent
    // within seconds of reception — is the more accurate of the two.

    serializeJson(doc, out);
}

// Every route hangs off the configured API root. The root is stored normalised
// (see configNormalizeIngestUrl), so this is a straight concatenation rather
// than the suffix-stripping guesswork it replaced — which quietly disabled
// message reporting whenever the configured URL was not the exact shape it
// expected.
bool buildApiUrl(const char *suffix, char *out, size_t cap) {
    const size_t baseLen = strlen(gCfg.ingestUrl);
    if (baseLen == 0) return false;
    if (baseLen + strlen(suffix) + 1 > cap) return false;

    memcpy(out, gCfg.ingestUrl, baseLen);
    out[baseLen] = '\0';
    strncat(out, suffix, cap - baseLen - 1);
    return true;
}

bool postReport(const NodeObs &n) {
    String body;
    buildReport(n, body);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);

    char url[192];
    if (!buildApiUrl("/nodes/heard", url, sizeof(url))) return false;
    if (!http.begin(client, url)) {
        Serial.printf("[uplink] bad endpoint URL: %s\n", url);
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    // The endpoint's own scheme. Its guard also accepts "Authorization: Bearer",
    // but x-api-key is what the OpenAPI document declares.
    if (gCfg.ingestToken[0]) http.addHeader("x-api-key", gCfg.ingestToken);

    const int status = http.POST(body);
    const bool ok = (status >= 200 && status < 300);

    if (ok) {
        // The response says whether this report brought the node into
        // existence. Without it the running total would drift: a node heard for
        // the first time *here* is very often already stored there.
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            if (doc["created"].as<bool>()) sCreatedSinceBoot++;
        }
    }

    if (!ok) {
        // 400 and 401 are configuration faults, not transient ones, and will
        // repeat forever at the retry interval — say which so the log points at
        // the fix rather than just counting failures.
        const char *hint = (status == 401) ? "  (check the API key)"
                         : (status == 400) ? "  (endpoint rejected the report)"
                         : (status < 0)    ? "  (connection failed)"
                                           : "";
        Serial.printf("[uplink] %08lx -> HTTP %d%s\n",
                      (unsigned long)n.nodeNum, status, hint);
    }
    http.end();
    return ok;
}

// The portal configures the report URL (".../nodes/heard"); the count lives
// next to it at ".../nodes/count". Deriving it keeps a second URL out of the
// config form, at the cost of assuming the two stay siblings — if the report
// URL is not a "/heard" route we simply never fetch a baseline, and the display
// falls back to counting only what this device contributed.
void fetchBaseline() {
    char url[192];
    if (!buildApiUrl("/nodes/count", url, sizeof(url))) {
        sBaselineFetched = true;
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) return;

    const int status = http.GET();
    if (status == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            sIngestorBaseline = doc["count"] | 0U;
            sBaselineFetched  = true;
            Serial.printf("[uplink] ingestor holds %lu nodes at boot\n",
                          (unsigned long)sIngestorBaseline);
        }
    } else {
        Serial.printf("[uplink] count fetch -> HTTP %d (will retry)\n", status);
    }
    http.end();
}

void fetchMqttChannelBaseline() {
    char url[192];
    if (!buildApiUrl("/mqtt/channels/count", url, sizeof(url))) {
        sMqttChannelsKnown = true;
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) return;

    if (http.GET() == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            sMqttChannels = doc["count"] | 0U;
            sMqttChannelsKnown = true;
        }
    }
    http.end();
}

void fetchMessageBaseline() {
    char url[192];
    if (!buildApiUrl("/messages/count", url, sizeof(url))) {
        sMessagesBaselineFetched = true;
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) return;

    if (http.GET() == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            sMessagesBaseline = doc["count"] | 0U;
            sMessagesBaselineFetched = true;
            Serial.printf("[uplink] ingestor holds %lu messages at boot\n",
                          (unsigned long)sMessagesBaseline);
        }
    }
    http.end();
}

bool postMessage(MessageObs &m) {
    char url[192];
    if (!buildApiUrl("/messages/heard", url, sizeof(url))) return false;

    JsonDocument doc;
    doc["packetId"] = m.packetId;
    doc["fromNum"]  = m.fromNum;
    doc["toNum"]    = m.toNum;
    doc["channel"]  = m.channel;
    doc["encrypted"] = m.encrypted;
    if (sOurNodeId[0]) doc["heardBy"] = sOurNodeId;

    if (m.hasPort) {
        doc["portnum"] = m.portnum;
        doc["portName"] = portnumName(m.portnum);
    }
    if (m.hasText) doc["text"] = m.text;

    const uint32_t rxTime = timeEpochAtMillis(m.rxMs);
    if (rxTime != 0) doc["rxTime"] = rxTime;

    doc["snr"]  = m.snr;
    doc["rssi"] = m.rssi;
    if (m.hasHops) doc["hopsAway"] = m.hopsAway;
    doc["viaMqtt"] = m.viaMqtt;

    if (m.hasTelemetry) {
        const TelemetryInfo &t = m.telemetry;
        if (t.hasDeviceMetrics) {
            doc["batteryLevel"] = (uint8_t)constrain((int)lroundf(t.battPct), 0, 255);
            doc["voltage"] = t.voltage;
            doc["channelUtilization"] = t.chUtil;
            doc["airUtilTx"] = t.airUtil;
        }
        if (t.hasEnvironmentMetrics) {
            doc["temperature"] = t.temperatureC;
            doc["humidity"]    = t.humidityPct;
            doc["pressure"]    = t.pressureHpa;
        }
    }

    String body;
    serializeJson(doc, body);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", "application/json");
    if (gCfg.ingestToken[0]) http.addHeader("x-api-key", gCfg.ingestToken);

    const int status = http.POST(body);
    const bool ok = (status >= 200 && status < 300);
    if (ok) {
        JsonDocument resp;
        if (deserializeJson(resp, http.getString()) == DeserializationError::Ok) {
            if (resp["created"].as<bool>()) sMessagesCreated++;
        }
    } else {
        Serial.printf("[uplink] msg %08lx:%lu -> HTTP %d\n",
                      (unsigned long)m.fromNum, (unsigned long)m.packetId, status);
    }
    http.end();
    return ok;
}

// Returns true when a batch was attempted, so the caller can spend its one
// blocking POST here and come back next pass.
bool postMqttTopics() {
    if (mqttPendingTopicCount() == 0) return false;

    char url[192];
    if (!buildApiUrl("/mqtt/captures", url, sizeof(url))) return false;

    const char *topics[kMqttBatchMax];
    int indices[kMqttBatchMax];
    const int n = mqttCollectUnreported(topics, indices, kMqttBatchMax);
    if (n == 0) return false;

    JsonDocument doc;
    JsonArray arr = doc["topics"].to<JsonArray>();
    for (int i = 0; i < n; i++) arr.add(topics[i]);

    String body;
    serializeJson(doc, body);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) return true;
    http.addHeader("Content-Type", "application/json");
    if (gCfg.ingestToken[0]) http.addHeader("x-api-key", gCfg.ingestToken);

    const int status = http.POST(body);
    if (status >= 200 && status < 300) {
        // Only confirmed on success. An unconfirmed batch stays pending and is
        // offered again next interval, so a failed POST loses nothing.
        mqttMarkTopicsReported(indices, n);

        // The response carries the store's current channel total, so the
        // display stays current without polling for it separately.
        JsonDocument resp;
        if (deserializeJson(resp, http.getString()) == DeserializationError::Ok) {
            if (!resp["channels"].isNull()) {
                sMqttChannels = resp["channels"].as<uint32_t>();
                sMqttChannelsKnown = true;
            }
        }
        Serial.printf("[uplink] reported %d MQTT topics\n", n);
    } else {
        Serial.printf("[uplink] MQTT topic batch -> HTTP %d\n", status);
    }
    http.end();
    return true;
}

// Fills the census with what the store already holds, so the screen opens
// showing the whole picture instead of only this session's share.
void fetchMqttSeed() {
    char url[192];
    if (!buildApiUrl("/mqtt/channels", url, sizeof(url))) {
        mqttCensusMarkSeeded();
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) { mqttCensusMarkSeeded(); return; }

    if (http.GET() == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            for (JsonObject c : doc["channels"].as<JsonArray>()) {
                const char *name = c["channel"] | "";
                if (name[0]) mqttCensusSeed(name, c["topics"] | 0U);
            }
            if (!doc["count"].isNull()) {
                sMqttChannels = doc["count"].as<uint32_t>();
                sMqttChannelsKnown = true;
            }
        }
    }
    http.end();
    // Marked either way: a failed seed must not leave the screen saying
    // "loading" forever, and the census is still correct — just not seeded.
    mqttCensusMarkSeeded();
}

bool linkReady() {
    return gCfg.ingestEnabled
        && gCfg.ingestUrl[0]
        && WiFi.status() == WL_CONNECTED;
}

}  // namespace

void uplinkBegin() {
    // Meshtastic derives a node number from the low 32 bits of the device MAC,
    // and renders the id as "!" plus those bytes in hex. Matching that means the
    // ingestor's heardBy values line up with how this device would appear in any
    // other Meshtastic tooling.
    const uint64_t mac = ESP.getEfuseMac();
    sOurNodeNum = (uint32_t)(mac & 0xFFFFFFFFu);
    snprintf(sOurNodeId, sizeof(sOurNodeId), "!%08lx", (unsigned long)sOurNodeNum);
    Serial.printf("[uplink] reporting as %s\n", sOurNodeId);
}

// Records a packet for reporting. Deliberately keeps encrypted packets: that a
// node transmitted at all, to whom, and how strongly it arrived is an
// observation worth cataloguing even when the payload cannot be read.
void queueMessage(const MeshPacket &pkt) {
    // A packet id of 0 carries no identity, so it cannot be deduplicated and
    // would be re-reported on every rebroadcast. Counted as heard, not stored.
    if (pkt.hdr.id == 0) return;
    if (seenBefore(pkt.hdr.from, pkt.hdr.id)) return;

    MessageObs &m = sMsgQueue[sMsgHead];
    // The ring overwrites the oldest unsent report when full. A monitor on a
    // busy mesh can hear packets faster than it can post them, and dropping the
    // stalest is better than dropping the newest — but it is counted, because
    // silently losing observations is exactly what a monitor must not do.
    if (m.used) sMessagesDropped++;
    sMsgHead = (uint8_t)((sMsgHead + 1) % kMsgQueue);

    m = MessageObs{};
    m.used      = true;
    m.packetId  = pkt.hdr.id;
    m.fromNum   = pkt.hdr.from;
    m.toNum     = pkt.hdr.to;
    m.channel   = pkt.hdr.channel;
    m.encrypted = !pkt.decrypted;
    m.snr       = pkt.snr;
    m.rssi      = (int16_t)lroundf(pkt.rssi);
    m.viaMqtt   = (pkt.hdr.flags & 0x10) != 0;
    m.rxMs      = pkt.rxMs;

    const uint8_t hopStart = (uint8_t)((pkt.hdr.flags >> 5) & 0x07);
    const uint8_t hopLimit = (uint8_t)(pkt.hdr.flags & 0x07);
    if (hopStart >= hopLimit) {
        m.hasHops  = true;
        m.hopsAway = (uint8_t)(hopStart - hopLimit);
    }

    if (!pkt.decrypted) return;

    m.hasPort = true;
    m.portnum = pkt.portnum;

    if (pkt.portnum == TEXT_MESSAGE_APP && pkt.payloadLen > 0) {
        const size_t copy = (pkt.payloadLen < sizeof(m.text) - 1) ? pkt.payloadLen
                                                                 : sizeof(m.text) - 1;
        memcpy(m.text, pkt.payload, copy);
        m.text[copy] = '\0';
        m.hasText = true;
    } else if (pkt.portnum == TELEMETRY_APP) {
        TelemetryInfo t{};
        if (decodeTelemetry(pkt.payload, pkt.payloadLen, t)) {
            m.telemetry = t;
            m.hasTelemetry = t.hasDeviceMetrics || t.hasEnvironmentMetrics;
        }
    }
}

void uplinkNotePacket(const MeshPacket &pkt) {
    const uint32_t from = pkt.hdr.from;
    if (from == 0 || from == kBroadcastNode) return;

    queueMessage(pkt);

    bool created = false;
    NodeObs *n = findOrCreate(from, created);
    if (!n) return;

    bool changed = created;
    n->lastHeardMs = pkt.rxMs;

    // ── Always-available observations, straight off the header ──────────────
    const int16_t rssi = (int16_t)lroundf(pkt.rssi);
    if (!n->hasSignal || n->rssi != rssi || fabsf(n->snr - pkt.snr) > 0.05f) {
        n->snr = pkt.snr;
        n->rssi = rssi;
        n->hasSignal = true;
        // Signal alone does not make a report urgent — it changes on every
        // packet, and treating that as news would mean one POST per packet.
        // It rides along with the next report instead.
    }

    const uint8_t hopStart = (uint8_t)((pkt.hdr.flags >> 5) & 0x07);
    const uint8_t hopLimit = (uint8_t)(pkt.hdr.flags & 0x07);
    if (hopStart >= hopLimit) {
        const uint8_t hops = (uint8_t)(hopStart - hopLimit);
        if (!n->hasHops || n->hopsAway != hops) {
            n->hasHops = true;
            n->hopsAway = hops;
            changed = true;   // a route change is worth reporting
        }
    }

    const bool viaMqtt = (pkt.hdr.flags & 0x10) != 0;
    if (n->viaMqtt != viaMqtt) { n->viaMqtt = viaMqtt; changed = true; }

    // ── Decoded payloads ────────────────────────────────────────────────────
    if (pkt.decrypted) {
        switch (pkt.portnum) {
            case NODEINFO_APP: {
                UserInfo u{};
                if (decodeUser(pkt.payload, pkt.payloadLen, u)) {
                    // Every NODEINFO is reported, whether or not anything in it
                    // differs from what we already hold. It is the one packet
                    // that carries a node's own account of itself, it is sent
                    // rarely, and a re-announcement is itself worth recording —
                    // so this is not folded into the change detection below.
                    changed = true;
                    setStr(n->nodeId, sizeof(n->nodeId), u.id, n->hasNodeId);
                    changed |= setStr(n->longName, sizeof(n->longName), u.longName,
                                      n->hasLongName);
                    changed |= setStr(n->shortName, sizeof(n->shortName), u.shortName,
                                      n->hasShortName);
                    if (u.hasHwModel && (!n->hasHwModel || n->hwModel != u.hwModel)) {
                        n->hwModel = u.hwModel; n->hasHwModel = true; changed = true;
                    }
                    if (u.hasRole && (!n->hasRole || n->role != u.role)) {
                        n->role = u.role; n->hasRole = true; changed = true;
                    }
                }
                break;
            }
            case POSITION_APP: {
                PositionInfo p{};
                if (decodePosition(pkt.payload, pkt.payloadLen, p) && p.hasLatLon) {
                    if (!n->hasLatLon || n->latI != p.latI || n->lonI != p.lonI) {
                        n->latI = p.latI; n->lonI = p.lonI;
                        n->hasLatLon = true; changed = true;
                    }
                    if (p.alt != 0 || n->hasAlt) {
                        if (!n->hasAlt || n->alt != p.alt) {
                            n->alt = p.alt; n->hasAlt = true; changed = true;
                        }
                    }
                    if (p.hasPrecisionBits) {
                        if (!n->hasPrecision || n->precisionBits != p.precisionBits) {
                            n->precisionBits = p.precisionBits;
                            n->hasPrecision = true; changed = true;
                        }
                    }
                }
                break;
            }
            case TELEMETRY_APP: {
                TelemetryInfo t{};
                if (decodeTelemetry(pkt.payload, pkt.payloadLen, t) && t.hasDeviceMetrics) {
                    // battPct above 100 is meaningful here rather than a fault:
                    // Meshtastic uses it for a node running on external power,
                    // and the endpoint accepts up to 255 for that reason.
                    const uint8_t pct = (uint8_t)constrain((int)lroundf(t.battPct), 0, 255);
                    if (!n->hasBatt || n->battLevel != pct) {
                        n->battLevel = pct; n->hasBatt = true; changed = true;
                    }
                    if (t.voltage > 0.0f &&
                        (!n->hasVoltage || fabsf(n->voltage - t.voltage) > 0.01f)) {
                        n->voltage = t.voltage; n->hasVoltage = true; changed = true;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // A node heard again with nothing new still gets re-reported periodically,
    // so the ingestor's "last heard" stays true for a node that is present but
    // quiet — a repeater relaying other people's traffic may never send a
    // packet of its own that decodes to anything.
    const uint32_t sinceReport = millis() - n->lastReportMs;
    if (n->reported && sinceReport >= gCfg.ingestIntervalS * 1000UL) changed = true;

    if (changed) n->pending = true;
}

void uplinkLoop() {
    // Handled before the link check, not after. A seed request that cannot be
    // served still has to be answered — with ingest off or WiFi down it would
    // otherwise stay pending forever, and the screen would sit on "loading"
    // rather than showing what it can actually see.
    if (sMqttSeedRequested && !linkReady()) {
        sMqttSeedRequested = false;
        Serial.println("[uplink] no ingestor link — census starts from what this device hears");
        mqttCensusMarkSeeded();
    }

    if (!linkReady()) return;

    // Ahead of everything else: the screen is open and waiting on it.
    if (sMqttSeedRequested) {
        sMqttSeedRequested = false;
        sLastPostMs = millis();
        fetchMqttSeed();
        return;
    }

    if ((!sBaselineFetched || !sMessagesBaselineFetched || !sMqttChannelsKnown)
        && millis() >= sBaselineNextTryMs) {
        sBaselineNextTryMs = millis() + 10000;
        if (!sBaselineFetched)         fetchBaseline();
        if (!sMessagesBaselineFetched) fetchMessageBaseline();
        if (!sMqttChannelsKnown)       fetchMqttChannelBaseline();
    }

    if (millis() - sLastPostMs < kPostGapMs) return;

    // Unreported nodes first: a newly discovered node is the thing this device
    // exists to surface, and it should not wait behind a queue of refreshes for
    // nodes the ingestor already knows about.
    NodeObs *target = nullptr;
    for (uint16_t i = 0; i < MAX_NODES && !target; i++) {
        NodeObs &n = sNodes[i];
        if (n.nodeNum != 0 && n.pending && !n.reported) target = &n;
    }

    // Then queued messages. Ahead of node refreshes because a message report is
    // perishable — the ring overwrites it — while a node refresh only carries
    // state that is still sitting in the table and will go out next time.
    if (!target) {
        for (uint8_t i = 0; i < kMsgQueue; i++) {
            // Oldest first: sMsgHead points at the next slot to fill, so that is
            // also the oldest occupied one.
            const uint8_t idx = (uint8_t)((sMsgHead + i) % kMsgQueue);
            MessageObs &m = sMsgQueue[idx];
            if (!m.used) continue;

            sLastPostMs = millis();
            sHasAttempted = true;
            if (postMessage(m)) {
                sLastPostOk = true;
                m.used = false;
                sSent++;
            } else {
                sLastPostOk = false;
                sFailed++;
            }
            return;
        }
    }

    // Then the MQTT topic batch, on its own slow clock. Ahead of node refreshes
    // only because it is rate-limited to once a minute and would otherwise be
    // starved on a busy mesh.
    if (!target && millis() >= sNextMqttBatchMs) {
        const bool backlog = mqttPendingTopicCount() >= (kMqttBatchMax / 2);
        sNextMqttBatchMs = millis() + (backlog ? kMqttBatchBusyMs : kMqttBatchIdleMs);
        if (postMqttTopics()) {
            sLastPostMs = millis();
            return;
        }
    }

    // Then refreshes, round-robin from where the last scan stopped so one noisy
    // node cannot starve the rest.
    for (uint16_t step = 0; step < MAX_NODES && !target; step++) {
        NodeObs &n = sNodes[sScanCursor];
        sScanCursor = (uint16_t)((sScanCursor + 1) % MAX_NODES);
        if (n.nodeNum != 0 && n.pending) target = &n;
    }

    if (!target) return;

    sLastPostMs = millis();
    sHasAttempted = true;
    if (postReport(*target)) {
        sLastPostOk = true;
        const bool first = !target->reported;
        target->pending = false;
        target->reported = true;
        target->lastReportMs = millis();
        sSent++;
        if (first) {
            Serial.printf("[uplink] new node %08lx%s%s\n",
                          (unsigned long)target->nodeNum,
                          target->hasLongName ? "  " : "",
                          target->hasLongName ? target->longName : "");
        }
    } else {
        // Left pending so it retries. Nothing is lost by a failed POST — the
        // observation stays in the table and the next attempt carries whatever
        // has been learned since.
        sLastPostOk = false;
        sFailed++;
    }
}

void uplinkRequestMqttSeed() { sMqttSeedRequested = true; }

UplinkState uplinkState() {
    if (!gCfg.ingestEnabled || !gCfg.ingestUrl[0]) return UplinkState::Disabled;
    if (WiFi.status() != WL_CONNECTED)             return UplinkState::NoNetwork;
    if (!sHasAttempted)                            return UplinkState::Idle;
    return sLastPostOk ? UplinkState::Ok : UplinkState::Failing;
}

const char *uplinkOurNodeId()  { return sOurNodeId; }

UplinkStats uplinkStats() {
    UplinkStats s{};
    s.known   = sKnown;
    s.sent    = sSent;
    s.failed  = sFailed;
    s.dropped = sDropped;
    s.ingestorTotalKnown = sBaselineFetched;
    s.ingestorTotal      = sIngestorBaseline + sCreatedSinceBoot;
    s.messagesTotalKnown = sMessagesBaselineFetched;
    s.messagesTotal      = sMessagesBaseline + sMessagesCreated;
    s.messagesDropped    = sMessagesDropped;
    s.mqttChannels       = sMqttChannels;
    s.mqttChannelsKnown  = sMqttChannelsKnown;
    for (uint8_t i = 0; i < kMsgQueue; i++) if (sMsgQueue[i].used) s.messagesQueued++;
    for (uint16_t i = 0; i < MAX_NODES; i++) {
        if (sNodes[i].nodeNum != 0 && sNodes[i].pending) s.pending++;
    }
    return s;
}
