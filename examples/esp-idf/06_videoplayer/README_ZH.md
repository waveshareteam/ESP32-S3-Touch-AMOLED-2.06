# 06_videoplayer

[English](README.md) | [简体中文](README_ZH.md)

## 功能

从 microSD 卡循环播放带声音的 MJPEG AVI 视频。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板
- microSD 卡
- 扬声器

## 怎么运行

1. 把本示例的 [`sdcard/avi`](sdcard/avi/) 文件夹复制到 microSD 卡根目录，并插入开发板。
2. 编译并烧录：

   ```bash
   idf.py set-target esp32s3 build flash monitor
   ```

## 预期现象

屏幕在 320 × 200 的窗口中依次播放卡上 `avi` 文件夹里的所有 AVI 文件，扬声器同步出声。串口打印帧率。

## 常见问题

- `No AVI files found in /sdcard/avi`：卡的根目录下需要有 `avi` 文件夹。
- 自备视频需为 MJPEG 编码的 AVI，分辨率不超过 320 × 200。
