#!/usr/bin/env python3
"""Check the repository's documented bilingual and local-link conventions.

This modest repository documentation checker intentionally covers only the
listed first-party pages, reciprocal language links, local links/fragments,
and homepage H2 icon symmetry. It is not a complete Markdown audit.
"""

from __future__ import annotations

import argparse
import os
import re
from pathlib import Path

PAIRS = {
    "README.md": "README_ZH.md",
    "docs/ci.md": "docs/ci_ZH.md",
    "docs/firmware.md": "docs/firmware_ZH.md",
    "docs/repository-structure.md": "docs/repository-structure_ZH.md",
    "FirmWare/README.md": "FirmWare/README_ZH.md",
    "releases/README.md": "releases/README_ZH.md",
    "CONTRIBUTING.md": "CONTRIBUTING_ZH.md",
    "SUPPORT.md": "SUPPORT_ZH.md",
    "SECURITY.md": "SECURITY_ZH.md",
    ".github/ISSUE_TEMPLATE/bug_report.md": ".github/ISSUE_TEMPLATE/bug_report_ZH.md",
    ".github/pull_request_template.md": ".github/pull_request_template_ZH.md",
}
LINK = re.compile(r"!?\[[^]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
HEADING = re.compile(r"^#{1,6}\s+(.+?)\s*$", re.MULTILINE)


def slug(text: str) -> str:
    value = re.sub(r"[`*_~]", "", text).strip().lower()
    value = re.sub(r"[^\w\-\u4e00-\u9fff ]", "", value)
    return re.sub(r"\s+", "-", value).strip("-")


def anchors(text: str) -> set[str]:
    return {slug(match.group(1)) for match in HEADING.finditer(text)}


def local_links(text: str) -> list[str]:
    return [target for target in LINK.findall(text) if not re.match(r"(?:https?:|mailto:)", target)]


def check(repo: Path, pairs: dict[str, str] = PAIRS) -> list[str]:
    errors: list[str] = []
    contents: dict[str, str] = {}
    for english, chinese in pairs.items():
        for relative in (english, chinese):
            file = repo / relative
            if not file.is_file():
                errors.append(f"missing paired page: {relative}")
            else:
                contents[relative] = file.read_text(encoding="utf-8")
        chinese_link = os.path.relpath(chinese, Path(english).parent).replace("\\", "/")
        english_link = os.path.relpath(english, Path(chinese).parent).replace("\\", "/")
        if english in contents and chinese_link not in contents[english]:
            errors.append(f"missing reciprocal language link: {english} -> {chinese}")
        if chinese in contents and english_link not in contents[chinese]:
            errors.append(f"missing reciprocal language link: {chinese} -> {english}")
    for relative, text in contents.items():
        for target in local_links(text):
            file_part, _, fragment = target.partition("#")
            target_file = (repo / relative).parent / file_part if file_part else repo / relative
            target_file = target_file.resolve()
            try:
                target_file.relative_to(repo.resolve())
            except ValueError:
                errors.append(f"repository-escaping link: {relative} -> {target}")
                continue
            if not target_file.exists():
                errors.append(f"missing local link: {relative} -> {target}")
            elif fragment and target_file.suffix.lower() == ".md":
                try:
                    target_text = target_file.read_text(encoding="utf-8")
                except UnicodeDecodeError:
                    continue
                if slug(fragment) not in anchors(target_text):
                    errors.append(f"missing local fragment: {relative} -> {target}")
    english_h2 = re.findall(r"^##\s+(\S+)", contents.get("README.md", ""), re.MULTILINE)
    chinese_h2 = re.findall(r"^##\s+(\S+)", contents.get("README_ZH.md", ""), re.MULTILINE)
    if english_h2 != chinese_h2:
        errors.append("homepage H2 icon/order mismatch")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", nargs="?", default=".")
    args = parser.parse_args()
    errors = check(Path(args.repo).resolve())
    if errors:
        print("\n".join(f"check_docs: {error}" for error in errors))
        return 1
    print("check_docs: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
