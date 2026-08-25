#pragma once
// ════════════════════════════════════════════════════════════════════════════
// hal/hardware.h — Heltec WiFi LoRa 32 V4 + T-Deck-style TFT expansion carrier
//
// Pin map for the Camillia Monitor: a passive Meshtastic listener. Values are
// carried over from camillia-mt's hal/hw_heltec_v4.h, where each was verified
// on this hardware; the comments that record *why* a value is what it is are
// kept, because several of them are counter-intuitive enough that a future
// reader would otherwise "fix" them back to something broken.
//
// Board features used here:
//   • 2.8" ST7789 320x240 TFT (portrait native, landscape in use)
//   • CHSC6X capacitive touch (I2C on Wire1)
//   • SX1262 LoRa with an external FEM (front-end module) on dedicated GPIOs
//   • BME280-class environment sensor on I2C (Wire)
//   • Battery ADC on a switched sense line
// ════════════════════════════════════════════════════════════════════════════

// ── Power & board peripherals ───────────────────────────────────────────────
#define BOARD_POWERON              7   // Hold HIGH to keep the module powered

// Switched peripheral rail. Drive LOW to enable. Do not change this.
//
// Verified twice on hardware in camillia-mt: driving GPIO36 HIGH makes the touch
// controller's I2C fail continuously (Wire.cpp requestFrom() error 263, once per
// second, each timeout blocking lv_timer_handler() for ~1 s). LOW restores it.
// Other ports of this board publish HIGH for the same pin; on this unit HIGH
// only ever broke touch. Do not revisit GPIO36 to chase a sensor problem.
#define BOARD_VEXT_ENABLE         36
#define BOARD_VEXT_ON_LEVEL       LOW

// ── TFT display — ST7789 320x240 ────────────────────────────────────────────
#define TFT_SPI_HOST          SPI3_HOST
#define TFT_SPI_SCK               17
#define TFT_SPI_MISO              -1   // Write-only (3-wire SPI)
#define TFT_SPI_MOSI              33
#define TFT_SPI_3WIRE           true
#define TFT_SPI_WRITE_HZ    40000000
#define TFT_SPI_READ_HZ      4000000
#define TFT_CS                    15
#define TFT_DC                    16
#define TFT_RST                   18
#define TFT_BL                    21
#define TFT_BL_INVERT          false
#define TFT_BL_FREQ            44100
#define TFT_BL_PWM_CH              7
#define TFT_BRIGHTNESS_DEFAULT   160
#define TFT_PANEL_WIDTH          240
#define TFT_PANEL_HEIGHT         320
#define TFT_PANEL_OFFSET_X         0
#define TFT_PANEL_OFFSET_Y         0
#define TFT_INVERT              true
#define TFT_RGB_ORDER          false
#define TFT_ROTATION_DEFAULT       3

// ── Capacitive touch — CHSC6X on Wire1 ──────────────────────────────────────
#define HAS_TOUCH                  1
#define TOUCH_SDA                 47
#define TOUCH_SCL                 48
#define TOUCH_ADDR              0x2E
#define TOUCH_INT                 -1   // No interrupt routed; uses polling
#define TOUCH_RST                 44
#define TOUCH_I2C_PORT             1   // Wire1

// ── LoRa — SX1262 on its own SPI bus (separate from the display) ────────────
// The FEM (PA/LNA front-end) must be powered and switched to RX before RadioLib
// yields anything at all. A monitor never transmits, so LORA_FEM_TX_MODE_PIN is
// parked LOW (RX mode) for the life of the process — see radio bring-up.
#define LORA_SPI_SCK               9
#define LORA_SPI_MISO             11
#define LORA_SPI_MOSI             10
#define LORA_CS                    8
#define LORA_DIO1                 14
#define LORA_RST                  12
#define LORA_BUSY                 13
#define LORA_FEM_POWER_PIN         7   // FEM power enable
#define LORA_FEM_ENABLE_PIN        2   // FEM RF switch enable
#define LORA_FEM_TX_MODE_PIN      46   // HIGH = TX mode, LOW = RX mode

// Radio TCXO reference voltage.
#define MESH_TCXO_V             1.8f

// ── Environment sensor (BME280-class) on Wire ───────────────────────────────
// SDA=4 / SCL=3, NOT 3/4. The board's generic pins_arduino.h declares SDA=3 /
// SCL=4, but the TFT carrier swaps them relative to the board JSON. Probing 3/4
// scans an empty bus and finds nothing — which is exactly what camillia-mt did
// before this was pinned down.
#define ENV_SDA                    4
#define ENV_SCL                    3
#define ENV_I2C_PORT               0   // Wire
#define HAS_ENV_SENSOR             1

// ── Battery ADC with a switched sense line ──────────────────────────────────
// The sense-enable pin is driven only around a reading, to keep the divider
// from loading the pack continuously.
#define BATT_ADC_PIN               1
#define BATT_DIV              5.1205f  // Precision-calibrated divider ratio
#define BATT_SENSE_ENABLE_PIN     37
#define BATT_SENSE_ENABLE_LEVEL   LOW

// ── GPS — L76K on UART1 ─────────────────────────────────────────────────────
// GPS_RX is the pin WE RECEIVE ON. Arduino's HardwareSerial::begin(baud, cfg,
// rx, tx) takes this core's RX third, which makes the naming the exact reverse
// of ports that name their pins after the *module's* TX. camillia-mt had these
// two swapped for a long time and heard nothing; on this hardware the module
// streams NMEA on GPIO39 and GPIO38 is silent.
#define HAS_GPS                    1
#define GPS_RX                    39
#define GPS_TX                    38
#define GPS_BAUD               38400
// Nothing drove these before camillia-mt found them, so the module was only
// ever heard when it happened to power up already enabled.
#define GPS_ENABLE_PIN            34
#define GPS_ENABLE_ON_LEVEL      LOW
#define GPS_RESET_PIN             42
#define GPS_RESET_ACTIVE_LEVEL   LOW

// ── Buttons ─────────────────────────────────────────────────────────────────
#define USER_BUTTON_PIN            0
#define USER_BUTTON_ACTIVE_LEVEL   LOW
#define DISPLAY_TOGGLE_BUTTON_PIN          35
#define DISPLAY_TOGGLE_BUTTON_ACTIVE_LEVEL LOW

// ── Memory / display geometry ───────────────────────────────────────────────
#define HAS_PSRAM                  1
#define DISPLAY_WIDTH            320
#define DISPLAY_HEIGHT           240
