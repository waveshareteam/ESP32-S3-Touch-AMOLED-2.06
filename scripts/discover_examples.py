#!/usr/bin/env python3
"""Discover first-party examples for CI."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def normalize(value: str) -> str:
    return value.replace("\\", "/").strip("/")


def slugify(value: str) -> str:
    value = normalize(value)
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value)
    value = re.sub(r"-+", "-", value).strip("-")
    return value or "example"


def selector_matches(entry: dict[str, str], selector: str) -> bool:
    if not selector or selector == "all":
        return True
    selector = normalize(selector)
    path = normalize(entry["path"])
    name = entry["name"]
    return (
        selector == name
        or selector == path
        or path.startswith(selector + "/")
        or selector in path.split("/")
    )


def sorted_unique(paths: list[Path]) -> list[Path]:
    return sorted(dict.fromkeys(paths), key=lambda item: item.as_posix().lower())


def sorted_unique_pairs(paths: list[tuple[Path, Path]]) -> list[tuple[Path, Path]]:
    unique: dict[str, tuple[Path, Path]] = {}
    for sketch_root, library_root in paths:
        unique[sketch_root.as_posix()] = (sketch_root, library_root)
    return [unique[key] for key in sorted(unique, key=str.lower)]


def is_esp_idf_project(path: Path) -> bool:
    return (path / "CMakeLists.txt").is_file() and (path / "main").is_dir()


def esp_idf_roots(repo: Path) -> list[Path]:
    roots: list[Path] = []
    canonical = repo / "examples" / "esp-idf"
    if canonical.is_dir():
        roots.append(canonical)


    return sorted_unique(roots)


def discover_esp_idf(repo: Path) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    for root in esp_idf_roots(repo):
        if is_esp_idf_project(root):
            entries.append({"name": slugify(root.name), "path": root.relative_to(repo).as_posix()})
        for project in sorted(root.iterdir(), key=lambda item: item.name.lower()):
            if project.is_dir() and is_esp_idf_project(project):
                entries.append({"name": slugify(project.name), "path": project.relative_to(repo).as_posix()})
    return entries


def arduino_roots(repo: Path) -> list[tuple[Path, Path]]:
    roots: list[tuple[Path, Path]] = []
    canonical = repo / "examples" / "arduino"
    if canonical.is_dir():
        sketch_root = canonical / "examples" if (canonical / "examples").is_dir() else canonical
        library_root = canonical / "libraries"
        roots.append((sketch_root, library_root if library_root.is_dir() else canonical))


    return sorted_unique_pairs(roots)


def discover_arduino(repo: Path) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    seen: set[str] = set()
    for sketch_root, library_root in arduino_roots(repo):
        for ino in sorted(sketch_root.glob("*/*.ino"), key=lambda item: item.as_posix().lower()):
            sketch_dir = ino.parent
            rel = sketch_dir.relative_to(repo).as_posix()
            if rel in seen:
                continue
            seen.add(rel)
            entries.append(
                {
                    "name": slugify(sketch_dir.name),
                    "path": rel,
                    "libraries": library_root.relative_to(repo).as_posix(),
                }
            )
    return entries


def build_matrix(args: argparse.Namespace) -> dict[str, list[dict[str, str]]]:
    repo = Path(args.repo).resolve()
    selector = normalize(args.selector)
    if args.surface == "esp-idf":
        projects = [entry for entry in discover_esp_idf(repo) if selector_matches(entry, selector)]
        versions = [item.strip() for item in args.idf_versions.split(",") if item.strip()]
        include = [entry | {"idf": version} for entry in projects for version in versions]
    else:
        sketches = [entry for entry in discover_arduino(repo) if selector_matches(entry, selector)]
        include = [entry | {"core": args.arduino_core, "fqbn": args.fqbn} for entry in sketches]
    return {"include": include}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=".")
    parser.add_argument("--surface", choices=("esp-idf", "arduino"), required=True)
    parser.add_argument("--selector", default="all")
    parser.add_argument("--idf-versions", default="v5.5.4,v6.0.2")
    parser.add_argument("--arduino-core", default="3.3.10")
    parser.add_argument("--fqbn", default="esp32:esp32:esp32s3")
    parser.add_argument("--github-output")
    args = parser.parse_args()

    matrix = build_matrix(args)
    output = json.dumps(matrix, separators=(",", ":"))
    count = len(matrix["include"])
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as fh:
            fh.write(f"matrix={output}\n")
            fh.write(f"count={count}\n")
    else:
        print(output)


if __name__ == "__main__":
    main()