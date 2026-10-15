# Factory Firmware Source

[简体中文](README_ZH.md)

This ESP-IDF project builds the firmware the board ships with: the ESP-Brookesia
application launcher with its built-in apps.

CI builds it with the ESP-IDF version set in `.github/product.json` and attaches
the flashable package to each release on the
[Releases page](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/releases).

To build it locally with that ESP-IDF version:

```bash
idf.py -C factory_firmware set-target esp32s3 build
```
