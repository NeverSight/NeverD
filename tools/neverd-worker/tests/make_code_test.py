#!/usr/bin/env python3
"""Code definitions are atomic, persistent instructions without new functions."""
import json
from pathlib import Path
import sys
import struct
import tempfile

from transport_test import Client

START = 0xffff800012342700


def ok(client, operation, payload=None):
    result = client.call(operation, payload)
    assert result["status"] == "ok", result
    return result["payload"]


def run(executable, real=False):
    global START
    with tempfile.TemporaryDirectory(prefix="neverd-make-code-") as directory:
        binary = Path(directory) / "make-code.bin"
        if real:
            base, entry = 0x400000, 0x400078
            code = bytes.fromhex("b807000000c3" "904883c428c30f")
            header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + bytes(9),
                                 2, 62, 1, entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
            segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                                  120 + len(code), 120 + len(code), 4096)
            binary.write_bytes(header + segment + code)
            START = entry + 6
        else:
            binary.write_bytes(b"fixture")
        sidecar = Path(str(binary) + ".neverd-items.json")
        rows = lambda: json.loads(sidecar.read_text()) if sidecar.exists() else []
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            before_count = ok(client, "functions", {"offset": 0, "limit": 1})["total"]
            defined = ok(client, "item_define", {"address": hex(START), "action": "code"})
            assert defined == {"address": hex(START), "kind": "code", "size": 6, "saved": True}, defined
            saved = rows()
            assert [(int(row["addr"], 16), row["kind"], row["size"]) for row in saved] == [
                (START, "code", 1), (START + 1, "code", 4), (START + 5, "code", 1)], saved
            listing = ok(client, "listing", {"address": hex(START), "before": 0, "after": 4})
            insns = [row for row in listing["lines"] if row["kind"] == "insn"]
            assert [int(row["address"], 16) for row in insns] == [START, START + 1, START + 5], listing
            assert "add" in insns[1]["text"] and "rsp, 28h" in insns[1]["text"], insns
            assert insns[2]["flow"] == "ret", insns
            assert ok(client, "functions", {"offset": 0, "limit": 1})["total"] == before_count
            assert not ok(client, "item_define", {"address": hex(START), "action": "code"})["saved"]
            ok(client, "undo")
            assert rows() == []
            ok(client, "redo")
            assert rows() == saved
            # U hides a manually defined instruction and one undo restores it.
            ok(client, "item_define", {"address": hex(START + 1), "action": "undefine"})
            assert rows()[1]["kind"] == "undefined"
            ok(client, "undo")
            assert rows() == saved
            # Redefining carved undefined bytes retains their neighbours.
            ok(client, "item_define", {"address": hex(START + 1), "action": "undefine"})
            ok(client, "item_define", {"address": hex(START + 1), "action": "code"})
            assert rows() == saved
            malformed = client.call("item_define", {"address": hex(START + 6), "action": "code"})
            assert malformed["status"] == "error", malformed
            assert rows() == saved
            with Client(executable) as reader:
                ok(reader, "open", {"path": str(binary), "read_only": True})
                denied = reader.call("item_define", {"address": hex(START + 6), "action": "code"})
                assert denied["error"]["code"] == "read_only", denied
        with Client(executable) as reopened:
            ok(reopened, "open", {"path": str(binary)})
            listing = ok(reopened, "listing", {"address": hex(START + 1), "before": 0, "after": 1})
            assert any("add" in row["text"] for row in listing["lines"]), listing
            # P can later give the code a function without losing its rows;
            # deleting that function leaves the explicit code definitions.
            ok(reopened, "function_create", {"address": hex(START)})
            listing = ok(reopened, "listing", {"address": hex(START + 1), "before": 0, "after": 1})
            assert any("add" in row["text"] for row in listing["lines"]), listing
            ok(reopened, "function_delete", {"address": hex(START)})
            assert rows() == saved
            # Remove all definitions, place data inside the next instruction,
            # then reject C without retaining its first (valid) instruction.
            while rows():
                ok(reopened, "undo")
            ok(reopened, "item_define", {"address": hex(START + 2), "action": "data"})
            conflict = rows()
            history = ok(reopened, "history")
            failed = reopened.call("item_define", {"address": hex(START), "action": "code"})
            assert failed["status"] == "error", failed
            assert rows() == conflict
            assert ok(reopened, "history") == history
            listing = ok(reopened, "listing", {"address": hex(START), "before": 0, "after": 1})
            assert not any(row["kind"] == "insn" for row in listing["lines"]), listing


if __name__ == "__main__":
    run(sys.argv[1], "--real" in sys.argv[2:])
