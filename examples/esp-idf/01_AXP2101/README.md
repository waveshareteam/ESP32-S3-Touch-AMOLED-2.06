# 01_AXP2101

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Reads the AXP2101 power management unit over I2C with XPowersLib and logs power rail, battery and charging data.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board
- Lithium battery (optional)

## How to Run

```bash
idf.py set-target esp32s3 build flash monitor
```

## Expected Result

The serial monitor prints the state and voltage of every power rail once, then the PMU temperature, the charging state and the battery, VBUS and system voltages every second. The display stays off.

## Troubleshooting

- `Init PMU FAILED!`: check the I2C pins under **XPowersLib Configuration** in `idf.py menuconfig` (SDA GPIO15, SCL GPIO14, set in `sdkconfig.defaults`).
