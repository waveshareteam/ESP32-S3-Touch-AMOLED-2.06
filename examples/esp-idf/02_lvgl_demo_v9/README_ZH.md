# 02_lvgl_demo_v9

[English](README.md) | [简体中文](README_ZH.md)

## 功能

通过 Waveshare 托管 BSP 在屏幕上运行 LVGL 9 音乐播放器演示。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板

## 怎么运行

```bash
idf.py set-target esp32s3 build flash monitor
```

## 预期现象

屏幕显示 LVGL 音乐播放器演示界面，可以触摸操作。

## 常见问题

- 想运行其他演示，在 `main/main.c` 中改用 `lv_demo_benchmark()` 或 `lv_demo_widgets()`。
