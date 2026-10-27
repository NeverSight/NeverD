"""Check CLI/Python byte overlays against the real SDK and exported file."""

from __future__ import annotations

import ctypes
from contextlib import ExitStack
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

    def _snapshot_host(self):
        from neverd_plugin.ffi import HostAPI

        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        return HostAPI(ctypes.CDLL(str(path)))

    def _snapshot(self, host, pointer):
        snapshot = host.call("neverd_byte_patches_snapshot_v1", pointer)
        self.assertTrue(snapshot, host.owned_string("neverd_last_error", pointer))
        self.addCleanup(host.call, "neverd_byte_patch_snapshot_free_v1", snapshot)
        return snapshot

    @staticmethod
    def _snapshot_rows(snapshot):
        return [{"addr": hex(row.file_offset), "file_off": hex(row.file_offset),
                 "va": hex(row.address), "size": row.size,
                 "original": ctypes.string_at(row.original, row.size).hex(),
                 "current": ctypes.string_at(row.current, row.size).hex(),
                 "kind": row.kind.decode("ascii")}
                for row in snapshot.contents.rows[:snapshot.contents.row_count]]

    def test_snapshot_distinguishes_unloaded_and_empty_sessions(self) -> None:
        host = self._snapshot_host()
        host.call("neverd_byte_patch_snapshot_free_v1", None)
        self.assertFalse(host.call("neverd_byte_patches_snapshot_v1", None))
        pointer = host.call("neverd_session_create")
        self.addCleanup(host.call, "neverd_session_destroy", pointer)
        self.assertFalse(host.call("neverd_byte_patches_snapshot_v1", pointer))
        self.assertIn("loaded session", host.owned_string("neverd_last_error", pointer))
        self.assertEqual(host.call("neverd_session_load", pointer, os.fsencode(self.binary)), 1)
        snapshot = self._snapshot(host, pointer)
        self.assertEqual(snapshot.contents.row_count, 0)
        self.assertFalse(snapshot.contents.rows)
        self.assertEqual(self._snapshot_rows(snapshot), [])

    def test_snapshots_own_rows_across_edits_reloads_and_session_destruction(self) -> None:
        from neverd_plugin import Session

        host = self._snapshot_host()
        with ExitStack() as sessions:
            pointer = host.call("neverd_session_create")
            sessions.callback(host.call, "neverd_session_destroy", pointer)
            self.assertEqual(host.call("neverd_session_load", pointer, os.fsencode(self.binary)), 1)
            session = Session(pointer, _host=host,
                              _native=SimpleNamespace(session_address=lambda handle: handle))
            session.apply_byte_patch(self.entry, kind="constant", value=42)
            session.apply_byte_patch(self.entry + 4, data=b"\xff\x00")
            first = self._snapshot(host, pointer)
            expected = [
                {"addr": "0x79", "file_off": "0x79", "va": hex(self.entry + 1),
                 "size": 1, "original": "07", "current": "2a", "kind": "constant"},
                {"addr": "0x7c", "file_off": "0x7c", "va": hex(self.entry + 4),
                 "size": 2, "original": "00c3", "current": "ff00", "kind": "bytes"},
            ]
            self.assertEqual(self._snapshot_rows(first), expected)
            self.assertEqual(session.byte_patches, expected)
            session.save_byte_patches()
            session.apply_byte_patch(self.entry + 1, data=b"\x7f", allow_overlap=True)
            second = self._snapshot(host, pointer)
            second_expected = session.byte_patches
            self.assertNotEqual(second_expected, expected)
            self.assertEqual(self._snapshot_rows(first), expected)
            session.load_byte_patches()
            self.assertEqual(session.byte_patches, expected)
            session.revert_byte_patch(self.entry, 6)
            self.assertEqual(session.byte_patches, [])
            empty = self._snapshot(host, pointer)
        self.assertEqual(self._snapshot_rows(first), expected)
        self.assertEqual(self._snapshot_rows(second), second_expected)
        self.assertEqual(self._snapshot_rows(empty), [])
        self.assertEqual(self.binary.read_bytes(), self.original)

    def test_snapshot_keeps_all_byte_values_high_addresses_and_split_runs(self) -> None:
        from neverd_plugin import Session

        host = self._snapshot_host()
        pointer = host.call("neverd_session_create")
        self.addCleanup(host.call, "neverd_session_destroy", pointer)
        base = 0xffff800012340000
        raw = self.root / "all-bytes.raw"
        original = b"\x80" * 256
        raw.write_bytes(original)
        options = json.dumps({"loader": "binary", "processor": "x86_64",
                              "platform": "sysv", "base": base}).encode()
        self.assertEqual(host.call("neverd_session_set_load_options", pointer, options), 0)
        self.assertEqual(host.call("neverd_session_load", pointer, os.fsencode(raw)), 1)
        session = Session(pointer, _host=host,
                          _native=SimpleNamespace(session_address=lambda handle: handle))
        current = bytes(range(256))
        session.apply_byte_patch(base, data=current)
        snapshot = self._snapshot(host, pointer)
        expected = [{"addr": hex(start), "file_off": hex(start),
                     "va": hex(base + start), "size": end - start,
                     "original": original[start:end].hex(),
                     "current": current[start:end].hex(), "kind": "bytes"}
                    for start, end in ((0, 128), (129, 256))]
        self.assertEqual(self._snapshot_rows(snapshot), expected)
        self.assertEqual(session.byte_patches, expected)
        self.assertEqual(raw.read_bytes(), original)

    def test_large_patch_input_snapshot_survives_rewrite_and_truncation(self) -> None:
        from neverd_plugin import Session

        host = self._snapshot_host()
        raw = self.root / "snapshot.raw"
        original = b"\xcc" * 65536
        raw.write_bytes(original)
        pointer = host.call("neverd_session_create")
        self.assertTrue(pointer)
        self.addCleanup(host.call, "neverd_session_destroy", pointer)
        self.assertEqual(host.call("neverd_session_set_load_options", pointer,
            b'{"loader":"binary","processor":"x86_64","platform":"sysv","base":4096}'), 0)
        self.assertEqual(host.call("neverd_session_load", pointer, os.fsencode(raw)), 1)
        session = Session(pointer, _host=host,
                          _native=SimpleNamespace(session_address=lambda handle: handle))
        session.apply_byte_patch(4096, kind="bytes", data=b"\x90")
        expected = b"\x90" + original[1:]
        changed = original[:-1] + b"\x42"
        raw.write_bytes(changed)
        output = self.root / "rewritten-export.raw"
        session.export_byte_patches(output)
        self.assertEqual(output.read_bytes(), expected)
        self.assertEqual(raw.read_bytes(), changed)
        # A mapped snapshot could fault on pages beyond this new EOF.
        raw.write_bytes(b"\xcc")
        output = self.root / "truncated-export.raw"
        session.export_byte_patches(output)
        self.assertEqual(output.read_bytes(), expected)
        self.assertEqual(raw.read_bytes(), b"\xcc")

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
        rows = json.loads(call())
        saved = next(self.root.rglob("patches.json")).read_bytes()
        self.assertEqual(saved, (json.dumps(rows, indent=2, sort_keys=True) + "\n").encode())
        sidecar = next(self.root.rglob("patches.json"))
        broken = ("[" + json.dumps(rows[0]) + ",null]").encode()
        sidecar.write_bytes(broken)
        call("--address", hex(self.entry), "--kind", "constant",
             "--value", "43", "--apply", success=False)
        self.assertEqual(sidecar.read_bytes(), broken)
        self.assertEqual(self.binary.read_bytes(), self.original)
        sidecar.write_bytes(saved)
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
        rows_text = host.owned_string("neverd_byte_patches_json", pointer)
        self.assertEqual(rows_text, json.dumps(session.byte_patches,
                         sort_keys=True, separators=(",", ":")))
        self.assertEqual(next(self.root.rglob("patches.json")).read_bytes(),
                         (json.dumps(session.byte_patches, indent=2,
                                     sort_keys=True) + "\n").encode())
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
        live_rows = session.byte_patches
        changed_row = {**live_rows[0], "current": "2b"}
        first = json.dumps(changed_row)
        for malformed in (
            "[" + first + ",null]",
            "[" + first + ",]",
            "[" + first[:-1] + ',"size":1.0}]',
            "[" + first + "," + json.dumps({**changed_row, "original": "xx"}) + "]",
        ):
            with self.subTest(malformed=malformed):
                encoded = malformed.encode()
                sidecar.write_bytes(encoded)
                with self.assertRaises(NeverDError):
                    session.load_byte_patches()
                self.assertEqual(session.byte_patches, live_rows)
                self.assertEqual(session.preview_byte_patch(
                    self.entry, kind="constant", value=7)["original"], "b82a000000")
                self.assertEqual(sidecar.read_bytes(), encoded)
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

    @unittest.skipIf(os.name == "nt", "requires POSIX file-size fault injection")
    def test_cli_failed_write_preserves_saved_patch(self) -> None:
        import resource
        import signal

        cli = os.environ.get("NEVERD_TEST_CLI")
        if not cli:
            self.skipTest("NEVERD_TEST_CLI is not configured")
        command = [cli, "patch-bytes", str(self.binary), "--address",
                   hex(self.entry), "--kind", "constant", "--apply", "--value"]
        subprocess.run(command + ["42"], capture_output=True, check=True, timeout=60)
        sidecar = next(self.root.rglob("patches.json"))
        saved = sidecar.read_bytes()

        def refuse_regular_file_writes():
            _, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1, hard))
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)

        failed = subprocess.run(command + ["43"], capture_output=True, timeout=60,
                                preexec_fn=refuse_regular_file_writes)
        self.assertGreater(failed.returncode, 0, failed.stderr)
        self.assertIn(b"cannot write byte patches", failed.stderr)
        self.assertEqual(sidecar.read_bytes(), saved)
        self.assertEqual(list(sidecar.parent.glob("patches.json.tmp-*")), [])
        subprocess.run(command + ["43"], capture_output=True, check=True, timeout=60)
        self.assertEqual(json.loads(sidecar.read_bytes())[0]["current"], "2b")
        self.assertEqual(self.binary.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main()
