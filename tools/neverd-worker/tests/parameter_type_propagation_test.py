#!/usr/bin/env python3
"""Editing a parameter retypes captured aliases, members and array elements."""
from pathlib import Path
import struct
import sys
import tempfile

from pe_browse_test import image
from transport_test import Client


ENTRY = 0x140001000


def ok(client, operation, payload=None):
    response = client.call(operation, payload or {})
    assert response["status"] == "ok", (operation, response)
    return response["payload"]


def source(client, representation):
    return ok(client, "decompile", {"address": hex(ENTRY),
              "representation": representation, "limit": 2048})["text"]


def fixture():
    # Full-width pointer spill/load, a call, then unrelated slot reuse. RBX
    # captures the pointer before the call and remains live for member stores.
    code = bytearray.fromhex("53 4883ec30 48894c2420 488b5c2420")
    code += b"\xe8" + struct.pack("<i", 0x80 - len(code) - 5)
    code += bytes.fromhex("48c744242000000000 48c743082a000000 "
                          "48c743182b000000 488b4308 4883c430 5b c3")
    code = code.ljust(0x80, b"\xcc") + bytes.fromhex("31c0 c3")
    binary, _ = image()
    data = bytearray(binary)
    data[0x200:0x200 + len(code)] = code
    return bytes(data), code


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-parameter-type-") as directory:
        path = Path(directory) / "parameter.exe"
        binary, code = fixture()
        path.write_bytes(binary)
        with Client(executable) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            ok(client, "local_types_declare", {"language": "c", "source":
               "struct CONTEXT { unsigned long long cookie; "
               "unsigned long long value; unsigned long long slots[2]; }; "
               "struct OTHER_CONTEXT { unsigned long long cookie; "
               "unsigned long long payload; unsigned long long items[2]; };"})
            original = source(client, "low")
            for representation in ("c", "llvmc"):
                text = source(client, representation)
                assert "->value" not in text and "->slots[1]" not in text, text
            declaration = ok(client, "function_type", {
                "address": hex(ENTRY), "parameter": 0, "type": "CONTEXT *arg0"})
            ok(client, "function_edit", {"address": hex(ENTRY),
               "attributes": {"type": declaration["declaration"]}})
            for representation in ("c", "llvmc"):
                text = source(client, representation)
                assert "->value" in text and "->slots[1]" in text, text
            changed = ok(client, "function_type", {
                "address": hex(ENTRY), "parameter": 0,
                "type": "OTHER_CONTEXT *arg0"})
            ok(client, "function_edit", {"address": hex(ENTRY),
               "attributes": {"type": changed["declaration"]}})
            for representation in ("c", "llvmc"):
                text = source(client, representation)
                assert "->payload" in text and "->items[1]" in text, text
                assert "->value" not in text and "->slots[1]" not in text, text
            ok(client, "undo")
            for representation in ("c", "llvmc"):
                assert "->value" in source(client, representation)
            assert source(client, "low") == original
            ok(client, "undo")
            for representation in ("c", "llvmc"):
                assert "->value" not in source(client, representation)
            ok(client, "redo")
            for representation in ("c", "llvmc"):
                assert "->slots[1]" in source(client, representation)
            ok(client, "save")
        with Client(executable) as client:
            ok(client, "open", {"path": str(path), "analysis": False,
                                "read_only": True})
            for representation in ("c", "llvmc"):
                text = source(client, representation)
                assert "->value" in text and "->slots[1]" in text, text
            raw = ok(client, "bytes", {"address": hex(ENTRY), "size": len(code)})
            assert bytes.fromhex(raw["data"]) == code, raw
        assert path.read_bytes() == binary
    print("parameter type propagation, array members, undo/redo and reopen passed")


if __name__ == "__main__":
    run(sys.argv[1])
