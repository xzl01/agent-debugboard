#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "version_sync.py"
SPEC = importlib.util.spec_from_file_location("version_sync", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {SCRIPT_PATH}")
version_sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(version_sync)


class VersionSyncTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.write_fixture("0.2.0")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write(self, relative_path: str, content: str) -> None:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def write_fixture(self, version: str) -> None:
        self.write("VERSION", f"{version}\n")
        self.write(
            "cmd-ng/Cargo.toml",
            f'[package]\nname = "radxa-linkr-debuggerctl-ng"\nversion = "{version}"\n\n[dependencies]\n',
        )
        self.write(
            "cmd-ng/Cargo.lock",
            f'[[package]]\nname = "dependency"\nversion = "1.0.0"\n\n[[package]]\nname = "radxa-linkr-debuggerctl-ng"\nversion = "{version}"\n',
        )
        self.write(
            "web/package.json",
            json.dumps({"name": "web", "version": version}, indent=2) + "\n",
        )
        self.write(
            "web/package-lock.json",
            json.dumps(
                {
                    "name": "web",
                    "version": version,
                    "packages": {"": {"name": "web", "version": version}},
                },
                indent=2,
            )
            + "\n",
        )
        self.write(
            "web/decoder/Cargo.toml",
            f'[package]\nname = "radxa-logic-decoder"\nversion = "{version}"\n',
        )
        self.write(
            "web/decoder/Cargo.lock",
            f'[[package]]\nname = "radxa-logic-decoder"\nversion = "{version}"\n',
        )
        self.write(
            "nix/package.nix",
            f'let\n  version = "{version}";\nin\n{{ inherit version; }}\n',
        )

    def run_main(self, *arguments: str) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = version_sync.main(["--root", str(self.root), *arguments])
        return result, stdout.getvalue(), stderr.getvalue()

    def test_check_accepts_consistent_versions_and_matching_tag(self) -> None:
        result, stdout, stderr = self.run_main("check", "--tag", "v0.2.0")
        self.assertEqual(result, 0)
        self.assertIn("Version gate passed: 0.2.0", stdout)
        self.assertEqual(stderr, "")

    def test_check_reports_each_mismatch(self) -> None:
        package_json = self.root / "web/package.json"
        package_json.write_text(
            json.dumps({"name": "web", "version": "0.1.0"}, indent=2) + "\n",
            encoding="utf-8",
        )
        result, _, stderr = self.run_main("check", "--tag", "v0.1.0")
        self.assertEqual(result, 1)
        self.assertIn("web/package.json:version", stderr)
        self.assertIn("release tag", stderr)
        self.assertIn("expected 'v0.2.0'", stderr)

    def test_set_updates_every_managed_version(self) -> None:
        result, stdout, stderr = self.run_main("set", "0.3.0-rc.1")
        self.assertEqual(result, 0)
        self.assertIn("Updated project version to 0.3.0-rc.1", stdout)
        self.assertEqual(stderr, "")

        result, stdout, stderr = self.run_main(
            "check", "--tag", "v0.3.0-rc.1"
        )
        self.assertEqual(result, 0)
        self.assertIn("across 8 fields", stdout)
        self.assertEqual(stderr, "")

    def test_set_rejects_leading_v_without_writing(self) -> None:
        before = (self.root / "VERSION").read_text(encoding="utf-8")
        result, _, stderr = self.run_main("set", "v0.3.0")
        self.assertEqual(result, 1)
        self.assertIn("without a leading 'v'", stderr)
        self.assertEqual((self.root / "VERSION").read_text(encoding="utf-8"), before)


if __name__ == "__main__":
    unittest.main()
