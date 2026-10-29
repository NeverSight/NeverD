"""Exercise explicit state C and both owned outputs through the real SDK ABI."""

import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

from neverd_plugin.ffi import HostAPI


class ContinuationIntegrationTests(unittest.TestCase):
    def setUp(self):
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        self.host = HostAPI(ctypes.CDLL(str(path)))
        # Reuse the worker's linked x64 PE: MOV RAX,42; RET.
        tests = Path(__file__).resolve().parents[3] / "tools/neverd-worker/tests"
        spec = importlib.util.spec_from_file_location(
            "continuation_binding_fixture", tests / "pe_browse_test.py"
        )
        fixture = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, "path", [str(tests), *sys.path]):
            spec.loader.exec_module(fixture)
        binary, base = fixture.image()
        temporary = tempfile.TemporaryDirectory(prefix="neverd-continuation-bindings-")
        self.addCleanup(temporary.cleanup)
        image = Path(temporary.name) / "entry.exe"
        image.write_bytes(binary)
        self.session = self.host.call("neverd_session_create")
        self.assertTrue(self.session)
        self.addCleanup(self.host.call, "neverd_session_destroy", self.session)
        self.assertEqual(self.host.call("neverd_session_load", self.session,
                                        os.fsencode(image)), 1)
        self.entry = base + 0x1000
        self.request = json.dumps({
            "profile": "kernel-events-v6",
            "instructions": [self.entry, self.entry + 10],
            "mxcsr_mask": "0xffbf",
            "paging_address_bits": 48,
        }).encode()

    def test_source_and_manifest_have_independent_ownership(self):
        for route, name in ((0, "high"), (1, "llvm")):
            with self.subTest(route=name):
                manifest = ctypes.c_void_p()
                try:
                    # The size, not NUL termination, bounds the request.
                    source = self.host.owned_string(
                        "neverd_x64_continuation_source_v1", self.session,
                        self.request + b"!", len(self.request), route,
                        ctypes.byref(manifest))
                    self.assertTrue(source)
                    self.assertTrue(manifest.value)
                    # owned_string has already freed the source. The manifest
                    # must still be live and must describe that exact output.
                    report = json.loads(ctypes.string_at(manifest).decode())
                    self.assertEqual(report["source_route"], name)
                    self.assertEqual(report["source_sha256"],
                                     hashlib.sha256(source.encode()).hexdigest())
                    self.assertEqual(report["state_version"], 6)
                    self.assertEqual(report["state_bytes"], 2496)
                    self.assertEqual(report["total"], 2)
                    self.assertEqual(report["native_requests"], 0)
                    self.assertEqual([int(unit["pc"], 16) for unit in report["units"]],
                                     [self.entry, self.entry + 10])
                    self.assertFalse(report["ordinary_source_abi"])
                    self.assertIn("neverd_x64_continue_v6", source)
                finally:
                    if manifest.value:
                        self.host.call("neverd_free_string",
                                       ctypes.cast(manifest, ctypes.c_char_p))
                self.assertEqual(self.host.owned_string(
                    "neverd_x64_continuation_source_v1", self.session,
                    self.request, len(self.request), route, None), source)

    def test_failure_clears_manifest_and_reports_error(self):
        for request, size, route in (
                (b"{}", 2, 0), (self.request, len(self.request), 2),
                (self.request, len(self.request) - 1, 0),
                (None, len(self.request), 0)):
            with self.subTest(request=request, size=size, route=route):
                manifest = ctypes.c_void_p(1)
                self.assertIsNone(self.host.owned_string(
                    "neverd_x64_continuation_source_v1", self.session,
                    request, size, route, ctypes.byref(manifest)))
                self.assertIsNone(manifest.value)
                self.assertTrue(self.host.owned_string("neverd_last_error", self.session))
