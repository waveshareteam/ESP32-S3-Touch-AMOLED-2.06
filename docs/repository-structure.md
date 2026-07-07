# Repository Structure

This repository currently preserves the product's historical versioned example layout:

```text
examples/ESP-IDF-v5.4.2/          first-party ESP-IDF projects
examples/Arduino-v3.2.0/examples/ first-party Arduino sketches
examples/Arduino-v3.2.0/libraries/ bundled Arduino libraries
FirmWare/                         checked-in factory or recovery binaries
Material/                         media and product assets
Schematic/                        schematic PDF
```

The CI discovery scripts treat the first-party example roots as product examples and exclude examples nested inside bundled libraries.

Future repository normalization can move first-party projects toward:

```text
examples/esp-idf/
examples/arduino/
firmware/
docs/
config/
```

Keep short compatibility notes or redirects if public links to the historical paths must remain valid.

## Component Notes

Several ESP-IDF examples still contain local reusable components such as `XPowersLib`, `bsp_extra`, and Brookesia components. Prefer managed Waveshare or Espressif components when a compatible release exists for the selected ESP-IDF matrix.

Keep local `bsp_extra` code only for board-specific glue or temporary adaptation that is not ready for a shared component.
