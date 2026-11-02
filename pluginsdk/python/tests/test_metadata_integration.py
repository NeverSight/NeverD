"""Official ctypes bindings preserve native metadata callbacks and owned text."""

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

from neverd_plugin import abi
from neverd_plugin.ffi import HostAPI


class MetadataIntegrationTests(unittest.TestCase):
    def test_native_metadata_bindings_preserve_rows_cursors_and_ownership(self):
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(path)))
        # Reuse the owned PE corpus already checked by the native worker suite.
        tests = Path(__file__).resolve().parents[3] / "tools/neverd-worker/tests"
        spec = importlib.util.spec_from_file_location(
            "metadata_binding_fixture", tests / "pe_listing_facts_test.py"
        )
        fixture = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, "path", [str(tests), *sys.path]):
            spec.loader.exec_module(fixture)

        for data in (b"", b"abc", bytes(range(256))):
            buffer = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
            self.assertEqual(host.owned_string("neverd_sha256_hex", buffer, len(data)),
                             hashlib.sha256(data).hexdigest())
        self.assertIsNone(host.owned_string("neverd_sha256_hex", None, 1))

        rows, errors = [], []
        stop = False

        @abi.MetadataVisitorV1
        def collect(context, pointer):
            try:
                self.assertEqual(context, 123)
                item = pointer.contents
                self.assertEqual(item.struct_size, ctypes.sizeof(abi.NeverDMetadataItemV1))
                fields = []
                for field in item.fields[:item.field_count]:
                    self.assertEqual(field.struct_size, ctypes.sizeof(abi.NeverDMetadataFieldV1))
                    fields.append({"offset": field.offset, "size": field.size,
                                   "target": hex(field.target), "name": field.name.decode(),
                                   "role": field.role.decode()})
                rows.append({"address": hex(item.address), "size": item.size,
                             "type": item.type.decode(), "fields": fields})
                return not stop
            except BaseException as error:
                errors.append(error)
                return 0

        cursor = ctypes.c_uint64()
        session = host.call("neverd_session_create")
        try:
            with tempfile.TemporaryDirectory(prefix="neverd-metadata-bindings-") as temporary:
                binary = Path(temporary) / "entry.dll"
                binary.write_bytes(fixture.fixture())
                self.assertEqual(host.call("neverd_session_load", session, os.fsencode(binary)), 1)
                presentation = json.loads(host.owned_string(
                    "neverd_function_presentation_json", session, fixture.ENTRY))
                self.assertTrue(presentation["prototype"].startswith("BOOL __stdcall DllEntryPoint("))
                expected, first = [], 0
                while True:
                    page = json.loads(host.owned_string(
                        "neverd_metadata_items_json", session, first, 2))
                    for item in page["items"]:
                        item["address"] = hex(int(item["address"], 16))
                        for field in item["fields"]:
                            field["target"] = hex(int(field["target"], 16))
                    count = host.call("neverd_visit_metadata_items", session, first,
                                      2, collect, 123, ctypes.byref(cursor))
                    self.assertFalse(errors, errors)
                    self.assertEqual(count, len(page["items"]))
                    expected.extend(page["items"])
                    self.assertEqual(cursor.value,
                                     int(page["next_address"], 16) if page["next_address"] else 0)
                    if not cursor.value:
                        break
                    first = cursor.value
                self.assertEqual(rows, expected)
                self.assertEqual(len(rows), 5)
                rows.clear()
                stop = True
                self.assertEqual(host.call("neverd_visit_metadata_items", session, 0,
                                           2, collect, 123, ctypes.byref(cursor)), 1)
                self.assertEqual(cursor.value, int(expected[1]["address"], 16))
                self.assertFalse(errors, errors)
        finally:
            host.call("neverd_session_destroy", session)
        self.assertEqual(rows, expected[:1])
