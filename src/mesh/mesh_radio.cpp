// ════════════════════════════════════════════════════════════════════════════
// mesh/mesh_radio.cpp — SX1262 receive path
// ════════════════════════════════════════════════════════════════════════════
#include "mesh_radio.h"
#include <SPI.h>

volatile bool MeshRadio::_rxFlag = false;
MeshRadio Radio;

namespace {
constexpr bool kVerboseRadioIo = false;

RadioRxHealth sRx = {};
float sChUtilMax = 0.0f;

// ── Airtime accounting (rolling 1-hour window, one bucket per minute) ───────
struct AirBucket { uint32_t minute; uint32_t busyMs; };
constexpr int kAirBuckets = 60;
AirBucket sAir[kAirBuckets] = {};

void airNote(uint32_t toaMs) {
    if (toaMs == 0) return;
    uint32_t m = millis() / 60000UL;
    AirBucket &b = sAir[m % kAirBuckets];
    if (b.minute != m) { b.minute = m; b.busyMs = 0; }
    b.busyMs += toaMs;
}

float airPct() {
    uint32_t m = millis() / 60000UL;
    uint64_t sum = 0;
    for (int i = 0; i < kAirBuckets; i++) {
        // Only count buckets that fall inside the last hour.
        if ((uint32_t)(m - sAir[i].minute) < (uint32_t)kAirBuckets) sum += sAir[i].busyMs;
    }
    float pct = (float)((double)sum * 100.0 / 3600000.0);  // window = 1 hour in ms
    return (pct > 100.0f) ? 100.0f : pct;
}
}  // namespace

void IRAM_ATTR MeshRadio::_onDio1() { _rxFlag = true; }

RadioRxHealth MeshRadio::rxHealth() { return sRx; }

void MeshRadio::logRxHealth(const char *why) {
    Serial.printf("[radio] rx health (%s): irq=%lu ok=%lu crcErr=%lu badLen=%lu readErr=%lu\n",
                  why ? why : "-",
                  (unsigned long)sRx.irqs, (unsigned long)sRx.good,
                  (unsigned long)sRx.crcErr, (unsigned long)sRx.badLen,
                  (unsigned long)sRx.readErr);
}

int MeshRadio::_armRx() { return _radio.startReceive(); }

bool MeshRadio::init() {
    SPI.begin(LORA_SPI_SCK, LORA_SPI_MISO, LORA_SPI_MOSI);

    Serial.printf("[radio] pins: sck=%d miso=%d mosi=%d cs=%d dio1=%d rst=%d busy=%d\n",
                  LORA_SPI_SCK, LORA_SPI_MISO, LORA_SPI_MOSI,
                  LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

    // ── FEM (front-end module) ──────────────────────────────────────────────
    // Power and RF-switch enable are unambiguous: both HIGH, or the radio hears
    // nothing at all.
    pinMode(LORA_FEM_POWER_PIN, OUTPUT);
    digitalWrite(LORA_FEM_POWER_PIN, HIGH);
    pinMode(LORA_FEM_ENABLE_PIN, OUTPUT);
    digitalWrite(LORA_FEM_ENABLE_PIN, HIGH);

    // LORA_FEM_TX_MODE_PIN is driven HIGH and left there, which reads wrong for
    // a receiver: the board header documents HIGH as TX mode. It is deliberate.
    // camillia-mt sets this pin HIGH once at init, never touches it again, and
    // receives packets on this exact hardware — including while transmitting
    // nothing. So either the documented polarity is inverted or the pin does not
    // gate the RX path the way the name suggests; what is *known* is that HIGH
    // receives. Matching the empirically-working configuration beats trusting a
    // comment, and a monitor that mutely hears nothing is the worst failure mode
    // this device has.
    //
    // If bring-up shows no packets, this is the first pin to try LOW.
    pinMode(LORA_FEM_TX_MODE_PIN, OUTPUT);
    digitalWrite(LORA_FEM_TX_MODE_PIN, HIGH);

    _radio.reset();
    // TX power is passed to begin() because RadioLib requires it, then never
    // used: nothing in this build calls transmit().
    int state = _radio.begin(_freq, _bw, _sf, _cr, MESH_SYNC,
                             /*power=*/0, MESH_PREAMBLE, MESH_TCXO_V);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] init failed: %d\n", state);
        if (state == RADIOLIB_ERR_CHIP_NOT_FOUND) {
            Serial.println("[radio] hint: SX1262 not detected — check board power (BOARD_POWERON),");
            Serial.println("[radio]       the VEXT rail, and the FEM power/enable pins.");
        }
        return false;
    }

    _radio.setDio2AsRfSwitch(true);
    const int boostState = _radio.setRxBoostedGainMode((bool)MY_LORA_RX_BOOST);
    _radio.setDio1Action(_onDio1);
    _armRx();

    _ready = true;
    // Print what the chip accepted, not what was asked for: in camillia-mt a
    // boosted-gain setting that was never actually applied sat unnoticed
    // precisely because the log echoed the request.
    Serial.printf("[radio] listening  %.3f MHz  SF%d  BW%.0f  CR4/%d  rxBoost=%s\n",
                  _freq, _sf, _bw, _cr,
                  (boostState != RADIOLIB_ERR_NONE) ? "FAILED"
                                                    : (MY_LORA_RX_BOOST ? "1" : "0"));
    if (boostState != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] setRxBoostedGainMode failed: %d\n", boostState);
    }
    return true;
}

bool MeshRadio::reconfigure(float freq, float bw, uint8_t sf, uint8_t cr) {
    if (!_ready) return false;
    _radio.standby();

    int state = _radio.setFrequency(freq);
    if (state == RADIOLIB_ERR_NONE) state = _radio.setBandwidth(bw);
    if (state == RADIOLIB_ERR_NONE) state = _radio.setSpreadingFactor(sf);
    if (state == RADIOLIB_ERR_NONE) state = _radio.setCodingRate(cr);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] reconfigure failed: %d\n", state);
        _armRx();   // back to listening on whatever still applied
        return false;
    }

    _freq = freq; _bw = bw; _sf = sf; _cr = cr;
    _rxFlag = false;
    _armRx();
    Serial.printf("[radio] retuned  %.3f MHz  SF%d  BW%.0f  CR4/%d\n", _freq, _sf, _bw, _cr);
    return true;
}

float MeshRadio::channelUtilPercent() {
    const float pct = airPct();
    if (pct > sChUtilMax) sChUtilMax = pct;
    return pct;
}

// Reads the peak without disturbing it. Note the peak only advances when
// channelUtilPercent() is called, which the UI does once a second — fine for a
// human-readable high-water mark, and it avoids recomputing the hour window on
// a second cadence just to maintain a maximum.
float MeshRadio::channelUtilMaxPercent() { return sChUtilMax; }

bool MeshRadio::pollRx(MeshPacket &pkt) {
    if (!_rxFlag) return false;
    _rxFlag = false;
    sRx.irqs++;

    uint8_t buf[256];
    size_t len = _radio.getPacketLength();
    if (len < sizeof(MeshHdr) || len > sizeof(buf)) {
        sRx.badLen++;
        _armRx();
        return false;
    }

    const int readState = _radio.readData(buf, len);
    if (readState != RADIOLIB_ERR_NONE) {
        // A CRC failure still counts as "something was out there" — for a
        // monitor a damaged packet is a signal-quality observation, not a
        // non-event, so it gets its own counter rather than being lumped in.
        if (readState == RADIOLIB_ERR_CRC_MISMATCH) sRx.crcErr++;
        else                                        sRx.readErr++;
        _armRx();
        return false;
    }
    sRx.good++;

    if (kVerboseRadioIo) {
        Serial.printf("[radio] RX hdr: ");
        for (size_t i = 0; i < 16 && i < len; i++) Serial.printf("%02x ", buf[i]);
        Serial.println();
    }

    airNote((uint32_t)(_radio.getTimeOnAir(len) / 1000UL));

    pkt = MeshPacket{};
    pkt.rssi = _radio.getRSSI();
    pkt.snr  = _radio.getSNR();
    pkt.rxMs = millis();
    memcpy(&pkt.hdr, buf, sizeof(MeshHdr));
    pkt.chanIdx = -1;

    const size_t payloadLen = len - sizeof(MeshHdr);
    const uint8_t *cipher = buf + sizeof(MeshHdr);

    // Channel hash 0 marks a PKI-encrypted packet. A monitor is not the
    // recipient and can never read one, so it is left undecrypted rather than
    // run through the channel keys — random ciphertext occasionally passes the
    // "looks like a Data protobuf" check, and a false positive here would
    // invent a portnum and a payload out of noise. It is still counted, and its
    // sender still attributed, which is all a monitor can honestly claim.
    if (payloadLen > 0 && pkt.hdr.channel != 0) {
        uint8_t plain[256];
        pkt.chanIdx   = decryptPacket(pkt.hdr, cipher, plain, payloadLen);
        pkt.decrypted = (pkt.chanIdx >= 0);

        if (pkt.decrypted) {
            const uint8_t *payPtr = nullptr; size_t payLen = 0;
            decodeData(plain, payloadLen, pkt.portnum, payPtr, payLen,
                       pkt.requestId, pkt.wantResponse,
                       &pkt.dataDest, &pkt.hasDataDest,
                       &pkt.dataSource, &pkt.hasDataSource);
            if (payPtr && payLen <= sizeof(pkt.payload)) {
                memcpy(pkt.payload, payPtr, payLen);
                pkt.payloadLen = payLen;
            } else if (payPtr) {
                Serial.printf("[radio] payload too long: port=%lu len=%u cap=%u (dropped)\n",
                              (unsigned long)pkt.portnum, (unsigned)payLen,
                              (unsigned)sizeof(pkt.payload));
            }
        }
    }

    _armRx();
    return true;
}
