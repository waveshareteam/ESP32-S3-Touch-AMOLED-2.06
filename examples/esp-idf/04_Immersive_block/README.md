# 04_Immersive_block

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Renders movable LVGL shapes on the AMOLED display and uses QMI8658 acceleration data to move them as the board tilts.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board

## How to Run

```bash
idf.py set-target esp32s3 build flash monitor
```

## Expected Result

After a short level calibration, shapes appear on the display and slide toward the lower side when the board is tilted.

## Troubleshooting

- Keep the board stationary on a level surface during calibration; while it moves, calibration repeats (`Calibration unstable ... Retrying...`) and the shapes do not appear. Press the BOOT button to recalibrate.
