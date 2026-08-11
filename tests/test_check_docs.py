from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import check_docs  # noqa: E402


class DocumentationCheckerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)
        self.pairs = {"README.md": "README_ZH.md"}
        self.homepage = {
            "english": "README.md", "chinese": "README_ZH.md", "profile": "single-product",
            "required_components": ["centered_header", "html_h1", "subtitle", "badges", "language_switch", "quick_links", "hero_image", "separator", "h2"],
            "required_quick_links": ["product", "documentation", "firmware", "esp_idf", "arduino"],
            "required_badges": ["build", "release", "license"], "required_h2_icons": ["\u2728"],
        }

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_pair(self, english: str, chinese: str) -> None:
        (self.repo / "README.md").write_text(english, encoding="utf-8")
        (self.repo / "README_ZH.md").write_text(chinese, encoding="utf-8")

    def write_homepage_pair(self, hero_source: str = "Material/images/hero.jpg") -> None:
        image = self.repo / "Material" / "images" / "hero.jpg"
        image.parent.mkdir(parents=True, exist_ok=True)
        image.write_bytes(b"test image")
        for relative, counterpart, product, docs, firmware in (
            ("README.md", "README_ZH.md", "https://www.waveshare.com/product.htm", "docs/ci.md", "FirmWare/README.md"),
            ("README_ZH.md", "README.md", "https://www.waveshare.net/shop/product.htm", "docs/ci_ZH.md", "FirmWare/README_ZH.md"),
        ):
            (self.repo / relative).write_text(
                f'''<div align="center">
<h1>Board</h1>
<p><strong>Subtitle</strong></p>
<p><img alt="Build Examples" src="https://example.invalid/build-badge.svg"><img alt="Latest Release" src="https://example.invalid/release-badge.svg"><img alt="License" src="https://example.invalid/license-badge.svg"></p>
<p><a href="{counterpart}">Language</a></p>
<p><a href="{product}">Product</a><a href="{docs}">Docs</a><a href="{firmware}">Firmware</a><a href="examples/esp-idf/">ESP-IDF</a><a href="examples/arduino/">Arduino</a></p>
<p><a href="{product}"><img src="{hero_source}" alt="Board hero" width="80%"></a></p>
</div>

---

## \u2728 Overview
''',
                encoding="utf-8",
            )

    def check_homepages(self) -> list[str]:
        return check_docs.check(self.repo, self.pairs, [self.homepage])

    def test_pair_links_fragments_and_h2_icons_pass(self) -> None:
        self.write_pair("[中文](README_ZH.md) [local](#section)\n\n## 📋 Section\n", "[English](README.md)\n\n## 📋 部分\n")
        self.assertEqual(check_docs.check(self.repo, self.pairs), [])

    def test_missing_pair_reciprocal_link_and_h2_mismatch_fail(self) -> None:
        (self.repo / "README.md").write_text("## 📋 Section\n", encoding="utf-8")
        errors = check_docs.check(self.repo, self.pairs)
        self.assertTrue(any("missing paired page" in error for error in errors))
        self.write_pair("[中文](README_ZH.md)\n\n## 📋 Section\n", "[English](README.md)\n\n## 🧩 部分\n")
        errors = check_docs.check(self.repo, self.pairs)
        self.assertTrue(any("H2 icon/order" in error for error in errors))

    def test_configured_homepage_passes(self) -> None:
        self.write_homepage_pair()
        self.assertEqual(self.check_homepages(), [])

    def test_configured_homepage_requires_hero_image(self) -> None:
        self.write_homepage_pair()
        for relative in self.pairs:
            page = self.repo / relative
            page.write_text(
                page.read_text(encoding="utf-8").replace(
                    '<img src="Material/images/hero.jpg" alt="Board hero" width="80%">', ""
                ),
                encoding="utf-8",
            )
        self.assertTrue(any("missing hero_image" in error for error in self.check_homepages()))

    def test_configured_homepage_requires_product_link(self) -> None:
        self.write_homepage_pair()
        for relative in self.pairs:
            page = self.repo / relative
            page.write_text(page.read_text(encoding="utf-8").replace("https://www.waveshare", "https://example.invalid"), encoding="utf-8")
        self.assertTrue(any("missing product quick link" in error for error in self.check_homepages()))

    def test_configured_homepage_rejects_escaping_hero_target(self) -> None:
        self.write_homepage_pair("../outside.jpg")
        self.assertTrue(any("repository-escaping hero image" in error for error in self.check_homepages()))

    def test_workflow_uses_exact_configured_checker_command(self) -> None:
        root = Path(__file__).resolve().parents[1]
        workflow = (root / ".github" / "workflows" / "examples.yml").read_text(encoding="utf-8")
        self.assertIn("python3 scripts/check_docs.py . --config config/markdown-audit.json", workflow)
        result = subprocess.run(
            [sys.executable, "scripts/check_docs.py", ".", "--config", "config/markdown-audit.json"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_relative_target_is_reported(self) -> None:
        self.write_pair("[中文](README_ZH.md) [missing](nope.md)\n\n## 📋 Section\n", "[English](README.md)\n\n## 📋 部分\n")
        self.assertTrue(any("missing local link" in error for error in check_docs.check(self.repo, self.pairs)))
