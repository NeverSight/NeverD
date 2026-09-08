"""Builtin Android publication must not require or silently run external tools."""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/neverd"))
from mobile.common import MobileError
from mobile.driver import parser, recover


class BuiltinAndroidTests(unittest.TestCase):
    def test_default_engine_recovers_every_smali_method_without_external_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "recovered"
            args = parser().parse_args([str(ROOT / "scripts/tests/fixtures/mobile"), "-o", str(output)])
            self.assertIsNone(args.jadx)
            with patch("mobile.android.run_tool", side_effect=AssertionError("external backend used")), \
                    patch("subprocess.Popen", side_effect=AssertionError("external process used")), \
                    patch("shutil.which", side_effect=AssertionError("external tool discovery used")):
                report = recover(args)
            self.assertEqual(report["backend"], {"name": "neverd", "version": "1", "execution": "builtin"})
            coverage = json.loads((output / "metadata/android-methods.json").read_text())
            self.assertEqual(coverage, report["android_method_recovery"])
            self.assertEqual((coverage["class_count"], coverage["method_count"], coverage["recovered_method_count"]), (3, 6, 6))
            self.assertEqual(coverage["declaration_only_method_count"], 0)
            self.assertEqual(coverage["unrecovered_method_count"], 0)
            self.assertEqual(len({row["identity"] for row in coverage["methods"]}), 6)
            self.assertTrue(all(row["status"] == "recovered" for row in coverage["methods"]))
            self.assertEqual(report["java_source_count"], 2)
            self.assertTrue(all((output / path).is_file() for path in report["java_sources"]))

    def test_environment_cannot_silently_select_external_compatibility_backend(self):
        with patch.dict(os.environ, {"NEVERD_JADX": "/untrusted/environment/tool"}):
            args = parser().parse_args(["sample.dex", "-o", "output"])
            self.assertIsNone(args.jadx)
            args = parser().parse_args(["sample.dex", "-o", "output", "--jadx", "chosen-tool"])
            self.assertEqual(args.jadx, "chosen-tool")

    def test_unsupported_method_cannot_publish_partial_java_or_use_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "classes"
            source.mkdir()
            (source / "Good.smali").write_text((ROOT / "scripts/tests/fixtures/mobile/Peer.smali").read_text())
            (source / "Bad.smali").write_text(""".class public Lfixture/Bad;
.super Ljava/lang/Object;
.method public static invalid()I
  .registers 1
  return v0
.end method
""")
            output = root / "recovered"
            with patch("mobile.android.run_tool", side_effect=AssertionError("fallback used")):
                with self.assertRaisesRegex(MobileError, "undefined"):
                    recover(parser().parse_args([str(source), "-o", str(output)]))
            self.assertFalse(output.exists())
            self.assertFalse(list(root.glob(".neverd-mobile-*")))

    def test_original_native_and_abstract_declarations_are_not_counted_as_bodies(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Declarations.smali"
            source.write_text(""".class public abstract Lfixture/Declarations;
.super Ljava/lang/Object;
.method public abstract work()I
.end method
.method public static native external(I)I
.end method
""")
            report = recover(parser().parse_args([str(source), "-o", str(root / "out")]))
            coverage = report["android_method_recovery"]
            self.assertEqual((coverage["method_count"], coverage["recovered_method_count"],
                              coverage["declaration_only_method_count"]), (2, 0, 2))
            self.assertTrue(all(row["status"] == "declaration-only" for row in coverage["methods"]))


if __name__ == "__main__":
    unittest.main()
