# 01_HelloWorld

[English](README.md) | [简体中文](README_ZH.md)

## 功能

用 Arduino_GFX 初始化 AMOLED 屏幕并显示 "Hello World!"。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板

## 怎么运行

1. 安装 `esp32` 开发板包，并把 [`examples/arduino/libraries/`](../../libraries/) 作为项目文件夹下的
   `libraries` 目录使用。
2. 开发板选 **ESP32S3 Dev Module**，Flash Size 选 **16MB**，Partition Scheme 选
   **16M Flash (3MB APP/9.9MB FATFS)**，PSRAM 选 **OPI PSRAM**，USB CDC On Boot 选 **Enabled**。
3. 打开 `01_HelloWorld.ino` 并上传。

## 预期现象

屏幕在白底上显示红色的 "Hello World!"，两秒后不断在随机位置以随机颜色和大小出现更多文字。

## 常见问题

暂无。
