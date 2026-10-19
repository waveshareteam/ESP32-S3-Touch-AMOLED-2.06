# Factory Firmware Source

[简体中文](README_ZH.md)

This ESP-IDF project builds the firmware the board ships with: the ESP-Brookesia
application launcher with its built-in apps.

CI builds it with the ESP-IDF version set in `.github/product.json`. The tested,
ready-to-flash image is in the parent [`firmware`](../) folder.

To build it locally with that ESP-IDF version:

```bash
idf.py -C firmware/sourcecode set-target esp32s3 build
```
