# 04_LVGL_QMI8658_ui

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Plots the QMI8658 three-axis acceleration on an LVGL line chart.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board

## How to Run

1. Install the `esp32` board package and make [`examples/arduino/libraries/`](../../libraries/)
   available as your sketchbook `libraries` folder.
2. Select **ESP32S3 Dev Module** and set Flash Size **16MB**, Partition Scheme
   **16M Flash (3MB APP/9.9MB FATFS)**, PSRAM **OPI PSRAM**, USB CDC On Boot **Enabled**.
3. Open `04_LVGL_QMI8658_ui.ino` and upload.

## Expected Result

The screen shows three curves for the X, Y and Z axes that change as the board moves. Accelerometer and gyroscope values are also printed to the serial monitor.

## Troubleshooting

None yet.
