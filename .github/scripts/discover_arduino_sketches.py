#!/usr/bin/env python3
"""Discover first-party Arduino sketches that should be built by CI."""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import subprocess
import sys
from pathlib import Path


GLOBAL_SKETCH_PATTERNS = (
    ".github/workflows/arduino-examples.yml",
    ".github/scripts/discover_arduino_sketches.py",
    "examples/arduino/libraries/**",
    "examples/Arduino-*/libraries/**",
)


def run_git(args: list[str]) -> list[str]:
    result = subprocess.run(
        ["git", *args],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def discover_roots() -> list[Path]:
    roots: list[Path] = []
    examples = Path("examples")
    if not examples.is_dir():
        return roots

    canonical = examples / "arduino"
    if canonical.is_dir():
        roots.append(canonical / "examples" if (canonical / "examples").is_dir() else canonical)

    for path in examples.iterdir():
        normalized_name = path.name.lower().replace("_", "-")
        if path.is_dir() and normalized_name.startswith("arduino-"):
            first_party = path / "examples"
            if first_party.is_dir():
                roots.append(first_party)

    return sorted(dict.fromkeys(roots), key=lambda item: item.as_posix().lower())


def list_sketches() -> list[str]:
    sketches: list[str] = []
    for root in discover_roots():
        for ino in root.glob("*/*.ino"):
            if ino.parent.name == ino.stem:
                sketches.append(ino.parent.as_posix())
            else:
                sketches.append(ino.as_posix())
    return sorted(dict.fromkeys(sketches))


def normalize_sketch(value: str, known_sketches: set[str]) -> str:
    value = value.strip().strip("/")
    if not value or value == "all":
        return value

    normalized = Path(value).as_posix()
    if normalized in known_sketches:
        return normalized

    matches = [
        sketch
        for sketch in known_sketches
        if Path(sketch).name == value or Path(sketch).stem == value
    ]
    if len(matches) == 1:
        return matches[0]

    return normalized


def discover_from_paths(paths: list[str], known_sketches: set[str]) -> list[str]:
    selected: set[str] = set()
    roots = discover_roots()

    for changed_path in paths:
        changed_path = changed_path.strip().strip("/")
        if any(fnmatch.fnmatch(changed_path, pattern) for pattern in GLOBAL_SKETCH_PATTERNS):
            selected.update(known_sketches)
            continue

        for sketch in known_sketches:
            sketch_path = Path(sketch)
            sketch_dir = sketch if sketch_path.suffix != ".ino" else sketch_path.parent.as_posix()
            if changed_path == sketch or changed_path.startswith(sketch_dir + "/"):
                selected.add(sketch)
                break
        else:
            for root in roots:
                root_path = root.as_posix()
                if changed_path == root_path or changed_path.startswith(root_path + "/"):
                    selected.update(known_sketches)
                    break

    return sorted(selected)


def discover_changed_sketches(base_ref: str | None, head_ref: str, known_sketches: set[str]) -> list[str]:
    if base_ref:
        diff_args = ["diff", "--name-only", f"{base_ref}...{head_ref}"]
    else:
        diff_args = ["diff-tree", "--no-commit-id", "--name-only", "-r", head_ref]

    return discover_from_paths(run_git(diff_args), known_sketches)


def github_output(name: str, value: str) -> None:
    output_path = os.environ.get("GITHUB_OUTPUT")
    if output_path:
        with open(output_path, "a", encoding="utf-8") as output:
            output.write(f"{name}={value}\n")


def build_matrix(selected: list[str]) -> dict[str, list[dict[str, str]]]:
    return {"include": [{"sketch": sketch} for sketch in selected]}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-ref")
    parser.add_argument("--head-ref", default="HEAD")
    parser.add_argument("--sketch", default="")
    parser.add_argument(
        "--fallback-all",
        action="store_true",
        help="Build all first-party sketches when no changed sketch is detected.",
    )
    args = parser.parse_args()

    known_sketches = set(list_sketches())
    requested_sketch = normalize_sketch(args.sketch, known_sketches)

    if requested_sketch == "all":
        selected = sorted(known_sketches)
    elif requested_sketch:
        if requested_sketch not in known_sketches:
            print(f"Unknown Arduino sketch: {args.sketch}", file=sys.stderr)
            print("Known sketches:", file=sys.stderr)
            for sketch in sorted(known_sketches):
                print(f"  {sketch}", file=sys.stderr)
            return 1
        selected = [requested_sketch]
    else:
        selected = discover_changed_sketches(args.base_ref, args.head_ref, known_sketches)
        if args.fallback_all and not selected:
            selected = sorted(known_sketches)

    matrix = build_matrix(selected)
    matrix_json = json.dumps(matrix, separators=(",", ":"))
    has_sketches = "true" if selected else "false"

    github_output("matrix", matrix_json)
    github_output("has_sketches", has_sketches)
    github_output("sketches", ",".join(selected))

    print(matrix_json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
