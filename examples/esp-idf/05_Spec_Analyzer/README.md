# 05_Spec_Analyzer

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Captures the on-board microphones through the ES7210 and draws a live audio spectrum.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board

## How to Run

```bash
idf.py set-target esp32s3 build flash monitor
```

## Expected Result

The display shows colored bars that rise and fall with the sound near the board.

## Troubleshooting

None yet.
