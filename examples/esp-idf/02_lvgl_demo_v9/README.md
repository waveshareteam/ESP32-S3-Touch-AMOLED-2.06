# 02_lvgl_demo_v9

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Runs the LVGL 9 music player demo on the display through the managed Waveshare BSP.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board

## How to Run

```bash
idf.py set-target esp32s3 build flash monitor
```

## Expected Result

The display shows the LVGL music player demo, which responds to touch.

## Troubleshooting

- To run another demo, enable `lv_demo_benchmark()` or `lv_demo_widgets()` in `main/main.c`.
