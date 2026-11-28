"""Check the two data-section counts against authored native object contents."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "pluginsdk/python"))
from neverd_plugin.abi import CancelProbeCallback
from neverd_plugin.api import Session
from neverd_plugin.ffi import HostAPI


X64_COFF = """
.text
.globl entry
.def entry
.scl 2
.type 32
.endef
entry:
    movl scalar(%rip), %eax
    addl $1, scalar(%rip)
    leaq scalar(%rip), %rcx
    movl marker(%rip), %eax
    ret
.data
.p2align 3
scalar: .quad 7
    .quad scalar, scalar, marker, entry
    .quad 0x140002000
.section .rdata,"dr"
marker: .long 11
"""


class DataPtrScanCLI(unittest.TestCase):
    neverd = os.environ.get("NEVERD_TEST_CLI")
    clang = os.environ.get("NEVERD_TEST_CLANG")
    sdk = os.environ.get("NEVERD_TEST_LIBNEVERD")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="neverd-data-ptr-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        source = self.root / "fixture.s"
        source.write_text(X64_COFF)
        self.obj = self.root / "fixture.obj"
        subprocess.run([self.clang, "--target=x86_64-pc-windows-msvc",
                        "-c", source, "-o", self.obj], check=True)

    def scan(self, image, *options, check=True):
        return subprocess.run([self.neverd, "scan", "--data-ptr", image, *options],
                              check=check, capture_output=True, text=True, timeout=45)

    def check_counts(self, image, pointers):
        report = json.loads(self.scan(image).stdout)
        self.assertEqual(report["schema"], "neverd.data-ptr-scan")
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["section"], ".data")
        self.assertGreater(report["functions_scanned"], 0)
        self.assertEqual(report["code_references"], 3)
        self.assertEqual(report["data_pointers"], pointers)

    def test_object(self):
        self.check_counts(self.obj, 3)
        output = self.root / "report.json"
        self.assertEqual(self.scan(self.obj, "-o", output).stdout, "")
        self.assertEqual(json.loads(output.read_text()), json.loads(self.scan(self.obj).stdout))

    def test_linked_pe_with_and_without_base_relocations(self):
        linker = shutil.which("lld-link")
        if not linker:
            self.skipTest("lld-link is unavailable")
        for flag, count in [("/dynamicbase", 3), ("/fixed", 0)]:
            with self.subTest(flag=flag):
                image = self.root / "fixture.exe"
                subprocess.run([linker, "/entry:entry", "/subsystem:console", "/nodefaultlib",
                                flag, "/out:" + str(image), self.obj], check=True)
                self.check_counts(image, count)

        subprocess.run([linker, "/entry:entry", "/subsystem:console", "/nodefaultlib",
                        "/merge:.data=.rdata", "/out:" + str(image), self.obj], check=True)
        missing = self.scan(image, check=False)
        self.assertEqual(missing.returncode, 1)
        self.assertIn("data section", missing.stderr)
        self.assertEqual(missing.stdout, "")

    def test_sdk_and_cancellation(self):
        class BorrowedSession:
            def session_address(self, handle):
                return handle

        host = HostAPI(ctypes.CDLL(str(self.sdk)))
        handle = host.call("neverd_session_create")
        self.assertTrue(handle)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=BorrowedSession(), _host=host)
        session.load(self.obj)
        canceled = host.owned_string("neverd_session_data_ptr_scan_json", handle,
                                     CancelProbeCallback(lambda _: 1), None)
        self.assertIsNone(canceled)
        self.assertIn("cancel", session.last_error)
        self.assertEqual(session.scan_data_pointers(), json.loads(self.scan(self.obj).stdout))

    def test_rejects_safety_options(self):
        for option in ["--analyzer=both", "--max-paths=1"]:
            with self.subTest(option=option):
                result = self.scan(self.obj, option, check=False)
                self.assertEqual(result.returncode, 1)
                self.assertIn("--data-ptr", result.stderr)
                self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    DataPtrScanCLI.neverd = options.neverd.resolve(strict=True)
    DataPtrScanCLI.clang = options.clang.resolve(strict=True)
    DataPtrScanCLI.sdk = options.sdk.resolve(strict=True)
    unittest.main(argv=[__file__, *remaining])
