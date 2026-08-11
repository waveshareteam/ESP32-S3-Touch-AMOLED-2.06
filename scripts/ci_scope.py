#!/usr/bin/env python3
"""Classify repository changes for example CI routing.

This is a repository CI routing helper, not a complete policy or documentation
audit.  It accepts Git name-status data and emits GitHub Actions outputs.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import PurePosixPath


@dataclass
class Scope:
    esp_idf: set[str] = field(default_factory=set)
    arduino: set[str] = field(default_factory=set)
    esp_all: bool = False
    arduino_all: bool = False
    firmware: list[str] = field(default_factory=list)
    unknown: list[str] = field(default_factory=list)

    def mark_all(self) -> None:
        self.esp_all = True
        self.arduino_all = True


def normalize(path: str) -> str:
    return path.replace("\\", "/").strip("/")


def parse_name_status(data: bytes) -> list[str]:
    """Return every affected path, retaining both sides of renames/deletions."""
    if not data:
        raise ValueError("empty changed-file data")
    if b"\0" not in data:
        paths: list[str] = []
        for line in data.decode("utf-8", "surrogateescape").splitlines():
            fields = line.split("\t")
            if len(fields) < 2 or not fields[0]:
                raise ValueError("expected git --name-status data")
            paths.append(normalize(fields[1]))
            if fields[0][0] in {"R", "C"}:
                if len(fields) < 3:
                    raise ValueError("rename/copy status missing destination path")
                paths.append(normalize(fields[2]))
        if not paths:
            raise ValueError("no changed paths")
        return paths
    fields = [item.decode("utf-8", "surrogateescape") for item in data.split(b"\0") if item]
    paths: list[str] = []
    index = 0
    while index < len(fields):
        field = fields[index]
        # `git diff --name-status -z` emits STATUS NUL PATH NUL (and two
        # paths for renames/copies); tolerate the tab form used by fixtures.
        if "\t" in field:
            status, path = field.split("\t", 1)
        else:
            status = field
            index += 1
            if index >= len(fields):
                raise ValueError("status missing path")
            path = fields[index]
        if not status:
            raise ValueError("missing name-status code")
        paths.append(normalize(path))
        index += 1
        if status[0] in {"R", "C"}:
            if index >= len(fields):
                raise ValueError("rename/copy status missing destination path")
            paths.append(normalize(fields[index]))
            index += 1
    if not paths:
        raise ValueError("no changed paths")
    return paths


def project_for(path: str, surface: str) -> str | None:
    parts = PurePosixPath(path).parts
    prefix = ("examples", surface)
    if tuple(parts[:2]) != prefix or len(parts) < 3:
        return None
    if surface == "arduino" and parts[2] == "libraries":
        return None
    return "/".join(parts[:3])


def classify(paths: list[str]) -> Scope:
    scope = Scope()
    for path in paths:
        lower = path.lower()
        if path.startswith("FirmWare/"):
            scope.firmware.append(path)
            continue
        if path.startswith(("Material/", "Schematic/")):
            continue
        if lower.endswith((".md", ".markdown", ".txt")):
            continue
        if path.startswith((".github/workflows/", "scripts/", "tests/", "config/")):
            scope.mark_all()
            continue
        if path.startswith("releases/"):
            if lower.endswith((".py", ".sh", ".yml", ".yaml", ".json")):
                scope.mark_all()
            continue
        if path.startswith("examples/arduino/libraries/"):
            scope.arduino_all = True
            continue
        if path.startswith(("examples/esp-idf/common/", "examples/esp-idf/components/")):
            scope.esp_all = True
            continue
        esp_project = project_for(path, "esp-idf")
        if esp_project:
            scope.esp_idf.add(esp_project)
            continue
        arduino_project = project_for(path, "arduino")
        if arduino_project:
            scope.arduino.add(arduino_project)
            continue
        scope.unknown.append(path)
        scope.mark_all()
    return scope


def payload(scope: Scope) -> dict[str, str]:
    def mode(all_selected: bool, paths: set[str]) -> str:
        return "all" if all_selected else ("selected" if paths else "none")

    return {
        "esp_idf_mode": mode(scope.esp_all, scope.esp_idf),
        "arduino_mode": mode(scope.arduino_all, scope.arduino),
        "esp_idf_paths": json.dumps(sorted(scope.esp_idf), separators=(",", ":")),
        "arduino_paths": json.dumps(sorted(scope.arduino), separators=(",", ":")),
        "firmware_paths": json.dumps(sorted(scope.firmware), separators=(",", ":")),
        "unknown_paths": json.dumps(sorted(scope.unknown), separators=(",", ":")),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name-status-file", type=argparse.FileType("rb"), default=sys.stdin.buffer)
    parser.add_argument("--github-output", type=argparse.FileType("a", encoding="utf-8"))
    args = parser.parse_args()
    try:
        result = payload(classify(parse_name_status(args.name_status_file.read())))
    except ValueError as exc:
        print(f"ci_scope: {exc}", file=sys.stderr)
        return 2
    lines = [f"{key}={value}" for key, value in result.items()]
    if args.github_output:
        args.github_output.write("\n".join(lines) + "\n")
    else:
        print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
