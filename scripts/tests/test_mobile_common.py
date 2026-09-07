"""Boundary tests for mobile ingestion, process failures and publication."""

import json
import os
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "neverd"))
from mobile.common import (Limits, MobileError, extract_zip, relative_member,
                           run_tool, safe_copy_tree, validate_tree, workspace_budget)
from mobile.driver import parser, recover


class MobileCommonTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="neverd mobile ")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def archive(self, entries):
        archive = self.root / "input.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as out:
            for name, data in entries:
                # Preserve raw member names: ZipInfo normally replaces the
                # host separator on Windows, accidentally repairing hostile
                # fixture paths before the importer can test them.
                if isinstance(name, str):
                    entry = zipfile.ZipInfo("fixture")
                    entry.filename = name
                    entry.orig_filename = name
                    entry.compress_type = zipfile.ZIP_DEFLATED
                else:
                    entry = name
                out.writestr(entry, data)
        return archive

    def test_extract_nested_binary_and_empty_file(self):
        source = self.archive([("data/a", b"\x00\xff"), ("empty", b"")])
        output = self.root / "extracted"
        files = extract_zip(source, output, Limits())
        self.assertEqual(len(files), 2)
        self.assertEqual((output / "data/a").read_bytes(), b"\x00\xff")

    def test_unsafe_paths_rejected_before_any_write(self):
        for index, name in enumerate(("../escape", "/absolute", "C:/escape", "a/../b", "a\\b", "a//b", "./a", "CON.txt", "a./x")):
            with self.subTest(name=name):
                source = self.archive([("good", b"ok"), (name, b"bad")])
                output = self.root / f"output-{index}"
                with self.assertRaises(MobileError):
                    extract_zip(source, output, Limits())
                self.assertFalse(output.exists())

    def test_raw_separator_is_rejected_when_zip_reader_normalizes_it(self):
        source = self.archive([("a\\b", b"x")])
        self.assertIn(b"a\\b", source.read_bytes())
        # Exercise the Windows reader's normalization on every test host.
        with patch.object(zipfile.os, "sep", "\\"):
            with self.assertRaises(MobileError):
                extract_zip(source, self.root / "output", Limits())
        self.assertFalse((self.root / "output").exists())

    def test_symlink_archive_rejected(self):
        link = zipfile.ZipInfo("link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        source = self.archive([(link, b"../elsewhere")])
        with self.assertRaisesRegex(MobileError, "link"):
            extract_zip(source, self.root / "output", Limits())

    def test_case_and_file_directory_conflicts(self):
        for names in (("A", "a"), ("a", "a/b"), ("A/x", "a/y")):
            with self.subTest(names=names):
                source = self.archive([(name, b"x") for name in names])
                with self.assertRaises(MobileError):
                    extract_zip(source, self.root / "output", Limits())

    def test_archive_size_and_file_count_limits(self):
        source = self.archive([("a", b"a" * 100), ("b", b"b")])
        for limits in (Limits(max_bytes=100), Limits(max_files=1)):
            with self.assertRaises(MobileError):
                extract_zip(source, self.root / "output", limits)

    def test_implicit_archive_directories_count_toward_limit(self):
        source = self.archive([("a/b/c/d", b"x")])
        with self.assertRaisesRegex(MobileError, "file-count"):
            extract_zip(source, self.root / "output", Limits(max_files=1))

    @unittest.skipIf(os.name == "nt", "POSIX directory permissions")
    def test_unreadable_directories_do_not_disappear(self):
        source = self.root / "source"
        hidden = source / "hidden"
        hidden.mkdir(parents=True)
        (hidden / "important.smali").write_bytes(b"x" * 100)
        hidden.chmod(0)
        try:
            if os.access(hidden, os.R_OK):
                self.skipTest("privileged account can enumerate unreadable directories")
            with self.assertRaisesRegex(MobileError, "enumerate"):
                safe_copy_tree(source, self.root / "copy", Limits())
            with self.assertRaisesRegex(MobileError, "enumerate"):
                validate_tree(source, Limits(max_bytes=1))
        finally:
            hidden.chmod(0o700)

    def test_corrupt_archive(self):
        source = self.root / "bad.zip"
        source.write_bytes(b"PK\x03\x04truncated")
        with self.assertRaisesRegex(MobileError, "archive"):
            extract_zip(source, self.root / "output", Limits())

    def test_nul_archive_path_rejected(self):
        source = self.archive([("validxx", b"x")])
        source.write_bytes(source.read_bytes().replace(b"validxx", b"a\x00etcxx"))
        with self.assertRaisesRegex(MobileError, "NUL"):
            extract_zip(source, self.root / "output", Limits())

    @unittest.skipIf(os.name == "nt", "symlink creation requires privileges on Windows")
    def test_directory_links_and_output_links_rejected(self):
        source = self.root / "source"
        source.mkdir()
        (source / "link").symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(MobileError, "link"):
            safe_copy_tree(source, self.root / "copy", Limits())
        with self.assertRaisesRegex(MobileError, "link"):
            validate_tree(source, Limits())

    def test_process_arguments_are_literal(self):
        log = self.root / "log"
        argument = "a b;$(touch evil)`test`&%PATH%"
        run_tool([sys.executable, "-c", "import sys;print(sys.argv[1])", argument], log, 5)
        self.assertEqual(log.read_text().strip(), argument)

    def test_nonzero_exit_has_bounded_diagnostic(self):
        with self.assertRaisesRegex(MobileError, "status 7: problem"):
            run_tool([sys.executable, "-c", "print('problem');raise SystemExit(7)"], self.root / "log", 5)

    def test_timeout(self):
        with self.assertRaisesRegex(MobileError, "timed out"):
            run_tool([sys.executable, "-c", "import time;time.sleep(10)"], self.root / "log", 1)

    def test_wrapper_cannot_leave_descendant_writing_after_success(self):
        marker = self.root / "late-output"
        child = "import pathlib,sys,time;time.sleep(0.6);pathlib.Path(sys.argv[1]).write_text('late')"
        parent = "import subprocess,sys;subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2]])"
        run_tool([sys.executable, "-c", parent, child, str(marker)], self.root / "log", 5)
        time.sleep(0.8)
        self.assertFalse(marker.exists())

    def test_diagnostic_output_is_capped(self):
        log = self.root / "large.log"
        with self.assertRaisesRegex(MobileError, "diagnostic output"):
            run_tool([sys.executable, "-c", "import sys;sys.stdout.write('x'*(17*1024*1024))"], log, 5)
        self.assertLessEqual(log.stat().st_size, 16 * 1024 * 1024)

    def test_generated_work_limit(self):
        work = self.root / "work"
        work.mkdir()
        with workspace_budget(work, Limits(max_bytes=100)):
            with self.assertRaisesRegex(MobileError, "limits"):
                run_tool([sys.executable, "-c", "import pathlib,sys,time;pathlib.Path(sys.argv[1]).write_bytes(b'x'*500);time.sleep(10)", str(work / "large")], self.root / "log", 5)

    def test_live_scan_allows_retired_temporaries_but_final_scan_is_strict(self):
        work = self.root / "work"
        work.mkdir()
        (work / "retired").write_text("temporary")
        original = Path.lstat

        def retired(path, *args, **kwargs):
            if path.name == "retired":
                raise FileNotFoundError("backend removed its temporary file")
            return original(path, *args, **kwargs)

        with patch.object(Path, "lstat", retired):
            validate_tree(work, Limits(), live=True)
            with self.assertRaises(FileNotFoundError):
                validate_tree(work, Limits())

    def test_missing_backend(self):
        with self.assertRaisesRegex(MobileError, "cannot execute"):
            run_tool([str(self.root / "missing")], self.root / "log", 5)

    def test_invalid_limits(self):
        with self.assertRaises(MobileError):
            Limits(timeout=0)

    def test_output_directory_preserved(self):
        source = self.root / "sample.smali"
        source.write_text(".class public LSample;")
        output = self.root / "output"
        output.mkdir()
        (output / "keep").write_text("untouched")
        with self.assertRaisesRegex(MobileError, "already exists"):
            recover(parser().parse_args([str(source), "-o", str(output)]))
        self.assertEqual((output / "keep").read_text(), "untouched")

    def test_output_inside_input_rejected(self):
        source = self.root / "smali"
        source.mkdir()
        with self.assertRaisesRegex(MobileError, "outside"):
            recover(parser().parse_args([str(source), "-o", str(source / "output")]))

    def test_wrong_platform_options_rejected(self):
        source = self.root / "sample.smali"
        source.write_text(".class public LSample;")
        with self.assertRaisesRegex(MobileError, "only to iOS"):
            recover(parser().parse_args([str(source), "-o", str(self.root / "output"), "--metadata-only"]))


if __name__ == "__main__":
    unittest.main()
