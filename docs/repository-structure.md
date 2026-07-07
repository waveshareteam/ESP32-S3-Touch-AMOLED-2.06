# Repository Structure

This repository uses the canonical Waveshare ESP32 product layout for maintained examples:

```text
examples/esp-idf/      first-party ESP-IDF projects
examples/arduino/      first-party Arduino sketches and bundled libraries
FirmWare/              checked-in factory or recovery binaries
Material/              media and product assets
Schematic/             schematic PDF
releases/              CI firmware packaging and artifact download helpers
```

The CI discovery script treats the first-party example roots as product examples and excludes examples nested inside bundled libraries.

Historical versioned paths before this migration were:

`	ext
examples/ESP-IDF-v5.4.2/
examples/Arduino-v3.2.0/
`$([Environment]::NewLine)
Use the canonical paths above for all maintained source, CI, and release packaging.

## Component Notes

Several ESP-IDF examples still contain local reusable components such as `XPowersLib`, `bsp_extra`, and Brookesia components. Prefer managed Waveshare or Espressif components when a compatible release exists for the selected ESP-IDF matrix.

Keep local `bsp_extra` code only for board-specific glue or temporary adaptation that is not ready for a shared component.