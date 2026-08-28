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

// What this device remembers about a node between reports: that it exists, and
// when it was last described to the ingestor. Nothing else — no name, no
// position, no signal history. Those are properties of a packet, not of the
// roster, and the ingestor is where they belong once a report has carried them.
//
// Keeping it to two words is what lets the roster hold thousands of nodes
// instead of a few hundred. A monitor is an observer: the thing it must not run
// out of room for is *which* nodes it has heard.
struct NodeSeen {
    uint32_t nodeNum;       // 0 marks a free slot
    uint32_t lastReportMs;  // 0 until a report about it has been accepted
};

NodeSeen sRoster[MAX_NODES];

// One report waiting to go out, built from the packet that prompted it. Lives
// only until the POST succeeds, so this is the only place node detail is held —
// and only for the handful of nodes queued at any moment, not for every node
// ever heard.
struct NodeReport {
    bool     used;
    bool     firstSighting;  // nothing about this node has been sent before
    bool     carriesFacts;   // decoded something the node said about itself
    uint32_t nodeNum;
    uint32_t lastHeardMs;    // when the packet behind this report arrived

    char     nodeId[16];   bool hasNodeId;
    char     longName[41]; bool hasLongName;
    char     shortName[9]; bool hasShortName;

    uint8_t  hwModel;  bool hasHwModel;
    uint8_t  role;     bool hasRole;

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
};

// Deep enough to absorb the burst of discoveries a cold start produces while
// the drain trickles them out at one per kPostGapMs. It holds at most one entry
// per node — a second report for a node already queued replaces it rather than
// taking another slot — so this is 32 *nodes* in flight, not 32 packets.
constexpr uint8_t kNodeQueue = 32;

NodeReport sNodeQueue[kNodeQueue];
uint8_t    sNodeHead = 0;

uint16_t sDropped = 0;
uint32_t sSent    = 0;
uint32_t sFailed  = 0;

uint32_t sOurNodeNum = 0;
char     sOurNodeId[12] = "";

uint32_t sLastPostMs = 0;

// Outcome of the most recent attempt, which is what the status icon reflects.
bool sHasAttempted = false;
bool sLastPostOk   = false;

// The ingestor's node total as it last reported it: fetched at boot, then
// replaced by the figure every accepted report carries back. Never derived
// here — other monitors add nodes too, and this device cannot see those.
uint32_t sIngestorTotal       = 0;
bool     sIngestorTotalKnown  = false;
uint32_t sBaselineNextTryMs   = 0;

// Nodes counted as heard: one per report the ingestor answered with
// created:true. See UplinkStats::heard for why the ingestor is the one counting.
uint32_t sNodesHeard          = 0;

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

NodeSeen *rosterFind(uint32_t nodeNum) {
    for (uint16_t i = 0; i < MAX_NODES; i++) {
        if (sRoster[i].nodeNum == nodeNum) return &sRoster[i];
    }
    return nullptr;
}

// Linear over 4000 slots, run once per received packet. At mesh packet rates
// that is nothing, and it keeps the roster a plain array — no buckets to size,
// no rehashing, and a full scan is still only 16 KB of sequential reads.
NodeSeen *rosterAdd(uint32_t nodeNum) {
    for (uint16_t i = 0; i < MAX_NODES; i++) {
        if (sRoster[i].nodeNum != 0) continue;
        sRoster[i] = NodeSeen{nodeNum, 0};
        return &sRoster[i];
    }

    // Deliberately not evicting the oldest: a monitor is meant to be watching a
    // mesh that fits, and rotating nodes out would make them get "discovered"
    // again on their next packet. Counted instead, so a full roster is visible
    // rather than inferred — though at MAX_NODES this should never happen.
    sDropped++;
    return nullptr;
}

// Copies a decoded string into a report, marking the field present. Non-empty
// only: an absent name and an empty one mean the same thing on the wire, and
// the ingestor merges by omission — sending "" would erase what it holds.
void setStr(char *dst, size_t cap, const char *src, bool &has) {
    if (!src || !src[0]) return;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    has = true;
}

// ── Report body ─────────────────────────────────────────────────────────────
void buildReport(const NodeReport &n, String &out) {
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

bool postReport(const NodeReport &n) {
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
        // Both counters come out of the response. `created` says this report is
        // what brought the node into existence, which is what makes it worth
        // counting as heard — a node heard for the first time *here* is very
        // often already stored there. `totalNodes` is the store's own total,
        // taken as given rather than added up locally.
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            if (doc["created"].as<bool>()) sNodesHeard++;
            if (doc["totalNodes"].is<uint32_t>()) {
                sIngestorTotal      = doc["totalNodes"].as<uint32_t>();
                sIngestorTotalKnown = true;
            }
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

// Seeds the total at boot, before any node has been heard, so the screen shows
// the store's figure from the first frame rather than dashes until something
// transmits. Every report after this carries the total back, so this runs once.
void fetchBaseline() {
    char url[192];
    if (!buildApiUrl("/nodes/count", url, sizeof(url))) {
        sIngestorTotalKnown = true;
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
            sIngestorTotal      = doc["count"] | 0U;
            sIngestorTotalKnown = true;
            Serial.printf("[uplink] ingestor holds %lu nodes at boot\n",
                          (unsigned long)sIngestorTotal);
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
                if (name[0]) {
                    mqttCensusSeed(name, c["topics"] | 0U, c["lastSeenAgeSeconds"] | 0xFFFFFFFFUL);
                }
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

// Queues a report, or folds it into the one already waiting for this node.
//
// At most one entry per node is in flight. A node that transmits three times
// while the queue drains should cost one POST carrying its latest state, not
// three carrying successively staler ones — and the newer report is a superset
// whenever it decoded at least as much, since the header observations it
// always carries are the fresher ones.
void queueNodeReport(const NodeReport &r) {
    for (uint8_t i = 0; i < kNodeQueue; i++) {
        NodeReport &q = sNodeQueue[i];
        if (!q.used || q.nodeNum != r.nodeNum) continue;
        // A bare reception must not displace a queued NodeInfo that has not
        // gone out yet — that would throw away the names it was carrying.
        if (r.carriesFacts || !q.carriesFacts) {
            const bool first = q.firstSighting;
            q = r;
            q.firstSighting = first;
        }
        return;
    }

    NodeReport &slot = sNodeQueue[sNodeHead];
    // Overwrites the oldest unsent report when full, and counts it. Nothing is
    // permanently lost: the evicted node's roster entry still says it has never
    // been reported, so its next packet queues it again.
    if (slot.used) sDropped++;
    sNodeHead = (uint8_t)((sNodeHead + 1) % kNodeQueue);
    slot = r;
}

void uplinkNotePacket(const MeshPacket &pkt) {
    const uint32_t from = pkt.hdr.from;
    if (from == 0 || from == kBroadcastNode) return;

    queueMessage(pkt);

    // Built fresh from this packet rather than accumulated across packets. The
    // ingestor upserts by omission, so a report that mentions only what this
    // packet carried cannot erase anything it learned from an earlier one —
    // which is what makes it safe for the device to forget.
    NodeReport r{};
    r.used        = true;
    r.nodeNum     = from;
    r.lastHeardMs = pkt.rxMs;

    // ── Always-available observations, straight off the header ──────────────
    r.snr       = pkt.snr;
    r.rssi      = (int16_t)lroundf(pkt.rssi);
    r.hasSignal = true;

    const uint8_t hopStart = (uint8_t)((pkt.hdr.flags >> 5) & 0x07);
    const uint8_t hopLimit = (uint8_t)(pkt.hdr.flags & 0x07);
    if (hopStart >= hopLimit) {
        r.hopsAway = (uint8_t)(hopStart - hopLimit);
        r.hasHops  = true;
    }
    r.viaMqtt = (pkt.hdr.flags & 0x10) != 0;

    // ── Decoded payloads ────────────────────────────────────────────────────
    // Whether this packet said anything about the node itself. Header
    // observations alone do not count: signal and hop count change on every
    // packet, and treating them as news would mean one POST per packet.

    if (pkt.decrypted) {
        switch (pkt.portnum) {
            case NODEINFO_APP: {
                UserInfo u{};
                if (decodeUser(pkt.payload, pkt.payloadLen, u)) {
                    // Every NodeInfo is reported, whether or not it differs from
                    // what was sent before. It is the one packet carrying a
                    // node's own account of itself, it is sent rarely, and a
                    // re-announcement is itself worth recording.
                    r.carriesFacts = true;
                    setStr(r.nodeId, sizeof(r.nodeId), u.id, r.hasNodeId);
                    setStr(r.longName, sizeof(r.longName), u.longName, r.hasLongName);
                    setStr(r.shortName, sizeof(r.shortName), u.shortName, r.hasShortName);
                    if (u.hasHwModel) { r.hwModel = u.hwModel; r.hasHwModel = true; }
                    if (u.hasRole)    { r.role    = u.role;    r.hasRole    = true; }
                }
                break;
            }
            case POSITION_APP: {
                PositionInfo p{};
                if (decodePosition(pkt.payload, pkt.payloadLen, p) && p.hasLatLon) {
                    r.carriesFacts = true;
                    r.latI = p.latI; r.lonI = p.lonI; r.hasLatLon = true;
                    if (p.alt != 0)       { r.alt = p.alt; r.hasAlt = true; }
                    if (p.hasPrecisionBits) {
                        r.precisionBits = p.precisionBits;
                        r.hasPrecision  = true;
                    }
                }
                break;
            }
            case TELEMETRY_APP: {
                TelemetryInfo t{};
                if (decodeTelemetry(pkt.payload, pkt.payloadLen, t) && t.hasDeviceMetrics) {
                    r.carriesFacts = true;
                    // battPct above 100 is meaningful here rather than a fault:
                    // Meshtastic uses it for a node running on external power,
                    // and the endpoint accepts up to 255 for that reason.
                    r.battLevel = (uint8_t)constrain((int)lroundf(t.battPct), 0, 255);
                    r.hasBatt   = true;
                    if (t.voltage > 0.0f) { r.voltage = t.voltage; r.hasVoltage = true; }
                }
                break;
            }
            default:
                break;
        }
    }

    NodeSeen *seen = rosterFind(from);
    if (!seen) {
        seen = rosterAdd(from);
        if (!seen) return;   // roster full — nothing to report against
    }
    r.firstSighting = (seen->lastReportMs == 0);

    // A node heard again with nothing new still gets re-reported periodically,
    // so the ingestor's "last heard" stays true for a node that is present but
    // quiet — a repeater relaying other people's traffic may never send a
    // packet of its own that decodes to anything.
    const bool refreshDue =
        seen->lastReportMs != 0 &&
        (millis() - seen->lastReportMs) >= gCfg.ingestIntervalS * 1000UL;

    if (!r.carriesFacts && !r.firstSighting && !refreshDue) return;

    queueNodeReport(r);
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

    if ((!sIngestorTotalKnown || !sMessagesBaselineFetched || !sMqttChannelsKnown)
        && millis() >= sBaselineNextTryMs) {
        sBaselineNextTryMs = millis() + 10000;
        if (!sIngestorTotalKnown)      fetchBaseline();
        if (!sMessagesBaselineFetched) fetchMessageBaseline();
        if (!sMqttChannelsKnown)       fetchMqttChannelBaseline();
    }

    if (millis() - sLastPostMs < kPostGapMs) return;

    // Unreported nodes first: a newly discovered node is the thing this device
    // exists to surface, and it should not wait behind a queue of refreshes for
    // nodes the ingestor already knows about.
    NodeReport *target = nullptr;
    for (uint8_t i = 0; i < kNodeQueue && !target; i++) {
        NodeReport &r = sNodeQueue[(sNodeHead + i) % kNodeQueue];
        if (r.used && r.firstSighting) target = &r;
    }

    // Then queued messages. Ahead of node refreshes because a message report is
    // perishable in a way a refresh is not: the next packet from the node
    // rebuilds the refresh, while a message dropped from its ring is gone.
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

    // Then the rest of the ring, oldest first — refreshes, and reports for
    // nodes the ingestor has already been told about.
    for (uint8_t i = 0; i < kNodeQueue && !target; i++) {
        NodeReport &r = sNodeQueue[(sNodeHead + i) % kNodeQueue];
        if (r.used) target = &r;
    }

    if (!target) return;

    sLastPostMs = millis();
    sHasAttempted = true;
    if (postReport(*target)) {
        sLastPostOk = true;
        if (NodeSeen *seen = rosterFind(target->nodeNum)) {
            // Any non-zero value marks the node as reported; it is also what the
            // refresh interval counts from. millis() is only 0 for one tick at
            // boot, but that tick would read as "never reported".
            const uint32_t now = millis();
            seen->lastReportMs = (now != 0) ? now : 1;
        }
        if (target->firstSighting) {
            Serial.printf("[uplink] new node %08lx%s%s\n",
                          (unsigned long)target->nodeNum,
                          target->hasLongName ? "  " : "",
                          target->hasLongName ? target->longName : "");
        }
        *target = NodeReport{};   // the detail existed only to be sent
        sSent++;
    } else {
        // Left queued so it retries, and the roster still says the node has
        // never been reported — so nothing is lost by a failed POST.
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
    s.heard   = sNodesHeard;
    s.sent    = sSent;
    s.failed  = sFailed;
    s.dropped = sDropped;
    s.ingestorTotalKnown = sIngestorTotalKnown;
    s.ingestorTotal      = sIngestorTotal;
    s.messagesTotalKnown = sMessagesBaselineFetched;
    s.messagesTotal      = sMessagesBaseline + sMessagesCreated;
    s.messagesDropped    = sMessagesDropped;
    s.mqttChannels       = sMqttChannels;
    s.mqttChannelsKnown  = sMqttChannelsKnown;
    for (uint8_t i = 0; i < kMsgQueue; i++) if (sMsgQueue[i].used) s.messagesQueued++;
    for (uint8_t i = 0; i < kNodeQueue; i++) if (sNodeQueue[i].used) s.pending++;
    return s;
}
