# 08_ES8311

[English](README.md) | [简体中文](README_ZH.md)

## 功能

通过 ES8311 播放一段内置音频，然后把麦克风采到的声音实时回放到扬声器。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板
- 扬声器

## 怎么运行

1. 安装 `esp32` 开发板包，并把 [`examples/arduino/libraries/`](../../libraries/) 作为项目文件夹下的
   `libraries` 目录使用。
2. 开发板选 **ESP32S3 Dev Module**，Flash Size 选 **16MB**，Partition Scheme 选
   **16M Flash (3MB APP/9.9MB FATFS)**，PSRAM 选 **OPI PSRAM**，USB CDC On Boot 选 **Enabled**。
3. 打开 `08_ES8311.ino` 并上传。

## 预期现象

复位后扬声器先播放一次音频，之后回放麦克风采到的声音。屏幕不点亮。

## 常见问题

- 扬声器啸叫时，调低草图开头的 `EXAMPLE_VOICE_VOLUME`。
