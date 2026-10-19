# 固件

[English](README.md) | [简体中文](README_ZH.md)

在 ESP32-S3-Touch-AMOLED-2.06 开发板上实测过、可直接烧录的固件。每个文件都是从 `0x0` 烧录的合并镜像，文件名中的日期即版本。

| 文件 | 用途 |
| --- | --- |
| `ESP32-S3-Touch-AMOLED-2.06_FactoryOnly_20261016.bin` | 出厂固件，恢复开箱演示 |

用 esptool 烧录：

```bash
python -m pip install esptool
esptool.py --chip esp32s3 --port <PORT> erase_flash
esptool.py --chip esp32s3 --port <PORT> write_flash 0x0 <FILE>.bin
```

出厂固件源码在 [`sourcecode`](sourcecode/) 目录。
