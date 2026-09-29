#!/usr/bin/env python3
"""Build manifest.json for the 09_LVGL_App_Launcher OTA server.

Upload the manifest and every referenced file to an HTTPS host, then point the device at
the manifest URL (Settings > Software Update > Edit server).

Example:
  python3 tools/make_ota_manifest.py \
      --base-url https://github.com/<owner>/<repo>/releases/download/v1.1.0/ \
      --firmware build/09_LVGL_App_Launcher.ino.bin \
      --data launcher=assets/launcher.bin:3 \
      --out build/manifest.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

SKETCH_DIR = Path(__file__).resolve().parent.parent
SCHEMA = 1
APP_IMAGE_MAGIC = 0xE9


def read_define(path: Path, name: str) -> str:
    match = re.search(rf'#define\s+{name}\s+"([^"]*)"', path.read_text(encoding="utf-8"))
    if not match:
        raise SystemExit(f"{name} not found in {path}")
    return match.group(1)


def module_versions() -> dict[str, str]:
    versions = {}
    for header in sorted((SKETCH_DIR / "src").glob("*/*_version.h")):
        module = header.parent.name
        versions[module] = read_define(header, f"{module.upper()}_MODULE_VERSION")
    return versions


def partition_sizes() -> dict[str, tuple[str, str, int]]:
    """label -> (type, subtype, size) from partitions.csv"""
    sizes = {}
    for line in (SKETCH_DIR / "partitions.csv").read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        sizes[fields[0]] = (fields[1], fields[2], int(fields[4], 0))
    return sizes


def describe(path: Path, url: str, version: str, limit: int, label: str) -> dict:
    data = path.read_bytes()
    if len(data) == 0:
        raise SystemExit(f"{path} is empty")
    if len(data) > limit:
        raise SystemExit(f"{path} ({len(data)} bytes) does not fit partition {label} ({limit} bytes)")
    return {
        "version": version,
        "url": url,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-url", required=True, help="https:// URL prefix where the files will be hosted")
    parser.add_argument("--firmware", type=Path, help="app image (*.ino.bin, not the merged image)")
    parser.add_argument("--firmware-version", help="default: FIRMWARE_VERSION in src/core/modules.h")
    parser.add_argument("--data", action="append", default=[], metavar="LABEL=FILE:VERSION",
                        help="data partition image, may be repeated")
    parser.add_argument("--out", type=Path, default=Path("manifest.json"))
    args = parser.parse_args()

    if not args.base_url.startswith("https://"):
        raise SystemExit("--base-url must start with https://")
    base_url = args.base_url if args.base_url.endswith("/") else args.base_url + "/"

    modules_h = SKETCH_DIR / "src" / "core" / "modules.h"
    partitions = partition_sizes()
    manifest: dict = {"schema": SCHEMA, "board": read_define(modules_h, "FIRMWARE_BOARD_ID")}

    if args.firmware:
        image = args.firmware.read_bytes()[:1]
        if not image or image[0] != APP_IMAGE_MAGIC:
            raise SystemExit(f"{args.firmware} is not an ESP app image (use *.ino.bin, not *.merged.bin)")
        version = args.firmware_version or read_define(modules_h, "FIRMWARE_VERSION")
        entry = describe(args.firmware, base_url + args.firmware.name, version, partitions["app0"][2], "app0")
        entry["modules"] = module_versions()
        manifest["firmware"] = entry

    data_items = []
    for spec in args.data:
        try:
            label, rest = spec.split("=", 1)
            file_name, version = rest.rsplit(":", 1)
        except ValueError:
            raise SystemExit(f"--data expects LABEL=FILE:VERSION, got {spec!r}")
        # Module data partitions use the custom subtypes 0x40-0x4F (see partitions.csv)
        if label not in partitions or partitions[label][0] != "data" or not partitions[label][1].lower().startswith("0x4"):
            raise SystemExit(f"{label!r} is not a module data partition in partitions.csv")
        path = Path(file_name)
        item = describe(path, base_url + path.name, version, partitions[label][2], label)
        data_items.append({"partition": label, **item})
    if data_items:
        manifest["data"] = data_items

    if "firmware" not in manifest and not data_items:
        raise SystemExit("nothing to publish: pass --firmware and/or --data")

    args.out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
