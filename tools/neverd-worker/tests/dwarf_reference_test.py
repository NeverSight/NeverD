#!/usr/bin/env python3
"""The DWARF of an ELF image, found before it opens, and both answers."""
from contextlib import ExitStack
from pathlib import Path
import shutil
import sys
import tempfile

from transport_test import Client


FIXTURES = Path(__file__).resolve().parents[3] / "unittests/safety/fixtures/binaries"


def ok(client, operation, payload=None):
    result = client.call(operation, payload or {})
    assert result["status"] == "ok", result
    return result["payload"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-dwarf-prompt-") as directory, ExitStack() as clients:
        root = Path(directory).resolve()
        binary = root / "input"
        shutil.copyfile(FIXTURES / "safety_cases_elf_x64", binary)

        owner = clients.enter_context(Client(executable))
        assert "dwarf_reference" in owner.hello["capabilities"]
        reference = ok(owner, "dwarf_reference", {"path": str(binary)})["dwarf"]
        assert reference == {"path": str(binary), "location": "input"}, reference
        assert ok(owner, "dwarf_reference", {"path": str(FIXTURES / "safety_cases_pe_x64.exe")})["dwarf"] is None
        missing = owner.call("dwarf_reference", {"path": str(root / "missing")})
        assert missing["status"] != "ok", missing
        # An answer other than none is refused rather than guessed at.
        refused = owner.call("open", {"path": str(binary), "analysis": False, "dwarf": {"mode": "verified"}})
        assert refused["status"] != "ok", refused

        # No: the open reads no DWARF, and neither does its replica.
        opened = ok(owner, "open", {"path": str(binary), "analysis": False, "dwarf": {"mode": "none"}})
        assert opened["dwarf"] == {"mode": "none"}, opened["dwarf"]
        assert opened["debug_info"]["kind"] == "none", opened["debug_info"]
        snapshot = ok(owner, "analysis_snapshot")
        assert snapshot["dwarf"] == {"mode": "none"}
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", snapshot)
        restored = ok(replica, "metadata")
        assert restored["dwarf"] == {"mode": "none"} and restored["debug_info"]["kind"] == "none", restored

        # Yes: the open reads the DWARF the reference named.
        reader = clients.enter_context(Client(executable))
        opened = ok(reader, "open", {"path": str(binary), "read_only": True, "analysis": False})
        assert opened["dwarf"] is None, opened["dwarf"]
        assert opened["debug_info"]["kind"] == "dwarf", opened["debug_info"]
        assert opened["debug_info"]["path"] == reference["path"], opened["debug_info"]


if __name__ == "__main__":
    run(sys.argv[1])
