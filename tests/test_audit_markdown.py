from __future__ import annotations

import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = REPOSITORY_ROOT / "scripts" / "audit_markdown.py"
SPEC = importlib.util.spec_from_file_location("repository_audit_markdown", SCRIPT_PATH)
assert SPEC and SPEC.loader
audit_markdown = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = audit_markdown
SPEC.loader.exec_module(audit_markdown)


EN_HOME = """<div align="center">
  <h1>Demo Board</h1>
  <p><strong>Compact synthetic development board</strong></p>
  <p>
    <a href="https://example.invalid/actions"><img alt="Build Examples" src="https://example.invalid/build.svg"></a>
    <a href="https://example.invalid/releases"><img alt="Latest Release" src="https://example.invalid/release.svg"></a>
    <a href="LICENSE"><img alt="License" src="https://example.invalid/license.svg"></a>
  </p>
  <p><a href="README_ZH.md">简体中文</a> · <a href="https://example.invalid/product">🌐 Product Page</a> · <a href="docs/guide.md">📚 Documentation</a> · <a href="https://example.invalid/releases">📦 Firmware</a> · <a href="docs/guide.md">🚀 Quick Start</a> · <a href="examples/esp-idf/">🧩 ESP-IDF</a> · <a href="examples/arduino/">🔧 Arduino</a></p>
  <img src="assets/hero.png" alt="Demo Board" width="500">
</div>

---

## ✨ Overview

[Guide](docs/guide.md)

### ESP-IDF
"""

ZH_HOME = """<div align="center">
  <h1>Demo Board</h1>
  <p><strong>紧凑型合成测试开发板</strong></p>
  <p>
    <a href="https://example.invalid/actions"><img alt="构建示例" src="https://example.invalid/build.svg"></a>
    <a href="https://example.invalid/releases"><img alt="最新版本" src="https://example.invalid/release.svg"></a>
    <a href="LICENSE"><img alt="许可证" src="https://example.invalid/license.svg"></a>
  </p>
  <p><a href="README.md">English</a> · <a href="https://zh.example.invalid/product">🌐 产品页面</a> · <a href="docs/guide_ZH.md">📚 产品文档</a> · <a href="https://example.invalid/releases">📦 固件</a> · <a href="docs/guide_ZH.md">🚀 快速开始</a> · <a href="examples/esp-idf/">🧩 ESP-IDF</a> · <a href="examples/arduino/">🔧 Arduino</a></p>
  <img src="assets/hero.png" alt="Demo Board" width="500">
</div>

---

## ✨ 概述

[指南](docs/guide_ZH.md)

### ESP-IDF
"""


class MarkdownAuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.write("README.md", EN_HOME)
        self.write("README_ZH.md", ZH_HOME)
        self.write(
            "docs/guide.md",
            "[简体中文](guide_ZH.md)\n\n[Home](../README.md)\n\nUse `COMx` or `/dev/ttyACM0`.\n",
        )
        self.write(
            "docs/guide_ZH.md",
            "[English](guide.md)\n\n[首页](../README_ZH.md)\n\n使用 `<PORT>`。\n",
        )
        self.write("examples/esp-idf/.keep", "")
        self.write("examples/arduino/.keep", "")
        self.write("assets/hero.png", b"synthetic-hero")
        self.write("LICENSE", "synthetic license\n")

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def write(self, relative: str, text: str | bytes) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(text, bytes):
            path.write_bytes(text)
        else:
            path.write_text(text, encoding="utf-8")

    def init_git(self, origin: str = "https://github.com/example-org/product-repo.git") -> None:
        commands = (
            ["git", "init"],
            ["git", "config", "user.name", "Synthetic Test"],
            ["git", "config", "user.email", "synthetic@example.invalid"],
            ["git", "remote", "add", "origin", origin],
        )
        for command in commands:
            subprocess.run(
                command,
                cwd=self.root,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

    def write_component_source_root(self, relative: str, manifest: str, readme: str) -> None:
        self.write(f"{relative}/idf_component.yml", manifest)
        self.write(f"{relative}/CMakeLists.txt", "idf_component_register(SRCS component.c)\n")
        self.write(f"{relative}/component.c", "void synthetic_component(void) {}\n")
        self.write(f"{relative}/LICENSE", "synthetic permissive license\n")
        self.write(f"{relative}/README.md", readme)

    def changes(self, *paths: str, status: str = "A") -> list:
        return [audit_markdown.Change(status, path) for path in paths]

    def report(
        self,
        changes: list,
        *,
        config: dict | None = None,
        docs_only: bool = False,
        all_mode: bool = False,
    ) -> dict:
        return audit_markdown.audit(
            self.root,
            changes,
            config or audit_markdown.load_config(None),
            all_mode=all_mode,
            expect_docs_only=docs_only,
        )

    @staticmethod
    def codes(report: dict) -> set[str]:
        return {item["code"] for item in report["findings"]}

    def test_clean_bilingual_repository_passes(self) -> None:
        report = self.report(
            self.changes("README.md", "README_ZH.md", "docs/guide.md", "docs/guide_ZH.md"),
            docs_only=True,
        )
        self.assertEqual({"errors": 0, "warnings": 0}, report["summary"])
        self.assertEqual("first_party_customer", report["selected_files"][0]["category"])

    def test_new_first_party_page_requires_pair(self) -> None:
        self.write("docs/new-feature.md", "[Home](../README.md)\n")
        report = self.report(self.changes("docs/new-feature.md"))
        self.assertIn("BILINGUAL_PAIR_MISSING", self.codes(report))
        self.assertEqual(1, report["summary"]["errors"])

    def test_modified_mixed_language_page_requires_split(self) -> None:
        self.write("docs/mixed.md", "# Guide / 指南\n\nEnglish.\n\n中文。\n")
        changed_report = self.report(self.changes("docs/mixed.md", status="M"))
        self.assertIn("BILINGUAL_PAIR_MISSING", self.codes(changed_report))
        self.assertEqual(1, changed_report["summary"]["errors"])

        historical_report = self.report(
            self.changes("docs/mixed.md", status="S"),
            all_mode=True,
        )
        self.assertEqual({"errors": 0, "warnings": 1}, historical_report["summary"])

    def test_deleting_one_language_companion_fails(self) -> None:
        (self.root / "docs" / "guide_ZH.md").unlink()
        report = self.report(self.changes("docs/guide_ZH.md", status="D"))
        self.assertIn("BILINGUAL_PAIR_REMOVED", self.codes(report))

    def test_reciprocal_and_same_language_links_are_enforced(self) -> None:
        self.write("docs/guide_ZH.md", "[首页](../README.md)\n")
        report = self.report(self.changes("docs/guide.md", "docs/guide_ZH.md", status="M"))
        self.assertIn("BILINGUAL_LINK_MISSING", self.codes(report))
        self.assertIn("WRONG_LANGUAGE_INTERNAL_LINK", self.codes(report))

    def test_side_by_side_language_index_is_not_wrong_language(self) -> None:
        self.write(
            "docs/index.md",
            "[简体中文](index_ZH.md)\n\n[Guide](guide.md)\n([指南](guide_ZH.md))\n",
        )
        self.write(
            "docs/index_ZH.md",
            "[English](index.md)\n\n[指南](guide_ZH.md)\n([Guide](guide.md))\n",
        )
        report = self.report(self.changes("docs/index.md", "docs/index_ZH.md"))
        self.assertNotIn("WRONG_LANGUAGE_INTERNAL_LINK", self.codes(report))

    def test_relative_links_cover_markdown_reference_html_and_code_fences(self) -> None:
        self.write("docs/My Guide.md", "target\n")
        self.write(
            "docs/links.md",
            "[inline](My%20Guide.md)\n[ref][guide]\n<img src=\"../assets/pixel.png\">\n"
            "[guide]: <My%20Guide.md>\n```md\n[ignored](missing.md)\n```\n",
        )
        self.write("docs/links_ZH.md", "[English](links.md)\n")
        self.write("assets/pixel.png", b"synthetic")
        report = self.report(self.changes("docs/links.md", "docs/links_ZH.md"))
        self.assertNotIn("RELATIVE_LINK_MISSING", self.codes(report))
        self.write("docs/links.md", "[简体中文](links_ZH.md)\n[broken](absent.md)\n")
        report = self.report(self.changes("docs/links.md", status="M"))
        self.assertIn("RELATIVE_LINK_MISSING", self.codes(report))

    def test_relative_link_fragments_use_github_slugs_duplicates_unicode_and_html_ids(self) -> None:
        self.write(
            "docs/target.md",
            "# Hello, World!\n\n"
            "## Repeat\n\n"
            "## Repeat\n\n"
            "## 中文 标题\n\n"
            '<a id="manual-anchor"></a>\n'
            '<span name="legacy-anchor"></span>\n',
        )
        self.write(
            "docs/anchors.md",
            "[简体中文](anchors_ZH.md)\n\n"
            "## Local Heading\n\n"
            "[same page](#local-heading)\n"
            "[punctuation](target.md#hello-world)\n"
            "[first duplicate](target.md#repeat)\n"
            "[second duplicate](target.md#repeat-1)\n"
            "[Unicode](target.md#%E4%B8%AD%E6%96%87-%E6%A0%87%E9%A2%98)\n"
            "[HTML id](target.md#manual-anchor)\n"
            "[HTML name](target.md#legacy-anchor)\n",
        )
        self.write("docs/anchors_ZH.md", "[English](anchors.md)\n")
        report = self.report(self.changes("docs/anchors.md", "docs/anchors_ZH.md"))
        self.assertNotIn("RELATIVE_LINK_FRAGMENT_MISSING", self.codes(report))

        self.write(
            "docs/anchors.md",
            (self.root / "docs" / "anchors.md").read_text(encoding="utf-8")
            + "[broken fragment](target.md#repeat-2)\n",
        )
        report = self.report(self.changes("docs/anchors.md", status="M"))
        self.assertIn("RELATIVE_LINK_FRAGMENT_MISSING", self.codes(report))

    def test_sensitive_allowlist_is_scoped_to_the_matched_value(self) -> None:
        token = "ghp_" + "B" * 36
        self.write(
            "docs/scoped.md",
            f"[简体中文](scoped_ZH.md)\nAllowed COM42 and forbidden {token} share a line.\n",
        )
        self.write("docs/scoped_ZH.md", "[English](scoped.md)\n")
        config = audit_markdown.load_config(None)
        config["sensitive_allow_regexes"] = [r"^COM42$"]
        report = self.report(
            self.changes("docs/scoped.md", "docs/scoped_ZH.md"), config=config
        )
        self.assertNotIn("ACTUAL_SERIAL_PORT", self.codes(report))
        self.assertIn("CREDENTIAL_OR_TOKEN", self.codes(report))
        self.assertNotIn(token, json.dumps(report))

    def test_homepage_visual_contract(self) -> None:
        self.write(
            "README.md",
            EN_HOME.replace("<h1>Demo Board</h1>", "<h1>🚀 Demo Board</h1>")
            .replace("🌐 Product Page", "Product Page")
            .replace("### ESP-IDF", "### 🧩 ESP-IDF"),
        )
        self.write("README_ZH.md", ZH_HOME.replace("## ✨ 概述", "## 📚 概述"))
        report = self.report(self.changes("README.md", "README_ZH.md", status="M"))
        codes = self.codes(report)
        self.assertIn("HOMEPAGE_H1_EMOJI", codes)
        self.assertIn("HOMEPAGE_QUICK_LINK_ICON", codes)
        self.assertIn("HOMEPAGE_H2_ICON", codes)
        self.assertIn("HOMEPAGE_H2_ASYMMETRY", codes)
        self.assertIn("HOMEPAGE_H3_EMOJI", codes)

    def test_single_product_profile_detects_symmetric_homepage_deletion(self) -> None:
        config = audit_markdown.load_config(None)
        config["homepage_pairs"] = [
            {
                "english": "README.md",
                "chinese": "README_ZH.md",
                "profile": "single-product",
                "required_components": [],
                "required_quick_links": [
                    "product",
                    "documentation",
                    "firmware",
                    "quick_start",
                    "esp_idf",
                    "arduino",
                ],
                "required_badges": ["build", "release", "license"],
                "required_h2_icons": ["✨"],
                "h3_emoji_allow_patterns": [],
            }
        ]
        baseline = self.report(
            self.changes("README.md", "README_ZH.md", status="M"), config=config
        )
        self.assertEqual({"errors": 0, "warnings": 0}, baseline["summary"])

        for path in ("README.md", "README_ZH.md"):
            text = (self.root / path).read_text(encoding="utf-8")
            text = text.replace('<div align="center">\n', "")
            text = text.replace("</div>\n\n---\n", "")
            text = re.sub(r"(?m)^## ✨ .*\n", "", text)
            self.write(path, text)
        report = self.report(
            self.changes("README.md", "README_ZH.md", status="M"), config=config
        )
        codes = self.codes(report)
        self.assertIn("HOMEPAGE_REQUIRED_COMPONENT_MISSING", codes)
        self.assertIn("HOMEPAGE_REQUIRED_QUICK_LINK_MISSING", codes)
        self.assertIn("HOMEPAGE_REQUIRED_BADGE_MISSING", codes)
        self.assertIn("HOMEPAGE_REQUIRED_H2_MISSING", codes)
        self.assertNotIn("HOMEPAGE_COMPONENT_ASYMMETRY", codes)

    def test_h3_emoji_requires_narrow_allow_pattern_and_bilingual_symmetry(self) -> None:
        self.write("README.md", EN_HOME.replace("### ESP-IDF", "### ⚠️ Caution"))
        self.write("README_ZH.md", ZH_HOME.replace("### ESP-IDF", "### ⚠️ 注意"))
        config = audit_markdown.load_config(None)
        config["homepage_h3_emoji_allow_patterns"] = [r"^⚠️ (?:Caution|注意)$"]
        report = self.report(
            self.changes("README.md", "README_ZH.md", status="M"), config=config
        )
        self.assertNotIn("HOMEPAGE_H3_EMOJI", self.codes(report))
        self.assertNotIn("HOMEPAGE_H3_EMOJI_ASYMMETRY", self.codes(report))

        self.write("README_ZH.md", ZH_HOME.replace("### ESP-IDF", "### ℹ️ 注意"))
        report = self.report(
            self.changes("README.md", "README_ZH.md", status="M"), config=config
        )
        self.assertIn("HOMEPAGE_H3_EMOJI", self.codes(report))
        self.assertIn("HOMEPAGE_H3_EMOJI_ASYMMETRY", self.codes(report))

        config_path = self.root / "broad-h3.json"
        config_path.write_text(
            json.dumps({"homepage_h3_emoji_allow_patterns": [".*"]}),
            encoding="utf-8",
        )
        with self.assertRaises(audit_markdown.AuditError):
            audit_markdown.load_config(config_path)

    def test_centralized_chinese_tree_pairs_links_and_homepage_profile(self) -> None:
        en_home = """<div align="center">
  <h1>Product Family</h1>
  <p><strong>Multi-product documentation hub</strong></p>
  <p><a href="docs/zh-CN/README.md">简体中文</a></p>
</div>

---

## ✨ Overview

[Guide](docs/guide.md)
"""
        zh_home = """<div align="center">
  <h1>Product Family</h1>
  <p><strong>多产品文档中心</strong></p>
  <p><a href="../../README.md">English</a></p>
</div>

---

## ✨ 概述

[指南](guide.md)
"""
        self.write("README.md", en_home)
        self.write("docs/zh-CN/README.md", zh_home)
        self.write(
            "docs/guide.md",
            "[简体中文](zh-CN/guide.md)\n\n[Home](../README.md)\n",
        )
        self.write(
            "docs/zh-CN/guide.md",
            "[English](../guide.md)\n\n[首页](README.md)\n",
        )
        config_path = self.root / "central-policy.json"
        config_path.write_text(
            json.dumps(
                {
                    "bilingual_pairs": [
                        {"english": "README.md", "chinese": "docs/zh-CN/README.md"}
                    ],
                    "bilingual_directory_mappings": [
                        {"english": "docs", "chinese": "docs/zh-CN"}
                    ],
                    "homepage_pairs": [
                        {
                            "english": "README.md",
                            "chinese": "docs/zh-CN/README.md",
                            "profile": "multi-product-hub",
                            "required_h2_icons": ["✨"],
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        config = audit_markdown.load_config(config_path)
        report = self.report(
            self.changes(
                "README.md",
                "docs/zh-CN/README.md",
                "docs/guide.md",
                "docs/zh-CN/guide.md",
            ),
            config=config,
        )
        relevant = {
            code for code in self.codes(report)
            if code.startswith("BILINGUAL_")
            or code.startswith("HOMEPAGE_")
            or code == "WRONG_LANGUAGE_INTERNAL_LINK"
        }
        self.assertEqual(set(), relevant, report["findings"])
        self.assertEqual(
            ("docs/zh-CN/guide.md", "english"),
            audit_markdown.pair_for("docs/guide.md", config),
        )
        self.assertEqual(
            ("docs/guide.md", "chinese"),
            audit_markdown.pair_for("docs/zh-CN/guide.md", config),
        )
        self.write(
            "docs/zh-CN/README.md",
            zh_home.replace("[指南](guide.md)", "[指南](../guide.md)"),
        )
        wrong = self.report(
            self.changes("docs/zh-CN/README.md", status="M"), config=config
        )
        self.assertIn("WRONG_LANGUAGE_INTERNAL_LINK", self.codes(wrong))

    def test_centralized_chinese_tree_supports_zh_filename_suffix(self) -> None:
        self.write(
            "docs/en/GUIDE.md",
            "[简体中文](../zh-CN/GUIDE_ZH.md)\n\n[Other](OTHER.md)\n",
        )
        self.write(
            "docs/zh-CN/GUIDE_ZH.md",
            "[English](../en/GUIDE.md)\n\n[其他](OTHER_ZH.md)\n",
        )
        self.write("docs/en/OTHER.md", "English target\n")
        self.write("docs/zh-CN/OTHER_ZH.md", "中文目标\n")
        config_path = self.root / "central-suffix-policy.json"
        config_path.write_text(
            json.dumps(
                {
                    "bilingual_directory_mappings": [
                        {
                            "english": "docs/en",
                            "chinese": "docs/zh-CN",
                            "chinese_suffix": "_ZH",
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        config = audit_markdown.load_config(config_path)
        self.assertEqual(
            ("docs/zh-CN/GUIDE_ZH.md", "english"),
            audit_markdown.pair_for("docs/en/GUIDE.md", config),
        )
        self.assertEqual(
            ("docs/en/GUIDE.md", "chinese"),
            audit_markdown.pair_for("docs/zh-CN/GUIDE_ZH.md", config),
        )
        report = self.report(
            self.changes("docs/en/GUIDE.md", "docs/zh-CN/GUIDE_ZH.md"),
            config=config,
        )
        self.assertFalse(
            {
                "BILINGUAL_PAIR_MISSING",
                "BILINGUAL_LINK_MISSING",
                "WRONG_LANGUAGE_INTERNAL_LINK",
            }
            & self.codes(report),
            report["findings"],
        )

        self.write(
            "docs/zh-CN/GUIDE_ZH.md",
            "[English](../en/GUIDE.md)\n\n[其他](../en/OTHER.md)\n",
        )
        wrong = self.report(
            self.changes("docs/zh-CN/GUIDE_ZH.md", status="M"), config=config
        )
        self.assertIn("WRONG_LANGUAGE_INTERNAL_LINK", self.codes(wrong))

    def test_public_text_rules_redact_values(self) -> None:
        drive_path = "Q:" + "\\Profiles\\sample-user\\build"
        port = "COM" + "42"
        mac = ":".join(["02", "00", "00", "00", "00", "01"])
        token = "ghp_" + "A" * 36
        provenance = "Generated by " + "ChatGPT"
        self.write("docs/private.md", "[简体中文](private_ZH.md)\n" + "\n".join([drive_path, port, mac, token, provenance]))
        self.write("docs/private_ZH.md", "[English](private.md)\n")
        report = self.report(self.changes("docs/private.md", "docs/private_ZH.md"))
        codes = self.codes(report)
        self.assertTrue(
            {"LOCAL_ABSOLUTE_PATH", "ACTUAL_SERIAL_PORT", "MAC_ADDRESS", "CREDENTIAL_OR_TOKEN", "TOOL_OR_MODEL_PROVENANCE"}.issubset(codes)
        )
        rendered = json.dumps(report)
        self.assertNotIn(token, rendered)
        self.assertNotIn(drive_path, rendered)

    def test_docs_only_scope_rejects_source_config_binary_and_package(self) -> None:
        self.write("firmware/app/main.c", "int main(void) { return 0; }\n")
        self.write("firmware/app/idf_component.yml", "dependencies: {}\n")
        self.write("firmware/factory.bin", b"synthetic-firmware")
        self.write("firmware/media.zip", b"synthetic-package")
        self.write("firmware/other.uf2", b"synthetic-other-binary")
        self.write("firmware/other.elf", b"synthetic-other-binary")
        self.write("firmware/other.hex", b"synthetic-other-binary")
        report = self.report(
            self.changes(
                "docs/guide.md",
                "firmware/app/main.c",
                "firmware/app/idf_component.yml",
                "firmware/factory.bin",
                "firmware/media.zip",
                "firmware/other.uf2",
                "firmware/other.elf",
                "firmware/other.hex",
            ),
            docs_only=True,
        )
        codes = self.codes(report)
        self.assertTrue(
            {
                "DOCS_ONLY_SOURCE_CHANGE",
                "DOCS_ONLY_CONFIG_CHANGE",
                "DOCS_ONLY_FIRMWARE_BINARY",
                "DOCS_ONLY_RELEASE_PACKAGE",
            }.issubset(codes)
        )
        by_path = {item["path"]: item["code"] for item in report["findings"]}
        self.assertEqual("DOCS_ONLY_FIRMWARE_BINARY", by_path["firmware/factory.bin"])
        self.assertEqual("DOCS_ONLY_RELEASE_PACKAGE", by_path["firmware/media.zip"])
        for path in ("firmware/other.uf2", "firmware/other.elf", "firmware/other.hex"):
            self.assertEqual("DOCS_ONLY_NON_MARKDOWN_CHANGE", by_path[path])

    def test_docs_only_assets_are_explicit_image_files(self) -> None:
        valid = self.root / "valid-doc-assets.json"
        valid.write_text(
            json.dumps({"docs_only_allowed_patterns": ["assets/hero.jpg"]}), encoding="utf-8"
        )
        self.assertEqual(["assets/hero.jpg"], audit_markdown.load_config(valid)["docs_only_allowed_patterns"])
        for index, value in enumerate((
            "**", "Material/**", "/tmp/image.jpg", "../outside.jpg", "assets/",
            "firmware/main.c", "config/a.yml", "FirmWare/recovery.bin", "FirmWare/package.zip", "",
        )):
            path = self.root / f"invalid-doc-assets-{index}.json"
            path.write_text(json.dumps({"docs_only_allowed_patterns": [value]}), encoding="utf-8")
            with self.assertRaises(audit_markdown.AuditError):
                audit_markdown.load_config(path)

    def test_docs_only_rename_keeps_the_old_path_in_scope(self) -> None:
        report = self.report(
            [audit_markdown.Change("R", "docs/renamed.md", "firmware/main.c")],
            docs_only=True,
        )
        self.assertIn("DOCS_ONLY_SOURCE_CHANGE", self.codes(report))

    def test_ownership_defaults_and_config_override(self) -> None:
        self.write("managed_components/pkg/README.md", "upstream\n")
        self.write("third_party/pkg/README.md", "upstream\n")
        self.write("libraries/pkg/README_CN.md", "upstream\n")
        self.write("components/bsp_extra/README.md", "local wrapper\n")
        self.write("components/player_safe/player/README.md", "local safety wrapper\n")
        self.write("components/vendor_core/README.md", "vendored\n")
        config_path = self.root / "policy.json"
        config_path.write_text(
            json.dumps(
                {
                    "classification_rules": [
                        {"category": "embedded_upstream", "patterns": ["components/vendor_core/**"]}
                    ]
                }
            ),
            encoding="utf-8",
        )
        config = audit_markdown.load_config(config_path)
        self.assertEqual("managed_component", audit_markdown.classify("managed_components/pkg/README.md", config))
        self.assertEqual("third_party", audit_markdown.classify("third_party/pkg/README.md", config))
        self.assertEqual("embedded_upstream", audit_markdown.classify("libraries/pkg/README_CN.md", config))
        self.assertEqual("first_party_wrapper", audit_markdown.classify("components/bsp_extra/README.md", config))
        self.assertEqual("first_party_wrapper", audit_markdown.classify("components/player_safe/player/README.md", config))
        self.assertEqual("embedded_upstream", audit_markdown.classify("components/vendor_core/README.md", config))
        report = self.report(
            self.changes(
                "managed_components/pkg/README.md",
                "third_party/pkg/README.md",
                "libraries/pkg/README_CN.md",
                "components/vendor_core/README.md",
            ),
            config=config,
        )
        self.assertNotIn("BILINGUAL_PAIR_MISSING", self.codes(report))
        self.assertEqual(4, sum(item["code"] == "UPSTREAM_MARKDOWN_CHANGED" for item in report["findings"]))

    def test_dependency_roots_are_conservative_and_wrapper_boundaries_are_nested(self) -> None:
        config = audit_markdown.load_config(None)
        for path in (
            "lib/pkg/README.md",
            "libs/pkg/README.md",
            "deps/pkg/README.md",
            "dependencies/pkg/README.md",
            "middleware/pkg/README.md",
            "sdk/pkg/README.md",
            "components/pkg/README.md",
        ):
            self.assertEqual("unknown", audit_markdown.classify(path, config), path)
        self.assertEqual(
            "first_party_wrapper",
            audit_markdown.classify("wrappers/board/README.md", config),
        )
        self.assertEqual(
            "unknown",
            audit_markdown.classify("wrappers/board/deps/core/README.md", config),
        )
        self.assertEqual(
            "embedded_upstream",
            audit_markdown.classify("wrappers/board/upstream/core/README.md", config),
        )

        self.write(
            ".gitmodules",
            '[submodule "custom-core"]\n\tpath = custom/core\n\turl = https://example.invalid/core.git\n',
        )
        self.write("custom/core/README.md", "upstream-owned\n")
        report = self.report(self.changes("custom/core/README.md"), config=config)
        self.assertEqual("embedded_upstream", report["selected_files"][0]["category"])
        self.assertIn("UPSTREAM_MARKDOWN_CHANGED", self.codes(report))

    def test_external_manifest_source_root_inside_wrapper_is_dynamic_upstream(self) -> None:
        self.init_git()
        upstream_root = "firmware/app/components/player_safe/vendor-player"
        upstream_readme = f"{upstream_root}/README.md"
        self.write_component_source_root(
            upstream_root,
            "description: Synthetic vendored component\n"
            "url: https://github.com/upstream-org/vendor-player\n"
            "version: 1.0.0\n",
            "# Vendored player\n\n[Upstream-only test](../test)\n",
        )
        subprocess.run(
            ["git", "add", "."], cwd=self.root, check=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )

        roots = audit_markdown.discover_dynamic_upstream_roots(self.root)
        self.assertEqual({upstream_root}, roots)
        self.assertEqual(
            "embedded_upstream",
            audit_markdown.classify_at_root(
                upstream_readme, audit_markdown.load_config(None), set(), roots
            ),
        )

        process = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), str(self.root), "--all", "--format", "json"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(0, process.returncode, process.stderr or process.stdout)
        report = json.loads(process.stdout)
        self.assertNotIn(
            upstream_readme,
            {item["path"] for item in report["selected_files"]},
        )
        self.assertFalse(
            any(item["path"] == upstream_readme for item in report["findings"]),
            report["findings"],
        )

    def test_self_manifest_and_incomplete_external_metadata_remain_audited(self) -> None:
        self.init_git()
        wrapper_root = "wrappers/audio_safe/local-component"
        wrapper_readme = f"{wrapper_root}/README.md"
        self.write_component_source_root(
            wrapper_root,
            "repository: https://github.com/example-org/product-repo/tree/main/wrappers/audio_safe\n"
            "url: https://example.invalid/product-page\n",
            "# Product-local wrapper\n\n[Missing local page](missing.md)\n",
        )
        unknown_root = "components/local-component"
        self.write_component_source_root(
            unknown_root,
            "url: git@github.com:example-org/product-repo.git\n",
            "# Ambiguous local component\n",
        )
        # A lone external manifest URL is not enough without a component-source
        # root shape; this must remain visible to first-party review.
        self.write(
            "wrappers/metadata_safe/metadata-only/idf_component.yml",
            "url: https://github.com/upstream-org/metadata-only\n",
        )
        self.write(
            "wrappers/metadata_safe/metadata-only/README.md",
            "# Metadata only\n\n[Missing](missing.md)\n",
        )

        roots = audit_markdown.discover_dynamic_upstream_roots(self.root)
        self.assertEqual(set(), roots)
        config = audit_markdown.load_config(None)
        self.assertEqual(
            "first_party_wrapper",
            audit_markdown.classify_at_root(wrapper_readme, config, set(), roots),
        )
        self.assertEqual(
            "unknown",
            audit_markdown.classify_at_root(
                f"{unknown_root}/README.md", config, set(), roots
            ),
        )
        report = self.report(self.changes(wrapper_readme), config=config, all_mode=True)
        self.assertIn("RELATIVE_LINK_MISSING", self.codes(report))

    def test_repository_identity_drops_credentials_and_non_source_urls(self) -> None:
        raw = (
            "https://synthetic-user:synthetic-password@github.com/"
            "example-org/product-repo.git?auth=synthetic-value#fragment"
        )
        identity = audit_markdown.repository_identity(raw)
        self.assertEqual("github.com/example-org/product-repo", identity)
        for private_part in (
            "synthetic-user", "synthetic-password", "synthetic-value", "fragment"
        ):
            self.assertNotIn(private_part, identity or "")
        self.assertIsNone(
            audit_markdown.repository_identity(
                "https://www.example.invalid/products/board", field="url"
            )
        )

    def test_gitlink_index_is_recognized_without_path_name_hints(self) -> None:
        isolated = tempfile.TemporaryDirectory()
        self.addCleanup(isolated.cleanup)
        root = Path(isolated.name)
        subprocess.run(["git", "init"], cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(["git", "config", "user.name", "Synthetic Test"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.email", "synthetic@example.invalid"], cwd=root, check=True)
        (root / "seed.txt").write_text("seed\n", encoding="utf-8")
        subprocess.run(["git", "add", "seed.txt"], cwd=root, check=True)
        subprocess.run(["git", "commit", "-m", "seed"], cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=root, check=True,
            stdout=subprocess.PIPE, text=True,
        ).stdout.strip()
        subprocess.run(
            ["git", "update-index", "--add", "--cacheinfo", f"160000,{commit},custom/module"],
            cwd=root,
            check=True,
        )
        self.assertIn("custom/module", audit_markdown.discover_gitlink_roots(root))

    def test_all_mode_inventories_upstream_without_treating_it_as_changed(self) -> None:
        self.write("managed_components/pkg/README.md", "[missing](absent.md)\n")
        self.write("third_party/pkg/README.md", "[missing](absent.md)\n")
        self.write("libraries/pkg/README.md", "[missing](absent.md)\n")
        self.write("lib/ambiguous/README.md", "[missing](absent.md)\n")
        report = self.report(
            self.changes(
                "managed_components/pkg/README.md",
                "third_party/pkg/README.md",
                "libraries/pkg/README.md",
                "lib/ambiguous/README.md",
                status="I",
            ),
            all_mode=True,
        )
        selected = {item["path"] for item in report["selected_files"]}
        self.assertEqual({"lib/ambiguous/README.md"}, selected)
        self.assertNotIn("UPSTREAM_MARKDOWN_CHANGED", self.codes(report))
        self.assertEqual(0, report["scope"]["changed_files"])
        self.assertGreaterEqual(report["classification_inventory"].get("managed_component", 0), 1)
        self.assertGreaterEqual(report["classification_inventory"].get("third_party", 0), 1)
        self.assertGreaterEqual(report["classification_inventory"].get("embedded_upstream", 0), 1)

    def test_all_mode_uses_tracked_files_and_skips_ignored_generated_components(self) -> None:
        commands = (
            ["git", "init"],
            ["git", "config", "user.name", "Synthetic Test"],
            ["git", "config", "user.email", "synthetic@example.invalid"],
        )
        for command in commands:
            subprocess.run(command, cwd=self.root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.write(".gitignore", "managed_components/\n")
        subprocess.run(["git", "add", "."], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "-m", "tracked synthetic fixture"],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.write("managed_components/generated/README.md", "ignored generated content\n")
        tracked = audit_markdown.tracked_markdown_files(
            self.root, audit_markdown.load_config(None)
        )
        self.assertNotIn("managed_components/generated/README.md", tracked)

    def test_ambiguous_example_component_requires_ownership_classification(self) -> None:
        path = "examples/esp-idf/demo/components/ProductFeature/README.md"
        self.write(path, "# Product-local or upstream?\n")
        config = audit_markdown.load_config(None)

        self.assertEqual("unknown", audit_markdown.classify(path, config))
        report = self.report(self.changes(path), config=config)
        self.assertIn("MARKDOWN_OWNERSHIP_UNKNOWN", self.codes(report))
        self.assertNotIn("UPSTREAM_MARKDOWN_CHANGED", self.codes(report))

    def test_cli_exit_codes_and_json_contract(self) -> None:
        changed = self.root / "changed.txt"
        changed.write_text("A\tREADME.md\nA\tREADME_ZH.md\nA\tdocs/guide.md\nA\tdocs/guide_ZH.md\n", encoding="utf-8")
        process = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), str(self.root), "--changed-files-from", str(changed), "--format", "json"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(0, process.returncode, process.stderr)
        parsed = json.loads(process.stdout)
        self.assertEqual(1, parsed["schema_version"])
        self.assertTrue(all(not Path(item["path"]).is_absolute() for item in parsed["selected_files"]))

        self.write("docs/guide.md", "[简体中文](guide_ZH.md)\n[missing](absent.md)\n")
        process = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), str(self.root), "--changed-files-from", str(changed), "--format", "json"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(1, process.returncode)

        empty = self.root / "empty-changed.txt"
        empty.write_text("# no changed paths\n\n", encoding="utf-8")
        process = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), str(self.root), "--changed-files-from", str(empty)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(2, process.returncode)
        self.assertIn("changed-files list is empty", process.stderr)

        bad_config = self.root / "bad.json"
        bad_config.write_text('{"misspelled": []}', encoding="utf-8")
        process = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), str(self.root), "--all", "--config", str(bad_config)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(2, process.returncode)
        self.assertIn("unknown config keys", process.stderr)

    def test_base_scope_rejects_dirty_checkout(self) -> None:
        commands = (
            ["git", "init"],
            ["git", "config", "user.name", "Synthetic Test"],
            ["git", "config", "user.email", "synthetic@example.invalid"],
            ["git", "add", "."],
            ["git", "commit", "-m", "initial synthetic fixture"],
        )
        for command in commands:
            subprocess.run(
                command,
                cwd=self.root,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
            )
        self.write("docs/guide.md", "[简体中文](guide_ZH.md)\n\nlocal edit\n")

        process = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), str(self.root), "--base", "HEAD"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(2, process.returncode)
        self.assertIn("--base requires a clean checkout", process.stderr)

    def test_working_tree_scope_supports_unborn_staged_unstaged_and_untracked(self) -> None:
        self.init_git()
        self.write("staged-then-deleted.txt", "synthetic staged content\n")
        subprocess.run(
            ["git", "add", "."],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        (self.root / "staged-then-deleted.txt").unlink()
        self.write(
            "docs/guide.md",
            "[简体中文](guide_ZH.md)\n\n[Home](../README.md)\n\nUse `<PORT>`.\n\nUnstaged edit.\n",
        )
        self.write("docs/extra.md", "[简体中文](extra_ZH.md)\n")
        self.write("docs/extra_ZH.md", "[English](extra.md)\n")

        changes = audit_markdown.changes_from_worktree(self.root)
        statuses = {change.path: change.status for change in changes}
        self.assertEqual("D", statuses["staged-then-deleted.txt"])

        process = subprocess.run(
            [
                sys.executable,
                str(SCRIPT_PATH),
                str(self.root),
                "--working-tree",
                "--format",
                "json",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(0, process.returncode, process.stderr or process.stdout)
        report = json.loads(process.stdout)
        selected = {item["path"]: item["status"] for item in report["selected_files"]}
        self.assertEqual("A", selected["README.md"])
        # The index has an addition and the worktree has a later modification;
        # relative to an unborn branch the combined status remains an addition.
        self.assertEqual("A", selected["docs/guide.md"])
        self.assertEqual("A", selected["docs/extra.md"])
        self.assertEqual("A", selected["docs/extra_ZH.md"])

    def test_working_tree_scope_preserves_deletion_and_rename_with_head(self) -> None:
        self.init_git()
        subprocess.run(
            ["git", "add", "."],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        subprocess.run(
            ["git", "commit", "-m", "initial synthetic fixture"],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        subprocess.run(
            ["git", "mv", "docs/guide.md", "docs/renamed.md"],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        (self.root / "docs" / "guide_ZH.md").unlink()

        changes = audit_markdown.changes_from_worktree(self.root)
        by_path = {change.path: change for change in changes}
        self.assertEqual("R", by_path["docs/renamed.md"].status)
        self.assertEqual("docs/guide.md", by_path["docs/renamed.md"].old_path)
        self.assertEqual("D", by_path["docs/guide_ZH.md"].status)

    def test_working_tree_scope_does_not_hide_non_git_errors(self) -> None:
        isolated = tempfile.TemporaryDirectory()
        self.addCleanup(isolated.cleanup)
        with self.assertRaises(audit_markdown.AuditError) as context:
            audit_markdown.changes_from_worktree(Path(isolated.name))
        self.assertIn("git rev-parse", str(context.exception))

    def test_repository_config_is_narrow_and_consumable(self) -> None:
        path = REPOSITORY_ROOT / "config" / "markdown-audit.json"
        raw = json.loads(path.read_text(encoding="utf-8"))
        for key in (
            "classification_rules",
            "exclude_patterns",
            "pair_exempt_patterns",
            "language_link_exempt_patterns",
            "relative_link_ignore_patterns",
            "sensitive_allow_regexes",
            "bilingual_pairs",
            "bilingual_directory_mappings",
            "homepage_h3_emoji_allow_patterns",
        ):
            self.assertEqual([], raw[key], key)
        self.assertEqual(
            ["Material/images/ESP32-S3-Touch-AMOLED-2.06.jpg"],
            raw["docs_only_allowed_patterns"],
        )
        self.assertEqual("single-product", raw["homepage_pairs"][0]["profile"])
        config = audit_markdown.load_config(path)
        self.assertEqual("unknown", audit_markdown.classify("deps/pkg/README.md", config))
        self.assertEqual(
            "first_party_wrapper",
            audit_markdown.classify("wrappers/board/README.md", config),
        )


if __name__ == "__main__":
    unittest.main()
