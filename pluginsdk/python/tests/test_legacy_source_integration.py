"""Check size-delimited protected source export and independent SDK ownership."""

import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

from neverd_plugin.ffi import HostAPI


class LegacySourceIntegrationTests(unittest.TestCase):
    def setUp(self):
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        self.host = HostAPI(ctypes.CDLL(str(path)))
        temporary = tempfile.TemporaryDirectory(prefix="neverd-legacy-bindings-")
        self.addCleanup(temporary.cleanup)
        # One ELF32 executable PT_LOAD: MOV EAX,0x12345678 at linear 0x1000.
        binary = bytearray(0x1005)
        binary[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<HHIIIIIHHHHHH", binary, 16,
                         2, 3, 1, 0x1000, 52, 0, 0, 52, 32, 1, 0, 0, 0)
        struct.pack_into("<IIIIIIII", binary, 52,
                         1, 0x1000, 0x1000, 0x1000, 5, 5, 5, 0x1000)
        binary[0x1000:] = b"\xb8\x78\x56\x34\x12"
        image = Path(temporary.name) / "entry.elf"
        image.write_bytes(binary)
        self.session = self.host.call("neverd_session_create")
        self.assertTrue(self.session)
        self.addCleanup(self.host.call, "neverd_session_destroy", self.session)
        self.assertEqual(self.host.call("neverd_session_load", self.session,
                                       os.fsencode(image)), 1)
        self.request = json.dumps({
            "profile": "protected-legacy-v1",
            "instructions": [{"address": "0x1000", "code_bits": 32, "size": 5}],
        }).encode()

    def test_source_and_manifest_have_independent_ownership(self):
        manifest = ctypes.c_void_p()
        try:
            source = self.host.owned_string(
                "neverd_x86_legacy_source_v1", self.session,
                self.request + b"!", len(self.request), ctypes.byref(manifest))
            self.assertTrue(source)
            self.assertTrue(manifest.value)
            # owned_string has freed the source; the manifest is still live.
            report = json.loads(ctypes.string_at(manifest).decode())
            self.assertEqual(report["profile"], "protected-legacy-v1")
            self.assertEqual(report["source_route"], "original-byte-runtime")
            self.assertEqual(report["source_sha256"],
                             hashlib.sha256(source.encode()).hexdigest())
            self.assertEqual(report["instructions"], [{
                "address": "0x1000", "code_bits": 32, "size": 5,
                "bytes": "b878563412",
            }])
            self.assertEqual(report["exits"]["PENDING_EVENT"], 259)
            self.assertIn("neverd_x86_legacy_continue_v1", source)
        finally:
            if manifest.value:
                self.host.call("neverd_free_string",
                               ctypes.cast(manifest, ctypes.c_char_p))
        self.assertEqual(self.host.owned_string(
            "neverd_x86_legacy_source_v1", self.session,
            self.request, len(self.request), None), source)

    def test_failure_clears_manifest_and_reports_error(self):
        for request, size in ((b"{}", 2), (self.request, len(self.request) - 1),
                              (None, len(self.request))):
            with self.subTest(request=request, size=size):
                manifest = ctypes.c_void_p(1)
                self.assertIsNone(self.host.owned_string(
                    "neverd_x86_legacy_source_v1", self.session,
                    request, size, ctypes.byref(manifest)))
                self.assertIsNone(manifest.value)
                self.assertTrue(self.host.owned_string("neverd_last_error", self.session))
