# 04_Immersive_block

[English](README.md) | [简体中文](README_ZH.md)

## 功能

在 AMOLED 屏幕上绘制 LVGL 图形，并根据 QMI8658 的加速度数据随开发板倾斜移动。

## 硬件需求

- ESP32-S3-Touch-AMOLED-2.06 开发板

## 怎么运行

```bash
idf.py set-target esp32s3 build flash monitor
```

## 预期现象

短暂的水平校准后屏幕出现图形，倾斜开发板时图形向较低的一侧滑动。

## 常见问题

- 校准期间把开发板平放静止；开发板晃动时校准会一直重试（`Calibration unstable ... Retrying...`），图形不会出现。按 BOOT 键可重新校准。
