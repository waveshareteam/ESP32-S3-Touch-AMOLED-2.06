# CI

This repository uses the `Build Examples` GitHub Actions workflow for build validation and firmware artifact packaging.

## ESP-IDF

The workflow discovers first-party projects under `examples/esp-idf/`.

The default matrix builds each discovered project for ESP32-S3 against:

- ESP-IDF `v5.5.4`
- ESP-IDF `v6.0.2`

## Arduino

The workflow discovers first-party sketches under `examples/arduino/`.

Bundled library examples under `examples/arduino/libraries/` are intentionally excluded from product CI. Changes to bundled libraries trigger the workflow because first-party sketches depend on those local library copies.

The default Arduino matrix uses:

- Arduino-ESP32 core `3.3.10`
- FQBN `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`
- Bundled libraries from `examples/arduino/libraries/`

## Manual Runs

Start the workflow manually with `target=all`, an example directory name such as `01_AXP2101`, or a repo-relative path such as `examples/arduino/01_HelloWorld`.

## Firmware Artifacts

Successful source builds are packaged with `releases/package_firmware.py` and uploaded as `firmware-*` workflow artifacts. Each archive contains a manifest, flash helper scripts, flash arguments, and firmware binaries.

Checked-in factory binaries under `FirmWare/` are recovery or release artifacts, not source projects. They are documented and excluded from build discovery.

See `releases/README.md` and `docs/firmware.md` for packaging and artifact download details.