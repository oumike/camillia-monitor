#pragma once
// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_enum_names.h — Meshtastic enum value -> name
//
// The ingestor's heard-node endpoint takes hwModel and role as enum *names*
// ("HELTEC_V3", "ROUTER"), while the wire carries their numeric values. These
// map one to the other.
//
// Both return nullptr for a value not in the table, and callers omit the field
// rather than substituting a placeholder. That matters more than it looks: the
// endpoint upserts, and a field left out keeps whatever was known before, so
// omitting is free — whereas sending "UNKNOWN_87" writes a fact that is not one
// and would have to be cleaned up later.
// ════════════════════════════════════════════════════════════════════════════
#include <stdint.h>

// HardwareModel enum name, or nullptr if this firmware does not know the value.
const char *meshHwModelName(uint8_t value);

// Config.DeviceConfig.Role enum name, or nullptr if unknown.
const char *meshRoleName(uint8_t value);
