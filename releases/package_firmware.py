#!/usr/bin/env python3
"""Create flashable firmware archives from CI build outputs."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import stat
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


DEFAULT_BAUD = "460800"


def slugify(value: str) -> str:
    value = value.strip().replace("\\", "/")
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value)
    value = re.sub(r"-+", "-", value).strip("-")
    return value or "firmware"


def parse_offset(value: str) -> int:
    return int(value, 0)


def safe_project_path(project: Path, repo: Path) -> str:
    try:
        return project.resolve().relative_to(repo.resolve()).as_posix()
    except ValueError:
        return project.name


def quote_shell(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def quote_batch(value: str) -> str:
    return '"' + value.replace('"', '\\"') + '"'


def write_text(path: Path, content: str, executable: bool = False) -> None:
    path.write_text(content, encoding="utf-8", newline="\n")
    if executable:
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def copy_file(src: Path, firmware_dir: Path, offset: str | None = None) -> str:
    if not src.exists():
        raise FileNotFoundError(f"missing firmware file: {src}")
    prefix = f"{offset.lower()}_" if offset else ""
    dst_name = slugify(prefix + src.name)
    dst = firmware_dir / dst_name
    shutil.copy2(src, dst)
    return f"bin/{dst_name}"


def esp_idf_flash_entries(build_dir: Path, firmware_dir: Path) -> tuple[list[str], list[dict[str, str]], dict]:
    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.exists():
        raise FileNotFoundError(f"missing ESP-IDF flasher args: {flasher_args_path}")

    data = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    flash_files = data.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError(f"no flash_files found in {flasher_args_path}")

    entries: list[dict[str, str]] = []
    command_pairs: list[str] = []
    for offset, rel_path in sorted(flash_files.items(), key=lambda item: parse_offset(item[0])):
        src = Path(rel_path)
        if not src.is_absolute():
            src = build_dir / src
        copied = copy_file(src, firmware_dir, offset)
        rel_source = Path(rel_path)
        source_name = rel_source.name if rel_source.is_absolute() else rel_source.as_posix()
        entries.append({"offset": offset, "file": copied, "source": source_name})
        command_pairs.extend([offset, copied])

    return command_pairs, entries, data


def arduino_flash_entries(build_dir: Path, firmware_dir: Path) -> tuple[list[str], list[dict[str, str]]]:
    bins = sorted(build_dir.rglob("*.bin"), key=lambda path: path.as_posix().lower())
    if not bins:
        raise FileNotFoundError(f"no Arduino .bin files found in {build_dir}")

    merged = next((path for path in bins if path.name.endswith(".merged.bin")), None)
    if merged:
        copied = copy_file(merged, firmware_dir)
        return ["0x0", copied], [{"offset": "0x0", "file": copied, "source": merged.name}]

    selected: list[tuple[str, Path]] = []
    for path in bins:
        name = path.name
        if name.endswith(".bootloader.bin"):
            selected.append(("0x0", path))
        elif name.endswith(".partitions.bin"):
            selected.append(("0x8000", path))
        elif name == "boot_app0.bin" or name.endswith(".boot_app0.bin"):
            selected.append(("0xe000", path))
        elif not any(token in name for token in (".bootloader.", ".partitions.", ".merged.")):
            selected.append(("0x10000", path))

    if not selected:
        raise ValueError(f"could not infer Arduino flash layout from {build_dir}")

    entries: list[dict[str, str]] = []
    command_pairs: list[str] = []
    for offset, src in sorted(selected, key=lambda item: parse_offset(item[0])):
        copied = copy_file(src, firmware_dir, offset)
        entries.append({"offset": offset, "file": copied, "source": src.name})
        command_pairs.extend([offset, copied])
    return command_pairs, entries


def build_esptool_prefix(chip: str, before: str, after: str) -> list[str]:
    return [
        "python",
        "-m",
        "esptool",
        "--chip",
        chip,
        "--port",
        "$PORT",
        "--baud",
        DEFAULT_BAUD,
        "--before",
        before,
        "--after",
        after,
        "write_flash",
    ]


def shell_command(parts: Iterable[str]) -> str:
    return " ".join("$PORT" if part == "$PORT" else quote_shell(part) for part in parts)


def batch_command(parts: Iterable[str]) -> str:
    return " ".join("%PORT%" if part == "$PORT" else quote_batch(part) for part in parts)


def write_flash_helpers(package_dir: Path, command: list[str], artifact_name: str) -> None:
    shell = f"""#!/usr/bin/env sh
set -eu
PORT="${{1:-}}"
if [ -z "$PORT" ]; then
    echo "Usage: $0 /dev/ttyUSB0"
    exit 2
fi
cd "$(dirname "$0")"
{shell_command(command)}
"""
    batch = f"""@echo off
set PORT=%1
if "%PORT%"=="" (
  echo Usage: flash.bat COMx
  exit /b 2
)
cd /d %~dp0
{batch_command(command)}
"""
    args_txt = " ".join("<PORT>" if part == "$PORT" else part for part in command) + "\n"
    write_text(package_dir / "flash.sh", shell, executable=True)
    write_text(package_dir / "flash.bat", batch)
    write_text(package_dir / "flash_args.txt", args_txt)
    write_text(
        package_dir / "README.md",
        f"""# {artifact_name}

Install esptool if needed:

```bash
python -m pip install esptool
```

Flash from this directory:

```bash
./flash.sh /dev/ttyUSB0
```

On Windows:

```bat
flash.bat COMx
```
""",
    )


def create_zip(package_dir: Path, zip_path: Path) -> None:
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(package_dir.rglob("*"), key=lambda item: item.as_posix()):
            if path.is_file():
                archive.write(path, path.relative_to(package_dir.parent).as_posix())


def package(args: argparse.Namespace) -> Path:
    repo = Path.cwd()
    project = Path(args.project)
    build_dir = Path(args.build_dir)
    output_dir = Path(args.output_dir)
    artifact_name = slugify(args.name or f"{project.name}-{args.framework_version or args.framework}")
    package_dir = output_dir / artifact_name
    firmware_dir = package_dir / "bin"

    if package_dir.exists():
        shutil.rmtree(package_dir)
    firmware_dir.mkdir(parents=True, exist_ok=True)

    if args.framework == "esp-idf":
        command_pairs, files, flasher_args = esp_idf_flash_entries(build_dir, firmware_dir)
        extra_args = flasher_args.get("extra_esptool_args", {})
        chip = args.target or extra_args.get("chip") or "esp32s3"
        before = extra_args.get("before", "default_reset")
        after = extra_args.get("after", "hard_reset")
        write_flash_args = [str(item) for item in flasher_args.get("write_flash_args", [])]
    else:
        command_pairs, files = arduino_flash_entries(build_dir, firmware_dir)
        chip = args.target or "esp32s3"
        before = "default_reset"
        after = "hard_reset"
        write_flash_args = []

    command = build_esptool_prefix(chip, before, after) + write_flash_args + command_pairs
    manifest = {
        "name": artifact_name,
        "framework": args.framework,
        "framework_version": args.framework_version,
        "target": chip,
        "project": safe_project_path(project, repo),
        "git_sha": args.git_sha,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "baud": DEFAULT_BAUD,
        "files": files,
        "flash_command": " ".join("<PORT>" if item == "$PORT" else item for item in command),
    }
    write_text(package_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    write_flash_helpers(package_dir, command, artifact_name)

    output_dir.mkdir(parents=True, exist_ok=True)
    zip_path = output_dir / f"{artifact_name}.zip"
    create_zip(package_dir, zip_path)
    print(zip_path.as_posix())
    return zip_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", choices=("esp-idf", "arduino"), required=True)
    parser.add_argument("--project", required=True, help="Repo-relative project or sketch path.")
    parser.add_argument("--build-dir", required=True, help="Build output directory.")
    parser.add_argument("--output-dir", default="releases/dist")
    parser.add_argument("--name", help="Firmware archive name.")
    parser.add_argument("--framework-version", help="ESP-IDF tag or Arduino core version.")
    parser.add_argument("--target", default="esp32s3")
    parser.add_argument("--git-sha", default="")
    args = parser.parse_args()
    try:
        package(args)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
