#!/usr/bin/env python3
"""Discover ESP-IDF examples that should be built by CI."""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import subprocess
import sys
from pathlib import Path


GLOBAL_EXAMPLE_PATTERNS = (
    ".github/workflows/esp-idf-examples.yml",
    ".github/workflows/esp-idf-projects.yml",
    ".github/scripts/discover_esp_idf_examples.py",
    ".github/scripts/discover_esp_idf_projects.py",
    "config/*.defaults",
    "config/**/*.defaults",
)
DEFAULT_IDF_VERSIONS = ("v5.5.4", "v6.0.1")
IDF_V5_ONLY_KEYWORDS = (
    "audio",
    "brookesia",
    "camera",
    "color",
    "display",
    "i2s",
    "lcd",
    "lvgl",
    "mp4",
    "rgb",
    "usb",
    "video",
    "wifi",
)


def run_git(args: list[str]) -> list[str]:
    result = subprocess.run(
        ["git", *args],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def is_project(path: Path) -> bool:
    return (path / "CMakeLists.txt").is_file() and (path / "main").is_dir()


def discover_roots() -> list[Path]:
    roots: list[Path] = []
    examples = Path("examples")
    if examples.is_dir():
        for path in examples.iterdir():
            if path.is_dir() and path.name.lower().replace("_", "-").startswith("esp-idf"):
                roots.append(path)

    for firmware_root in (Path("firmware"), Path("Firmware"), Path("FirmWare")):
        if firmware_root.is_dir():
            roots.append(firmware_root)

    return sorted(dict.fromkeys(roots), key=lambda item: item.as_posix().lower())


def list_examples() -> list[str]:
    examples: list[str] = []
    for root in discover_roots():
        if is_project(root):
            examples.append(root.as_posix())
        for path in root.iterdir():
            if path.is_dir() and is_project(path):
                examples.append(path.as_posix())
    return sorted(dict.fromkeys(examples))


def normalize_example(value: str, known_examples: set[str]) -> str:
    value = value.strip().strip("/")
    if not value or value == "all":
        return value

    normalized = Path(value).as_posix()
    if normalized in known_examples:
        return normalized

    matches = [example for example in known_examples if Path(example).name == value]
    if len(matches) == 1:
        return matches[0]

    return normalized


def discover_from_paths(paths: list[str], known_examples: set[str]) -> list[str]:
    selected: set[str] = set()
    roots = discover_roots()

    for changed_path in paths:
        changed_path = changed_path.strip().strip("/")
        if any(fnmatch.fnmatch(changed_path, pattern) for pattern in GLOBAL_EXAMPLE_PATTERNS):
            selected.update(known_examples)
            continue

        for example in known_examples:
            if changed_path == example or changed_path.startswith(example + "/"):
                selected.add(example)
                break
        else:
            for root in roots:
                root_path = root.as_posix()
                if changed_path == root_path or changed_path.startswith(root_path + "/"):
                    selected.update(known_examples)
                    break

    return sorted(selected)


def discover_changed_examples(base_ref: str | None, head_ref: str, known_examples: set[str]) -> list[str]:
    if base_ref:
        diff_args = ["diff", "--name-only", f"{base_ref}...{head_ref}"]
    else:
        diff_args = ["diff-tree", "--no-commit-id", "--name-only", "-r", head_ref]

    return discover_from_paths(run_git(diff_args), known_examples)


def github_output(name: str, value: str) -> None:
    output_path = os.environ.get("GITHUB_OUTPUT")
    if output_path:
        with open(output_path, "a", encoding="utf-8") as output:
            output.write(f"{name}={value}\n")


def versions_for_example(example: str) -> tuple[str, ...]:
    normalized = example.lower().replace("-", "_")
    if any(keyword in normalized for keyword in IDF_V5_ONLY_KEYWORDS):
        return ("v5.5.4",)
    return DEFAULT_IDF_VERSIONS


def build_matrix(selected: list[str]) -> dict[str, list[dict[str, str]]]:
    return {
        "include": [
            {"example": example, "idf_version": idf_version}
            for example in selected
            for idf_version in versions_for_example(example)
        ]
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-ref")
    parser.add_argument("--head-ref", default="HEAD")
    parser.add_argument("--example", default="")
    parser.add_argument(
        "--fallback-all",
        action="store_true",
        help="Build all examples when no changed example is detected.",
    )
    args = parser.parse_args()

    known_examples = set(list_examples())
    requested_example = normalize_example(args.example, known_examples)

    if requested_example == "all":
        selected = sorted(known_examples)
    elif requested_example:
        if requested_example not in known_examples:
            print(f"Unknown ESP-IDF example: {args.example}", file=sys.stderr)
            print("Known examples:", file=sys.stderr)
            for example in sorted(known_examples):
                print(f"  {example}", file=sys.stderr)
            return 1
        selected = [requested_example]
    else:
        selected = discover_changed_examples(args.base_ref, args.head_ref, known_examples)
        if args.fallback_all and not selected:
            selected = sorted(known_examples)

    matrix = build_matrix(selected)
    matrix_json = json.dumps(matrix, separators=(",", ":"))
    has_examples = "true" if selected else "false"

    github_output("matrix", matrix_json)
    github_output("has_examples", has_examples)
    github_output("examples", ",".join(selected))

    print(matrix_json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())