#pragma once
// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_config.h — Mesh-layer constants for the Camillia Monitor
//
// The subset of camillia-mt's config.h that the receive path actually reads.
// TX-side settings (power, hop budget, broadcast intervals, position sharing)
// are absent by design: this node never keys the transmitter.
// ════════════════════════════════════════════════════════════════════════════
#include <stdint.h>

// ── Default LoRa modem settings — Meshtastic US LongFast ────────────────────
// These are the compile-time defaults only. The operator can retune the
// listener at runtime (region + preset, or fully custom BW/SF/CR) through the
// config portal, which is the whole point of a monitor: you point it at
// whatever the local mesh is actually running.
#define MESH_FREQ       906.875f  // MHz — US LongFast slot
#define MESH_BW         250.0f    // kHz
#define MESH_SF         11
#define MESH_CR         5         // 4/5 coding rate
#define MESH_SYNC       0x2B      // Meshtastic sync word (public LoRa)
#define MESH_PREAMBLE   16

// Receiver LNA boost. Costs a little idle current for a few dB of sensitivity —
// always on here, because hearing weak/distant nodes is this device's only job.
#define MY_LORA_RX_BOOST 1

// ── Channels ────────────────────────────────────────────────────────────────
// Decryption key slots. Slot 0 is the Meshtastic default public channel; the
// rest can be filled from imported channel URLs so the monitor can also read
// private channels the operator holds keys for.
#define MESH_CHANNELS    8
#define MAX_CHANNELS     MESH_CHANNELS

// ── Observation limits ──────────────────────────────────────────────────────
// How many distinct nodes this device will remember having reported. A monitor
// sees far more nodes than a client does — it is deliberately listening to
// everything, including traffic relayed from meshes it is not part of — so the
// 250 a Meshtastic client keeps is the wrong ceiling here.
//
// Affordable because the roster holds identity and nothing else: 8 bytes per
// node, so 4000 of them cost 32 KB — the same as the 250-entry table of full
// node state this replaced. Everything a report says about a node is built
// from the packet that prompted it and discarded once it has been sent.
#define MAX_NODES              4000

// Longest text payload retained from a decoded TEXT_MESSAGE_APP packet.
#define MESH_TEXT_MAX_LEN      200

// Meshtastic HardwareModel for this board, reported alongside our own
// observations so the ingestor knows what kind of device is watching.
#define MESH_HW_MODEL_HELTEC_V4  110
#define MY_HW_MODEL              MESH_HW_MODEL_HELTEC_V4
