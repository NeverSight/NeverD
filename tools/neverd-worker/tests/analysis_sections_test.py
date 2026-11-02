#!/usr/bin/env python3
"""Real C ABI/worker: add code to an existing image, analyze and persist it."""
import copy
import ctypes
import hashlib
import json
import re
import struct
from pathlib import Path
import sys
import tempfile

from native_mapping_test import native_elf
from pe_browse_test import image as pe_image
from transport_test import Client


def section(address, data, name=".recovered", arm64=False):
    width = 8 if arm64 else 6
    return {"schema": "neverd.analysis-section.v1", "name": name,
            "address": hex(address), "permissions": "rx", "data_hex": data.hex(),
            "functions": [{"offset": "0x0", "size": width, "name": "recovered_first"},
                          {"offset": hex(width), "size": width, "name": "recovered_second",
                           "source_address": "0x400078"}]}


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", (operation, reply)
    return reply


def exercise_worker(executable, directory, arm64):
    base = 0xffff800000400000 if arm64 else 0x400000
    original, entry = native_elf(arm64, base)
    binary = directory / ("arm64.elf" if arm64 else "x64.elf")
    binary.write_bytes(original)
    code = bytes.fromhex("e0008052c0035fd6e0038052c0035fd6" if arm64 else
                         "b807000000c3b81f000000c3")
    added = base + 0x2000
    payload = section(added, code, arm64=arm64)
    with Client(executable) as client:
        opened = ok(client, "open", {"path": str(binary)})
        assert opened["payload"]["analysis_sections_supported"]
        assert not ok(client, "analysis_sections")["payload"]["sections"]
        before = ok(client, "bytes", {"address": hex(entry), "size": len(original) - 120})
        # Prepare original code first: section edits must retire its pipeline.
        ok(client, "decompile", {"address": hex(entry), "representation": "c"})
        revision = ok(client, "metadata")["revision"]
        bad = copy.deepcopy(payload)
        bad["address"] = hex(entry)
        refused = client.call("analysis_section_add", bad)
        assert refused["status"] == "error" and refused["revision"] == revision, refused
        assert not ok(client, "analysis_sections")["payload"]["sections"]
        for change in ({"address": "0xffffffffffffffff"}, {"permissions": "x"},
                       {"functions": [{"offset": "0x0", "size": len(code) + 1, "name": "bad"}]},
                       {"unexpected": True}):
            bad = dict(payload, **change)
            refused = client.call("analysis_section_add", bad)
            assert refused["status"] == "error" and refused["revision"] == revision, refused
        result = ok(client, "analysis_section_add", payload)
        assert result["revision"] != revision and result["payload"]["dirty"]
        assert ok(client, "bytes", {"address": hex(added), "size": len(code)})["payload"]["data"] == code.hex()
        assert ok(client, "bytes", {"address": hex(entry), "size": len(original) - 120})["payload"] == before["payload"]
        # The current GUI runs background analysis in a fresh read-only worker.
        # Unsaved additions belong to the captured image, not just disk state.
        snapshot = ok(client, "analysis_snapshot")["payload"]
        with Client(executable) as replica:
            ok(replica, "analysis_restore", snapshot)
            assert ok(replica, "bytes", {"address": hex(added), "size": len(code)})["payload"]["data"] == code.hex()
            assert len(ok(replica, "analysis_sections")["payload"]["sections"]) == 1
        declarations = ok(client, "analysis_sections")["payload"]["sections"]
        assert len(declarations) == 1 and int(declarations[0]["address"], 16) == added
        functions = ok(client, "functions")["payload"]["items"]
        assert {"recovered_first", "recovered_second"} <= {f["name"] for f in functions}, functions
        for name, offset in (("recovered_first", 0), ("recovered_second", 8 if arm64 else 6)):
            addr = hex(added + offset)
            resolved = ok(client, "resolve", {"query": name})
            assert resolved["payload"]["function_address"] == addr
            instructions = ok(client, "disasm", {"address": addr, "limit": 2})
            assert instructions["payload"]["items"][0]["address"] == addr
            listing = ok(client, "listing", {"address": addr, "before": 0, "after": 64})["payload"]["lines"]
            assert any(row.get("kind") == "insn" and row.get("function_address") == addr
                       for row in listing), listing
            for representation in ("low", "med", "c"):
                text = ok(client, "decompile", {"address": addr, "representation": representation})["payload"]["text"]
                assert text.strip(), (name, representation, text)
                if representation == "c":
                    assert name in text, text
                    assert ("31" in text or "0x1F" in text or "0x1f" in text) if offset else ("7" in text), text
            graph = ok(client, "cfg", {"address": addr})
            assert graph["payload"]["nodes"] and graph["payload"]["complete"], graph
        # Same name/range and unknown removals fail without publishing a revision.
        revision = ok(client, "metadata")["revision"]
        for op, request in (("analysis_section_add", payload),
                            ("analysis_section_remove", {"name": ".text"})):
            refused = client.call(op, request)
            assert refused["status"] == "error" and refused["revision"] == revision, refused
        # A file-backed payload and a non-executable data section use the same API.
        data_file = directory / "constant.bin"
        data_file.write_bytes(b"section data\0")
        data_section = {"schema": payload["schema"], "name": ".added_data",
                        "address": hex(added + 0x1000), "permissions": "rw", "path": str(data_file)}
        ok(client, "analysis_section_add", data_section)
        ok(client, "analysis_section_remove", {"name": ".added_data"})
        if not arm64:
            # A chosen debug file reads the input again: the additions stay in
            # the image it reads, beside the names the file gives.
            chosen = directory / "chosen-names.map"
            chosen.write_text("VMA LMA Size Align Out In Symbol\n"
                              "00400078 00400078 00000006 1 .text\n"
                              "00400078 00400078 00000006 1 kept_function\n", encoding="utf-8")
            ok(client, "map_load", {"path": str(chosen), "mode": "unchecked"})
            for name, address in (("kept_function", entry), ("recovered_first", added)):
                assert ok(client, "resolve", {"query": name})["payload"]["function_address"] == hex(address)
            assert ok(client, "bytes", {"address": hex(added), "size": len(code)})["payload"]["data"] == code.hex()
        ok(client, "save")
        ok(client, "analysis_section_add", data_section)
        identity = ok(client, "metadata")["project_id"]
        reloaded = ok(client, "reload")
        assert reloaded["project_id"] == identity and not reloaded["payload"]["dirty"]
        assert [r["name"] for r in ok(client, "analysis_sections")["payload"]["sections"]] == [".recovered"]
        if not arm64:
            # The reload opens the input with the debug file chosen for it,
            # which loads over the saved sections again.
            assert ok(client, "metadata")["payload"]["map"]["mode"] == "unchecked"
            for name, address in (("kept_function", entry), ("recovered_first", added)):
                assert ok(client, "resolve", {"query": name})["payload"]["function_address"] == hex(address)
        assert binary.read_bytes() == original
    sidecar = Path(str(binary) + ".neverd/analysis-sections.json")
    saved = json.loads(sidecar.read_text(encoding="utf-8"))
    assert saved["sample_sha256"] == hashlib.sha256(original).hexdigest()
    assert saved["sections"][0]["data_hex"] == code.hex()
    with Client(executable) as client:
        ok(client, "open", {"path": str(binary), "read_only": True})
        assert len(ok(client, "analysis_sections")["payload"]["sections"]) == 1
        assert client.call("analysis_section_remove", {"name": ".recovered"})["error"]["code"] == "read_only"
        assert client.call("analysis_section_add", payload)["error"]["code"] == "read_only"
        ok(client, "decompile", {"address": hex(added), "representation": "c"})
    with Client(executable) as client:
        ok(client, "open", {"path": str(binary)})
        ok(client, "analysis_section_remove", {"name": ".recovered"})
        assert not ok(client, "analysis_sections")["payload"]["sections"]
        assert all(f["name"] != "recovered_first" for f in ok(client, "functions")["payload"]["items"])
        assert ok(client, "bytes", {"address": hex(added), "size": 1})["payload"]["data"] == ""
        ok(client, "save")
        assert not json.loads(sidecar.read_text(encoding="utf-8"))["sections"]
        # Project files that change outside this worker replace its comments,
        # not an unsaved section edit: the project still needs a save.
        ok(client, "rename", {"address": hex(entry), "name": "renamed_here"})
        ok(client, "save")
        renames = Path(str(binary) + ".neverd/renames.json")
        ok(client, "analysis_section_add", payload)
        renames.write_text(renames.read_text(encoding="utf-8").replace(
            "renamed_here", "renamed_elsewhere"), encoding="utf-8")
        synced = ok(client, "project_sync")["payload"]
        assert synced["tables"] and synced["dirty"], synced
        ok(client, "analysis_section_remove", {"name": ".recovered"})
        ok(client, "save")
        # A foreign section sidecar cannot replace the current active project.
        other = directory / ("bad_arm64.elf" if arm64 else "bad_x64.elf")
        other.write_bytes(original)
        saved["sample_sha256"] = "0" * 64
        Path(str(other) + ".neverd-analysis-sections.json").write_text(json.dumps(saved), encoding="utf-8")
        revision = ok(client, "metadata")["revision"]
        failed = client.call("open", {"path": str(other)})
        assert failed["status"] == "error" and failed["revision"] == revision, failed
        assert ok(client, "bytes", {"address": hex(entry), "size": len(original) - 120})["payload"] == before["payload"]


def exercise_capi(library, directory):
    """Direct ABI ownership and transactional validation, without the worker."""
    lib = ctypes.CDLL(library)
    ptr, cstr, size, integer = ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int
    for name, args, result in [
        ("neverd_session_create", [], ptr), ("neverd_session_destroy", [ptr], None),
        ("neverd_session_load", [ptr, cstr], integer),
        ("neverd_free_string", [ptr], None), ("neverd_last_error", [ptr], ptr),
        ("neverd_analysis_section_add", [ptr, cstr, ptr, size], integer),
        ("neverd_analysis_section_remove", [ptr, cstr], integer),
        ("neverd_analysis_sections_json", [ptr, integer], ptr),
        ("neverd_session_load_map", [ptr, cstr, integer], integer),
        ("neverd_session_debug_info_kind", [ptr], ptr),
        ("neverd_func_find_by_name", [ptr, cstr], integer),
        ("neverd_search_page", [ptr, cstr], ptr),
        ("neverd_lift_to_obj", [ptr, cstr, integer, integer], integer),
        ("neverd_roundtrip_obj", [ptr, ctypes.POINTER(ctypes.c_ulonglong)], ptr)]:
        function = getattr(lib, name)
        function.argtypes, function.restype = args, result
    lib.neverd_analysis_sections_import_json.argtypes = [ptr, cstr]
    lib.neverd_analysis_sections_import_json.restype = integer
    lib.neverd_decompile_all.argtypes = [ptr, cstr, integer, integer, integer]
    lib.neverd_decompile_all.restype = ptr
    session = lib.neverd_session_create()

    def text(function, *args):
        value = function(session, *args)
        assert value
        try:
            return ctypes.string_at(value).decode("utf-8")
        finally:
            lib.neverd_free_string(value)

    def add(payload, data):
        buffer = ctypes.create_string_buffer(data)
        return lib.neverd_analysis_section_add(session, json.dumps(payload).encode(), buffer, len(data))

    try:
        original, _ = native_elf(False, 0x400000)
        binary = directory / "direct.elf"
        binary.write_bytes(original)
        assert lib.neverd_session_load(session, str(binary).encode()) == 1
        payload = section(0x500000, b"\xb8\x07\x00\x00\x00\xc3" * 2)
        data = bytes.fromhex(payload.pop("data_hex"))
        assert add(payload, data) == 1, text(lib.neverd_last_error)
        before = json.loads(text(lib.neverd_analysis_sections_json, 1))
        assert before["sample_sha256"] == hashlib.sha256(original).hexdigest()
        assert before["sections"][0]["data_hex"] == data.hex()
        for data in (b"", b"\x0f", b"\xc3"):
            malformed = dict(payload, name=".bad", address="0x600000",
                             functions=[{"offset": "0x0", "size": len(data), "name": "bad"}])
            if data == b"\xc3":
                malformed["functions"][0]["offset"] = "0x1"
            assert add(malformed, data) == 0
            assert text(lib.neverd_last_error)
            assert json.loads(text(lib.neverd_analysis_sections_json, 1)) == before
        assert lib.neverd_analysis_section_remove(session, b".recovered") == 1
        assert not json.loads(text(lib.neverd_analysis_sections_json, 1))["sections"]
        assert lib.neverd_analysis_sections_import_json(session, json.dumps(before).encode()) == 1
        assert json.loads(text(lib.neverd_analysis_sections_json, 1)) == before
        invalid = copy.deepcopy(before)
        later = copy.deepcopy(invalid["sections"][0])
        later.update(name=".bad_later", address="0x600000", size=1, data_hex="0f",
                     functions=[{"name": "bad_later", "offset": "0x0", "size": 1}])
        invalid["sections"].append(later)
        foreign = dict(before, sample_sha256="0" * 64)
        for state in (invalid, foreign, dict(before, sections={})):
            assert lib.neverd_analysis_sections_import_json(session, json.dumps(state).encode()) == 0
            assert text(lib.neverd_last_error)
            assert json.loads(text(lib.neverd_analysis_sections_json, 1)) == before
        source = text(lib.neverd_decompile_all, str(binary).encode(), 0, 0, 0)
        assert "recovered_first" in source, source
        # A path-based view of another file must not inherit this session's
        # additions, even when the two base files happen to contain equal bytes.
        other = directory / "other_direct.elf"
        other.write_bytes(original)
        foreign_source = text(lib.neverd_decompile_all, str(other).encode(), 0, 0, 0)
        assert "recovered_first" not in foreign_source, foreign_source

        # A debug file chosen later reads the input again: the additions join
        # the image it reads, or a file they cannot join is refused whole.
        names = "VMA LMA Size Align Out In Symbol\n00400078 00400078 00000006 1 .text\n"
        chosen = directory / "chosen.map"
        chosen.write_text(names + "00400078 00400078 00000006 1 kept_function\n", encoding="utf-8")
        assert lib.neverd_session_load_map(session, str(chosen).encode(), 1) == 1, text(lib.neverd_last_error)
        assert text(lib.neverd_session_debug_info_kind) == "map"
        for function in (b"kept_function", b"recovered_first", b"recovered_second"):
            assert lib.neverd_func_find_by_name(session, function) >= 0, function
        assert json.loads(text(lib.neverd_analysis_sections_json, 1)) == before
        clash = directory / "clash.map"
        clash.write_text(names + "00400078 00400078 00000006 1 recovered_first\n", encoding="utf-8")
        assert lib.neverd_session_load_map(session, str(clash).encode(), 1) == 0
        assert ".recovered" in text(lib.neverd_last_error)
        assert lib.neverd_func_find_by_name(session, b"kept_function") >= 0
        assert json.loads(text(lib.neverd_analysis_sections_json, 1)) == before

        # The object a round trip lends outlives section edits, which never
        # change the input file it was built from.
        assert lib.neverd_lift_to_obj(session, str(binary).encode(), 0, 0) == 0, text(lib.neverd_last_error)
        length = ctypes.c_ulonglong()
        lent = lib.neverd_roundtrip_obj(session, ctypes.byref(length))
        assert lent and length.value
        built = ctypes.string_at(lent, length.value)

        # Immediate search indexes an added executable section, even one that
        # declares no function, and forgets a removed one.
        def immediates(value):
            request = json.dumps({"kind": "immediate", "pattern": hex(value), "limit": 16}).encode()
            for _ in range(64):
                page = json.loads(text(lib.neverd_search_page, request))
                if not page["building"]:
                    return [item["addr"] for item in page["items"]]
            raise AssertionError("the immediate index did not finish")

        assert immediates(0x1234) == []
        probe = {"schema": payload["schema"], "name": ".probe", "address": "0x700000", "permissions": "rx"}
        assert add(probe, b"\xb8\x34\x12\x00\x00\xc3") == 1, text(lib.neverd_last_error)
        assert immediates(0x1234) == ["0x700000"]
        assert lib.neverd_analysis_section_remove(session, b".probe") == 1
        assert immediates(0x1234) == []

        again = ctypes.c_ulonglong()
        assert lib.neverd_roundtrip_obj(session, ctypes.byref(again)) == lent
        assert again.value == length.value and ctypes.string_at(lent, length.value) == built
        assert binary.read_bytes() == original
    finally:
        lib.neverd_session_destroy(session)


def exercise_original_references(executable, directory):
    original, helper = native_elf(False, 0x400000)
    # Materialize one global after the original helper and extend its segment.
    global_address = 0x400000 + len(original)
    original = bytearray(original + struct.pack("<I", 35))
    struct.pack_into("<QQ", original, 64 + 32, len(original), len(original))
    binary = directory / "original_context.elf"
    binary.write_bytes(original)
    address = 0x500000
    # sub rsp,40; call original_helper; add rsp,40; add eax,[original_global]; ret
    code = (b"\x48\x83\xec\x28\xe8" + struct.pack("<i", helper - (address + 9)) +
            b"\x48\x83\xc4\x28\x03\x05" + struct.pack("<i", global_address - (address + 19)) + b"\xc3")
    with Client(executable) as client:
        ok(client, "open", {"path": str(binary)})
        request = {"schema": "neverd.analysis-section.v1", "name": ".with_context",
                   "address": hex(address), "permissions": "rx", "data_hex": code.hex(),
                   "functions": [{"name": "uses_original_image", "offset": "0x0", "size": len(code)}]}
        ok(client, "analysis_section_add", request)
        instructions = ok(client, "disasm", {"address": hex(address), "limit": 8})["payload"]["items"]
        call = next(row for row in instructions if row["mnemonic"] == "call")
        assert hex(helper) in call["operands"], instructions
        for representation in ("low", "med", "c"):
            text = ok(client, "decompile", {"address": hex(address), "representation": representation})["payload"]["text"]
            assert text.strip()
            if representation == "c":
                assert "uses_original_image" in text and "400078" in text, text
        assert ok(client, "bytes", {"address": hex(global_address), "size": 4})["payload"]["data"] == "23000000"
        regions = ok(client, "regions")["payload"]["items"]
        recovered = next(r for r in regions if r["name"] == ".with_context")
        assert "file_offset" not in recovered, recovered
        assert binary.read_bytes() == original


def exercise_pe(executable, directory):
    for wide in (False, True):
        original, base = pe_image(wide)
        binary = directory / ("pe64.exe" if wide else "pe32.exe")
        binary.write_bytes(original)
        code = bytes.fromhex("b807000000c3b81f000000c3")
        address = base + 0x10000
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            imports = ok(client, "imports")["payload"]["items"]
            payload = section(address, code)
            if not wide:
                bad = dict(payload, address="0x100000000")
                revision = ok(client, "metadata")["revision"]
                failed = client.call("analysis_section_add", bad)
                assert failed["status"] == "error" and failed["revision"] == revision, failed
            ok(client, "analysis_section_add", payload)
            assert ok(client, "imports")["payload"]["items"] == imports
            text = ok(client, "decompile", {"address": hex(address + 6), "representation": "c"})["payload"]["text"]
            result = re.search(r"\breturn\s+(0[xX][0-9a-fA-F]+|[0-9]+)\s*;", text)
            assert "recovered_second" in text and result is not None, text
            assert int(result[1], 0) == 31, text
            assert binary.read_bytes() == original


def run(executable, library):
    with tempfile.TemporaryDirectory(prefix="neverd-sections-") as path:
        directory = Path(path)
        exercise_capi(library, directory)
        for arm64 in (False, True):
            exercise_worker(executable, directory, arm64)
        exercise_original_references(executable, directory)
        exercise_pe(executable, directory)
    print("analysis sections: direct C ABI, ELF x64/AArch64 high VA, PE32/PE64 and original imports, "
          "multi-function listing/Low/Med/HighC/CFG, original calls/globals, atomic rejection, "
          "removal, file payload, read-only, source-hash persistence passed")


if __name__ == "__main__":
    run(*sys.argv[1:])
