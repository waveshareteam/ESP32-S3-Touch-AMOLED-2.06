#!/usr/bin/env python3
"""Audit repository Markdown ownership, bilingual structure, and diff scope.

The script is read-only and uses only the Python standard library.  It is designed
for both local maintenance and a lightweight GitHub Actions documentation gate.
"""

from __future__ import annotations

import argparse
import copy
import fnmatch
import html
import json
import os
import re
import subprocess
import sys
import unicodedata
import urllib.parse
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence


VALID_CATEGORIES = {
    "first_party_customer",
    "first_party_maintainer",
    "first_party_wrapper",
    "managed_component",
    "third_party",
    "embedded_upstream",
    "unknown",
}
AUDITED_OWNERSHIP = {
    "first_party_customer",
    "first_party_maintainer",
    "first_party_wrapper",
}
UPSTREAM_OWNERSHIP = {"managed_component", "third_party", "embedded_upstream"}

DEFAULT_CONFIG = {
    "classification_rules": [
        {
            "category": "managed_component",
            "patterns": ["managed_components/**", "**/managed_components/**"],
        },
        {
            "category": "third_party",
            "patterns": [
                "third_party/**",
                "**/third_party/**",
                "third-party/**",
                "**/third-party/**",
                "vendor/**",
                "**/vendor/**",
                "external/**",
                "**/external/**",
            ],
        },
        {
            "category": "embedded_upstream",
            "patterns": [
                "upstream/**",
                "**/upstream/**",
                "submodules/**",
                "**/submodules/**",
                "libraries/**",
                "**/libraries/**",
                "**/components/waveshare__*/**",
            ],
        },
        {
            "category": "unknown",
            "patterns": [
                "lib/**",
                "**/lib/**",
                "libs/**",
                "**/libs/**",
                "deps/**",
                "**/deps/**",
                "dependencies/**",
                "**/dependencies/**",
                "middleware/**",
                "**/middleware/**",
                "sdk/**",
                "**/sdk/**",
            ],
        },
        {
            "category": "first_party_wrapper",
            "patterns": [
                "wrappers/**",
                "**/wrappers/**",
                "overrides/**",
                "**/overrides/**",
                "**/*_safe/**",
                "**/bsp_extra/**",
            ],
        },
        {
            "category": "unknown",
            "patterns": [
                "components/**",
                "**/components/**",
                "examples/**/components/**",
            ],
        },
        {
            "category": "first_party_maintainer",
            "patterns": [
                ".github/**",
                "**/.github/**",
                "CONTRIBUTING*.md",
                "SUPPORT*.md",
                "SECURITY*.md",
                "CHANGELOG*.md",
                "releases/*.md",
                "releases/**/*.md",
                "config/*.md",
                "config/**/*.md",
                "assets/*.md",
                "assets/**/*.md",
                "docs/ci*.md",
                "docs/**/ci*.md",
                "docs/component*.md",
                "docs/**/component*.md",
                "docs/repository*.md",
                "docs/**/repository*.md",
            ],
        },
    ],
    "exclude_patterns": [
        ".git/**",
        "**/.git/**",
        "build/**",
        "**/build/**",
        "build*/**",
        "**/build*/**",
        "node_modules/**",
        "**/node_modules/**",
        "__pycache__/**",
        "**/__pycache__/**",
    ],
    "pair_exempt_patterns": [],
    "language_link_exempt_patterns": [],
    "relative_link_ignore_patterns": [],
    "docs_only_allowed_patterns": [],
    "sensitive_allow_regexes": [],
    "bilingual_pairs": [],
    "bilingual_directory_mappings": [],
    "homepage_h3_emoji_allow_patterns": [],
    "homepage_pairs": [
        {"english": "README.md", "chinese": "README_ZH.md", "profile": "auto"},
    ],
}

MARKDOWN_LINK_RE = re.compile(
    r"!?\[[^\]]*\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+['\"][^'\"]*['\"])?\s*\)"
)
HTML_LINK_RE = re.compile(r"\b(?:href|src)\s*=\s*['\"]([^'\"]+)['\"]", re.IGNORECASE)
REFERENCE_LINK_RE = re.compile(r"^\s*\[[^\]]+\]:\s*(<[^>]+>|\S+)", re.MULTILINE)
HTML_ANCHOR_RE = re.compile(
    r"<a\b[^>]*\bhref\s*=\s*['\"]([^'\"]+)['\"][^>]*>(.*?)</a>",
    re.IGNORECASE | re.DOTALL,
)
HTML_H1_RE = re.compile(r"<h1\b[^>]*>(.*?)</h1>", re.IGNORECASE | re.DOTALL)
HTML_ID_RE = re.compile(
    r"<[A-Za-z][^>]*\b(?:id|name)\s*=\s*(?:['\"]([^'\"]+)['\"]|([^\s>]+))",
    re.IGNORECASE,
)
HTML_IMG_RE = re.compile(r"<img\b[^>]*>", re.IGNORECASE)
HTML_ALT_RE = re.compile(r"\balt\s*=\s*['\"]([^'\"]*)['\"]", re.IGNORECASE)
CENTERED_DIV_RE = re.compile(
    r"<div\b[^>]*\balign\s*=\s*(?:['\"]center['\"]|center)(?:\s|>)",
    re.IGNORECASE,
)
STRONG_RE = re.compile(r"<strong\b[^>]*>.*?</strong>", re.IGNORECASE | re.DOTALL)
H2_RE = re.compile(r"^##(?!#)\s+(.+?)\s*$", re.MULTILINE)
H3_RE = re.compile(r"^###(?!#)\s+(.+?)\s*$", re.MULTILINE)
ATX_HEADING_RE = re.compile(r"^(#{1,6})[ \t]+(.+?)[ \t]*$", re.MULTILINE)
TAG_RE = re.compile(r"<[^>]+>")
EMOJI_RE = re.compile(r"[\U0001F000-\U0001FAFF\u2139\u2600-\u27BF]")
FENCE_RE = re.compile(r"^\s*(```|~~~)")

QUICK_LINK_RULES = (
    ("quick_start", "🚀", re.compile(r"quick\s*start|getting\s*started|快速开始|快速入门", re.I)),
    ("esp_idf", "🧩", re.compile(r"esp[- ]idf", re.I)),
    ("arduino", "🔧", re.compile(r"arduino", re.I)),
    ("firmware", "📦", re.compile(r"firmware|固件", re.I)),
    ("documentation", "📚", re.compile(r"documentation|documents?|docs\b|文档|资料", re.I)),
    ("product", "🌐", re.compile(r"product|产品", re.I)),
)

H2_ICON_RULES = (
    ("🖥️", re.compile(r"hardware|硬件", re.I)),
    ("✨", re.compile(r"overview|概述|简介", re.I)),
    ("📱", re.compile(r"brookesia|application\s+firmware|应用固件", re.I)),
    ("📦", re.compile(r"firmware|release|固件|发布", re.I)),
    ("🧪", re.compile(r"examples?|示例|例程", re.I)),
    ("🛠️", re.compile(r"toolchains?|工具链", re.I)),
    ("🗂️", re.compile(r"repository\s+(?:layout|structure)|仓库(?:结构|布局)", re.I)),
    ("📚", re.compile(r"documentation|文档", re.I)),
    ("🤝", re.compile(r"support|contribut|支持|贡献", re.I)),
    ("📄", re.compile(r"licen[cs]e|许可证|许可", re.I)),
)

BADGE_RULES = (
    ("build", re.compile(r"build|actions?|workflow|\bci\b|构建", re.I)),
    ("release", re.compile(r"release|firmware|version|固件|发布|版本", re.I)),
    ("license", re.compile(r"licen[cs]e|许可证|许可", re.I)),
)

HOMEPAGE_COMPONENTS = {
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
HOMEPAGE_PROFILES = {
    "auto": set(),
    "single-product": set(HOMEPAGE_COMPONENTS),
    "single_product": set(HOMEPAGE_COMPONENTS),
    "multi-product-hub": {
        "centered_header",
        "html_h1",
        "subtitle",
        "language_switch",
        "separator",
        "h2",
    },
    "multi_product_hub": {
        "centered_header",
        "html_h1",
        "subtitle",
        "language_switch",
        "separator",
        "h2",
    },
}
VALID_QUICK_LINK_KEYS = {item[0] for item in QUICK_LINK_RULES}
VALID_BADGE_KEYS = {item[0] for item in BADGE_RULES}

SENSITIVE_RULES = (
    (
        "LOCAL_ABSOLUTE_PATH",
        re.compile(r"(?i)(?<![A-Za-z0-9])(?:[A-Z]:[\\/]|\\\\[A-Za-z0-9._-]+[\\/][A-Za-z0-9$._-]+[\\/]|/(?:home|Users)/[A-Za-z0-9._-]+/)"),
        "replace machine-specific paths with repository-relative paths or placeholders",
    ),
    (
        "ACTUAL_SERIAL_PORT",
        re.compile(r"(?i)\bCOM[1-9][0-9]*\b|/dev/serial/by-id/[A-Za-z0-9._:+-]+|/dev/cu\.[A-Za-z0-9._-]+"),
        "use a placeholder such as COMx or <PORT> in public documentation",
    ),
    (
        "MAC_ADDRESS",
        re.compile(r"(?i)\b(?:[0-9A-F]{2}[:-]){5}[0-9A-F]{2}\b"),
        "remove or replace device-specific MAC addresses",
    ),
    (
        "CREDENTIAL_OR_TOKEN",
        re.compile(
            r"(?i)(?:\b(?:github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|sk-[A-Za-z0-9_-]{20,}|Bearer\s+[A-Za-z0-9._~+/-]{20,})|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)"
        ),
        "remove credentials and rotate any real secret before publishing",
    ),
    (
        "TOOL_OR_MODEL_PROVENANCE",
        re.compile(
            r"(?i)\b(?:(?:generated|written|edited|translated|created|reviewed)\s+(?:by|with|using)|(?:tool|model)(?:\s+(?:used|source|name))?\s*[:=])\s*(?:OpenAI|ChatGPT|Codex|Claude|Gemini|Copilot|GPT[-\s]?[0-9][A-Za-z0-9_.-]*)"
        ),
        "remove editing-tool or model provenance from repository-public text",
    ),
)

SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".ino", ".py", ".ps1",
    ".sh", ".bat", ".cmake", ".s", ".S",
}
SOURCE_NAMES = {"CMakeLists.txt", "Kconfig", "Makefile"}
CONFIG_SUFFIXES = {".yml", ".yaml", ".json", ".toml", ".ini", ".conf", ".defaults", ".csv"}
CONFIG_NAMES = {"sdkconfig", "sdkconfig.defaults", "idf_component.yml", "partitions.csv"}
# Checked-in Waveshare factory/delivery firmware convention.  Other binary
# suffixes remain non-Markdown scope, but are not labelled as a Waveshare
# firmware convention without repository evidence.
FIRMWARE_SUFFIXES = {".bin"}
PACKAGE_SUFFIXES = {".zip", ".7z", ".tar", ".tgz", ".gz", ".xz", ".bz2"}
DOCUMENTATION_ASSET_SUFFIXES = {".gif", ".jpeg", ".jpg", ".png", ".svg", ".webp"}

# ``url`` in an ESP-IDF component manifest is sometimes a product page rather
# than source provenance.  Treat it as a repository only for established source
# hosts or an explicitly Git-shaped URL; ``repository`` remains authoritative
# for self-hosted forges as well.
SOURCE_REPOSITORY_HOSTS = {
    "bitbucket.org",
    "codeberg.org",
    "gitee.com",
    "github.com",
    "gitlab.com",
}
COMPONENT_SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".s", ".S",
}


@dataclass(frozen=True)
class Change:
    status: str
    path: str
    old_path: str | None = None


@dataclass(frozen=True)
class Finding:
    severity: str
    code: str
    path: str
    line: int | None
    message: str
    hint: str


class AuditError(RuntimeError):
    """Operational or configuration error (exit code 2)."""


def posix_path(value: str | Path) -> str:
    value = str(value).replace("\\", "/")
    while value.startswith("./"):
        value = value[2:]
    return value


def matches(path: str, patterns: Iterable[str]) -> bool:
    normalized = posix_path(path)
    return any(fnmatch.fnmatchcase(normalized, posix_path(pattern)) for pattern in patterns)


def normalized_config_path(value: str, label: str, *, allow_root: bool = False) -> str:
    normalized = posix_path(value.strip())
    if allow_root and normalized in {"", "."}:
        return ""
    normalized = normalized.strip("/")
    pure = PurePosixPath(normalized)
    if (
        not normalized
        or pure.is_absolute()
        or ".." in pure.parts
        or re.match(r"^[A-Za-z]:", normalized)
    ):
        raise AuditError(f"{label} must be a repository-relative path: {value!r}")
    return normalized


def parse_language_pair(value: object, label: str) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != {"english", "chinese"}:
        raise AuditError(f"{label} must contain english and chinese")
    if not all(isinstance(value[name], str) for name in ("english", "chinese")):
        raise AuditError(f"{label} paths must be strings")
    pair = {
        name: normalized_config_path(value[name], f"{label}.{name}")
        for name in ("english", "chinese")
    }
    if pair["english"] == pair["chinese"]:
        raise AuditError(f"{label} must use two different paths")
    return pair


def validate_narrow_h3_regex(pattern: str, label: str) -> None:
    try:
        compiled = re.compile(pattern)
    except re.error as exc:
        raise AuditError(f"invalid {label} entry {pattern!r}: {exc}") from exc
    if not pattern.startswith("^") or not pattern.endswith("$") or compiled.fullmatch(""):
        raise AuditError(
            f"{label} entry must be a non-empty, fully anchored narrow regex: {pattern!r}"
        )


def parse_homepage_pair(value: object, label: str) -> dict:
    required = {"english", "chinese"}
    optional = {
        "profile",
        "required_components",
        "required_quick_links",
        "required_badges",
        "required_h2_icons",
        "h3_emoji_allow_patterns",
    }
    if not isinstance(value, dict) or not required.issubset(value) or set(value) - required - optional:
        raise AuditError(
            f"{label} must contain english and chinese plus only supported homepage settings"
        )
    pair = parse_language_pair(
        {"english": value["english"], "chinese": value["chinese"]}, label
    )
    profile = value.get("profile", "auto")
    if not isinstance(profile, str) or profile not in HOMEPAGE_PROFILES:
        raise AuditError(
            f"{label}.profile must be one of: {', '.join(sorted(HOMEPAGE_PROFILES))}"
        )
    pair["profile"] = profile
    list_keys = {
        "required_components",
        "required_quick_links",
        "required_badges",
        "required_h2_icons",
        "h3_emoji_allow_patterns",
    }
    for key in list_keys:
        items = value.get(key, [])
        if not isinstance(items, list) or not all(isinstance(item, str) for item in items):
            raise AuditError(f"{label}.{key} must contain strings")
        pair[key] = list(items)
    invalid_components = sorted(set(pair["required_components"]) - HOMEPAGE_COMPONENTS)
    if invalid_components:
        raise AuditError(f"{label}.required_components has unknown values: {', '.join(invalid_components)}")
    invalid_quick = sorted(set(pair["required_quick_links"]) - VALID_QUICK_LINK_KEYS)
    if invalid_quick:
        raise AuditError(f"{label}.required_quick_links has unknown values: {', '.join(invalid_quick)}")
    invalid_badges = sorted(set(pair["required_badges"]) - VALID_BADGE_KEYS)
    if invalid_badges:
        raise AuditError(f"{label}.required_badges has unknown values: {', '.join(invalid_badges)}")
    for pattern in pair["h3_emoji_allow_patterns"]:
        validate_narrow_h3_regex(pattern, f"{label}.h3_emoji_allow_patterns")
    return pair


def validate_pair_conflicts(config: dict) -> None:
    partners: dict[str, str] = {}
    for index, pair in enumerate([*config["bilingual_pairs"], *config["homepage_pairs"]]):
        english, chinese = pair["english"], pair["chinese"]
        for source, target in ((english, chinese), (chinese, english)):
            previous = partners.get(source)
            if previous is not None and previous != target:
                raise AuditError(
                    f"conflicting bilingual companion for {source!r}: {previous!r} and {target!r}"
                )
            partners[source] = target


def parse_docs_only_asset_paths(value: object, label: str) -> list[str]:
    """Validate explicit documentation assets allowed beside Markdown files."""
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise AuditError(f"{label} must be a list of strings")
    paths: list[str] = []
    for item in value:
        normalized = normalized_config_path(item, label)
        if (
            item != normalized
            or item.endswith("/")
            or "//" in item
            or any(character in item for character in "*?[]")
            or PurePosixPath(normalized).suffix.lower() not in DOCUMENTATION_ASSET_SUFFIXES
        ):
            raise AuditError(
                f"{label} entries must be explicit repository-relative documentation asset files"
            )
        paths.append(normalized)
    return paths


def load_config(path: Path | None) -> dict:
    config = copy.deepcopy(DEFAULT_CONFIG)
    if path is None:
        return config
    try:
        user = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AuditError(f"cannot read JSON config {path}: {exc}") from exc
    if not isinstance(user, dict):
        raise AuditError("config root must be a JSON object")
    known = set(DEFAULT_CONFIG)
    unknown = sorted(set(user) - known)
    if unknown:
        raise AuditError("unknown config keys: " + ", ".join(unknown))
    for key, value in user.items():
        if not isinstance(value, list):
            raise AuditError(f"config key {key!r} must be a list")
        if key == "classification_rules":
            rules = []
            for index, rule in enumerate(value):
                if not isinstance(rule, dict) or set(rule) != {"category", "patterns"}:
                    raise AuditError(f"classification_rules[{index}] must contain category and patterns")
                category = rule["category"]
                patterns = rule["patterns"]
                if category not in VALID_CATEGORIES:
                    raise AuditError(f"invalid classification category: {category!r}")
                if not isinstance(patterns, list) or not all(isinstance(item, str) for item in patterns):
                    raise AuditError(f"classification_rules[{index}].patterns must be strings")
                rules.append({"category": category, "patterns": patterns})
            config[key] = rules + config[key]
        elif key == "docs_only_allowed_patterns":
            config[key] = parse_docs_only_asset_paths(value, key)
        elif key == "homepage_pairs":
            config[key] = [
                parse_homepage_pair(pair, f"homepage_pairs[{index}]")
                for index, pair in enumerate(value)
            ]
        elif key == "bilingual_pairs":
            config[key] = [
                parse_language_pair(pair, f"bilingual_pairs[{index}]")
                for index, pair in enumerate(value)
            ]
        elif key == "bilingual_directory_mappings":
            mappings = []
            for index, mapping in enumerate(value):
                label = f"bilingual_directory_mappings[{index}]"
                if (
                    not isinstance(mapping, dict)
                    or not {"english", "chinese"}.issubset(mapping)
                    or set(mapping) - {"english", "chinese", "chinese_suffix"}
                ):
                    raise AuditError(
                        f"{label} must contain english and chinese plus optional chinese_suffix"
                    )
                if not all(isinstance(mapping[name], str) for name in ("english", "chinese")):
                    raise AuditError(f"{label} paths must be strings")
                parsed = {
                    name: normalized_config_path(mapping[name], f"{label}.{name}", allow_root=True)
                    for name in ("english", "chinese")
                }
                chinese_suffix = mapping.get("chinese_suffix", "")
                if not isinstance(chinese_suffix, str) or not re.fullmatch(
                    r"[A-Za-z0-9_-]*", chinese_suffix
                ):
                    raise AuditError(
                        f"{label}.chinese_suffix must be an optional filename suffix such as _ZH"
                    )
                parsed["chinese_suffix"] = chinese_suffix
                if parsed["english"] == parsed["chinese"]:
                    raise AuditError(f"{label} must use two different directory roots")
                mappings.append(parsed)
            config[key] = mappings
        elif not all(isinstance(item, str) for item in value):
            raise AuditError(f"config key {key!r} must contain strings")
        else:
            config[key] = config[key] + value
    for pattern in config["sensitive_allow_regexes"]:
        try:
            re.compile(pattern)
        except re.error as exc:
            raise AuditError(f"invalid sensitive_allow_regexes entry {pattern!r}: {exc}") from exc
    for pattern in config["homepage_h3_emoji_allow_patterns"]:
        validate_narrow_h3_regex(pattern, "homepage_h3_emoji_allow_patterns")
    validate_pair_conflicts(config)
    return config


def classify(path: str, config: dict) -> str:
    for rule in config["classification_rules"]:
        if matches(path, rule["patterns"]):
            return rule["category"]
    return "first_party_customer"


def repository_identity(value: str, *, field: str = "repository") -> str | None:
    """Return a credential-free forge/repository identity for comparison.

    Browser suffixes such as GitHub ``/tree/...`` are ignored.  The returned
    value never contains user-info, query strings, fragments, or the original
    URL, so a credential-bearing Git remote cannot leak into audit output.
    """
    raw = value.strip().strip('"\'')
    if not raw or field not in {"repository", "url"}:
        return None

    host = ""
    path = ""
    scheme = ""
    scp_match = re.fullmatch(
        r"(?:[^/@:\s]+@)?(?P<host>[^/:\s]+):(?P<path>[^?#\s]+)", raw
    )
    if scp_match and "://" not in raw:
        scheme = "ssh"
        host = scp_match.group("host")
        path = scp_match.group("path")
    else:
        try:
            parsed = urllib.parse.urlsplit(raw)
        except ValueError:
            return None
        scheme = parsed.scheme.lower()
        if scheme not in {"http", "https", "git", "ssh"} or not parsed.hostname:
            return None
        host = parsed.hostname
        path = parsed.path

    host = host.lower().rstrip(".")
    if host.startswith("www.") and host[4:] in SOURCE_REPOSITORY_HOSTS:
        host = host[4:]
    raw_path = urllib.parse.unquote(path).strip("/")
    if not raw_path:
        return None

    git_shaped = scheme in {"git", "ssh"} or raw_path.lower().endswith(".git")
    if field == "url" and host not in SOURCE_REPOSITORY_HOSTS and not git_shaped:
        return None

    parts = [part for part in raw_path.split("/") if part]
    if host in {"github.com", "bitbucket.org", "gitee.com", "codeberg.org"}:
        if len(parts) < 2:
            return None
        parts = parts[:2]
    else:
        # GitLab and compatible forges may use nested groups.  Their web URLs
        # put browsing state after /-/; common non-GitLab tree/blob forms are
        # also cut without retaining branch or file names.
        if "-" in parts:
            dash = parts.index("-")
            if dash + 1 < len(parts) and parts[dash + 1] in {"tree", "blob"}:
                parts = parts[:dash]
        for marker in ("tree", "blob"):
            if marker in parts[2:]:
                parts = parts[:parts.index(marker)]
                break
        if len(parts) < 2:
            return None
    parts[-1] = re.sub(r"(?i)\.git$", "", parts[-1])
    if not parts[-1]:
        return None
    return host + "/" + "/".join(parts)


def discover_self_repository_identities(root: Path) -> set[str]:
    """Read Git remotes and return only normalized, credential-free identities."""
    remotes = subprocess.run(
        ["git", "remote"], cwd=root, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, check=False, text=True,
    )
    if remotes.returncode != 0:
        return set()
    identities: set[str] = set()
    for remote in remotes.stdout.splitlines():
        if not remote or not re.fullmatch(r"[^\s\0]+", remote):
            continue
        urls = subprocess.run(
            ["git", "remote", "get-url", "--all", remote], cwd=root,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False,
            text=True,
        )
        if urls.returncode != 0:
            continue
        for value in urls.stdout.splitlines():
            identity = repository_identity(value)
            if identity:
                identities.add(identity)
    return identities


def _manifest_repository_identities(manifest: Path) -> tuple[set[str], set[str]]:
    """Extract top-level repository/url metadata without requiring a YAML parser."""
    try:
        lines = manifest.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return set(), set()
    found: dict[str, set[str]] = {"repository": set(), "url": set()}
    for line in lines:
        # A dependency may itself contain url/repository keys.  Only component
        # metadata at column zero describes the directory being classified.
        if line[:1].isspace():
            continue
        match = re.match(r"^(repository|url)\s*:\s*(.*?)\s*$", line, re.IGNORECASE)
        if not match:
            continue
        field = match.group(1).lower()
        value = match.group(2).strip()
        if value[:1] in {'"', "'"} and value[-1:] == value[:1]:
            value = value[1:-1]
        else:
            value = re.split(r"\s+#", value, maxsplit=1)[0].strip()
        identity = repository_identity(value, field=field)
        if identity:
            found[field].add(identity)
    return found["repository"], found["url"]


def _is_nested_component_source_root(repo_root: Path, component_root: Path) -> bool:
    """Require a conservative, self-contained component/source-root shape."""
    if component_root == repo_root or not (component_root / "CMakeLists.txt").is_file():
        return False
    try:
        children = list(component_root.iterdir())
    except OSError:
        return False
    has_readme = any(
        child.is_file() and child.name.lower().startswith("readme")
        and child.suffix.lower() == ".md"
        for child in children
    )
    has_license = any(
        child.is_file()
        and child.name.lower().startswith(("license", "licence", "copying"))
        for child in children
    )
    has_source = any(
        (child.is_file() and child.suffix in COMPONENT_SOURCE_SUFFIXES)
        or (child.is_dir() and child.name.lower() in {"include", "src"})
        for child in children
    )
    return has_readme and has_license and has_source


def discover_dynamic_upstream_roots(root: Path) -> set[str]:
    """Discover conservative nested ESP-IDF source roots owned by another repo.

    Discovery is intentionally disabled when the checkout has no comparable Git
    remote.  This preserves first-party/unknown auditing for archives and avoids
    turning a lone external homepage URL into an ownership assertion.
    """
    root = root.resolve()
    self_identities = discover_self_repository_identities(root)
    if not self_identities:
        return set()
    roots: set[str] = set()
    for manifest in root.rglob("idf_component.yml"):
        component_root = manifest.parent
        if not _is_nested_component_source_root(root, component_root):
            continue
        repositories, urls = _manifest_repository_identities(manifest)
        candidates = repositories or urls
        if not candidates or candidates & self_identities:
            continue
        try:
            relative = component_root.relative_to(root)
        except ValueError:
            continue
        roots.add(posix_path(relative).strip("/"))
    return roots


def path_is_within(path: str, root: str) -> bool:
    normalized = posix_path(path).strip("/")
    normalized_root = posix_path(root).strip("/")
    return normalized == normalized_root or normalized.startswith(normalized_root + "/")


def discover_gitlink_roots(root: Path) -> set[str]:
    """Return submodule/gitlink roots without requiring an initialized checkout."""
    roots: set[str] = set()
    gitmodules = root / ".gitmodules"
    if gitmodules.is_file():
        try:
            content = gitmodules.read_text(encoding="utf-8", errors="replace")
        except OSError:
            content = ""
        for match in re.finditer(r"(?mi)^\s*path\s*=\s*(.+?)\s*$", content):
            value = posix_path(match.group(1).strip().strip('"\''))
            pure = PurePosixPath(value)
            if value and not pure.is_absolute() and ".." not in pure.parts:
                roots.add(value.strip("/"))

    process = subprocess.run(
        ["git", "ls-files", "--stage", "-z"],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if process.returncode == 0:
        for entry in process.stdout.decode("utf-8", errors="surrogateescape").split("\0"):
            if not entry or "\t" not in entry:
                continue
            metadata, path = entry.split("\t", 1)
            if metadata.split(maxsplit=1)[0] == "160000":
                roots.add(posix_path(path).strip("/"))
    return {item for item in roots if item}


def classify_at_root(
    path: str,
    config: dict,
    gitlink_roots: set[str],
    dynamic_upstream_roots: set[str] | None = None,
) -> str:
    upstream_roots = gitlink_roots | (dynamic_upstream_roots or set())
    if any(path_is_within(path, root) for root in upstream_roots):
        return "embedded_upstream"
    return classify(path, config)


def is_excluded(path: str, config: dict) -> bool:
    return matches(path, config["exclude_patterns"])


def iter_markdown_files(root: Path, config: dict) -> list[str]:
    result: list[str] = []
    for current, dirnames, filenames in os.walk(root):
        current_path = Path(current)
        kept: list[str] = []
        for dirname in dirnames:
            relative = posix_path((current_path / dirname).relative_to(root)) + "/"
            if not is_excluded(relative, config):
                kept.append(dirname)
        dirnames[:] = kept
        for filename in filenames:
            path = current_path / filename
            relative = posix_path(path.relative_to(root))
            if path.suffix.lower() == ".md" and not is_excluded(relative, config):
                result.append(relative)
    return sorted(result, key=str.lower)


def tracked_markdown_files(root: Path, config: dict) -> list[str]:
    """Use the repository index for --all, excluding ignored/generated files."""
    top = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
        text=True,
    )
    if top.returncode != 0:
        return iter_markdown_files(root, config)
    try:
        if Path(top.stdout.strip()).resolve() != root.resolve():
            return iter_markdown_files(root, config)
    except OSError:
        return iter_markdown_files(root, config)
    process = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        return iter_markdown_files(root, config)
    result = []
    for item in process.stdout.decode("utf-8", errors="surrogateescape").split("\0"):
        path = posix_path(item)
        if (
            path
            and path.lower().endswith(".md")
            and not is_excluded(path, config)
            and (root / path).is_file()
        ):
            result.append(path)
    return sorted(set(result), key=str.lower)


def run_git(root: Path, args: Sequence[str]) -> bytes:
    process = subprocess.run(
        ["git", *args], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
    )
    if process.returncode:
        detail = process.stderr.decode("utf-8", errors="replace").strip()
        raise AuditError(f"git {' '.join(args)} failed: {detail or 'unknown error'}")
    return process.stdout


def parse_name_status_z(data: bytes) -> list[Change]:
    tokens = data.decode("utf-8", errors="surrogateescape").split("\0")
    if tokens and tokens[-1] == "":
        tokens.pop()
    changes: list[Change] = []
    index = 0
    while index < len(tokens):
        status = tokens[index]
        index += 1
        if not status:
            continue
        code = status[0]
        if code in {"R", "C"}:
            if index + 1 >= len(tokens):
                raise AuditError("malformed git rename/copy output")
            old_path, new_path = tokens[index], tokens[index + 1]
            index += 2
            changes.append(Change(status=code, path=posix_path(new_path), old_path=posix_path(old_path)))
        else:
            if index >= len(tokens):
                raise AuditError("malformed git name-status output")
            changes.append(Change(status=code, path=posix_path(tokens[index])))
            index += 1
    return changes


def changes_from_base(root: Path, base: str) -> list[Change]:
    return parse_name_status_z(run_git(root, ["diff", "--name-status", "-z", "--find-renames", f"{base}...HEAD", "--"]))


def require_clean_base_checkout(root: Path) -> None:
    status = run_git(root, ["status", "--porcelain=v1", "-z", "--untracked-files=all"])
    if status:
        raise AuditError(
            "--base requires a clean checkout so audited content matches HEAD; "
            "use an isolated worktree/clone for the committed range or --working-tree for local edits"
        )


def repository_has_head(root: Path) -> bool:
    """Return whether ``HEAD`` resolves to a commit, including unborn repos safely.

    ``git rev-parse --verify --quiet`` deliberately returns 1 with no output for
    an unborn branch.  Any other failure is operational (for example, a path
    outside a Git worktree or a corrupt repository) and must remain visible to
    callers instead of being mistaken for an empty change set.
    """

    args = ["rev-parse", "--verify", "--quiet", "HEAD^{commit}"]
    process = subprocess.run(
        ["git", *args],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode == 0:
        return True
    if process.returncode == 1 and not process.stdout and not process.stderr:
        return False
    detail = process.stderr.decode("utf-8", errors="replace").strip()
    raise AuditError(f"git {' '.join(args)} failed: {detail or 'unknown error'}")


def merge_unborn_changes(staged: Sequence[Change], unstaged: Sequence[Change]) -> list[Change]:
    """Merge index and worktree diffs when the repository has no commit yet.

    A path added to the index and then edited in the worktree is still an
    addition relative to an unborn branch, while an unstaged deletion must stay
    a deletion.  Likewise, a later content edit must not discard rename/copy
    provenance already reported by Git.  Other later worktree statuses replace
    the index status because they describe the current filesystem state.
    """

    unique: dict[str, Change] = {}
    for change in staged:
        unique[change.path] = change
    for change in unstaged:
        previous = unique.get(change.path)
        if (
            previous is not None
            and change.status == "M"
            and previous.status in {"A", "R", "C"}
        ):
            continue
        unique[change.path] = change
    return list(unique.values())


def changes_from_worktree(root: Path) -> list[Change]:
    if repository_has_head(root):
        changes = parse_name_status_z(
            run_git(root, ["diff", "--name-status", "-z", "--find-renames", "HEAD", "--"])
        )
    else:
        # With no HEAD, ``git diff --cached`` intentionally compares the index
        # to Git's empty tree.  The second diff contributes edits between the
        # index and worktree; neither command needs a hard-coded object hash.
        staged = parse_name_status_z(
            run_git(root, ["diff", "--cached", "--name-status", "-z", "--find-renames", "--"])
        )
        unstaged = parse_name_status_z(
            run_git(root, ["diff", "--name-status", "-z", "--find-renames", "--"])
        )
        changes = merge_unborn_changes(staged, unstaged)
    untracked = run_git(root, ["ls-files", "--others", "--exclude-standard", "-z"]).decode(
        "utf-8", errors="surrogateescape"
    )
    changes.extend(Change("A", posix_path(item)) for item in untracked.split("\0") if item)
    unique: dict[str, Change] = {}
    for change in changes:
        unique[change.path] = change
    return sorted(unique.values(), key=lambda item: item.path.lower())


def changes_from_file(path: Path) -> list[Change]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise AuditError(f"cannot read changed-files list {path}: {exc}") from exc
    changes: list[Change] = []
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) == 1:
            changes.append(Change("M", posix_path(parts[0])))
        elif parts[0] and parts[0][0] in {"R", "C"} and len(parts) == 3:
            changes.append(Change(parts[0][0], posix_path(parts[2]), posix_path(parts[1])))
        elif len(parts) == 2 and parts[0] and parts[0][0] in "ADMTCU":
            changes.append(Change(parts[0][0], posix_path(parts[1])))
        else:
            raise AuditError(f"invalid changed-files entry at {path}:{number}: {raw!r}")
    if not changes:
        raise AuditError(f"changed-files list is empty: {path}")
    return changes


def validate_change_paths(changes: Iterable[Change]) -> None:
    for change in changes:
        path = PurePosixPath(change.path)
        if path.is_absolute() or ".." in path.parts or re.match(r"^[A-Za-z]:", change.path):
            raise AuditError(f"changed path must be repository-relative: {change.path!r}")


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise AuditError(f"cannot read {path}: {exc}") from exc


def strip_fenced_code(text: str) -> str:
    output: list[str] = []
    fence: str | None = None
    for line in text.splitlines(keepends=True):
        marker = FENCE_RE.match(line)
        if marker:
            current = marker.group(1)
            if fence is None:
                fence = current
            elif current == fence:
                fence = None
            output.append("\n" if line.endswith("\n") else "")
        elif fence is None:
            output.append(re.sub(r"`[^`\n]*`", "", line))
        else:
            output.append("\n" if line.endswith("\n") else "")
    return "".join(output)


def iter_links(text: str) -> Iterable[tuple[str, int]]:
    searchable = strip_fenced_code(text)
    seen: set[tuple[int, str]] = set()
    for pattern in (MARKDOWN_LINK_RE, HTML_LINK_RE, REFERENCE_LINK_RE):
        for match in pattern.finditer(searchable):
            target = html.unescape(match.group(1).strip().strip("<>"))
            item = (match.start(1), target)
            if item in seen:
                continue
            seen.add(item)
            yield target, searchable.count("\n", 0, match.start(1)) + 1


def local_reference(raw: str) -> tuple[str, str] | None:
    if not raw or raw.startswith("//"):
        return None
    parsed = urllib.parse.urlsplit(raw)
    if parsed.scheme or parsed.netloc:
        return None
    value = urllib.parse.unquote(parsed.path).replace("\\", "/")
    fragment = unicodedata.normalize(
        "NFC", html.unescape(urllib.parse.unquote(parsed.fragment))
    )
    if not value and not fragment:
        return None
    return value, fragment


def local_target(raw: str) -> str | None:
    reference = local_reference(raw)
    return reference[0] or None if reference is not None else None


def resolve_repo_link(root: Path, document: str, target: str) -> tuple[Path | None, str | None]:
    candidate = (root / PurePosixPath(document).parent / PurePosixPath(target)).resolve()
    try:
        relative = posix_path(candidate.relative_to(root.resolve()))
    except ValueError:
        return None, None
    return candidate, relative


def strip_fenced_blocks(text: str) -> str:
    output: list[str] = []
    fence: str | None = None
    for line in text.splitlines(keepends=True):
        marker = FENCE_RE.match(line)
        if marker:
            current = marker.group(1)
            if fence is None:
                fence = current
            elif current == fence:
                fence = None
            output.append("\n" if line.endswith("\n") else "")
        elif fence is None:
            output.append(line)
        else:
            output.append("\n" if line.endswith("\n") else "")
    return "".join(output)


def github_heading_slug(title: str) -> str:
    value = html.unescape(title)
    value = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", value)
    value = TAG_RE.sub("", value).replace("`", "")
    value = unicodedata.normalize("NFC", value).strip().lower()
    kept: list[str] = []
    for character in value:
        if character == "\ufe0f":
            continue
        category = unicodedata.category(character)
        if character.isspace():
            kept.append(" ")
        elif character in {"-", "_"} or category[0] in {"L", "N", "M"}:
            kept.append(character)
    return re.sub(r"\s+", "-", "".join(kept)).strip("-")


def document_anchors(text: str) -> set[str]:
    searchable = strip_fenced_blocks(text)
    anchors: set[str] = set()
    for match in HTML_ID_RE.finditer(searchable):
        raw = match.group(1) if match.group(1) is not None else match.group(2)
        anchors.add(unicodedata.normalize("NFC", html.unescape(urllib.parse.unquote(raw))))

    headings: list[str] = []
    lines = searchable.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        match = re.match(r"^(#{1,6})[ \t]+(.+?)[ \t]*$", line)
        if match:
            headings.append(re.sub(r"[ \t]+#+[ \t]*$", "", match.group(2)))
        elif (
            line.strip()
            and index + 1 < len(lines)
            and re.match(r"^[ \t]*(?:=+|-+)[ \t]*$", lines[index + 1])
        ):
            headings.append(line.strip())
            index += 1
        index += 1

    counts: Counter[str] = Counter()
    for heading in headings:
        base = github_heading_slug(heading)
        if not base:
            continue
        occurrence = counts[base]
        counts[base] += 1
        anchors.add(base if occurrence == 0 else f"{base}-{occurrence}")
    return anchors


def link_findings(root: Path, document: str, text: str, config: dict) -> list[Finding]:
    findings: list[Finding] = []
    anchor_cache: dict[str, set[str]] = {}
    for raw, line in iter_links(text):
        reference = local_reference(raw)
        if reference is None:
            continue
        target, fragment = reference
        if not target:
            candidate = root / document
            relative = document
        else:
            candidate, relative = resolve_repo_link(root, document, target)
            if candidate is None or relative is None:
                findings.append(Finding(
                    "error", "RELATIVE_LINK_ESCAPES_REPO", document, line,
                    f"relative link escapes the repository: {raw}",
                    "use a repository-relative target that remains inside the checkout",
                ))
                continue
        if matches(relative, config["relative_link_ignore_patterns"]):
            continue
        if not candidate.exists():
            findings.append(Finding(
                "error", "RELATIVE_LINK_MISSING", document, line,
                f"relative link target does not exist: {raw}",
                "fix the path or add the intended tracked target",
            ))
            continue
        if fragment and candidate.is_file() and candidate.suffix.lower() in {".md", ".html", ".htm"}:
            if relative not in anchor_cache:
                target_text = text if relative == document else read_text(candidate)
                anchor_cache[relative] = document_anchors(target_text)
            if fragment not in anchor_cache[relative]:
                findings.append(Finding(
                    "error", "RELATIVE_LINK_FRAGMENT_MISSING", document, line,
                    f"relative link fragment does not exist in {relative}: #{fragment}",
                    "fix the fragment to match a GitHub-style heading or explicit HTML id/name",
                ))
    return findings


def configured_pair_map(config: dict) -> dict[str, tuple[str, str]]:
    result: dict[str, tuple[str, str]] = {}
    for pair in [*config.get("bilingual_pairs", []), *config.get("homepage_pairs", [])]:
        english, chinese = pair["english"], pair["chinese"]
        result[english] = (chinese, "english")
        result[chinese] = (english, "chinese")
    return result


def relative_under(path: str, root: str) -> str | None:
    normalized = posix_path(path).strip("/")
    normalized_root = posix_path(root).strip("/")
    if not normalized_root:
        return normalized
    if normalized == normalized_root:
        return ""
    prefix = normalized_root + "/"
    if normalized.startswith(prefix):
        return normalized[len(prefix):]
    return None


def join_mapping_root(root: str, relative: str) -> str:
    return "/".join(item for item in (root.strip("/"), relative.strip("/")) if item)


def add_chinese_filename_suffix(relative: str, suffix: str) -> str | None:
    if not suffix:
        return relative
    if not relative.lower().endswith(".md"):
        return None
    return relative[:-3] + suffix + ".md"


def remove_chinese_filename_suffix(relative: str, suffix: str) -> str | None:
    if not suffix:
        return relative
    marker = suffix + ".md"
    if not relative.endswith(marker):
        return None
    return relative[:-len(marker)] + ".md"


def pair_for(path: str, config: dict | None = None) -> tuple[str, str]:
    normalized = posix_path(path)
    policy = config or DEFAULT_CONFIG
    explicit = configured_pair_map(policy).get(normalized)
    if explicit is not None:
        return explicit

    candidates: list[tuple[int, str, str]] = []
    for mapping in policy.get("bilingual_directory_mappings", []):
        english_root, chinese_root = mapping["english"], mapping["chinese"]
        chinese_suffix = mapping.get("chinese_suffix", "")
        chinese_relative = relative_under(normalized, chinese_root)
        if chinese_relative is not None:
            english_relative = remove_chinese_filename_suffix(
                chinese_relative, chinese_suffix
            )
            if english_relative is not None:
                candidates.append((
                    len(chinese_root),
                    join_mapping_root(english_root, english_relative),
                    "chinese",
                ))
            # A nested Chinese root must not also be interpreted through an
            # overlapping English root from the same mapping.
            continue
        english_relative = relative_under(normalized, english_root)
        if english_relative is not None:
            suffixed = add_chinese_filename_suffix(english_relative, chinese_suffix)
            if suffixed is not None:
                candidates.append((
                    len(english_root),
                    join_mapping_root(chinese_root, suffixed),
                    "english",
                ))
    if candidates:
        _, counterpart, role = max(candidates, key=lambda item: item[0])
        return counterpart, role
    if normalized.endswith("_ZH.md"):
        return normalized[:-6] + ".md", "chinese"
    if normalized.endswith(".md"):
        return normalized[:-3] + "_ZH.md", "english"
    raise ValueError(path)


def resolved_link_paths(root: Path, document: str, text: str) -> list[tuple[str, int]]:
    result: list[tuple[str, int]] = []
    for raw, line in iter_links(text):
        target = local_target(raw)
        if target is None:
            continue
        _, relative = resolve_repo_link(root, document, target)
        if relative is not None:
            result.append((relative, line))
    return result


def markdown_block_ids(text: str) -> dict[int, int]:
    """Map source lines to blank-line-delimited Markdown blocks."""
    result: dict[int, int] = {}
    block = 0
    for number, line in enumerate(text.splitlines(), 1):
        result[number] = block
        if not line.strip():
            block += 1
    return result


def bilingual_findings(
    root: Path, selected: list[Change], classifications: dict[str, str], config: dict, all_mode: bool
) -> list[Finding]:
    findings: list[Finding] = []
    pairs: set[tuple[str, str]] = set()
    for change in selected:
        path = change.path
        if classifications.get(path) not in AUDITED_OWNERSHIP or matches(path, config["pair_exempt_patterns"]):
            continue
        if change.status == "D":
            counterpart, _ = pair_for(path, config)
            if (root / counterpart).is_file():
                findings.append(Finding(
                    "error", "BILINGUAL_PAIR_REMOVED", counterpart, None,
                    f"deleting {path} leaves its first-party language companion orphaned",
                    "restore the deleted companion, remove both pages deliberately, or add a narrow documented exception",
                ))
            continue
        if re.search(r"_(?:CN|zh|cn)\.md$", path):
            findings.append(Finding(
                "warning", "NONSTANDARD_FIRST_PARTY_LANGUAGE_SUFFIX", path, None,
                "first-party Simplified Chinese documentation does not use the _ZH.md convention",
                "prefer an English main file with a _ZH.md companion; preserve upstream names in upstream-owned trees",
            ))
        counterpart, role = pair_for(path, config)
        counterpart_path = root / counterpart
        if not counterpart_path.is_file():
            severity = "warning" if all_mode else "error"
            findings.append(Finding(
                severity, "BILINGUAL_PAIR_MISSING", path, None,
                f"first-party Markdown has no expected companion: {counterpart}",
                "split the page into an English main file and _ZH.md companion; exempt only an explicit user requirement or machine consumer",
            ))
            continue
        english, chinese = (path, counterpart) if role == "english" else (counterpart, path)
        pairs.add((english, chinese))

    for english, chinese in sorted(pairs):
        for source, target in ((english, chinese), (chinese, english)):
            text = read_text(root / source)
            links = resolved_link_paths(root, source, text)
            target_lines = [line for linked, line in links if linked == target]
            if not target_lines:
                findings.append(Finding(
                    "error", "BILINGUAL_LINK_MISSING", source, None,
                    f"language companion is not linked: {target}",
                    "add a reciprocal language entry near the top of both files",
                ))
            elif min(target_lines) > 40:
                findings.append(Finding(
                    "warning", "BILINGUAL_LINK_NOT_NEAR_TOP", source, min(target_lines),
                    f"language companion link appears late in the document: {target}",
                    "place the language switch near the top so readers can find it immediately",
                ))

    checked_paths = {change.path for change in selected}
    for path in sorted(checked_paths):
        if classifications.get(path) not in AUDITED_OWNERSHIP:
            continue
        if matches(path, config["language_link_exempt_patterns"]):
            continue
        full_path = root / path
        if not full_path.is_file():
            continue
        counterpart, source_role = pair_for(path, config)
        own_pair = counterpart
        document_text = read_text(full_path)
        resolved_links = resolved_link_paths(root, path, document_text)
        block_ids = markdown_block_ids(document_text)
        links_by_block: dict[int, set[str]] = {}
        for linked, line in resolved_links:
            links_by_block.setdefault(block_ids.get(line, line), set()).add(linked)
        for linked, line in resolved_links:
            if linked == own_pair or not linked.lower().endswith(".md"):
                continue
            sibling, linked_role = pair_for(linked, config)
            same_block = links_by_block.get(block_ids.get(line, line), set())
            if source_role == "chinese":
                if linked_role == "chinese":
                    continue
                if sibling in same_block:
                    continue
                if (root / sibling).is_file():
                    findings.append(Finding(
                        "error", "WRONG_LANGUAGE_INTERNAL_LINK", path, line,
                        f"Chinese page links to English target although a Chinese companion exists: {linked}",
                        f"link to {sibling} instead",
                    ))
            elif linked_role == "chinese":
                if sibling in same_block:
                    continue
                if (root / sibling).is_file():
                    findings.append(Finding(
                        "error", "WRONG_LANGUAGE_INTERNAL_LINK", path, line,
                        f"English page links to Chinese target although an English companion exists: {linked}",
                        f"link to {sibling} instead",
                    ))
    return findings


def line_for_offset(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def sensitive_findings(path: str, text: str, config: dict) -> list[Finding]:
    findings: list[Finding] = []
    allow = [re.compile(pattern) for pattern in config["sensitive_allow_regexes"]]
    for code, pattern, hint in SENSITIVE_RULES:
        for match in pattern.finditer(text):
            # Allow only this exact detected value.  A safe placeholder elsewhere
            # on the line must never suppress another credential or identifier.
            if any(item.fullmatch(match.group(0)) for item in allow):
                continue
            findings.append(Finding(
                "error", code, path, line_for_offset(text, match.start()),
                "public Markdown contains a disallowed environment or private-data shape (value redacted)",
                hint,
            ))
    local_user = os.environ.get("USERNAME") or os.environ.get("USER") or Path.home().name
    if local_user and len(local_user) >= 3:
        pattern = re.compile(rf"(?i)(?<![A-Za-z0-9_.-]){re.escape(local_user)}(?![A-Za-z0-9_.-])")
        for match in pattern.finditer(text):
            if any(item.fullmatch(match.group(0)) for item in allow):
                continue
            findings.append(Finding(
                "error", "LOCAL_USERNAME", path, line_for_offset(text, match.start()),
                "public Markdown contains the current machine username (value redacted)",
                "remove the local identity or replace it with a role-based placeholder",
            ))
    return findings


def heading_token(title: str) -> str:
    value = html.unescape(TAG_RE.sub("", title)).strip()
    if value and EMOJI_RE.match(value[0]):
        return value.split(maxsplit=1)[0]
    return ""


def header_quick_links(text: str) -> list[tuple[str, str, str]]:
    header_end = text.lower().find("</div>")
    header = text if header_end < 0 else text[:header_end]
    result: list[tuple[str, str, str]] = []
    for href, body in HTML_ANCHOR_RE.findall(header):
        label = html.unescape(TAG_RE.sub("", body)).strip()
        if not label:
            continue
        for key, expected, pattern in QUICK_LINK_RULES:
            if pattern.search(label):
                result.append((key, heading_token(label), expected))
                break
    return result


def homepage_header(text: str) -> tuple[str, int, bool]:
    centered = CENTERED_DIV_RE.search(text)
    start = centered.start() if centered else 0
    end_start = text.lower().find("</div>", start)
    if end_start < 0:
        return (text[start:] if centered else "", len(text), bool(centered))
    end = end_start + len("</div>")
    return text[start:end], end, bool(centered)


def homepage_badges(header: str) -> list[str]:
    result: list[str] = []
    for href, body in HTML_ANCHOR_RE.findall(header):
        image = HTML_IMG_RE.search(body)
        if image is None:
            continue
        alt_match = HTML_ALT_RE.search(image.group(0))
        alt = alt_match.group(1) if alt_match else ""
        searchable = html.unescape(alt + " " + href)
        role = "other"
        for key, pattern in BADGE_RULES:
            if pattern.search(searchable):
                role = key
                break
        result.append(role)
    return result


def ordered_subsequence(required: Sequence[str], actual: Sequence[str]) -> bool:
    if not required:
        return True
    index = 0
    for item in actual:
        if item == required[index]:
            index += 1
            if index == len(required):
                return True
    return False


def homepage_findings(
    root: Path,
    english: str,
    chinese: str,
    settings: dict | None = None,
    global_h3_allow_patterns: Sequence[str] = (),
) -> list[Finding]:
    findings: list[Finding] = []
    settings = settings or {"profile": "auto"}
    profile = settings.get("profile", "auto")
    required_components = HOMEPAGE_PROFILES[profile] | set(settings.get("required_components", []))
    required_quick = settings.get("required_quick_links", [])
    required_badges = settings.get("required_badges", [])
    required_h2 = settings.get("required_h2_icons", [])
    h3_allow = [
        re.compile(pattern)
        for pattern in [
            *global_h3_allow_patterns,
            *settings.get("h3_emoji_allow_patterns", []),
        ]
    ]
    texts = {english: read_text(root / english), chinese: read_text(root / chinese)}
    h2_tokens: dict[str, list[str]] = {}
    quick: dict[str, list[tuple[str, str, str]]] = {}
    badges: dict[str, list[str]] = {}
    components: dict[str, dict[str, bool]] = {}
    allowed_h3: dict[str, list[tuple[int, str]]] = {}
    for path, text in texts.items():
        counterpart = chinese if path == english else english
        header, header_end, centered = homepage_header(text)
        h1 = HTML_H1_RE.search(header)
        if not h1:
            findings.append(Finding(
                "warning", "HOMEPAGE_HTML_H1_MISSING", path, None,
                "homepage does not contain the mature centered HTML product-title pattern",
                "preserve or add a plain product-name <h1> inside the centered visual header",
            ))
        elif EMOJI_RE.search(html.unescape(TAG_RE.sub("", h1.group(1)))):
            findings.append(Finding(
                "error", "HOMEPAGE_H1_EMOJI", path, line_for_offset(text, h1.start(1)),
                "HTML product title contains an emoji",
                "keep the level-one product title as the plain product name",
            ))
        tokens: list[str] = []
        for match in H2_RE.finditer(text):
            token = heading_token(match.group(1))
            tokens.append(token)
            if not token:
                findings.append(Finding(
                    "error", "HOMEPAGE_H2_EMOJI_MISSING", path, line_for_offset(text, match.start(1)),
                    f"primary section lacks a leading semantic emoji: {match.group(1).strip()}",
                    "restore a stable semantic emoji on each primary ## section",
                ))
            else:
                title = EMOJI_RE.sub("", html.unescape(TAG_RE.sub("", match.group(1))), count=1).lstrip("️ ")
                for expected, pattern in H2_ICON_RULES:
                    if pattern.search(title):
                        if token != expected:
                            findings.append(Finding(
                                "error", "HOMEPAGE_H2_ICON", path, line_for_offset(text, match.start(1)),
                                f"primary section semantic icon is {token}, expected {expected} for {title!r}",
                                "restore the stable semantic icon while keeping the localized section text",
                            ))
                        break
        h2_tokens[path] = tokens
        allowed_h3[path] = []
        for index, match in enumerate(H3_RE.finditer(text)):
            raw_title = html.unescape(TAG_RE.sub("", match.group(1))).strip()
            token = heading_token(raw_title)
            if token and any(pattern.fullmatch(raw_title) for pattern in h3_allow):
                allowed_h3[path].append((index, token))
            elif token:
                findings.append(Finding(
                    "error", "HOMEPAGE_H3_EMOJI", path, line_for_offset(text, match.start(1)),
                    f"tertiary section unexpectedly starts with an emoji: {match.group(1).strip()}",
                    "keep ### headings plain or add a narrow anchored allow pattern for an intentional bilingual exception",
                ))
        quick[path] = header_quick_links(header)
        for key, actual, expected in quick[path]:
            if actual != expected:
                findings.append(Finding(
                    "error", "HOMEPAGE_QUICK_LINK_ICON", path, None,
                    f"quick-link semantic {key!r} uses {actual or 'no icon'} instead of {expected}",
                    "restore the standard semantic icon without copying product-specific URLs or text",
                ))
        badges[path] = homepage_badges(header)
        header_images = len(HTML_IMG_RE.findall(header))
        language_targets = {
            linked for linked, _ in resolved_link_paths(root, path, header)
        }
        tail = text[header_end:]
        has_separator = bool(
            re.match(r"(?:[ \t]*\r?\n)*[ \t]*(?:---|\*\*\*|___)[ \t]*(?:\r?\n|$)", tail)
        )
        components[path] = {
            "centered_header": centered,
            "html_h1": h1 is not None,
            "subtitle": bool(STRONG_RE.search(header)),
            "badges": bool(badges[path]),
            "language_switch": counterpart in language_targets,
            "quick_links": bool(quick[path]),
            "hero_image": header_images > len(badges[path]),
            "separator": has_separator,
            "h2": bool(tokens),
        }
        for component in sorted(required_components):
            if not components[path][component]:
                findings.append(Finding(
                    "error", "HOMEPAGE_REQUIRED_COMPONENT_MISSING", path, None,
                    f"homepage profile {profile!r} requires component {component!r}",
                    "restore the mature baseline component or select/configure the correct generic homepage profile",
                ))
        actual_quick = [key for key, _, _ in quick[path]]
        if not ordered_subsequence(required_quick, actual_quick):
            findings.append(Finding(
                "error", "HOMEPAGE_REQUIRED_QUICK_LINK_MISSING", path, None,
                f"required quick-link sequence is absent: {required_quick}",
                "restore the configured semantic quick-link roles in order; localized URLs may differ",
            ))
        if not ordered_subsequence(required_badges, badges[path]):
            findings.append(Finding(
                "error", "HOMEPAGE_REQUIRED_BADGE_MISSING", path, None,
                f"required badge-role sequence is absent: {required_badges}",
                "restore the configured generic badge roles without hardcoding a product URL",
            ))
        if not ordered_subsequence(required_h2, tokens):
            findings.append(Finding(
                "error", "HOMEPAGE_REQUIRED_H2_MISSING", path, None,
                f"required primary-section emoji sequence is absent: {required_h2}",
                "restore the configured baseline sections and their semantic emoji in order",
            ))
    for component in sorted(HOMEPAGE_COMPONENTS):
        if components[english][component] != components[chinese][component]:
            findings.append(Finding(
                "error", "HOMEPAGE_COMPONENT_ASYMMETRY", english, None,
                f"English and Chinese homepage component {component!r} differs",
                "keep the generic visual/header contract symmetric across languages",
            ))
    if h2_tokens[english] != h2_tokens[chinese]:
        findings.append(Finding(
            "error", "HOMEPAGE_H2_ASYMMETRY", english, None,
            f"English and Chinese primary-section emoji sequences differ: {h2_tokens[english]} != {h2_tokens[chinese]}",
            "keep primary section presence, order, and semantic emoji symmetric across languages",
        ))
    english_quick = [(key, icon) for key, icon, _ in quick[english]]
    chinese_quick = [(key, icon) for key, icon, _ in quick[chinese]]
    if english_quick != chinese_quick:
        findings.append(Finding(
            "error", "HOMEPAGE_QUICK_LINK_ASYMMETRY", english, None,
            f"English and Chinese quick-link semantics differ: {english_quick} != {chinese_quick}",
            "keep semantic slots, icons, order, and presence symmetric; localized external URLs may differ",
        ))
    if badges[english] != badges[chinese]:
        findings.append(Finding(
            "error", "HOMEPAGE_BADGE_ASYMMETRY", english, None,
            f"English and Chinese badge roles differ: {badges[english]} != {badges[chinese]}",
            "keep badge roles and order symmetric while localizing accessible labels",
        ))
    if allowed_h3[english] != allowed_h3[chinese]:
        findings.append(Finding(
            "error", "HOMEPAGE_H3_EMOJI_ASYMMETRY", english, None,
            f"allowed tertiary emoji positions differ: {allowed_h3[english]} != {allowed_h3[chinese]}",
            "use the same allowed emoji at the corresponding ### position in both languages",
        ))
    return findings


def docs_only_findings(changes: list[Change], config: dict) -> list[Finding]:
    findings: list[Finding] = []
    for change in changes:
        for path in (change.path, change.old_path):
            if path is None or path.lower().endswith(".md") or path in config["docs_only_allowed_patterns"]:
                continue
            name = PurePosixPath(path).name
            suffix = PurePosixPath(path).suffix.lower()
            if suffix in FIRMWARE_SUFFIXES:
                code = "DOCS_ONLY_FIRMWARE_BINARY"
                detail = "firmware binary"
                hint = "restore the immutable artifact; documentation work must not rebuild or replace it"
            elif suffix in PACKAGE_SUFFIXES:
                code = "DOCS_ONLY_RELEASE_PACKAGE"
                detail = "release or delivery package"
                hint = "restore the immutable package; documentation splitting must not repackage or change its hash"
            elif suffix in SOURCE_SUFFIXES or name in SOURCE_NAMES:
                code = "DOCS_ONLY_SOURCE_CHANGE"
                detail = "source or executable script"
                hint = "separate implementation changes from the documentation-only diff"
            elif suffix in CONFIG_SUFFIXES or name in CONFIG_NAMES:
                code = "DOCS_ONLY_CONFIG_CHANGE"
                detail = "configuration or workflow input"
                hint = "separate configuration changes from the documentation-only diff"
            else:
                code = "DOCS_ONLY_NON_MARKDOWN_CHANGE"
                detail = "non-Markdown file"
                hint = "allow a narrow documentation asset pattern in config or remove it from the docs-only diff"
            findings.append(Finding(
                "error", code, path, None,
                f"documentation-only scope includes a {detail}", hint,
            ))
    return findings


def audit(
    root: Path,
    changes: list[Change],
    config: dict,
    *,
    all_mode: bool,
    expect_docs_only: bool,
) -> dict:
    root = root.resolve()
    validate_change_paths(changes)
    inventory = tracked_markdown_files(root, config) if all_mode else iter_markdown_files(root, config)
    gitlink_roots = discover_gitlink_roots(root)
    dynamic_upstream_roots = discover_dynamic_upstream_roots(root)
    classifications = {
        path: classify_at_root(path, config, gitlink_roots, dynamic_upstream_roots)
        for path in inventory
    }
    for change in changes:
        if change.path.lower().endswith(".md"):
            classifications.setdefault(
                change.path,
                classify_at_root(change.path, config, gitlink_roots, dynamic_upstream_roots),
            )
    candidates = [
        change for change in changes
        if change.status != "D" and change.path.lower().endswith(".md") and (root / change.path).is_file()
        and not is_excluded(change.path, config)
    ]
    selected = [
        change for change in candidates
        if not (all_mode and classifications[change.path] in UPSTREAM_OWNERSHIP)
    ]
    findings: list[Finding] = []
    if expect_docs_only:
        findings.extend(docs_only_findings(changes, config))
    for change in selected:
        category = classifications[change.path]
        text = read_text(root / change.path)
        findings.extend(link_findings(root, change.path, text, config))
        if category in AUDITED_OWNERSHIP or category == "unknown":
            findings.extend(sensitive_findings(change.path, text, config))
        elif category in UPSTREAM_OWNERSHIP:
            if not all_mode:
                findings.append(Finding(
                    "warning", "UPSTREAM_MARKDOWN_CHANGED", change.path, None,
                    f"changed Markdown is classified as {category}",
                    "avoid product-local translation or rewriting; update from upstream or override classification narrowly",
                ))
        if category == "unknown":
            findings.append(Finding(
                "warning" if all_mode else "error", "MARKDOWN_OWNERSHIP_UNKNOWN", change.path, None,
                "Markdown ownership is unknown",
                "classify the path narrowly as first-party, managed, third-party, or embedded upstream before editing",
            ))
    deleted_markdown = [
        change for change in changes
        if change.status == "D" and change.path.lower().endswith(".md") and not is_excluded(change.path, config)
    ]
    findings.extend(bilingual_findings(root, selected + deleted_markdown, classifications, config, all_mode))
    selected_paths = {change.path for change in selected}
    for pair in config["homepage_pairs"]:
        english, chinese = pair["english"], pair["chinese"]
        if not (root / english).is_file() or not (root / chinese).is_file():
            continue
        if all_mode or english in selected_paths or chinese in selected_paths:
            findings.extend(homepage_findings(
                root,
                english,
                chinese,
                pair,
                config["homepage_h3_emoji_allow_patterns"],
            ))
    findings.sort(key=lambda item: (0 if item.severity == "error" else 1, item.path.lower(), item.line or 0, item.code))
    return {
        "schema_version": 1,
        "repository": root.name,
        "scope": {
            "all_markdown": all_mode,
            "expect_docs_only": expect_docs_only,
            "changed_files": 0 if all_mode else len(changes),
            "inventory_markdown": len(inventory),
            "selected_markdown": len(selected),
        },
        "classification_inventory": dict(sorted(Counter(classifications.values()).items())),
        "selected_files": [
            {
                "status": "I" if all_mode else change.status,
                "path": change.path,
                "category": classifications[change.path],
            }
            for change in selected
        ],
        "findings": [asdict(item) for item in findings],
        "summary": {
            "errors": sum(item.severity == "error" for item in findings),
            "warnings": sum(item.severity == "warning" for item in findings),
        },
    }


def print_text(report: dict) -> None:
    scope = report["scope"]
    counts = ", ".join(f"{key}={value}" for key, value in report["classification_inventory"].items()) or "none"
    print(f"Markdown audit: {report['repository']}")
    print(f"Scope: changed_files={scope['changed_files']} selected_markdown={scope['selected_markdown']} all={scope['all_markdown']} docs_only={scope['expect_docs_only']}")
    print(f"Classification inventory: {counts}")
    for finding in report["findings"]:
        location = finding["path"] + (f":{finding['line']}" if finding["line"] else "")
        print(f"[{finding['severity'].upper()}] {finding['code']} {location}: {finding['message']}")
        print(f"  hint: {finding['hint']}")
    summary = report["summary"]
    print(f"Summary: errors={summary['errors']} warnings={summary['warnings']}")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Audit Markdown ownership, bilingual links, homepage symmetry, privacy, and docs-only scope."
    )
    parser.add_argument("repo", type=Path, help="repository checkout to audit")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--base", help="audit files changed from merge-base(BASE, HEAD)")
    source.add_argument("--working-tree", action="store_true", help="audit staged, unstaged, and untracked files")
    source.add_argument("--changed-files-from", type=Path, help="read paths or git name-status lines from a UTF-8 file")
    source.add_argument("--all", action="store_true", help="audit all Markdown files; existing unpaired pages are warnings")
    parser.add_argument("--config", type=Path, help="optional JSON policy extension")
    parser.add_argument("--expect-docs-only", action="store_true", help="fail if the selected diff contains non-doc scope")
    parser.add_argument("--strict", action="store_true", help="make warnings fail the command")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    if not args.repo.is_dir():
        print(f"error: repository path does not exist or is not a directory: {args.repo}", file=sys.stderr)
        return 2
    if args.all and args.expect_docs_only:
        print("error: --expect-docs-only requires a changed-file scope, not --all", file=sys.stderr)
        return 2
    try:
        config = load_config(args.config)
        root = args.repo.resolve()
        if args.base:
            require_clean_base_checkout(root)
            changes = changes_from_base(root, args.base)
        elif args.working_tree:
            changes = changes_from_worktree(root)
        elif args.changed_files_from:
            changes = changes_from_file(args.changed_files_from)
        else:
            changes = [Change("I", path) for path in tracked_markdown_files(root, config)]
        report = audit(
            root, changes, config, all_mode=args.all, expect_docs_only=args.expect_docs_only
        )
    except AuditError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:  # pragma: no cover - defensive CLI boundary
        print(f"internal error: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 3
    if args.format == "json":
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print_text(report)
    if report["summary"]["errors"] or (args.strict and report["summary"]["warnings"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
