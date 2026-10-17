#!/usr/bin/env python3
"""Real-engine Unicode paths, sidecars and writer ownership on every host."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile

from pe_image_fixture import image
from transport_test import Client


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def run(worker):
    with tempfile.TemporaryDirectory(prefix="neverd-unicode-path-") as directory:
        root = Path(directory)
        data, base = image()
        # A stated COFF function is required for the rename API; the PE entry
        # by itself is a navigation symbol, not a declared function.
        data = bytearray(data)
        data[0x280:0x286] = b"\xb8\x09\0\0\0\xc3"
        struct.pack_into("<II", data, 0x8c, len(data), 1)
        data.extend(b"entry_fn" + struct.pack("<IhHBB", 0, 1, 0x20, 2, 0))
        data.extend(struct.pack("<I", 4))
        data = bytes(data)
        names = ["ascii/input.exe", "\u4e2d\u6587\u76ee\u5f55/input.exe",
                 "ascii/\u4e2d\u6587\u6587\u4ef6.exe",
                 "\u4e2d\u6587\u76ee\u5f55 \u7a7a\u683c/\u6837\u672c #% \U0001f680.exe"]
        with Client(worker) as client:
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                payload = ok(client, "open", {"path": str(path), "read_only": True})
                assert Path(payload["path"]) == path.resolve(), payload
                assert payload["import_count"] == 1, payload
                assembly = ok(client, "disasm", {"address": hex(base + 0x1000), "limit": 4})
                assert assembly["items"], assembly
                assert Path(ok(client, "metadata")["path"]) == path.resolve()
                assert hashlib.sha256(path.read_bytes()).digest() == hashlib.sha256(data).digest()

            invalid = root / "\u4e2d\u6587\u76ee\u5f55" / "\u975e\u6cd5.bin"
            invalid.write_bytes(b"not an executable")
            rejected = client.call("open", {"path": str(invalid), "read_only": True})
            assert rejected["status"] == "error", rejected
            assert "\u975e\u6cd5.bin" in rejected["error"]["message"], rejected
            assert Path(ok(client, "metadata")["path"]) == path.resolve()

            # A real writer exercises returned UTF-8 paths, the native lock,
            # history and atomic sidecar commits, rather than just open().
            ok(client, "open", {"path": str(path)})
            with Client(worker) as competitor:
                rejected = competitor.call("open", {"path": str(path)})
                assert rejected["error"]["code"] == "project_locked", rejected
                ok(competitor, "open", {"path": str(path), "read_only": True})
            address = hex(base + 0x1000)
            note = "\u4e2d\u6587\u6ce8\u91ca \U0001f680"
            ok(client, "annotation_set", {"address": address, "text": note})
            assert ok(client, "save")["saved"]
            annotations = Path(str(path) + ".neverd/annotations.json")
            assert json.loads(annotations.read_text(encoding="utf-8"))[0]["text"] == note
            assert ok(client, "rename", {"address": address, "name": "unicode_entry"})["saved"]
            renames = Path(str(path) + ".neverd/renames.json")
            assert renames.exists()
            created_address = base + 0x1080
            ok(client, "function_create", {"address": hex(created_address)})
            functions = Path(str(path) + ".neverd/functions.json")
            assert functions.exists()
            assert any(int(row["addr"], 16) == created_address and row["state"] == "created"
                       for row in json.loads(functions.read_text(encoding="utf-8")))
            item_address = base + 0x21c0
            defined = ok(client, "item_define", {"address": hex(item_address),
                                                "action": "data", "size": 4})
            assert defined["kind"] == "dword" and defined["saved"], defined
            items = Path(str(path) + ".neverd/items.json")
            assert any(int(row["addr"], 16) == item_address and row["kind"] == "dword"
                       for row in json.loads(items.read_text(encoding="utf-8")))
            # mov rax, 2Ah shows its number in binary.
            formatted = ok(client, "operand_format", {"address": address, "operand": 1,
                                                      "action": "binary"})
            assert formatted["format"]["base"] == "binary" and formatted["saved"], formatted
            operands = Path(str(path) + ".neverd/operands.json")
            assert any(int(row["addr"], 16) == base + 0x1000
                       and row["operands"][0]["base"] == "binary"
                       for row in json.loads(operands.read_text(encoding="utf-8")))
            ok(client, "reload")
            assert ok(client, "annotations")["items"][0]["text"] == note
            assert ok(client, "functions", {"filter": "unicode_entry"})["total"] == 1
            assert created_address in {int(row["address"], 16) for row in
                                       ok(client, "functions", {"limit": 512})["items"]}
            listing = ok(client, "listing", {"address": hex(item_address),
                                              "before": 0, "after": 1})
            line = listing["lines"][0]
            assert (line["address"], line["kind"], line["text"].split()) == (
                hex(item_address), "data", ["dd", "0"]), listing
            # The segment's header lines come first.
            code = ok(client, "listing", {"address": address, "before": 0, "after": 40})
            assert any(line["text"].split()[:3] == ["mov", "rax,", "101010b"]
                       for line in code["lines"]), code
            assert hashlib.sha256(path.read_bytes()).digest() == hashlib.sha256(data).digest()

        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "read_only": True})
            assert ok(client, "annotations")["items"][0]["text"] == note
            assert ok(client, "functions", {"filter": "unicode_entry"})["total"] == 1
            assert created_address in {int(row["address"], 16) for row in
                                       ok(client, "functions", {"limit": 512})["items"]}
            listing = ok(client, "listing", {"address": hex(item_address),
                                              "before": 0, "after": 1})
            line = listing["lines"][0]
            assert (line["address"], line["kind"], line["text"].split()) == (
                hex(item_address), "data", ["dd", "0"]), listing
            # The segment's header lines come first.
            code = ok(client, "listing", {"address": address, "before": 0, "after": 40})
            assert any(line["text"].split()[:3] == ["mov", "rax,", "101010b"]
                       for line in code["lines"]), code
            evm = root / "\u4e2d\u6587\u76ee\u5f55" / "\u5408\u7ea6.hex"
            evm.write_text("600160005500", encoding="ascii")
            payload = ok(client, "open", {"path": str(evm), "read_only": True})
            assert payload["format"] == "EVM", payload
            assert Path(payload["path"]) == evm.resolve(), payload
        print("Unicode paths: ASCII, Chinese directories/names, spaces, #% and emoji; "
              "mapped PE disassembly, failed-load preservation, writer locks, "
              "sidecar save/reload/reopen and EVM input passed")


if __name__ == "__main__":
    run(sys.argv[1])
