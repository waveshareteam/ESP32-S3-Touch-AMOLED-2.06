# 06_LVGL_Arduino_v9

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Runs the LVGL 9 widgets demo with touch input.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board

## How to Run

1. Install the `esp32` board package and make [`examples/arduino/libraries/`](../../libraries/)
   available as your sketchbook `libraries` folder.
2. Select **ESP32S3 Dev Module** and set Flash Size **16MB**, Partition Scheme
   **16M Flash (3MB APP/9.9MB FATFS)**, PSRAM **OPI PSRAM**, USB CDC On Boot **Enabled**.
3. Open `06_LVGL_Arduino_v9.ino` and upload.

## Expected Result

The screen shows the LVGL widgets demo, which responds to touch.

## Troubleshooting

None yet.
