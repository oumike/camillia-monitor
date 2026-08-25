#pragma once
// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_radio.h — SX1262 receiver for the Camillia Monitor
//
// Trimmed from camillia-mt's MeshRadio. Differences, all deliberate:
//   • No transmit path at all. This node is a passive listener; the only way to
//     stay off the air reliably is not to compile the code that keys the PA.
//   • No LR11x0 branches — this board is an SX1262 and only an SX1262.
//   • Airtime accounting keeps only the "channel busy" half. With no TX there
//     is no air_util_tx to report, and channel utilisation is a headline
//     statistic for a monitor rather than a telemetry side-effect.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>
// RadioLib raises an unconditional #warning when ARDUINO_USB_CDC_ON_BOOT=1
// (RadioLib.h:67), advising a hardware UART for debug output. USB CDC *is* the
// console on this board, so the advice cannot be taken; it is suppressed via
// build_src_flags in platformio.ini, because #warning is emitted during
// preprocessing where `#pragma GCC diagnostic` does not reach it.
#include <RadioLib.h>
#include "hardware.h"   // LORA_* pin map (-Isrc/hal)
#include "mesh_proto.h"

// What the receiver reported, counted since boot. A packet can go missing in
// three distinguishable places, and collapsing them into one silent `return
// false` throws away the only evidence that tells them apart: arriving damaged
// (RF — switch, gain, clock), not arriving at all (RF — sensitivity, sync), or
// arriving intact and being dropped above the radio (crypto, dedup). For a
// device whose entire purpose is reception, these are first-class output.
struct RadioRxHealth {
    uint32_t irqs;      // DIO1 assertions serviced
    uint32_t good;      // packets handed up
    uint32_t crcErr;    // arrived, failed CRC — heard but damaged
    uint32_t badLen;    // IRQ with a length no packet can have
    uint32_t readErr;   // the buffer read itself failed
};

class MeshRadio {
public:
    // Brings up SPI, the FEM, and the SX1262, then parks in continuous receive.
    bool init();

    // Retune the listener. This is the one control a monitor really needs: the
    // operator points it at whatever the local mesh is running.
    bool reconfigure(float freq, float bw, uint8_t sf, uint8_t cr);

    // Called from loop() — returns true and fills pkt when a packet is ready.
    bool pollRx(MeshPacket &pkt);

    bool isReady() const { return _ready; }

    // Live modem settings, for display and for the observation uplink.
    float freqMhz() const { return _freq; }
    float bwKhz()   const { return _bw; }
    uint8_t sf()    const { return _sf; }
    uint8_t cr()    const { return _cr; }

    // Percentage of the last hour the channel was busy with traffic we heard.
    // Note this is strictly lower than a transmitting node's channel_utilization
    // telemetry: it cannot include our own airtime, because there isn't any.
    float channelUtilPercent();

    // Highest channel utilisation seen since boot. The rolling hour window
    // means the live figure alone hides a burst that has since aged out — a
    // monitor should be able to say how busy the channel got, not just how busy
    // it is now.
    float channelUtilMaxPercent();

    // Force the next pollRx() to read the radio even if the DIO1 edge ISR did
    // not run (e.g. after waking on a DIO1 level).
    void wakeRxCheck() { _rxFlag = true; }

    static RadioRxHealth rxHealth();
    static void logRxHealth(const char *why);

private:
    int _armRx();

    bool    _ready = false;
    float   _freq = MESH_FREQ;
    float   _bw   = MESH_BW;
    uint8_t _sf   = MESH_SF;
    uint8_t _cr   = MESH_CR;

    SX1262  _radio{new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY)};

    static void IRAM_ATTR _onDio1();
    static volatile bool  _rxFlag;
};

extern MeshRadio Radio;
