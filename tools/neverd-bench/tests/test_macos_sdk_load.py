import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest


BENCH = Path(__file__).resolve().parents[1] / "sigprof" / "macos_sdk_load.py"


@unittest.skipUnless(sys.platform == "darwin" and
                     int((platform.mac_ver()[0] or "0").split(".")[0]) >= 13,
                     "macOS 13 or later required")
class MacOSSDKLoadTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scratch = self.root / "scratch"
        self.scratch.mkdir()
        self.output = self.root / "report.json"
        self.sdk = self.root / "libneverd.dylib"

    def dylib(self, arch, symbol):
        path = self.root / f"{arch}.dylib"
        subprocess.run(
            ["xcrun", "clang", "-arch", arch, "-dynamiclib", "-Wl,-adhoc_codesign",
             "-Wl,-install_name,@rpath/libneverd.dylib", "-x", "c", "-", "-o", path],
            input=f"int {symbol}(void) {{ return 1; }}", text=True, check=True,
        )
        return path

    def measure(self, pairs):
        return subprocess.run(
            [sys.executable, BENCH, self.sdk, "--pairs", str(pairs),
             "--scratch-dir", self.scratch, "--json", self.output],
            capture_output=True, text=True,
        )

    def test_native_slice_and_separate_first_and_repeat_loads(self):
        # Each slice has its own embedded signature at a slice-relative offset.
        images = [self.dylib(a, "neverd_session_create") for a in ("x86_64", "arm64")]
        subprocess.run(["xcrun", "lipo", "-create", *images, "-output", self.sdk], check=True)
        result = self.measure(2)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.output.read_text())
        self.assertEqual(report["sdk_sha256"], hashlib.sha256(self.sdk.read_bytes()).hexdigest())
        rows = report["observations"]
        self.assertEqual(len(rows), 16)
        for pair in (1, 2):
            for state in ("fresh", "pre_read"):
                cells = [r for r in rows if r["pair"] == pair and r["state"] == state]
                self.assertEqual([r["launch"] for r in cells], ["first", "repeat"] * 2)
                modes = ["direct", "staged"]
                if (pair % 2 == 0) != (state == "pre_read"):
                    modes.reverse()
                self.assertEqual([r["mode"] for r in cells], [m for m in modes for _ in range(2)])
                for first, repeat in (cells[:2], cells[2:]):
                    self.assertEqual(first["image_id"], repeat["image_id"])
        for row in rows:
            self.assertGreater(row["pages"], 0)
            self.assertTrue(0 <= row["resident_pages"] <= row["pages"])
            phases = row["phases"]
            self.assertIn("dlopen", phases)
            self.assertEqual("check_library" in phases, row["mode"] == "staged")
            self.assertTrue(all(v["wall_ms"] >= 0 for v in phases.values()))
            if row["mode"] == "staged":
                stages = ("inspect", "register_signature", "check_library", "dlopen")
                for key, value in phases["staged_total"].items():
                    self.assertAlmostEqual(value, sum(phases[s][key] for s in stages))
        self.assertEqual(list(self.scratch.iterdir()), [])

    def test_missing_sdk_entry_point_suppresses_report(self):
        self.dylib(platform.machine(), "unrelated_function").rename(self.sdk)
        result = self.measure(1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SDK entry point", result.stderr)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.scratch.iterdir()), [])

    def test_report_path_cannot_overwrite_input_sdk(self):
        self.sdk.write_bytes(b"input")
        self.output.hardlink_to(self.sdk)
        result = self.measure(1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("report path", result.stderr)
        self.assertEqual(self.sdk.read_bytes(), b"input")
