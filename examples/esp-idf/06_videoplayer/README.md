# 06_videoplayer

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Plays MJPEG AVI videos with sound from the microSD card in a loop.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board
- microSD card
- Speaker

## How to Run

1. Copy the [`sdcard/avi`](sdcard/avi/) folder of this example to the root of a microSD card and insert the card.
2. Build and flash:

   ```bash
   idf.py set-target esp32s3 build flash monitor
   ```

## Expected Result

The display plays every AVI file in the card's `avi` folder in turn in a 320 x 200 window, with sound from the speaker. The serial monitor prints the frame rate.

## Troubleshooting

- `No AVI files found in /sdcard/avi`: the card needs an `avi` folder at its root.
- Your own videos must be MJPEG AVI files no larger than 320 x 200.
