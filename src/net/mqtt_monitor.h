#pragma once
// ════════════════════════════════════════════════════════════════════════════
// net/mqtt_monitor.h — a live census of what is arriving on an MQTT broker
//
// Subscribe-only, and deliberately not a bridge: camillia-mt's mqtt_bridge
// mirrors traffic in both directions and re-injects downlink onto LoRa. This
// only ever listens, and it does not decode envelopes at all — topics are
// counted and payloads dropped. Nothing here feeds the node or message tables.
//
// Rows are the channel, not the whole topic. A Meshtastic envelope topic is
// "<root>/2/e/<channel>/<gateway>", so counting whole topics would give one row
// per gateway per channel — three gateways on one channel are three rows saying
// the same thing. The gateway is dropped and the counts merge:
//     msh/US/MI/2/e/KAM-NET/!699c90c8  ┐
//     msh/US/MI/2/e/KAM-NET/!699c9234  ┴─→  KAM-NET   2
//     msh/US/MI/2/e/CFW/!b2a77a48      ───→  CFW      1
//
// Bounded on both axes, so the cost of leaving the screen up does not depend on
// how long it is left up: the table is capacity-capped (channels past the cap
// are counted in bulk as "other") and every counter saturates instead of
// wrapping.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>

// Room for a channel name. Meshtastic channel ids run well under this; the cap
// only bites on a topic that did not carry the expected prefix, which is kept
// whole and truncated so it stays recognisable.
#define MQTT_MON_CHANNEL_MAX 32
#define MQTT_MON_SLOTS       32

struct MqttChannelStat {
    // Distinct topics on this channel, not messages. The store keeps topics and
    // has no per-message counts, so seeding from it and then adding messages
    // would produce a number that means neither thing.
    uint32_t topics;
    // millis() when this channel was last active. Seeded rows get a synthetic
    // stamp derived from the age the ingestor reported, so a channel last heard
    // before the screen opened still shows its true recency.
    uint32_t lastHeardMs;
    char     channel[MQTT_MON_CHANNEL_MAX];
};

// Saturation ceiling. Past any plausible session, and low enough that the
// formatted number always fits its column.
static constexpr uint32_t kMqttCountMax = 999999UL;

// Applies config and (re)starts the connection state machine. Call at boot and
// whenever MQTT settings change.
void mqttBegin();

// Pumps the connection and services the client. Call every loop; never blocks
// for longer than the socket timeout.
void mqttLoop();

bool mqttConnected();

// Why nothing is arriving, or nullptr when the session is healthy.
const char *mqttBlockedReason();

// ── Census ──────────────────────────────────────────────────────────────────
// Counting runs only while the monitor screen is open, so a closed screen costs
// nothing but the connection. Starting clears the previous session's counts.
void     mqttCensusStart();

// Seeds a channel with the topic count the ingestor already holds. Called for
// each channel when the screen opens, before local observations are added, so
// the screen shows the whole picture rather than only this session's share.
void     mqttCensusSeed(const char *channel, uint32_t topics, uint32_t ageSeconds);

// True once seeding has been attempted, whether or not it found anything — the
// screen uses it to tell "nothing recorded" apart from "still loading".
bool     mqttCensusSeeded();
void     mqttCensusMarkSeeded();
void     mqttCensusStop();
uint32_t mqttCensusStartedMs();

int  mqttChannelCount();
const MqttChannelStat *mqttChannelAt(int index);

uint32_t mqttTotalMessages();

// Messages on a channel past the table's capacity. Non-zero means the list on
// screen is a partial view, and the screen says so.
uint32_t mqttOtherMessages();
// Bumped when a channel is *added*, so a screen can skip the rebuild and just
// retally when only the numbers moved.
uint32_t mqttChannelSeq();

// ── Topic registry ──────────────────────────────────────────────────────────
// Distinct full topics seen since boot, held for batch reporting to the
// ingestor. Separate from the channel census above, and for a different reason:
// the census collapses gateways so the screen stays readable, while the
// ingestor wants the topic exactly as it appeared on the wire.
//
// This runs whenever the broker session is up, not only while the screen is
// open.
//
// Split into two structures on purpose. The first version kept 64 full topic
// strings and used them for both jobs at once, which failed badly in practice:
// a topic is channel x gateway, and one busy channel has far more gateways
// bridging to MQTT than that. LongFast filled all 64 slots within seconds and
// every other channel was dropped, so the store only ever learned about one
// channel.
//
// Now the two jobs are sized independently:
//   * remembering what has been reported — a ring of 32-bit hashes, cheap
//     enough to hold many, and a wrap only risks re-reporting a topic, which
//     the endpoint treats as idempotent.
//   * holding what is waiting to go out — full strings, but only for topics
//     not yet sent, so the queue drains instead of filling permanently.
#define MQTT_TOPIC_MAX      80
#define MQTT_REPORTED_SLOTS 512
#define MQTT_PENDING_SLOTS  32

// Topics waiting to go out.
int mqttPendingTopicCount();

// Topics discarded because the pending queue was full when they arrived. The
// broker outran the batch interval; the count makes that visible rather than
// leaving the store quietly incomplete.
uint32_t mqttTopicsMissed();

// Fills `topics` with up to `max` unreported topics and `indices` with their
// slot numbers. Returns how many were written. The caller posts them, then
// confirms with mqttMarkTopicsReported() — a batch that fails is simply not
// confirmed and is offered again next time.
int mqttCollectUnreported(const char **topics, int *indices, int max);
void mqttMarkTopicsReported(const int *indices, int count);
