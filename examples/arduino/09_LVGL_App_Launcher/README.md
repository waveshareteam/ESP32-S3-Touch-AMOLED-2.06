# 09_LVGL_App_Launcher

[中文](README_ZH.md)

Watch-style launcher modelled on the ESP-Brookesia phone `AppLauncher`, built with LVGL 9 and Arduino.

- Page 1: watch face (time from RTC/NTP, battery, Wi-Fi, location)
- Page 2: apps — AI Voice (starts the xiaozhi-esp32 firmware in voice0/voice1) and Settings
- Settings: Display, Sound, Time & Location, Battery, Wi-Fi, Software Update (OTA)
- Idle dim: after 30 s without a touch the screen drops to ~10 % brightness; the Settings sleep timeout then counts from that moment. A touch, PWR or raising the wrist (QMI8658 accelerometer) restores it; a raise also wakes a sleeping screen to the watch face

## Module layout

Each module lives in its own folder under `src/` and carries its own version in `<module>_version.h`.

| Module | Folder | Responsibility | Data partition |
| --- | --- | --- | --- |
| Core | `src/core` | Boot, LVGL port, settings (NVS), power policy, OTA engine, module registry | `nvs` |
| Board | `src/board` | CO5300 display, FT3168 touch, AXP2101 PMU, PCF85063 RTC, QMI8658 IMU | `board` |
| Audio | `src/audio` | ES8311 codec and I2S | `audio` |
| Protocol | `src/protocol` | Wi-Fi, NTP, HTTPS transport | `protocol` |
| Server | `src/server` | OTA server URL and manifest contract | `server` |
| Apps | `src/apps` | AI Voice and Settings apps | `apps` |
| Components | `src/components` | Shared UI theme, time zone table | `components` |
| Launcher | `src/launcher` | Pages, watch face, app lifecycle | `launcher` |

## Partition table

`partitions.csv` in this folder replaces the board menu partition scheme.

| Name | Offset | Size | Use |
| --- | --- | --- | --- |
| nvs | 0x9000 | 20 KB | Settings, Wi-Fi, server URL, installed data versions (shared with AI Voice) |
| otadata | 0xE000 | 8 KB | Active app slot and rollback state |
| app0 / app1 | 0x10000 / 0x250000 | 2.25 MB each | Launcher A/B firmware slots |
| voice0 / voice1 | 0x490000 / 0x7D0000 | 3.25 MB each | AI Voice (xiaozhi) A/B firmware slots |
| assets | 0xB10000 | 3 MB | AI Voice fonts, wake word and emoji |
| board, protocol, server | 0xE10000, 0xE20000, 0xE30000 | 64 KB each | Module data |
| audio, components | 0xE40000, 0xF00000 | 256 KB each | Module data |
| apps, launcher | 0xE80000, 0xF40000 | 512 KB each | Module data |
| storage | 0xFC0000 | 192 KB | FAT for user files |
| coredump | 0xFF0000 | 64 KB | Crash dumps |

The AI Voice app switches the boot slot to voice0/voice1 and restarts. Pressing PWR inside
AI Voice returns to the Launcher (holding BOOT there opens its LAN update page). The Launcher OTA and rollback only use app0/app1.

All code modules are linked into one app image, so a firmware OTA replaces them together.
The per-module versions in the manifest show which modules changed. Module data partitions
can be updated on their own without touching the firmware.

A partition table cannot be changed over OTA. Flash the first build with this table over USB.

## OTA update

1. Build the sketch and create a manifest:

   ```bash
   python3 tools/make_ota_manifest.py --base-url https://example.com/fw/ --firmware build/09_LVGL_App_Launcher.ino.bin --out build/manifest.json
   ```

   Add `--data launcher=launcher.bin:2` (repeatable) to publish a module data image.
2. Upload `manifest.json` and the referenced files to an HTTPS host, for example GitHub Releases.
3. On the device, open Settings > Software Update > Edit server, enter the manifest URL,
   then use Check for updates and Install.

Safety checks:

- Only `https://` URLs are accepted, including redirects. Certificates are verified with the ESP-IDF CA bundle.
- The manifest must match this board ID and schema.
- The SHA-256 digest and size of every image are verified before an image is activated.
- A new firmware stays in pending-verify state until it has run for 15 seconds. If it crashes or
  resets earlier, the bootloader rolls back to the previous slot.
- The manifest is not signed. Protect the HTTPS host, or use Secure Boot v2 signed images for stronger guarantees.

The Wi-Fi password is stored in NVS without encryption.
