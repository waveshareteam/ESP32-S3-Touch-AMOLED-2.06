# 05_LVGL_AXP2101_ADC_Data

[English](README.md) | [简体中文](README_ZH.md)

## 功能

用 LVGL 在屏幕上显示 AXP2101 的电源和电池数据。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板
- 锂电池（可选）

## 怎么运行

1. 安装 `esp32` 开发板包，并把 [`examples/arduino/libraries/`](../../libraries/) 作为项目文件夹下的
   `libraries` 目录使用。
2. 开发板选 **ESP32S3 Dev Module**，Flash Size 选 **16MB**，Partition Scheme 选
   **16M Flash (3MB APP/9.9MB FATFS)**，PSRAM 选 **OPI PSRAM**，USB CDC On Boot 选 **Enabled**。
3. 打开 `05_LVGL_AXP2101_ADC_Data.ino` 并上传。

## 预期现象

屏幕列出 PMU 温度、充电状态，以及电池、VBUS 和系统电压；接了电池时还显示电量百分比，并持续刷新。

## 常见问题

暂无。
