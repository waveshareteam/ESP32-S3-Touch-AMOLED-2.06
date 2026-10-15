# 03_LVGL_PCF85063_simpleTime

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Sets the PCF85063 real-time clock and shows the time and date with LVGL.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board

## How to Run

1. Install the `esp32` board package and make [`examples/arduino/libraries/`](../../libraries/)
   available as your sketchbook `libraries` folder.
2. Select **ESP32S3 Dev Module** and set Flash Size **16MB**, Partition Scheme
   **16M Flash (3MB APP/9.9MB FATFS)**, PSRAM **OPI PSRAM**, USB CDC On Boot **Enabled**.
3. Open `03_LVGL_PCF85063_simpleTime.ino` and upload.

## Expected Result

The screen shows the time and date, updated every second and starting from 11:09:41 on 21-07-2025. The serial monitor prints the same values.

## Troubleshooting

- The sketch writes a fixed date and time to the RTC at every boot; change the values in `setup()` to start from the current time.
