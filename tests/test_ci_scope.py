from __future__ import annotations

import sys
import subprocess
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ci_scope  # noqa: E402


def changed(*fields: str) -> list[str]:
    return ci_scope.parse_name_status("\0".join(fields).encode() + b"\0")


class ScopeTests(unittest.TestCase):
    def route(self, *fields: str) -> ci_scope.Scope:
        return ci_scope.classify(changed(*fields))

    def test_markdown_never_builds_examples(self) -> None:
        for path in (
            "README.md",
            "examples/esp-idf/01_AXP2101/README.md",
            "examples/arduino/01_HelloWorld/README.md",
            "examples/arduino/libraries/XPowersLib/README.md",
        ):
            scope = self.route("M", path)
            self.assertFalse(scope.esp_all or scope.arduino_all or scope.esp_idf or scope.arduino)

    def test_direct_sources_select_only_their_entry(self) -> None:
        idf = self.route("M", "examples/esp-idf/01_AXP2101/main/main.c")
        self.assertEqual(idf.esp_idf, {"examples/esp-idf/01_AXP2101"})
        self.assertFalse(idf.arduino or idf.arduino_all)
        arduino = self.route("M", "examples/arduino/01_HelloWorld/01_HelloWorld.ino")
        self.assertEqual(arduino.arduino, {"examples/arduino/01_HelloWorld"})
        self.assertFalse(arduino.esp_idf or arduino.esp_all)

    def test_library_and_global_inputs_expand_only_expected_surfaces(self) -> None:
        library = self.route("M", "examples/arduino/libraries/XPowersLib/src/a.cpp")
        self.assertTrue(library.arduino_all)
        self.assertFalse(library.esp_all)
        for path in (".github/workflows/examples.yml", "scripts/ci_scope.py", "tests/test_ci_scope.py", "releases/package_firmware.py"):
            global_scope = self.route("M", path)
            self.assertTrue(global_scope.esp_all and global_scope.arduino_all)

    def test_shared_esp_idf_roots_select_all_esp_idf_projects(self) -> None:
        for path in ("examples/esp-idf/common/compat.c", "examples/esp-idf/components/board_glue.c"):
            scope = self.route("M", path)
            self.assertTrue(scope.esp_all)
            self.assertFalse(scope.arduino_all)

    def test_firmware_and_assets_are_reported_or_ignored_without_builds(self) -> None:
        for path in ("FirmWare/README.md", "FirmWare/a.c", "FirmWare/sdkconfig", "FirmWare/a.bin", "FirmWare/a.zip"):
            scope = self.route("M", path)
            self.assertEqual(scope.firmware, [path])
            self.assertFalse(scope.esp_all or scope.arduino_all)
        for path in ("Material/photo.png", "Schematic/board.pdf"):
            scope = self.route("M", path)
            self.assertFalse(scope.esp_all or scope.arduino_all)

    def test_rename_deletion_and_unknown_paths_are_conservative(self) -> None:
        renamed = self.route("R100", "examples/esp-idf/01_AXP2101/main/main.c", "docs/note.md")
        self.assertEqual(renamed.esp_idf, {"examples/esp-idf/01_AXP2101"})
        self.assertEqual(
            ci_scope.parse_name_status(b"R100\texamples/arduino/01_HelloWorld/a.ino\tdocs/note.md\n"),
            ["examples/arduino/01_HelloWorld/a.ino", "docs/note.md"],
        )
        unknown = self.route("D", "unclassified/build.input")
        self.assertTrue(unknown.esp_all and unknown.arduino_all)
        self.assertEqual(unknown.unknown, ["unclassified/build.input"])

    def test_empty_or_unavailable_data_fails_closed(self) -> None:
        with self.assertRaises(ValueError):
            ci_scope.parse_name_status(b"")
        result = subprocess.run(
            [sys.executable, str(Path(ci_scope.__file__))],
            input=b"",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 2)

    def test_workflow_cli_shape_writes_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            changed_file = root / "changed-files.z"
            output_file = root / "github-output"
            changed_file.write_bytes(b"M\0examples/arduino/01_HelloWorld/01_HelloWorld.ino\0")
            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(ci_scope.__file__)),
                    "--name-status-file",
                    str(changed_file),
                    "--github-output",
                    str(output_file),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(result.returncode, 0)
            outputs = output_file.read_text(encoding="utf-8")
            self.assertIn("esp_idf_mode=none", outputs)
            self.assertIn("arduino_mode=selected", outputs)
            self.assertIn('arduino_paths=["examples/arduino/01_HelloWorld"]', outputs)
