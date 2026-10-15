# 08_ES8311

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Plays a built-in audio clip through the ES8311, then loops the microphone back to the speaker.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board
- Speaker

## How to Run

1. Install the `esp32` board package and make [`examples/arduino/libraries/`](../../libraries/)
   available as your sketchbook `libraries` folder.
2. Select **ESP32S3 Dev Module** and set Flash Size **16MB**, Partition Scheme
   **16M Flash (3MB APP/9.9MB FATFS)**, PSRAM **OPI PSRAM**, USB CDC On Boot **Enabled**.
3. Open `08_ES8311.ino` and upload.

## Expected Result

The speaker plays the clip once after reset, then plays back what the microphone picks up. The display stays off.

## Troubleshooting

- If the speaker howls, lower `EXAMPLE_VOICE_VOLUME` at the top of the sketch.
