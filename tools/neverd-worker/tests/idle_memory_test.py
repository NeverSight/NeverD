#!/usr/bin/env python3
"""Idle allocator policy must preserve cached source and request wakeups."""
import ctypes
import os
from pathlib import Path
import sys
import tempfile
import time

from transport_test import Client


def source(client):
    chunks, offset = [], 0
    while True:
        reply = client.call("decompile", {
            "address": "0x0", "representation": "c",
            "offset": offset, "limit": 256,
        })
        assert reply["status"] == "ok", reply
        chunks.append(reply["payload"]["text"])
        offset = reply["payload"].get("next_offset")
        if offset is None:
            return "".join(chunks)


def run(executable):
    maximum_delay = (1 << (ctypes.sizeof(ctypes.c_long) * 8 - 1)) - 1
    with tempfile.TemporaryDirectory(prefix="neverd-idle-memory-") as directory:
        binary = Path(directory) / "arithmetic.evm"
        binary.write_text("0x600160020100", encoding="ascii")
        for delay in (-1, 0, 20, maximum_delay):
            os.environ["MIMALLOC_PURGE_DELAY"] = str(delay)
            client = Client(executable)
            try:
                # The executor can already be waiting on the largest delay.
                time.sleep(0.1)
                opened = client.call("open", {
                    "path": str(binary), "read_only": True,
                    "analysis": False, "warm": False,
                })
                assert opened["status"] == "ok", opened
                functions = client.call("functions")
                assert functions["status"] == "ok", functions
                before = source(client)
                assert before
                # The positive short delay expires with the source cached.
                time.sleep(0.1)
                assert source(client) == before, delay
            finally:
                client.close()
    print("idle memory: requests wake every delay and cached source survives")


if __name__ == "__main__":
    run(sys.argv[1])
