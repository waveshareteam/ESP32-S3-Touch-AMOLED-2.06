from __future__ import annotations

import sys
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ci_scope  # noqa: E402


def changed(*fields: str) -> list[str]:
    return ci_scope.parse_name_status("\0".join(fields).encode() + b"\0")


class ScopeTests(unittest.TestCase):
    def route(self, *fields: str, docs_only_allowed_patterns: tuple[str, ...] = ()) -> ci_scope.Scope:
        return ci_scope.classify(changed(*fields), docs_only_allowed_patterns)

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

    def test_nul_renames_and_deletions_retain_every_source_impact(self) -> None:
        renamed = self.route(
            "R100", "examples/esp-idf/01_AXP2101/main/main.c", "docs/renamed.md"
        )
        self.assertEqual(renamed.esp_idf, {"examples/esp-idf/01_AXP2101"})
        self.assertFalse(renamed.docs_only)
        moved_between_surfaces = self.route(
            "R100", "examples/esp-idf/01_AXP2101/main/main.c",
            "examples/arduino/01_HelloWorld/01_HelloWorld.ino",
        )
        self.assertEqual(moved_between_surfaces.esp_idf, {"examples/esp-idf/01_AXP2101"})
        self.assertEqual(moved_between_surfaces.arduino, {"examples/arduino/01_HelloWorld"})
        arduino_deleted = self.route("D", "examples/arduino/01_HelloWorld/01_HelloWorld.ino")
        self.assertEqual(arduino_deleted.arduino, {"examples/arduino/01_HelloWorld"})
        idf_deleted = self.route("D", "examples/esp-idf/01_AXP2101/main/main.c")
        self.assertEqual(idf_deleted.esp_idf, {"examples/esp-idf/01_AXP2101"})

    def test_nul_input_rejects_truncated_or_malformed_records(self) -> None:
        for payload in (b"M\0", b"R100\0old\0", b"M\0README.md"):
            with self.assertRaises(ValueError):
                ci_scope.parse_name_status(payload)

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

    def test_docs_only_requires_only_documentation_paths(self) -> None:
        docs = self.route("M", "README.md", "M", "examples/esp-idf/01_AXP2101/README.md")
        self.assertTrue(docs.docs_only)
        mixed = self.route(
            "M", "FirmWare/README.md", "M", "FirmWare/recovery.bin", "M", "FirmWare/package.zip",
            "M", "FirmWare/tool.c", "M", "examples/arduino/01_HelloWorld/01_HelloWorld.ino",
        )
        self.assertFalse(mixed.docs_only)
        self.assertEqual(
            mixed.firmware,
            ["FirmWare/README.md", "FirmWare/recovery.bin", "FirmWare/package.zip", "FirmWare/tool.c"],
        )
        self.assertEqual(mixed.arduino, {"examples/arduino/01_HelloWorld"})

    def test_docs_only_uses_the_audit_allowlist_for_every_rename_path(self) -> None:
        allowed = ("Material/images/ESP32-S3-Touch-AMOLED-2.06.jpg",)
        self.assertTrue(self.route(
            "M", "Material/images/ESP32-S3-Touch-AMOLED-2.06.jpg",
            docs_only_allowed_patterns=allowed,
        ).docs_only)
        self.assertTrue(self.route(
            "R100", "README.md", "Material/images/ESP32-S3-Touch-AMOLED-2.06.jpg",
            docs_only_allowed_patterns=allowed,
        ).docs_only)
        for path in ("Material/photo.png", "Schematic/board.pdf", "docs/note.txt", "docs/note.markdown"):
            self.assertFalse(self.route("M", path, docs_only_allowed_patterns=allowed).docs_only, path)

    def test_docs_only_config_rejects_malformed_unknown_and_unsafe_patterns(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cases = (
                ("not-json.json", "{", "cannot read JSON config"),
                ("unknown.json", json.dumps({"unexpected": []}), "unknown config keys"),
                ("not-list.json", json.dumps({"docs_only_allowed_patterns": "Material/**"}), "list of strings"),
                ("globstar.json", json.dumps({"docs_only_allowed_patterns": ["**"]}), "explicit repository-relative documentation asset files"),
                ("glob.json", json.dumps({"docs_only_allowed_patterns": ["Material/**"]}), "explicit repository-relative documentation asset files"),
                ("absolute.json", json.dumps({"docs_only_allowed_patterns": ["/tmp/image.jpg"]}), "explicit repository-relative documentation asset files"),
                ("parent.json", json.dumps({"docs_only_allowed_patterns": ["../outside.jpg"]}), "explicit repository-relative documentation asset files"),
                ("directory.json", json.dumps({"docs_only_allowed_patterns": ["Material/"]}), "explicit repository-relative documentation asset files"),
                ("source.json", json.dumps({"docs_only_allowed_patterns": ["docs/check.py"]}), "explicit repository-relative documentation asset files"),
                ("config.json", json.dumps({"docs_only_allowed_patterns": ["docs/check.yml"]}), "explicit repository-relative documentation asset files"),
                ("binary.json", json.dumps({"docs_only_allowed_patterns": ["FirmWare/recovery.bin"]}), "explicit repository-relative documentation asset files"),
                ("archive.json", json.dumps({"docs_only_allowed_patterns": ["FirmWare/package.zip"]}), "explicit repository-relative documentation asset files"),
                ("empty.json", json.dumps({"docs_only_allowed_patterns": [""]}), "explicit repository-relative documentation asset files"),
            )
            for name, content, message in cases:
                path = root / name
                path.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, message):
                    ci_scope.load_docs_only_asset_paths(path)
            accepted = root / "accepted.json"
            accepted.write_text(json.dumps({"docs_only_allowed_patterns": ["Material/images/ESP32-S3-Touch-AMOLED-2.06.jpg"]}), encoding="utf-8")
            self.assertEqual(
                ("Material/images/ESP32-S3-Touch-AMOLED-2.06.jpg",),
                ci_scope.load_docs_only_asset_paths(accepted),
            )

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
                    "--config",
                    str(Path(__file__).resolve().parents[1] / "config" / "markdown-audit.json"),
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
            self.assertIn("docs_only=false", outputs)
            self.assertIn('arduino_paths=["examples/arduino/01_HelloWorld"]', outputs)

    def test_cli_exposes_rename_scope_and_rejects_malformed_nul_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            changed_file = root / "changed-files.z"
            changed_file.write_bytes(
                b"R100\0examples/esp-idf/01_AXP2101/main/main.c\0docs/renamed.md\0"
            )
            result = subprocess.run(
                [sys.executable, str(Path(ci_scope.__file__)), "--name-status-file", str(changed_file)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('esp_idf_paths=["examples/esp-idf/01_AXP2101"]', result.stdout)
            self.assertIn("docs_only=false", result.stdout)
            changed_file.write_bytes(b"R100\0old\0")
            malformed = subprocess.run(
                [sys.executable, str(Path(ci_scope.__file__)), "--name-status-file", str(changed_file)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(malformed.returncode, 2)
            self.assertIn("rename/copy status missing destination path", malformed.stderr)

    def test_workflow_uses_complete_changed_scope_and_conditional_docs_gate(self) -> None:
        workflow = (Path(__file__).resolve().parents[1] / ".github/workflows/examples.yml").read_text(encoding="utf-8")
        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn('git diff --name-status -z --find-renames "$base" "${{ github.sha }}"', workflow)
        self.assertIn('python3 scripts/ci_scope.py --config config/markdown-audit.json --name-status-file changed-files.z --github-output "$GITHUB_OUTPUT"', workflow)
        self.assertIn("--changed-files-from changed-files.txt --config config/markdown-audit.json", workflow)
        self.assertIn('audit_args+=(--expect-docs-only)', workflow)
        self.assertEqual(2, workflow.count("SELECTED_PATHS: ${{ needs.scope.outputs."))
        self.assertEqual(2, workflow.count('args+=(--selected-paths "$SELECTED_PATHS")'))
        self.assertNotIn("args+=(--selected-paths '${{ needs.scope.outputs.", workflow)
        self.assertIn('base="${{ github.event.pull_request.base.sha }}"', workflow)
        self.assertIn('base="${{ github.event.before }}"', workflow)
