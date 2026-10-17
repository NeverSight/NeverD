#!/usr/bin/env python3
"""A file no header describes opens as the load dialog places it.

Writes x86-64 code under a firmware image's usual name, which EVM tools give
their bytecode too, and checks the engine does not guess either: the file
opens only with a chosen loader.  Opened as a binary file at the address the
user chose, it is browsed -- listing, functions, references, a control flow
graph -- and decompiled under the platform its code shows; three
instructions show none, so System V is assumed and the metadata says so.
The engine keeps the choice with the input, so the next open reads the file
the same way unasked.

Runs against the real engine; the bytes are analyzed, never executed.
"""
import json
from pathlib import Path
import sys
import tempfile
from transport_test import Client

BASE = 0x400000
# start: call add_seven; ret
# add_seven: lea eax, [rdi + 7]; ret
CODE = bytes.fromhex("e801000000c3" "8d4707c3")
CALLEE = BASE + 6


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-binary-file-") as directory:
        firmware = Path(directory) / "firmware.bin"
        firmware.write_bytes(CODE)
        sidecar = Path(str(firmware) + ".neverd/load.json")
        client = Client(executable)
        try:
            # Only the name ties these bytes to EVM bytecode, so that row is
            # listed for the name alone; the binary file is the default.
            rows = ok(client, "identify", {"path": str(firmware)})["rows"]
            listed = [(row["loader"], row["loadable"], row.get("by_name", False)) for row in rows]
            assert listed == [("evm", True, True), ("binary", True, False)], rows
            guessed = client.call("open", {"path": str(firmware)})
            assert guessed["status"] == "error", guessed
            assert "choose its loader" in guessed["error"]["message"], guessed

            opened = ok(client, "open", {"path": str(firmware), "loader": "binary", "processor": "x86_64",
                                         "base": hex(BASE)})
            assert (opened["format"], opened["architecture"]) == ("Binary", "x86_64"), opened
            assert int(opened["entry_address"], 16) == BASE, opened
            assert json.loads(sidecar.read_text())["processor"] == "x86_64"

            # Function discovery, which references run first, finds the
            # function start calls.
            references = ok(client, "xrefs", {"address": hex(CALLEE)})
            assert hex(BASE) in json.dumps(references), references
            functions = ok(client, "functions", {"offset": 0, "limit": 64})["items"]
            entries = {int(row["address"], 16) for row in functions}
            assert {BASE, CALLEE} <= entries, functions
            page = ok(client, "listing", {"address": hex(BASE), "before": 0, "after": 16})["lines"]
            assert any("call" in line["text"] for line in page), page
            graph = ok(client, "cfg", {"address": hex(BASE)})
            assert graph["nodes"] and all(node["disasm"] for node in graph["nodes"]), graph
            assert ok(client, "cfg_summary", {"address": hex(BASE)})["node_count"] >= 1

            load = opened["load_options"]
            assert (load["platform"], load["platform_source"]) == ("sysv", "detected"), load
            assert "assumed" in load["platform_evidence"], load
            # lea eax, [rdi + 7] takes its one argument in rdi.
            text = ok(client, "decompile", {"address": hex(CALLEE)})["text"]
            assert "7" in text, text
        finally:
            client.close()

        # The next open reads the file as it was read last time.
        again = Client(executable)
        try:
            reopened = ok(again, "open", {"path": str(firmware)})
            assert (reopened["format"], reopened["architecture"]) == ("Binary", "x86_64"), reopened
            assert int(reopened["base_address"], 16) == BASE, reopened
        finally:
            again.close()
    print("binary file: opens only as chosen, browses, decompiles, and reopens the same way")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
