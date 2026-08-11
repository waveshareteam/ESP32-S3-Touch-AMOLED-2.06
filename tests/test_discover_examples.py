from __future__ import annotations

import argparse
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import discover_examples  # noqa: E402


class DiscoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)
        (self.repo / "examples/esp-idf/idf-one/main").mkdir(parents=True)
        (self.repo / "examples/esp-idf/idf-one/CMakeLists.txt").write_text("project(test)")
        (self.repo / "examples/arduino/sketch-one").mkdir(parents=True)
        (self.repo / "examples/arduino/sketch-one/sketch-one.ino").write_text("void setup() {}")
        (self.repo / "examples/arduino/libraries").mkdir(parents=True)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_exact_selected_paths_filter_without_changing_selector(self) -> None:
        args = argparse.Namespace(repo=self.repo, surface="esp-idf", selector="all", idf_versions="v5.5.5,v6.0.2", arduino_core="3.3.11", fqbn="fqbn", selected_paths='["examples/esp-idf/idf-one"]')
        self.assertEqual(len(discover_examples.build_matrix(args)["include"]), 2)
        args.selected_paths = "[]"
        self.assertEqual(discover_examples.build_matrix(args)["include"], [])

    def test_default_versions_are_current(self) -> None:
        parser = argparse.ArgumentParser()
        # The CLI defaults are intentionally asserted from source-facing parser behavior.
        self.assertIn("v5.5.5", Path(discover_examples.__file__).read_text(encoding="utf-8"))
        self.assertIn('default="3.3.11"', Path(discover_examples.__file__).read_text(encoding="utf-8"))

