#!/usr/bin/env python3
"""Portable C ABI smoke: analyze benign arithmetic EVM bytecode, never execute it."""
from pathlib import Path
import hashlib
import sys
import tempfile
from transport_test import Client


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-real-worker-") as directory:
        binary = Path(directory) / "arithmetic.evm"
        binary.write_text("0x600160020100", encoding="ascii")
        client = Client(executable)
        try:
            opened = client.call("open", {"path": str(binary), "read_only": True})
            assert opened["status"] == "ok", opened
            assert opened["payload"]["architecture"] == "evm"
            assert opened["payload"]["bitness"] == 256
            assert opened["payload"]["function_count"] is None
            functions = client.call("functions")
            assert functions["status"] == "ok", functions
            assert functions["revision"] != opened["revision"]
            assert functions["payload"]["items"][0]["address"] == "0x0"
            first = client.call("disasm", {"address": "0x0", "limit": 2})
            assert first["status"] == "ok", first
            assert first["payload"]["next_address"] == "0x4"
            second = client.call("disasm", {"address": "0x4", "limit": 8})
            assert [item["mnemonic"] for item in second["payload"]["items"]] == ["ADD", "STOP"]
            assert second["payload"]["complete"]
            assert client.call("bytes", {"address": "0x0", "size": 6})["payload"]["data"] == "600160020100"
            for representation in ("c", "low", "med", "high", "llvm"):
                code = client.call("decompile", {"address": "0x0", "representation": representation, "limit": 32})
                assert code["status"] == "ok", (representation, code)
                assert code["payload"]["text"]
            graph = client.call("cfg", {"address": "0x0"})
            assert graph["status"] == "ok" and graph["payload"]["complete"], graph
            assert graph["payload"]["nodes"][0]["start"] == "0x0"
            assert isinstance(graph["payload"]["nodes"][0]["id"], str)
            summary = client.call("cfg_summary", {"address": "0x0"})
            assert summary["status"] == "ok", summary
            viewport = dict(summary["payload"]["bounds"], address="0x0", scale=1,
                            layout_revision=summary["payload"]["layout_revision"])
            tile = client.call("cfg_viewport", viewport)
            assert tile["status"] == "ok" and tile["payload"]["complete"], tile
            assert tile["payload"]["nodes"][0]["address"] == "0x0"
            assert client.call("xrefs", {"address": "0x0"})["error"]["code"] == "unsupported"
            assert client.call("annotation_set", {"address": "0x0", "text": "forbidden"})["error"]["code"] == "read_only"
            history = client.call("history")
            assert history["status"] == "ok", history
            assert history["payload"]["source_sha256"] == hashlib.sha256(binary.read_bytes()).hexdigest()
            print("real engine: EVM load, explicit analysis revision, disassembly boundaries, bytes, C/all IR stages, CFG and read-only checks passed")
        finally:
            client.close()


if __name__ == "__main__":
    run(sys.argv[1])
