# 03_esp-brookesia

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Runs the ESP-Brookesia phone-style UI with the bundled SquareLine demo app.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board

## How to Run

```bash
idf.py set-target esp32s3 build flash monitor
```

## Expected Result

The display shows a home screen with a status bar clock and app icons; touch navigates between screens and opens the demo app.

## Troubleshooting

- Keep the memory options in `sdkconfig.defaults` unless the change is re-tested on hardware with every supported ESP-IDF version.
- Before updating LVGL, display, touch or Brookesia dependencies, check that this example still builds with every supported ESP-IDF version.
