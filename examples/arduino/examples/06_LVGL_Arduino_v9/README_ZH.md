# 06_LVGL_Arduino_v9

[English](README.md) | [简体中文](README_ZH.md)

## 功能

运行带触摸输入的 LVGL 9 控件演示。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板

## 怎么运行

1. 安装 `esp32` 开发板包，并把 [`examples/arduino/libraries/`](../../libraries/) 作为项目文件夹下的
   `libraries` 目录使用。
2. 开发板选 **ESP32S3 Dev Module**，Flash Size 选 **16MB**，Partition Scheme 选
   **16M Flash (3MB APP/9.9MB FATFS)**，PSRAM 选 **OPI PSRAM**，USB CDC On Boot 选 **Enabled**。
3. 打开 `06_LVGL_Arduino_v9.ino` 并上传。

## 预期现象

屏幕显示 LVGL 控件演示界面，可以触摸操作。

## 常见问题

暂无。
