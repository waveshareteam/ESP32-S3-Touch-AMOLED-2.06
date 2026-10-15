# 07_LVGL_SD_Test

[English](README.md) | [简体中文](README_ZH.md)

## 功能

以 1 线 SD_MMC 模式挂载 microSD 卡，显示卡类型、容量和根目录内容。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板
- microSD 卡

## 怎么运行

1. 安装 `esp32` 开发板包，并把 [`examples/arduino/libraries/`](../../libraries/) 作为项目文件夹下的
   `libraries` 目录使用。
2. 开发板选 **ESP32S3 Dev Module**，Flash Size 选 **16MB**，Partition Scheme 选
   **16M Flash (3MB APP/9.9MB FATFS)**，PSRAM 选 **OPI PSRAM**，USB CDC On Boot 选 **Enabled**。
3. 打开 `07_LVGL_SD_Test.ino` 并上传。

## 预期现象

屏幕和串口显示卡类型、容量和根目录下的文件。

## 常见问题

- `Card Mount Failed` 或 `No SD_MMC card attached`：先插入 FAT 格式的卡，再复位开发板。
