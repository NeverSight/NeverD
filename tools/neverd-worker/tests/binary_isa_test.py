#!/usr/bin/env python3
"""A binary file opens as the processor its bytes name.

The worker's own executable -- headers, code and data -- with its ELF magic
cleared, so no header describes it: identification must settle the
processor it was built for, and opening it as a binary file without a
processor must read it as that one and keep it as detected.  A Cortex-M
vector table names Thumb and where the code runs.  Exits 77 (skip) on a
host processor the model does not cover.
"""
from pathlib import Path
import json
import platform
import struct
import sys
import tempfile
from transport_test import Client

HOST = {"x86_64": "x86_64", "amd64": "x86_64", "aarch64": "aarch64",
        "arm64": "aarch64", "i386": "x86", "i686": "x86"}.get(platform.machine().lower())


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def binary_row(rows):
    return next(row for row in rows if row["loader"] == "binary")


def run(executable):
    if not HOST:
        print(f"SKIP: the ISA model does not cover {platform.machine()}")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-binary-isa-") as directory:
        dump = Path(directory) / "dump.bin"
        dump.write_bytes(bytes(4) + Path(executable).read_bytes()[4:4 << 20])
        client = Client(executable)
        try:
            rows = ok(client, "identify", {"path": str(dump)})["rows"]
            assert len(rows) == 1, rows
            row = binary_row(rows)
            assert (row["status"], row["detected"]) == ("settled", HOST), row
            assert row["guesses"][0]["processor"] == HOST, row
            assert row["guesses"][0]["share"] >= 0.9, row
            opened = ok(client, "open", {"path": str(dump), "loader": "binary"})
            load = opened["load_options"]
            assert (load["processor"], load["processor_source"]) == (HOST, "detected"), load
            assert opened["architecture"] == HOST, opened
        finally:
            client.close()
        # The kept choice reopens the file the same way.
        kept = json.loads(Path(str(dump) + ".neverd/load.json").read_text())
        assert (kept["processor"], kept["processor_source"]) == (HOST, "detected"), kept

        # Stack in SRAM, Thumb handlers, reserved words zero, then bx lr.
        table = [0x20005000, 0x08000041, 0x08000045, 0x08000045, 0, 0, 0,
                 0, 0, 0, 0, 0x08000045]
        image = bytearray(b"\xff" * 0x4000)
        image[:len(table) * 4] = struct.pack(f"<{len(table)}I", *table)
        image[0x40:0x42] = image[0x44:0x46] = b"\x70\x47"
        firmware = Path(directory) / "stm32.bin"
        firmware.write_bytes(bytes(image))
        client = Client(executable)
        try:
            row = binary_row(ok(client, "identify", {"path": str(firmware)})["rows"])
            assert row["detected"] == "thumb", row
            assert (int(row["fingerprint"]["entry"], 16), int(row["fingerprint"]["base"], 16)) == \
                (0x08000040, 0x08000000), row
        finally:
            client.close()
    print(f"binary isa: the bytes name {HOST}, and a Cortex-M vector table names Thumb")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
