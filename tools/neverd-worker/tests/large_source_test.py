#!/usr/bin/env python3
"""Oversized source retains every page, mapping and checked edit on disk."""
from pathlib import Path
import sys
import tempfile

from transport_test import BASE, Client


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-large-source-") as directory:
        binary = Path(directory) / "source-cache-spilled.bin"
        binary.write_bytes(b"independent large source fixture")
        with Client(executable) as client:
            def ok(operation, payload):
                response = client.call(operation, payload, timeout=120)
                assert response["status"] == "ok", response
                return response["payload"]

            def source(offset=0, address=BASE):
                return ok("decompile", {"address": address, "representation": "source",
                                        "source_options": {"symbolic": True},
                                        "offset": offset, "limit": 4})

            ok("open", {"path": str(binary), "analysis": False})
            first = source()
            assert first["total_lines"] == 120000, first
            last = source(119996)
            assert last["complete"] and last["byte_offset"] > 32 * 1024 * 1024
            for index, row in enumerate(last["rows"], 119996):
                assert row["line"] == index
                assert row["addresses"] == [hex(int(BASE, 16) + index * 4)]
            assert first["fixture_pages"] == last["fixture_pages"]
            assert source() == first
            # A second giant document may replace the active engine pipeline.
            # Both retained documents answer without another engine page call.
            source(address=hex(int(BASE, 16) + 16))
            assert source(119996) == last
            variable = next(name for name in first["code_names"]
                            if name.get("kind") == "local" and name.get("original") == "value")
            ok("code_edit", {"address": BASE, "representation": "source",
                              "source_options": {"symbolic": True}, "kind": "name",
                              "original": "value", "identity": variable["identity"],
                              "name": "renamed_value"})
            renamed = source()
            assert "renamed_value" in renamed["text"]
            assert renamed["fixture_pages"] == first["fixture_pages"]
            # Checked editing must read the selected row from its disk chunk.
            ok("code_edit", {"address": BASE, "representation": "source",
                              "source_options": {"symbolic": True},
                              "kind": "comment", "line": 119999,
                              "anchor": last["rows"][-1]["code_anchor"],
                              "text": "large source tail comment"})
            changed = source(119996)
            assert "large source tail comment" in changed["text"], changed
            assert changed["fixture_pages"] > first["fixture_pages"]
            assert source(119996) == changed
            print("120000-line source: complete pages, exact tail anchors, cached switches and tail edit passed")


if __name__ == "__main__":
    run(sys.argv[1])
