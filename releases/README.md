# Release Scripts

This directory contains helper scripts for packaging build outputs into flashable firmware archives.

## ESP-IDF

Build an example first, then package the generated build directory:

```bash
idf.py -C examples/esp-idf/02_lvgl_demo_v9 -B build/02_lvgl_demo_v9-v6.0.2 set-target esp32s3 build
python3 releases/package_firmware.py \
  --framework esp-idf \
  --project examples/esp-idf/02_lvgl_demo_v9 \
  --build-dir build/02_lvgl_demo_v9-v6.0.2 \
  --framework-version v6.0.2 \
  --target esp32s3
```

The script reads ESP-IDF's `flasher_args.json`, combines the required binary segments into `bin/combined.bin`, writes flash helper scripts, and creates a zip under `releases/dist/`.

## Arduino

Export binaries into a stable output directory, then package them:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3 \
  --libraries examples/arduino/libraries \
  --export-binaries \
  --output-dir build/01_HelloWorld-3.3.10 \
  examples/arduino/01_HelloWorld

python3 releases/package_firmware.py \
  --framework arduino \
  --project examples/arduino/01_HelloWorld \
  --build-dir build/01_HelloWorld-3.3.10 \
  --framework-version 3.3.10 \
  --target esp32s3
```

Each archive includes `manifest.json`, `flash.sh`, `flash.bat`, `flash_args.txt`, and a flashable `bin/combined.bin` image.

## Download CI Artifacts

After a CI run completes, download and extract firmware artifacts with:

```bash
python3 releases/download_artifacts.py --run-id <run-id> --clean
```

If `--run-id` is omitted, the script finds the latest successful `examples.yml` run for the current branch:

```bash
python3 releases/download_artifacts.py --clean
```

The extracted firmware is written to `releases/downloads/run-<run-id>/`. Each artifact gets its own folder, for example `firmware-esp-idf-02_lvgl_demo_v9-v6.0.2/`, with `flash.sh`, `flash.bat`, `manifest.json`, and `bin/combined.bin` ready for flashing.

Use `--artifact <name>` to download one firmware package, or `--pattern "firmware-esp-idf-*v6.0.2"` to filter by glob pattern. The script uses `GH_TOKEN`, `GITHUB_TOKEN`, or `gh auth token` for GitHub artifact access. When GitHub CLI is installed, artifact downloads use `gh run download` so `gh auth login` can be reused directly.
## Tag Builds

Pushing a `v*` tag publishes the generated `firmware-*` archives to the matching GitHub Release. The release assets are zip archives; each archive contains a single combined BIN image at `bin/combined.bin`.
