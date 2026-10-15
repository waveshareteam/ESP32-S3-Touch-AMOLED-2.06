# 出厂固件源码

[English](README.md)

本 ESP-IDF 工程构建开发板的出厂固件：ESP-Brookesia 应用启动器及其内置应用。

CI 使用 `.github/product.json` 中指定的 ESP-IDF 版本构建它，并把可烧录的固件包附在
[Releases 页面](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/releases)的每个版本中。

用同一 ESP-IDF 版本在本地构建：

```bash
idf.py -C factory_firmware set-target esp32s3 build
```
