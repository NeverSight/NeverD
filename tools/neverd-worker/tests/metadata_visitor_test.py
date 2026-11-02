#!/usr/bin/env python3
"""Native metadata visitation preserves JSON facts, paging and early stop."""
import ctypes as C
import json
import os
from pathlib import Path
import struct
import sys
import tempfile

from pe_listing_facts_test import BASE, ENTRY, fixture


class Field(C.Structure):
    _fields_ = [("struct_size", C.c_size_t), ("offset", C.c_ulonglong),
                ("size", C.c_ulonglong), ("target", C.c_ulonglong),
                ("name", C.c_char_p), ("role", C.c_char_p)]


class Item(C.Structure):
    _fields_ = [("struct_size", C.c_size_t), ("address", C.c_ulonglong),
                ("size", C.c_ulonglong), ("type", C.c_char_p),
                ("fields", C.POINTER(Field)), ("field_count", C.c_size_t)]


Visitor = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(Item))


def run(path):
    path = Path(path).resolve()
    directory = os.add_dll_directory(str(path.parent)) if os.name == "nt" else None
    try:
        lib = C.CDLL(str(path))
        lib.neverd_session_create.restype = C.c_void_p
        lib.neverd_session_destroy.argtypes = [C.c_void_p]
        lib.neverd_session_load.argtypes = [C.c_void_p, C.c_char_p]
        lib.neverd_free_string.argtypes = [C.c_void_p]
        lib.neverd_metadata_items_json.argtypes = [C.c_void_p, C.c_ulonglong, C.c_int]
        lib.neverd_metadata_items_json.restype = C.c_void_p
        lib.neverd_function_presentation_json.argtypes = [C.c_void_p, C.c_ulonglong]
        lib.neverd_function_presentation_json.restype = C.c_void_p
        lib.neverd_sha256_hex.argtypes = [C.c_char_p, C.c_size_t]
        lib.neverd_sha256_hex.restype = C.c_void_p
        lib.neverd_func_count.argtypes = [C.c_void_p]
        lib.neverd_func_find_by_addr.argtypes = [C.c_void_p, C.c_ulonglong]
        lib.neverd_func_delete.argtypes = [C.c_void_p, C.c_ulonglong]
        lib.neverd_visit_metadata_items.argtypes = [C.c_void_p, C.c_ulonglong, C.c_int,
                                                   Visitor, C.c_void_p, C.POINTER(C.c_ulonglong)]
        rows, errors = [], []
        stop = False

        @Visitor
        def collect(context, pointer):
            try:
                assert context == 123
                item = pointer.contents
                assert item.struct_size == C.sizeof(Item)
                fields = []
                for index in range(item.field_count):
                    field = item.fields[index]
                    assert field.struct_size == C.sizeof(Field)
                    fields.append({"offset": field.offset, "size": field.size,
                                   "target": hex(field.target), "name": field.name.decode(),
                                   "role": field.role.decode()})
                rows.append({"address": hex(item.address), "size": item.size,
                             "type": item.type.decode(), "fields": fields})
                return not stop
            except BaseException as error:
                errors.append(error)
                return 0

        def read(pointer):
            assert pointer
            try:
                return json.loads(C.string_at(pointer))
            finally:
                lib.neverd_free_string(pointer)

        session = lib.neverd_session_create()
        try:
            import hashlib
            for data in (b"", b"abc", bytes(range(256))):
                pointer = lib.neverd_sha256_hex(data, len(data))
                assert pointer
                try:
                    assert C.string_at(pointer).decode() == hashlib.sha256(data).hexdigest()
                finally:
                    lib.neverd_free_string(pointer)
            assert not lib.neverd_sha256_hex(None, 1)
            cursor = C.c_ulonglong()
            assert lib.neverd_visit_metadata_items(session, 0, 2, collect, 123, C.byref(cursor)) == -1
            with tempfile.TemporaryDirectory(prefix="neverd-metadata-visitor-") as temporary:
                binary = Path(temporary) / "entry.dll"
                binary.write_bytes(fixture())
                assert lib.neverd_session_load(session, os.fsencode(binary))
                entry = read(lib.neverd_function_presentation_json(session, ENTRY))
                assert entry["prototype"].startswith("BOOL __stdcall DllEntryPoint("), entry
                assert not lib.neverd_function_presentation_json(session, ENTRY + 1)
                assert lib.neverd_visit_metadata_items(session, 0, 2, Visitor(), 123, C.byref(cursor)) == -1
                assert lib.neverd_visit_metadata_items(session, 0, 2, collect, 123, None) == -1
                expected, first = [], 0
                while True:
                    page = read(lib.neverd_metadata_items_json(session, first, 2))
                    for item in page["items"]:
                        item["address"] = hex(int(item["address"], 16))
                        for field in item["fields"]:
                            field["target"] = hex(int(field["target"], 16))
                    count = lib.neverd_visit_metadata_items(session, first, 2, collect, 123, C.byref(cursor))
                    assert not errors, errors
                    assert count == len(page["items"])
                    expected.extend(page["items"])
                    assert cursor.value == (int(page["next_address"], 16) if page["next_address"] else 0)
                    if not cursor.value:
                        break
                    first = cursor.value
                assert rows == expected and len(rows) == 5, (rows, expected)
                assert rows[-1]["address"] == hex(BASE + 0x3084)
                rows.clear()
                stop = True
                assert lib.neverd_visit_metadata_items(session, 0, 65536, collect, 123, C.byref(cursor)) == 1
                assert rows == expected[:1] and cursor.value == int(expected[1]["address"], 16)
                stop = False
                rows.clear()
                assert lib.neverd_visit_metadata_items(session, cursor.value, 65536, collect, 123, C.byref(cursor)) == 4
                assert rows == expected[1:] and cursor.value == 0
                assert lib.neverd_visit_metadata_items(session, BASE + 0x4000, 2, collect, 123, C.byref(cursor)) == 0
                assert cursor.value == 0 and not errors
                # A direct transfer outside a bounded entry is a forwarder
                # before its destination has a discovered function identity.
                unknown = BASE + 0x11C0
                data = bytearray(fixture())
                data[0x200:0x205] = b"\xe9" + struct.pack("<i", unknown - ENTRY - 5)
                data[0x3C0] = 0xC3
                other = Path(temporary) / "undiscovered.dll"
                other.write_bytes(data)
                assert lib.neverd_session_load(session, os.fsencode(other))
                # The small loader can seed this direct target. Remove that
                # identity to exercise the query before discovery and ensure
                # presentation cannot resurrect a user-deleted function.
                assert lib.neverd_func_delete(session, unknown) == 0
                count = lib.neverd_func_count(session)
                assert lib.neverd_func_find_by_addr(session, unknown) == -1
                row = read(lib.neverd_function_presentation_json(session, ENTRY))
                assert row["thunk"] and int(row["thunk_target"], 16) == unknown, row
                assert lib.neverd_func_count(session) == count
                assert lib.neverd_func_find_by_addr(session, unknown) == -1
        finally:
            lib.neverd_session_destroy(session)
    finally:
        if directory:
            directory.close()
    print("Native metadata: JSON equivalence, chain rows, page/stop cursors and admission passed")


if __name__ == "__main__":
    run(sys.argv[1])
