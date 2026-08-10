from __future__ import annotations

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

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_pair(self, english: str, chinese: str) -> None:
        (self.repo / "README.md").write_text(english, encoding="utf-8")
        (self.repo / "README_ZH.md").write_text(chinese, encoding="utf-8")

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

    def test_missing_relative_target_is_reported(self) -> None:
        self.write_pair("[中文](README_ZH.md) [missing](nope.md)\n\n## 📋 Section\n", "[English](README.md)\n\n## 📋 部分\n")
        self.assertTrue(any("missing local link" in error for error in check_docs.check(self.repo, self.pairs)))
