# 05_LVGL_AXP2101_ADC_Data

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Shows AXP2101 power and battery data on the screen with LVGL.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board
- Lithium battery (optional)

## How to Run

1. Install the `esp32` board package and make [`examples/arduino/libraries/`](../../libraries/)
   available as your sketchbook `libraries` folder.
2. Select **ESP32S3 Dev Module** and set Flash Size **16MB**, Partition Scheme
   **16M Flash (3MB APP/9.9MB FATFS)**, PSRAM **OPI PSRAM**, USB CDC On Boot **Enabled**.
3. Open `05_LVGL_AXP2101_ADC_Data.ino` and upload.

## Expected Result

The screen lists the PMU temperature, charging state and the battery, VBUS and system voltages, plus the battery percentage when a battery is connected, and keeps them updated.

## Troubleshooting

None yet.
