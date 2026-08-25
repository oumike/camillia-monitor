# Camillia Monitor

LVGL firmware for the Heltec WiFi LoRa 32 V4 with the expansion display and
CHSC6x capacitive touch controller.

## Target

The project intentionally contains one PlatformIO environment:

- `heltec-v4`: 320x240 landscape ST7789 display, CHSC6x touch, 16 MB flash,
  and 2 MB QSPI PSRAM.

The initial screen is a live LVGL 9 dashboard for uptime, heap, PSRAM, and CPU
frequency. Persistent settings should use ESP32 Preferences/NVS. The partition
table contains dual OTA slots and no filesystem partition.

## Build And Flash

Build without a connected device:

```bash
./build-upload-monitor.sh --just-build
```

Build, upload, and open the serial monitor:

```bash
./build-upload-monitor.sh
```

Useful options:

```text
--erase, -E       Erase flash before uploading
--fullclean, -F   Run PlatformIO fullclean first
--just-build, -B  Compile only
```

The underlying PlatformIO command is:

```bash
pio run -e heltec-v4
```

## Release

Edit `RELEASE_NOTES.md`, then run:

```bash
./release.sh
```

Like the Camillia-MT release workflow, this performs a clean build, packages a
factory image and OTA image in `dist/`, commits the release state, pushes the
commit and tag, and publishes the binaries with GitHub CLI. Use `--no-clean`
only when intentionally reusing the current PlatformIO build output.