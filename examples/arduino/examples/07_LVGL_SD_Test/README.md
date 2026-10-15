# 07_LVGL_SD_Test

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Mounts the microSD card in 1-bit SD_MMC mode and shows its type, size and root directory.

## Hardware Required

- ESP32-S3-Touch-AMOLED-2.06 development board
- microSD card

## How to Run

1. Install the `esp32` board package and make [`examples/arduino/libraries/`](../../libraries/)
   available as your sketchbook `libraries` folder.
2. Select **ESP32S3 Dev Module** and set Flash Size **16MB**, Partition Scheme
   **16M Flash (3MB APP/9.9MB FATFS)**, PSRAM **OPI PSRAM**, USB CDC On Boot **Enabled**.
3. Open `07_LVGL_SD_Test.ino` and upload.

## Expected Result

The screen and the serial monitor show the card type, the card size and the files in the root directory.

## Troubleshooting

- `Card Mount Failed` or `No SD_MMC card attached`: insert a FAT-formatted card before resetting the board.
