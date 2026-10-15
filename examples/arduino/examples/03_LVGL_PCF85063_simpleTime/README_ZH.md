# 03_LVGL_PCF85063_simpleTime

[English](README.md) | [简体中文](README_ZH.md)

## 功能

设置 PCF85063 实时时钟，并用 LVGL 显示时间和日期。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板

## 怎么运行

1. 安装 `esp32` 开发板包，并把 [`examples/arduino/libraries/`](../../libraries/) 作为项目文件夹下的
   `libraries` 目录使用。
2. 开发板选 **ESP32S3 Dev Module**，Flash Size 选 **16MB**，Partition Scheme 选
   **16M Flash (3MB APP/9.9MB FATFS)**，PSRAM 选 **OPI PSRAM**，USB CDC On Boot 选 **Enabled**。
3. 打开 `03_LVGL_PCF85063_simpleTime.ino` 并上传。

## 预期现象

屏幕显示时间和日期，每秒刷新，从 2025-07-21 11:09:41 开始走时。串口同步打印。

## 常见问题

- 草图每次启动都会把固定的日期时间写入 RTC；想从当前时间开始，修改 `setup()` 中的数值。
