#!/usr/bin/env python3
"""Check the repository's documented bilingual and local-link conventions.

This modest repository documentation checker intentionally covers only the
listed first-party pages, reciprocal language links, local links/fragments,
and homepage H2 icon symmetry. It is not a complete Markdown audit.
"""

from __future__ import annotations

import argparse
import json
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
HTML_IMAGE = re.compile(r"<img\b(?P<attributes>[^>]*)>", re.IGNORECASE)
HTML_ATTRIBUTE = re.compile(r'''([\w:-]+)\s*=\s*["']([^"']*)["']''')
HTML_LINK = re.compile(r'''<a\b[^>]*\bhref\s*=\s*["']([^"']+)["']''', re.IGNORECASE)

QUICK_LINK_TARGETS = {
    "product": re.compile(r"https://www\.waveshare\.(?:com|net)/", re.IGNORECASE),
    "documentation": re.compile(r"^docs/ci(?:_ZH)?\.md$"),
    "firmware": re.compile(r"^FirmWare/README(?:_ZH)?\.md$"),
    "esp_idf": re.compile(r"^examples/esp-idf/?$"),
    "arduino": re.compile(r"^examples/arduino/?$"),
}
BADGE_ALT_TEXT = {
    "build": "Build Examples",
    "release": "Latest Release",
    "license": "License",
}
COMPONENT_NAMES = {
    "centered_header",
    "html_h1",
    "subtitle",
    "badges",
    "language_switch",
    "quick_links",
    "hero_image",
    "separator",
    "h2",
}


def slug(text: str) -> str:
    value = re.sub(r"[`*_~]", "", text).strip().lower()
    value = re.sub(r"[^\w\-\u4e00-\u9fff ]", "", value)
    return re.sub(r"\s+", "-", value).strip("-")


def anchors(text: str) -> set[str]:
    return {slug(match.group(1)) for match in HEADING.finditer(text)}


def local_links(text: str) -> list[str]:
    return [target for target in LINK.findall(text) if not re.match(r"(?:https?:|mailto:)", target)]


def load_homepage_pairs(config_path: Path) -> list[dict[str, object]]:
    """Load the small homepage-contract subset used by this checker."""
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read config: {error}") from error
    if not isinstance(config, dict):
        raise ValueError("config root must be an object")
    homepage_pairs = config.get("homepage_pairs")
    if not isinstance(homepage_pairs, list) or not homepage_pairs:
        raise ValueError("config homepage_pairs must be a non-empty list")
    for index, homepage in enumerate(homepage_pairs):
        if not isinstance(homepage, dict):
            raise ValueError(f"config homepage_pairs[{index}] must be an object")
        for key in ("english", "chinese", "profile"):
            if not isinstance(homepage.get(key), str) or not homepage[key]:
                raise ValueError(f"config homepage_pairs[{index}].{key} must be a non-empty string")
        if homepage["profile"] not in {"single-product", "multi-product-hub", "auto"}:
            raise ValueError(f"config homepage_pairs[{index}].profile is invalid")
        for key in ("required_components", "required_quick_links", "required_badges", "required_h2_icons"):
            if not isinstance(homepage.get(key), list) or not all(isinstance(value, str) for value in homepage[key]):
                raise ValueError(f"config homepage_pairs[{index}].{key} must be a string list")
        unknown_components = set(homepage["required_components"]) - COMPONENT_NAMES
        if unknown_components:
            raise ValueError(f"config homepage_pairs[{index}] has unknown components: {sorted(unknown_components)}")
        unknown_links = set(homepage["required_quick_links"]) - set(QUICK_LINK_TARGETS)
        if unknown_links:
            raise ValueError(f"config homepage_pairs[{index}] has unknown quick links: {sorted(unknown_links)}")
        unknown_badges = set(homepage["required_badges"]) - set(BADGE_ALT_TEXT)
        if unknown_badges:
            raise ValueError(f"config homepage_pairs[{index}] has unknown badges: {sorted(unknown_badges)}")
    return homepage_pairs


def image_attributes(match: re.Match[str]) -> dict[str, str]:
    return {name.lower(): value for name, value in HTML_ATTRIBUTE.findall(match.group("attributes"))}


def check_homepage(
    repo: Path, relative: str, text: str, counterpart: str, homepage: dict[str, object]
) -> list[str]:
    errors: list[str] = []
    header = re.search(r"<div\s+align=[\"']center[\"'][^>]*>", text, re.IGNORECASE)
    header_end = text.find("</div>", header.end()) if header else -1
    if not header or header_end < 0:
        return [f"missing centered header: {relative}"]
    header_text = text[header.start() : header_end + len("</div>")]
    header_offset = header.start()
    positions: dict[str, int | None] = {"centered_header": header.start()}
    h1 = re.search(r"<h1>\s*\S.*?</h1>", header_text, re.IGNORECASE | re.DOTALL)
    subtitle = re.search(r"<strong>\s*\S.*?</strong>", header_text, re.IGNORECASE | re.DOTALL)
    positions["html_h1"] = header_offset + h1.start() if h1 else None
    positions["subtitle"] = header_offset + subtitle.start() if subtitle else None

    image_matches = list(HTML_IMAGE.finditer(header_text))
    badge_positions: list[int] = []
    for role in homepage["required_badges"]:
        badge = next(
            (
                match
                for match in image_matches
                if image_attributes(match).get("alt") == BADGE_ALT_TEXT[role]
            ),
            None,
        )
        if badge is None:
            errors.append(f"missing {role} badge: {relative}")
        else:
            badge_positions.append(header_offset + badge.start())
    positions["badges"] = min(badge_positions) if badge_positions else None

    language_link = re.search(
        rf'''<a\b[^>]*\bhref\s*=\s*["']{re.escape(counterpart)}["']''', header_text, re.IGNORECASE
    )
    positions["language_switch"] = header_offset + language_link.start() if language_link else None

    links = [(match.group(1), header_offset + match.start()) for match in HTML_LINK.finditer(header_text)]
    quick_positions: list[int] = []
    for role in homepage["required_quick_links"]:
        quick_link = next((position for href, position in links if QUICK_LINK_TARGETS[role].search(href)), None)
        if quick_link is None:
            errors.append(f"missing {role} quick link: {relative}")
        else:
            quick_positions.append(quick_link)
    positions["quick_links"] = min(quick_positions) if quick_positions else None

    hero_position: int | None = None
    for image in image_matches:
        attributes = image_attributes(image)
        source = attributes.get("src", "")
        if not source or re.match(r"(?:https?:|data:)", source, re.IGNORECASE) or "badge" in source.lower():
            continue
        if not attributes.get("alt", "").strip():
            errors.append(f"hero image has empty alt text: {relative}")
            continue
        target = ((repo / relative).parent / source).resolve()
        try:
            target.relative_to(repo.resolve())
        except ValueError:
            errors.append(f"repository-escaping hero image: {relative} -> {source}")
            continue
        if not target.is_file():
            errors.append(f"missing hero image target: {relative} -> {source}")
            continue
        hero_position = header_offset + image.start()
        break
    positions["hero_image"] = hero_position

    separator = re.search(r"^---\s*$", text[header_end + len("</div>") :], re.MULTILINE)
    positions["separator"] = header_end + len("</div>") + separator.start() if separator else None
    first_h2 = re.search(r"^##\s+", text, re.MULTILINE)
    positions["h2"] = first_h2.start() if first_h2 else None
    h2_icons = re.findall(r"^##\s+(\S+)", text, re.MULTILINE)
    expected_icons = homepage["required_h2_icons"]
    if h2_icons != expected_icons:
        errors.append(f"homepage H2 icon/order mismatch: {relative}")

    required_components = homepage["required_components"]
    for component in required_components:
        if positions[component] is None:
            errors.append(f"missing {component}: {relative}")
    configured_positions = [positions[component] for component in required_components if positions[component] is not None]
    if configured_positions != sorted(configured_positions):
        errors.append(f"homepage component order mismatch: {relative}")
    return errors


def check(
    repo: Path, pairs: dict[str, str] = PAIRS, homepage_pairs: list[dict[str, object]] | None = None
) -> list[str]:
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
    for homepage in homepage_pairs or []:
        english = homepage["english"]
        chinese = homepage["chinese"]
        for relative, counterpart in ((english, chinese), (chinese, english)):
            file = repo / relative
            if not file.is_file():
                errors.append(f"missing configured homepage: {relative}")
                continue
            errors.extend(check_homepage(repo, relative, file.read_text(encoding="utf-8"), counterpart, homepage))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", nargs="?", default=".")
    parser.add_argument("--config", required=True, type=Path, help="homepage contract JSON")
    args = parser.parse_args()
    try:
        homepage_pairs = load_homepage_pairs(args.config)
    except ValueError as error:
        parser.error(str(error))
    errors = check(Path(args.repo).resolve(), homepage_pairs=homepage_pairs)
    if errors:
        print("\n".join(f"check_docs: {error}" for error in errors))
        return 1
    print("check_docs: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
