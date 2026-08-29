#!/usr/bin/env python3
"""Cross-platform integration tests for the native plugin CLI."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class PluginCLITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.neverd = Path(os.environ["NEVERD_TEST_CLI"]).resolve()
        cls.example_plugin = Path(
            os.environ["NEVERD_TEST_EXAMPLE_PLUGIN"]
        ).resolve()
        cls.lifecycle_probe = Path(
            os.environ["NEVERD_TEST_LIFECYCLE_PLUGIN"]
        ).resolve()
        for path in (cls.neverd, cls.example_plugin, cls.lifecycle_probe):
            if not path.is_file():
                raise AssertionError(f"required plugin CLI test artifact is missing: {path}")

    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="neverd-plugin-cli-")
        self.root = Path(self._temporary.name)
        self.home = self.root / "home"
        self.home.mkdir()

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def clean_environment(self) -> dict[str, str]:
        environment = os.environ.copy()
        environment.pop("NEVERD_PLUGIN_PATH", None)
        environment["HOME"] = str(self.home)
        environment["USERPROFILE"] = str(self.home)
        if os.name == "nt":
            environment["HOMEDRIVE"] = self.home.drive
            environment["HOMEPATH"] = str(self.home)[len(self.home.drive) :]
        return environment

    def run_cli(
        self,
        *arguments: str,
        environment: dict[str, str] | None = None,
        bare_name: bool = False,
        cwd: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        executable = self.neverd.name if bare_name else str(self.neverd)
        return subprocess.run(
            (executable, *arguments),
            cwd=cwd or self.root,
            env=environment or self.clean_environment(),
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

    def assert_success(
        self, result: subprocess.CompletedProcess[str]
    ) -> None:
        self.assertEqual(
            result.returncode,
            0,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        self.assertEqual(result.stderr, "")

    def parse_listing(
        self, result: subprocess.CompletedProcess[str]
    ) -> list[dict[str, object]]:
        self.assert_success(result)
        try:
            document = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            self.fail(
                f"plugin --list --json did not emit one clean JSON document: {error}\n"
                f"stdout:\n{result.stdout}"
            )
        self.assertIsInstance(document, list)
        self.assertTrue(all(isinstance(entry, dict) for entry in document))
        return document

    def assert_example_present_once(
        self, listing: list[dict[str, object]]
    ) -> None:
        examples = [entry for entry in listing if entry.get("name") == "Example Plugin"]
        self.assertEqual(len(examples), 1, listing)
        self.assertEqual(examples[0].get("kind"), "native")
        self.assertEqual(examples[0].get("version"), "1.0.0")
        self.assertEqual(
            Path(str(examples[0].get("path"))).resolve(), self.example_plugin
        )

    def test_list_is_clean_json_and_contains_the_example(self) -> None:
        listing = self.parse_listing(
            self.run_cli("plugins", "--list", "--json")
        )
        self.assert_example_present_once(listing)

    def test_bare_path_invocation_discovers_plugins_beside_the_executable(self) -> None:
        environment = self.clean_environment()
        environment["PATH"] = os.pathsep.join(
            (str(self.neverd.parent), environment.get("PATH", ""))
        )
        listing = self.parse_listing(
            self.run_cli(
                "plugins",
                "--list",
                "--json",
                environment=environment,
                bare_name=True,
            )
        )
        self.assert_example_present_once(listing)

    def test_example_plugin_runs_through_the_cli(self) -> None:
        result = self.run_cli("plugins", "--run", "Example Plugin")
        self.assert_success(result)
        self.assertIn("Plugin 'Example Plugin' returned 0", result.stdout)
        self.assertEqual(result.stdout.count("[ExamplePlugin] initialized"), 1)
        self.assertEqual(result.stdout.count("[ExamplePlugin] terminated"), 1)

    def test_failed_plugin_initialization_is_reported_and_not_terminated(
        self,
    ) -> None:
        plugin_directory = self.root / "failed-init-plugins"
        plugin_directory.mkdir()
        installed_probe = plugin_directory / self.lifecycle_probe.name
        shutil.copy2(self.lifecycle_probe, installed_probe)
        trace_path = self.root / "failed-init.trace"
        environment = self.clean_environment()
        environment["NEVERD_LIFECYCLE_PROBE_FAIL_INIT"] = "1"
        environment["NEVERD_LIFECYCLE_PROBE_TRACE"] = str(trace_path)

        result = self.run_cli(
            "plugins",
            "--plugin-dir",
            str(plugin_directory),
            "--run",
            "Lifecycle Probe",
            environment=environment,
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("failed to initialize plugins", result.stderr.lower())
        self.assertEqual(trace_path.read_text(encoding="utf-8"), "init:failed\n")

    def test_unknown_plugin_name_is_reported(self) -> None:
        result = self.run_cli("plugins", "--run", "Missing Plugin")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("plugin not found: Missing Plugin", result.stderr)

    def test_missing_explicit_plugin_directory_is_an_error(self) -> None:
        missing = self.root / "missing-plugins"
        result = self.run_cli(
            "plugins", "--plugin-dir", str(missing), "--list", "--json"
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(str(missing), result.stderr)
        self.assertIn("cannot resolve plugin directory", result.stderr.lower())

    def test_explicit_plugin_directory_must_be_a_directory(self) -> None:
        not_a_directory = self.root / "plugins.txt"
        not_a_directory.write_text("not a directory\n", encoding="utf-8")
        result = self.run_cli(
            "plugins",
            "--plugin-dir",
            str(not_a_directory),
            "--list",
            "--json",
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(str(not_a_directory), result.stderr)
        self.assertIn("directory", result.stderr.lower())

    def test_malformed_plugin_in_explicit_directory_is_an_error(self) -> None:
        plugin_directory = self.root / "malformed-plugins"
        plugin_directory.mkdir()
        malformed = plugin_directory / f"broken{self.example_plugin.suffix}"
        malformed.write_bytes(b"this is not a shared library\n")
        result = self.run_cli(
            "plugins",
            "--plugin-dir",
            str(plugin_directory),
            "--list",
            "--json",
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(malformed.name, result.stderr)
        self.assertIn("plugin", result.stderr.lower())

    def test_explicit_alias_of_failed_optional_directory_is_still_an_error(
        self,
    ) -> None:
        plugin_directory = self.home / ".neverd" / "plugins"
        plugin_directory.mkdir(parents=True)
        malformed = plugin_directory / f"broken{self.example_plugin.suffix}"
        malformed.write_bytes(b"this is not a shared library\n")

        result = self.run_cli(
            "plugins",
            "--plugin-dir",
            str(plugin_directory),
            "--list",
            "--json",
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(str(plugin_directory), result.stderr)
        self.assertIn(malformed.name, result.stderr)

    def test_absent_optional_user_plugin_directory_is_not_an_error(self) -> None:
        self.assertFalse((self.home / ".neverd" / "plugins").exists())
        result = self.run_cli("plugins", "--list", "--json")
        listing = self.parse_listing(result)
        self.assert_example_present_once(listing)

    def test_home_environment_plugin_directory_is_scanned(self) -> None:
        plugin_directory = self.home / ".neverd" / "plugins"
        plugin_directory.mkdir(parents=True)
        installed_probe = plugin_directory / self.lifecycle_probe.name
        shutil.copy2(self.lifecycle_probe, installed_probe)

        listing = self.parse_listing(
            self.run_cli("plugins", "--list", "--json")
        )

        probes = [
            entry for entry in listing if entry.get("name") == "Lifecycle Probe"
        ]
        self.assertEqual(len(probes), 1, listing)
        self.assertEqual(
            Path(str(probes[0].get("path"))).resolve(), installed_probe.resolve()
        )

    def test_empty_environment_path_segments_do_not_scan_the_working_directory(
        self,
    ) -> None:
        local_probe = self.root / self.lifecycle_probe.name
        shutil.copy2(self.lifecycle_probe, local_probe)
        environment = self.clean_environment()
        environment["NEVERD_PLUGIN_PATH"] = os.pathsep * 3
        listing = self.parse_listing(
            self.run_cli(
                "plugins", "--list", "--json", environment=environment
            )
        )
        self.assertNotIn("Lifecycle Probe", {entry.get("name") for entry in listing})
        self.assert_example_present_once(listing)

    def test_canonical_directory_alias_is_scanned_only_once(self) -> None:
        plugin_directory = self.example_plugin.parent
        alias = plugin_directory / ".." / plugin_directory.name
        listing = self.parse_listing(
            self.run_cli(
                "plugins", "--plugin-dir", str(alias), "--list", "--json"
            )
        )
        self.assert_example_present_once(listing)


if __name__ == "__main__":
    unittest.main()
