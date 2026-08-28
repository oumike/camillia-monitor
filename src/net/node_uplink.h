#pragma once
// ════════════════════════════════════════════════════════════════════════════
// net/node_uplink.h — reports heard nodes to the camillia-monitor-ingestor
//
// POSTs to the ingestor's /api/nodes/heard endpoint, which upserts by node
// number: fields left out of a report keep their previously known values. That
// shapes the whole design here — every field is sent only when it was actually
// observed, so a bare relay sighting cannot erase a name learned an hour ago.
//
// Reports are queued and drained one at a time from loop(). A POST is blocking
// and the SX1262 holds exactly one packet, so sending a backlog in one burst
// would deafen the monitor for as long as the burst lasted.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include "mesh_proto.h"

// Derives this device's own node number and id (used as the `heardBy` field).
void uplinkBegin();

// Queues a report about the packet's sender when the packet is worth reporting:
// the node is new, the packet decoded to something the node said about itself,
// or the refresh interval has passed since it was last described.
//
// The report is built from this packet alone. Between reports the device keeps
// only the node's number and when it was last reported — the ingestor merges by
// omission, so nothing is lost by forgetting the detail once it has been sent.
void uplinkNotePacket(const MeshPacket &pkt);

// Drains at most one queued report per call. Safe to call when WiFi is down or
// the uplink is disabled; it does nothing.
void uplinkLoop();

// This device's own identity, as reported in `heardBy`.
const char *uplinkOurNodeId();

// What the endpoint connection is actually doing right now. Totals alone
// cannot answer this: a device that posted a thousand reports and then lost the
// endpoint an hour ago still shows a large `sent` and a small `failed`.
enum class UplinkState : uint8_t {
    Disabled,    // switched off, or no endpoint URL configured
    NoNetwork,   // enabled, but WiFi is not associated
    Idle,        // ready, nothing sent yet this boot
    Ok,          // last POST succeeded
    Failing,     // last POST failed
};
UplinkState uplinkState();

struct UplinkStats {
    // Nodes counted as heard since boot. The ingestor decides this, not us: a
    // node is counted when the report announcing it is the one that brought it
    // into the store. So this is what this device added to the mesh's picture,
    // not how many distinct senders its radio saw — those differ whenever
    // another monitor got there first.
    uint16_t heard;
    uint16_t pending;  // reports waiting to be sent
    uint32_t sent;     // successful POSTs
    uint32_t failed;   // POSTs that did not return 2xx

    // Node reports lost before they could be sent: evicted from a full queue,
    // or — which should not happen at MAX_NODES — a node the roster had no room
    // for. An eviction is not permanent: the node is still marked unreported,
    // so its next packet queues it again.
    uint16_t dropped;

    // What the ingestor holds, as the ingestor last said it. Fetched once at
    // boot and then replaced by the total every report comes back with, so the
    // figure follows the store rather than approximating it — a locally
    // maintained total would go stale the moment a second monitor stored a node.
    uint32_t ingestorTotal;
    bool     ingestorTotalKnown;   // false until the first count arrives

    // Distinct LoRa packets the ingestor holds. "Distinct" matters: a mesh
    // rebroadcasts heavily, so counting receptions would report several times
    // the number of things actually said.
    uint32_t messagesTotal;
    bool     messagesTotalKnown;
    uint16_t messagesQueued;
    uint16_t messagesDropped;      // queued reports lost to a full ring

    // Distinct MQTT channels the ingestor holds. Server-side rather than
    // counted here: the store aggregates every monitor that has ever reported,
    // and this device only knows what it heard itself since boot.
    uint32_t mqttChannels;
    bool     mqttChannelsKnown;
};
UplinkStats uplinkStats();

// Asks for the ingestor's per-channel topic counts to be fetched and pushed
// into the MQTT census. Queued rather than performed inline: this is called
// from a touch handler, and an HTTP round trip there would stall the radio for
// as long as the broker took to answer.
void uplinkRequestMqttSeed();
