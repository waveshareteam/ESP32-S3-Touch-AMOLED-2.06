# Repository Structure

[简体中文](repository-structure_ZH.md)

This repository uses the maintained product layout:

```text
examples/esp-idf/      first-party ESP-IDF projects
examples/arduino/      first-party Arduino sketches and bundled libraries
FirmWare/              checked-in factory or recovery binaries
Material/              media and product assets
Schematic/             schematic PDF
releases/              CI firmware packaging and artifact download helpers
```

The CI discovery script treats the first-party example roots as product
examples and excludes examples nested inside bundled libraries. Historical
versioned paths are retained only as migration history; use the paths above for
maintained source, CI, and release packaging.

## Component Ownership

The bundled Arduino `XPowersLib` is retained because no equivalent managed
replacement was established for this repository. Product-local `bsp_extra`
remains because it supplies the audio/video adapter and board glue used by its
examples. The Brookesia tree is owned with the Brookesia example and is not a
generic board-support replacement.

The `FirmWare/` directory is an immutable delivery boundary for this
repository. It currently contains one checked-in binary and no source project
or release archive; binaries and artifacts are inventoried separately and
never enter the example build matrix.
