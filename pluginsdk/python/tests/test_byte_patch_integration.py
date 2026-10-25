"""Check CLI/Python byte overlays against the real SDK and exported file."""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest


class BytePatchIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="neverd-patch-adapters-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.binary = self.root / "input.elf"
        self.entry = 0x400078
        # Owned x64 ELF: mov eax, 7; ret. No execution or compiler required.
        code = bytes.fromhex("b807000000c3")
        ident = b"\x7fELF\x02\x01\x01" + bytes(9)
        header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1,
                             self.entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
        segment = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000,
                              120 + len(code), 120 + len(code), 4096)
        self.original = header + segment + code
        self.binary.write_bytes(self.original)

    def test_source_preview_retains_explicit_raw_load_options(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        from neverd_plugin.abi import NeverDBytePatchOptionsV1
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        pointer = host.call("neverd_session_create")
        self.assertTrue(pointer)
        self.addCleanup(host.call, "neverd_session_destroy", pointer)
        raw = self.root / "input.raw"
        original = bytes.fromhex("b807000000c3")
        raw.write_bytes(original)
        self.assertEqual(host.call("neverd_session_set_load_options", pointer,
            b'{"loader":"binary","processor":"x86_64","platform":"sysv","base":4096}'), 0)
        self.assertEqual(host.call("neverd_session_load", pointer, str(raw).encode("utf-8")), 1)
        context = json.loads(host.owned_string("neverd_source_patch_context_json", pointer, 4096))
        options = NeverDBytePatchOptionsV1()
        options.struct_size = ctypes.sizeof(options)
        options.address = 4096
        options.kind = b"constant"
        options.value = 42
        options.fill_nops = 1
        result = host.owned_string("neverd_byte_patch_source_preview_json", pointer,
                                   4096, b"c", ctypes.byref(options))
        self.assertIsNotNone(result, host.owned_string("neverd_last_error", pointer))
        preview = json.loads(result)
        self.assertEqual(preview["before"], context["source"])
        self.assertRegex(preview["after"], r"\breturn\s+(42|0x2[Aa]);")
        self.assertEqual(json.loads(host.owned_string("neverd_byte_patches_json", pointer)), [])
        self.assertEqual(json.loads(host.owned_string("neverd_source_patch_context_json", pointer, 4096)),
                         context)
        self.assertEqual(raw.read_bytes(), original)
        self.assertIsNotNone(host.owned_string("neverd_byte_patch_apply", pointer, ctypes.byref(options)))
        applied = json.loads(host.owned_string("neverd_ir_view_json", pointer, 4096, b"c", 0, 128))
        self.assertEqual(applied["text"], preview["after"])

    def test_cli_values_persistence_and_exclusive_export(self) -> None:
        cli = os.environ.get("NEVERD_TEST_CLI")
        if not cli:
            self.skipTest("NEVERD_TEST_CLI is not configured")

        def call(*arguments, success=True):
            result = subprocess.run(
                [cli, "patch-bytes", str(self.binary), *arguments],
                capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(result.returncode == 0, success,
                             (arguments, result.stdout, result.stderr))
            return result.stdout

        for value in ("42", "0x2a", "0X2A", "042"):
            plan = json.loads(call("--address", hex(self.entry),
                                   "--kind", "constant", "--value", value))
            self.assertEqual(plan["current"], "b82a000000")
        call("--address", hex(self.entry), "--kind", "constant",
             "--value", "-1", success=False)
        self.assertEqual(json.loads(call()), [])
        call("--address", hex(self.entry), "--kind", "constant",
             "--value", "42", "--apply")
        output = self.root / "cli.elf"
        call("--export", "-o", str(output))
        self.assertEqual(output.read_bytes()[120:125], bytes.fromhex("b82a000000"))
        self.assertEqual(self.binary.read_bytes(), self.original)
        call("--export", "-o", str(output), success=False)
        call("--address", hex(self.entry), "--size", "5", "--revert")
        self.assertEqual(json.loads(call()), [])
        plan = json.loads(call("--address", hex(self.entry), "--assemble",
                               "mov eax, [rsp+var_20]", "--symbols", '{"var_20":-32}'))
        self.assertEqual(plan["current"], "8b4424e090")

    def test_python_stale_bytes_save_conflicts_and_native_assembly(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        from neverd_plugin import NeverDError, Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        pointer = host.call("neverd_session_create")
        self.assertTrue(pointer)
        self.addCleanup(host.call, "neverd_session_destroy", pointer)
        self.assertEqual(host.call("neverd_session_load", pointer,
                                   str(self.binary).encode("utf-8")), 1)
        session = Session(pointer, _host=host,
                          _native=SimpleNamespace(session_address=lambda handle: handle))
        self.assertEqual(session.byte_patches, [])
        self.assertEqual(session.byte_patch_ranges, [])
        assembled = session.assemble(self.entry, "mov eax, [rsp+var_20]",
                                     symbols={"var_20": -32})
        self.assertEqual(assembled["hex"], "8b4424e0")
        plan = session.preview_byte_patch(self.entry, kind="constant", value=42)
        session.apply_byte_patch(self.entry, kind="constant", value=42,
                                 expected=plan["original"])
        context = json.loads(host.owned_string("neverd_byte_patch_context_json",
                                               pointer, self.entry))
        self.assertEqual(context["text"], plan["decoded"][0]["text"])
        self.assertIn("revert", context["actions"])
        self.assertEqual(context["revert_ranges"],
                         [{"address": hex(self.entry + 1), "size": 1}])
        self.assertEqual(session.byte_patch_ranges, context["revert_ranges"])
        session.save_byte_patches()
        output = self.root / "python.elf"
        session.export_byte_patches(output)
        self.assertEqual(output.read_bytes()[120:125], bytes.fromhex("b82a000000"))
        self.assertEqual(self.binary.read_bytes(), self.original)
        self.assertTrue(session.byte_patches)
        with self.assertRaisesRegex(NeverDError, "stale"):
            session.apply_byte_patch(self.entry, kind="constant", value=7,
                                     expected=plan["original"])
        sidecar = next(self.root.rglob("patches.json"))
        saved = sidecar.read_bytes()
        sidecar.write_bytes(b"[]\n")
        with self.assertRaisesRegex(NeverDError, "changed"):
            session.save_byte_patches()
        self.assertEqual(sidecar.read_bytes(), b"[]\n")
        sidecar.write_bytes(saved)
        session.load_byte_patches()
        # Equal row count/address/size is insufficient for the reload shortcut.
        # A foreign original must still fail admission without touching bytes.
        foreign = json.loads(saved)
        foreign[0]["original"] = "ff"
        sidecar.write_text(json.dumps(foreign))
        with self.assertRaisesRegex(NeverDError, "original"):
            session.load_byte_patches()
        self.assertEqual(session.byte_patches[0]["current"], "2a")
        sidecar.write_bytes(saved)
        # The file digest remains the saved one after an in-memory edit.
        # Reload must still restore the saved overlay, not mistake it for a
        # clean session based only on that unchanged digest.
        session.apply_byte_patch(self.entry, kind="constant", value=43)
        session.load_byte_patches()
        self.assertEqual(session.byte_patches[0]["current"], "2a")
        session.revert_byte_patch(self.entry, 5)
        session.save_byte_patches()
        self.assertEqual(session.byte_patches, [])
        self.assertEqual(session.byte_patch_ranges, [])


if __name__ == "__main__":
    unittest.main()
