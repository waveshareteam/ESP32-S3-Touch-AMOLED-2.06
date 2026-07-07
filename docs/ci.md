# CI

This repository uses GitHub Actions for build validation.

## ESP-IDF

The ESP-IDF workflow discovers first-party projects under versioned ESP-IDF example roots and firmware roots:

- `examples/ESP-IDF-v5.4.2/`
- `firmware/`
- `Firmware/`
- `FirmWare/`

The default matrix builds each discovered project for ESP32-S3 against:

- ESP-IDF `v5.5.4`
- ESP-IDF `v6.0.2`

The workflow can be started manually with `project=all`, a project directory name such as `01_AXP2101`, or a repo-relative project path.

## Arduino

The Arduino workflow discovers only first-party sketches under:

- `examples/Arduino-v3.2.0/examples/`
- `examples/arduino/`

Bundled library examples under `examples/Arduino-v3.2.0/libraries/` are intentionally excluded from product CI. Changes to bundled libraries trigger all first-party sketches because those sketches depend on the local library copies.

The default Arduino matrix uses:

- Arduino-ESP32 core `3.3.10`
- FQBN `esp32:esp32:esp32s3`
- Bundled libraries from `examples/Arduino-v3.2.0/libraries/`

The workflow can be started manually with `sketch=all`, a sketch directory name such as `01_HelloWorld`, or a repo-relative sketch path.

## Firmware Artifacts

Checked-in factory binaries under `FirmWare/` are recovery or release artifacts, not source projects. They are documented and excluded from build discovery.

Source-built CI firmware packaging is not enabled yet. If release artifacts are required, add packaging that reads ESP-IDF `flasher_args.json` or Arduino exported binaries and uploads flashable archives from successful CI runs.
