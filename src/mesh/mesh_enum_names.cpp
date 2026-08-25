// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_enum_names.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "mesh_enum_names.h"
#include <stddef.h>

namespace {

// Config.DeviceConfig.Role. Small, stable, and complete as of Meshtastic 2.7.
const char *const kRoles[] = {
    "CLIENT",          // 0
    "CLIENT_MUTE",     // 1
    "ROUTER",          // 2
    "ROUTER_CLIENT",   // 3  (deprecated upstream, still seen on older nodes)
    "REPEATER",        // 4
    "TRACKER",         // 5
    "SENSOR",          // 6
    "TAK",             // 7
    "CLIENT_HIDDEN",   // 8
    "LOST_AND_FOUND",  // 9
    "TAK_TRACKER",     // 10
    "ROUTER_LATE",     // 11
};

// HardwareModel. Deliberately *not* a dense array indexed by value: the enum is
// sparse in practice, grows with every Meshtastic release, and a dense table
// invites an off-by-one that silently renames every board after the mistake.
// Pairs cost a linear scan of ~80 entries once per newly-identified node, which
// is nothing next to the packet rate.
//
// This table is not exhaustive. It covers the models common enough to actually
// turn up in a mesh, plus the ones this project cares about. Anything missing
// reports no hwModel at all rather than a wrong one — see the header.
struct HwModel { uint8_t value; const char *name; };
const HwModel kHwModels[] = {
    {   0, "UNSET" },
    {   1, "TLORA_V2" },
    {   2, "TLORA_V1" },
    {   3, "TLORA_V2_1_1P6" },
    {   4, "TBEAM" },
    {   5, "HELTEC_V2_0" },
    {   6, "TBEAM_V0P7" },
    {   7, "T_ECHO" },
    {   8, "TLORA_V1_1P3" },
    {   9, "RAK4631" },
    {  10, "HELTEC_V2_1" },
    {  11, "HELTEC_V1" },
    {  12, "LILYGO_TBEAM_S3_CORE" },
    {  13, "RAK11200" },
    {  14, "NANO_G1" },
    {  15, "TLORA_V2_1_1P8" },
    {  16, "TLORA_T3_S3" },
    {  17, "NANO_G1_EXPLORER" },
    {  18, "NANO_G2_ULTRA" },
    {  19, "LORA_TYPE" },
    {  20, "WIPHONE" },
    {  21, "WIO_WM1110" },
    {  22, "RAK2560" },
    {  23, "HELTEC_HRU_3601" },
    {  24, "HELTEC_WIRELESS_BRIDGE" },
    {  25, "STATION_G1" },
    {  26, "RAK11310" },
    {  27, "SENSELORA_RP2040" },
    {  28, "SENSELORA_S3" },
    {  29, "CANARYONE" },
    {  30, "RP2040_LORA" },
    {  31, "STATION_G2" },
    {  32, "LORA_RELAY_V1" },
    {  33, "NRF52840DK" },
    {  34, "PPR" },
    {  35, "GENIEBLOCKS" },
    {  36, "NRF52_UNKNOWN" },
    {  37, "PORTDUINO" },
    {  38, "ANDROID_SIM" },
    {  39, "DIY_V1" },
    {  40, "NRF52840_PCA10059" },
    {  41, "DR_DEV" },
    {  42, "M5STACK" },
    {  43, "HELTEC_V3" },
    {  44, "HELTEC_WSL_V3" },
    {  45, "BETAFPV_2400_TX" },
    {  46, "BETAFPV_900_NANO_TX" },
    {  47, "RPI_PICO" },
    {  48, "HELTEC_WIRELESS_TRACKER" },
    {  49, "HELTEC_WIRELESS_PAPER" },
    {  50, "T_DECK" },
    {  51, "T_WATCH_S3" },
    {  52, "PICOMPUTER_S3" },
    {  53, "HELTEC_HT62" },
    {  54, "EBYTE_ESP32_S3" },
    {  55, "ESP32_S3_PICO" },
    {  56, "CHATTER_2" },
    {  57, "HELTEC_WIRELESS_PAPER_V1_0" },
    {  58, "HELTEC_WIRELESS_TRACKER_V1_0" },
    {  59, "UNPHONE" },
    {  60, "TD_LORAC" },
    {  61, "CDEBYTE_EORA_S3" },
    {  62, "TWC_MESH_V4" },
    {  63, "NRF52_PROMICRO_DIY" },
    {  64, "RADIOMASTER_900_BANDIT_NANO" },
    {  65, "HELTEC_CAPSULE_SENSOR_V3" },
    {  66, "HELTEC_VISION_MASTER_T190" },
    {  67, "HELTEC_VISION_MASTER_E213" },
    {  68, "HELTEC_VISION_MASTER_E290" },
    {  69, "HELTEC_MESH_NODE_T114" },
    {  70, "SENSECAP_INDICATOR" },
    {  71, "TRACKER_T1000_E" },
    {  72, "RAK3172" },
    {  73, "WIO_E5" },
    {  74, "RADIOMASTER_900_BANDIT" },
    {  75, "ME25LS01_4Y10TD" },
    {  76, "RP2040_FEATHER_RFM95" },
    {  77, "M5STACK_COREBASIC" },
    {  78, "M5STACK_CORE2" },
    {  79, "RPI_PICO2" },
    {  80, "M5STACK_CORES3" },
    {  81, "SEEED_XIAO_S3" },
    {  82, "MS24SF1" },
    {  83, "TLORA_C6" },
    // The board this monitor runs on. camillia-mt pins the same value.
    { 110, "HELTEC_V4" },
    { 255, "PRIVATE_HW" },
};

}  // namespace

const char *meshRoleName(uint8_t value) {
    if (value >= (sizeof(kRoles) / sizeof(kRoles[0]))) return nullptr;
    return kRoles[value];
}

const char *meshHwModelName(uint8_t value) {
    for (size_t i = 0; i < sizeof(kHwModels) / sizeof(kHwModels[0]); i++) {
        if (kHwModels[i].value == value) return kHwModels[i].name;
    }
    return nullptr;
}
