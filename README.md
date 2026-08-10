<div align="center">
  <h1>ESP32-S3-Touch-AMOLED-2.06</h1>
  <p><strong>ESP32-S3 2.06-inch 410 x 502 QSPI AMOLED touch development board</strong></p>
  <p>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/actions/workflows/examples.yml"><img alt="Build Examples" src="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/actions/workflows/examples.yml/badge.svg"></a>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/releases/latest"><img alt="Latest Release" src="https://img.shields.io/github/v/release/waveshareteam/ESP32-S3-Touch-AMOLED-2.06"></a>
    <a href="LICENSE"><img alt="License" src="https://img.shields.io/github/license/waveshareteam/ESP32-S3-Touch-AMOLED-2.06"></a>
  </p>
  <p><a href="README_ZH.md">简体中文</a></p>
  <p>
    <a href="https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm">🌐 Product</a> &middot;
    <a href="docs/ci.md">📚 Documentation</a> &middot;
    <a href="FirmWare/README.md">📦 Firmware</a> &middot;
    <a href="examples/esp-idf/">🧩 ESP-IDF</a> &middot;
    <a href="examples/arduino/">🔧 Arduino</a>
  </p>
</div>

---

## ✨ Overview

This repository provides first-party ESP-IDF and Arduino examples, source-built
firmware packages, factory recovery firmware, schematics, and product media for
the Waveshare ESP32-S3-Touch-AMOLED-2.06.

## 🖥️ Hardware Overview

| Feature | Device / interface |
| --- | --- |
| MCU | ESP32-S3 32-bit LX7 dual-core processor |
| Display | 2.06-inch 410 x 502 QSPI AMOLED using CO5300 |
| Touch | CST9220 capacitive touch controller using the CST92xx driver |
| Power management | AXP2101 |
| Motion sensor | QMI8658 six-axis IMU |
| Audio | Dual digital microphones with ES7210 ADC and ES8311 codec |
| Board support | Managed component `waveshare/esp32_s3_touch_amoled_2_06` |
| Hardware files | [Schematic](Schematic/) and [product material](Material/) |

## 📦 Firmware Releases

Use a ready-to-flash package from the [latest release](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/releases/latest)
or a completed `Build Examples` workflow run.

1. Download the `firmware-*` archive for the required example and framework version.
2. Extract it and install esptool with `python -m pip install esptool`.
3. Connect the board over USB.
4. Run `flash.bat COMx` on Windows or `./flash.sh /dev/ttyUSB0` on Linux.
5. Reset the board if it does not restart automatically.

Each package contains a combined firmware image at offset `0x0`, source
segments, flash arguments, helper scripts, and a manifest. Factory recovery
files under [FirmWare](FirmWare/) are separate immutable artifacts; see
[Firmware Artifacts](docs/firmware.md).

## 🧪 Examples

| ESP-IDF example | Focus |
| --- | --- |
| [01_AXP2101](examples/esp-idf/01_AXP2101/) | Power management and battery telemetry |
| [02_lvgl_demo_v9](examples/esp-idf/02_lvgl_demo_v9/) | LVGL 9 display and touch demo |
| [03_esp-brookesia](examples/esp-idf/03_esp-brookesia/) | ESP-Brookesia application UI |
| [04_Immersive_block](examples/esp-idf/04_Immersive_block/) | Motion-driven LVGL block demo |
| [05_Spec_Analyzer](examples/esp-idf/05_Spec_Analyzer/) | Microphone spectrum analyzer |
| [06_videoplayer](examples/esp-idf/06_videoplayer/) | SD card video playback with audio |

| Arduino example | Focus |
| --- | --- |
| [01_HelloWorld](examples/arduino/01_HelloWorld/) | Display bring-up |
| [02_GFX_AsciiTable](examples/arduino/02_GFX_AsciiTable/) | GFX text and character rendering |
| [03_LVGL_PCF85063_simpleTime](examples/arduino/03_LVGL_PCF85063_simpleTime/) | LVGL clock UI using the PCF85063 RTC |
| [04_LVGL_QMI8658_ui](examples/arduino/04_LVGL_QMI8658_ui/) | LVGL IMU data UI |
| [05_LVGL_AXP2101_ADC_Data](examples/arduino/05_LVGL_AXP2101_ADC_Data/) | LVGL power and battery telemetry UI |
| [06_LVGL_Arduino_v9](examples/arduino/06_LVGL_Arduino_v9/) | LVGL 9 UI demo |
| [07_LVGL_SD_Test](examples/arduino/07_LVGL_SD_Test/) | SD card test |
| [08_ES8311](examples/arduino/08_ES8311/) | ES8311 audio codec example |

Bundled libraries in [`examples/arduino/libraries/`](examples/arduino/libraries/)
are used by product sketches; their upstream examples are excluded from product CI.

## 🛠️ Supported Toolchains

| Surface | Version |
| --- | --- |
| ESP-IDF | `v5.5.5` and `v6.0.2` |
| Arduino-ESP32 | `3.3.11` |

The [Build Examples workflow](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/actions/workflows/examples.yml)
discovers maintained first-party projects, packages successful builds, and
accepts `target=all`, an example name, or an example path for manual runs. See
[Continuous Integration](docs/ci.md) for routing and matrix details.

## 🗂️ Repository Layout

| Path | Purpose |
| --- | --- |
| [`examples/esp-idf/`](examples/esp-idf/) | First-party ESP-IDF projects |
| [`examples/arduino/`](examples/arduino/) | First-party Arduino sketches and bundled libraries |
| [`FirmWare/`](FirmWare/) | Checked-in factory and recovery binaries |
| [`Material/`](Material/) | Product media and reference material |
| [`Schematic/`](Schematic/) | Schematic files |
| [`releases/`](releases/) | Firmware packaging and artifact download tools |
| [`scripts/`](scripts/) | Example discovery and CI helper scripts |
| [`docs/`](docs/) | Repository, CI, and firmware notes |

## 📚 Documentation

- [Repository Structure](docs/repository-structure.md)
- [Continuous Integration](docs/ci.md)
- [Firmware Artifacts](docs/firmware.md)
- [Release Tools](releases/README.md)

## 🤝 Support and Contributions

Contributions and reproducible issue reports are welcome. Include the example
path, framework version, reproduction steps, expected behavior, actual
behavior, and relevant serial logs.

- [Contributing Guide](CONTRIBUTING.md)
- [Support](SUPPORT.md)
- [Security Policy](SECURITY.md)
- [Open an Issue](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/issues/new/choose)

## 📄 License

This repository is licensed under the Apache License 2.0. See [LICENSE](LICENSE).
