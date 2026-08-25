// ════════════════════════════════════════════════════════════════════════════
// net/mqtt_monitor.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "mqtt_monitor.h"
#include "monitor_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

namespace {

WiFiClient       sPlain;
WiFiClientSecure sSecure;
PubSubClient     sClient;

bool     sBegun         = false;
uint32_t sNextAttemptMs = 0;
char     sClientId[24]  = "";

// Long enough that a broker that is simply down does not have this device
// retrying in a tight loop, short enough that a transient failure recovers
// without anyone noticing.
constexpr uint32_t kRetryIntervalMs = 8000;

// PubSubClient's default is 256 bytes and it silently drops anything larger.
// Meshtastic envelopes routinely exceed that, and a dropped message is a
// message this census never counts — the whole point of the screen.
constexpr uint16_t kMqttBufferBytes = 1024;

// ── Census ──────────────────────────────────────────────────────────────────
MqttChannelStat sTable[MQTT_MON_SLOTS];
uint32_t        sHashes[MQTT_MON_SLOTS];
int             sCount     = 0;
uint32_t        sTotal     = 0;
uint32_t        sOther     = 0;
uint32_t        sSeq       = 0;
bool            sActive    = false;
uint32_t        sStartedMs = 0;
bool            sSeeded    = false;

void bump(uint32_t &counter) {
    if (counter < kMqttCountMax) counter++;
}

uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261UL;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619UL;
    return h;
}

// "<root>/2/e/<channel>/<gateway>" reduced to "<channel>".
//
// Anchored on the "/2/e/" marker rather than on the configured root string:
// "/2/e/" is the structural part of the topic and cannot move, whereas matching
// the root by text would key every message on the wrong segment the moment the
// root carried a stray trailing slash or was edited while the screen was up.
void topicKey(const char *topic, char *out, size_t outLen) {
    const char *start = strstr(topic, "/2/e/");
    start = start ? start + 5 : topic;

    size_t n = 0;
    while (start[n] && start[n] != '/' && n + 1 < outLen) n++;
    memcpy(out, start, n);
    out[n] = '\0';
}

// `isNewTopic` is decided by the topic registry, which spans the whole boot
// rather than this screen session — so a topic first heard while the screen was
// closed does not get counted again on reopen. The seed already accounts for it.
void record(const char *topic, bool isNewTopic) {
    if (!sActive || !topic || !topic[0]) return;
    bump(sTotal);
    if (!isNewTopic) return;

    // Nothing is added to the channel table until the ingestor's figures have
    // been applied. Otherwise a channel heard in the second between the screen
    // opening and the seed arriving would appear with a count of 1, then jump
    // to its real total — which reads as a glitch rather than as loading.
    //
    // Messages heard in that window still count toward the total above, and
    // their topics are still queued for reporting; only the per-channel rows
    // wait.
    if (!sSeeded) return;

    char key[MQTT_MON_CHANNEL_MAX];
    topicKey(topic, key, sizeof(key));
    if (!key[0]) return;

    const uint32_t h = fnv1a(key);

    // Hash first, strcmp only on a hit: a busy broker pushes hundreds of
    // messages a second through here, on the main loop.
    for (int i = 0; i < sCount; i++) {
        if (sHashes[i] != h) continue;
        if (strcmp(sTable[i].channel, key) != 0) continue;
        bump(sTable[i].topics);
        return;
    }

    if (sCount >= MQTT_MON_SLOTS) {
        // Table full. New channels are counted in bulk rather than evicting a
        // row someone is watching — a list that reshuffles itself is not
        // readable, and the screen reports the off-list total separately.
        bump(sOther);
        return;
    }

    memcpy(sTable[sCount].channel, key, sizeof(key));
    sTable[sCount].topics = 1;
    sHashes[sCount] = h;
    sCount++;
    sSeq++;
}

// ── Topic registry ──────────────────────────────────────────────────────────
// Hashes of topics already accepted by the ingestor. Hash-only because the
// string is not needed once it has been sent — only the ability to recognise it
// again — and 4 bytes per topic is what lets this hold enough of them to cover
// a busy channel's whole gateway population.
uint32_t sReported[MQTT_REPORTED_SLOTS];
uint16_t sReportedCount  = 0;
uint16_t sReportedCursor = 0;

// Topics seen but not yet sent. Full strings, because these still have to be
// put in a request body.
struct PendingTopic {
    char     topic[MQTT_TOPIC_MAX];
    uint32_t hash;
    bool     used;
};
PendingTopic sPending[MQTT_PENDING_SLOTS];
uint32_t     sTopicsMissed = 0;

bool isReported(uint32_t h) {
    for (uint16_t i = 0; i < sReportedCount; i++) if (sReported[i] == h) return true;
    return false;
}

bool isPending(uint32_t h) {
    for (int i = 0; i < MQTT_PENDING_SLOTS; i++) {
        if (sPending[i].used && sPending[i].hash == h) return true;
    }
    return false;
}

void markReported(uint32_t h) {
    if (sReportedCount < MQTT_REPORTED_SLOTS) {
        sReported[sReportedCount++] = h;
        return;
    }
    // Full: overwrite oldest. Losing the memory of a topic only risks sending
    // it a second time, which the endpoint absorbs as a last-seen refresh.
    sReported[sReportedCursor] = h;
    sReportedCursor = (uint16_t)((sReportedCursor + 1) % MQTT_REPORTED_SLOTS);
}

// Returns true when the topic had not been seen before.
bool recordTopic(const char *topic) {
    const uint32_t h = fnv1a(topic);
    if (isReported(h) || isPending(h)) return false;

    for (int i = 0; i < MQTT_PENDING_SLOTS; i++) {
        if (sPending[i].used) continue;
        strncpy(sPending[i].topic, topic, MQTT_TOPIC_MAX - 1);
        sPending[i].topic[MQTT_TOPIC_MAX - 1] = '\0';
        sPending[i].hash = h;
        sPending[i].used = true;
        return true;
    }

    // Queue full: the broker is producing new topics faster than the batch
    // interval drains them. Counted rather than dropped silently, and still
    // reported as new so the on-screen census stays honest.
    sTopicsMissed++;
    return true;
}

void onMessage(char *topic, uint8_t *payload, unsigned int len) {
    (void)payload;
    (void)len;
    // Payloads are never decoded or kept. This is a census, and the node and
    // message tables are fed from LoRa only.
    const bool isNew = (topic && topic[0]) ? recordTopic(topic) : false;
    record(topic, isNew);
}

bool credentialsPresent() {
    return gCfg.mqttEnabled && gCfg.mqttServer[0] && gCfg.mqttRoot[0];
}

void attemptConnect() {
    if (!sClientId[0]) {
        snprintf(sClientId, sizeof(sClientId), "camillia-%08lx",
                 (unsigned long)(ESP.getEfuseMac() & 0xFFFFFFFFu));
    }

    Client &transport = gCfg.mqttTls ? (Client &)sSecure : (Client &)sPlain;
    if (gCfg.mqttTls) {
        // No certificate pinning. This device only ever subscribes to public
        // mesh traffic, so TLS here buys transport privacy from the local
        // network rather than authentication of the broker — and shipping a CA
        // bundle for an arbitrary user-chosen host is not something the config
        // form can meaningfully collect.
        sSecure.setInsecure();
    }

    sClient.setClient(transport);
    sClient.setServer(gCfg.mqttServer, gCfg.mqttPort ? gCfg.mqttPort : 1883);
    sClient.setBufferSize(kMqttBufferBytes);
    // Bounds how long a dead broker can hold the main loop, and with it how long
    // the radio goes undrained.
    sClient.setSocketTimeout(2);
    sClient.setKeepAlive(30);
    sClient.setCallback(onMessage);

    const bool ok = gCfg.mqttUser[0]
        ? sClient.connect(sClientId, gCfg.mqttUser, gCfg.mqttPass)
        : sClient.connect(sClientId);

    if (!ok) {
        Serial.printf("[mqtt] connect to %s:%u failed (state %d)\n",
                      gCfg.mqttServer, gCfg.mqttPort, sClient.state());
        return;
    }

    char topic[80];
    snprintf(topic, sizeof(topic), "%s/2/e/#", gCfg.mqttRoot);
    if (sClient.subscribe(topic)) {
        Serial.printf("[mqtt] connected to %s, watching %s\n", gCfg.mqttServer, topic);
    } else {
        Serial.printf("[mqtt] subscribe to %s failed\n", topic);
    }
}

}  // namespace

void mqttBegin() {
    // Drop any existing session so the next loop reconnects with new settings.
    if (sClient.connected()) sClient.disconnect();
    sBegun = true;
    sNextAttemptMs = 0;
}

void mqttLoop() {
    if (!sBegun || !credentialsPresent()) return;
    if (WiFi.status() != WL_CONNECTED) return;

    if (!sClient.connected()) {
        if (millis() < sNextAttemptMs) return;
        sNextAttemptMs = millis() + kRetryIntervalMs;
        attemptConnect();
        return;
    }
    sClient.loop();
}

bool mqttConnected() { return sClient.connected(); }

const char *mqttBlockedReason() {
    if (!gCfg.mqttEnabled)             return "MQTT is off";
    if (!gCfg.mqttServer[0])           return "No broker configured";
    if (!gCfg.mqttRoot[0])             return "No topic root configured";
    if (WiFi.status() != WL_CONNECTED) return "WiFi not connected";
    if (!sClient.connected())          return "Connecting to broker";
    return nullptr;
}

void mqttCensusStart() {
    memset(sTable, 0, sizeof(sTable));
    memset(sHashes, 0, sizeof(sHashes));
    sCount = 0;
    sTotal = 0;
    sOther = 0;
    sSeq++;
    sActive = true;
    sSeeded = false;
    sStartedMs = millis();
}

void mqttCensusSeed(const char *channel, uint32_t topics) {
    if (!channel || !channel[0] || sCount >= MQTT_MON_SLOTS) return;

    const uint32_t h = fnv1a(channel);
    for (int i = 0; i < sCount; i++) {
        if (sHashes[i] == h && strcmp(sTable[i].channel, channel) == 0) {
            // Only reachable if the response listed a channel twice. Local
            // observations cannot have created this row: record() adds nothing
            // until seeding has finished. Takes the larger of the two so a
            // repeated entry cannot lower a figure already applied.
            if (topics > sTable[i].topics) sTable[i].topics = topics;
            return;
        }
    }

    strncpy(sTable[sCount].channel, channel, MQTT_MON_CHANNEL_MAX - 1);
    sTable[sCount].channel[MQTT_MON_CHANNEL_MAX - 1] = '\0';
    sTable[sCount].topics = topics;
    sHashes[sCount] = h;
    sCount++;
    sSeq++;
}

bool mqttCensusSeeded()     { return sSeeded; }
void mqttCensusMarkSeeded() { sSeeded = true; }

void mqttCensusStop() { sActive = false; }
uint32_t mqttCensusStartedMs() { return sStartedMs; }

int mqttChannelCount() { return sCount; }

const MqttChannelStat *mqttChannelAt(int index) {
    if (index < 0 || index >= sCount) return nullptr;
    return &sTable[index];
}

uint32_t mqttTotalMessages() { return sTotal; }
uint32_t mqttOtherMessages() { return sOther; }
uint32_t mqttChannelSeq()    { return sSeq; }


int mqttPendingTopicCount() {
    int n = 0;
    for (int i = 0; i < MQTT_PENDING_SLOTS; i++) if (sPending[i].used) n++;
    return n;
}

uint32_t mqttTopicsMissed() { return sTopicsMissed; }

int mqttCollectUnreported(const char **topics, int *indices, int max) {
    int n = 0;
    for (int i = 0; i < MQTT_PENDING_SLOTS && n < max; i++) {
        if (!sPending[i].used) continue;
        topics[n] = sPending[i].topic;
        indices[n] = i;
        n++;
    }
    return n;
}

void mqttMarkTopicsReported(const int *indices, int count) {
    // Only on a confirmed batch: the slot is freed and the hash remembered, so
    // the topic is neither re-sent nor re-queued.
    for (int i = 0; i < count; i++) {
        const int idx = indices[i];
        if (idx < 0 || idx >= MQTT_PENDING_SLOTS || !sPending[idx].used) continue;
        markReported(sPending[idx].hash);
        sPending[idx].used = false;
    }
}
