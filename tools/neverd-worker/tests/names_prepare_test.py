#!/usr/bin/env python3
"""Idle time reads what a rename checks a name against of the image's symbols
(neverd_names_prepare).

The idle step after a view reads them, which metadata counts.  Renames then
answer as they do in a worker that read nothing ahead.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import time

from kept_reads_test import page
from pe_listing_facts_test import fixture as functions_fixture
from source_variable_types_test import ok
from transport_test import Client

BASE = 0x140000000
TARGET = BASE + 0x1040
OTHERS = (BASE + 0x1100, BASE + 0x1140)


def prepared(client):
    return ok(client, "metadata")["names_prepared"]


def answer(reply):
    error = reply.get("error") or {}
    return reply["status"], error.get("code"), error.get("message")


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-names-prepare-") as directory, \
            ExitStack() as clients:
        root = Path(directory)
        replies = []
        for name, wait in (("ahead.dll", True), ("plain.dll", False)):
            path = root / name
            path.write_bytes(functions_fixture())
            client = clients.enter_context(Client(executable))
            ok(client, "open", {"path": str(path), "analysis": False,
                                "warm": False, "write_behind": True})
            page(client, TARGET)
            if wait:
                # The idle step after the view reads them, once requests pause
                # for longer than the worker's grace period (150 ms).
                deadline = time.monotonic() + 30
                while prepared(client) == 0:
                    assert time.monotonic() < deadline, "names never read ahead"
                    time.sleep(0.3)
            # A new name, the same name for another address, an automatic
            # name, and the first name again once its address took another.
            steps = [(TARGET, "renamed_ahead"), (OTHERS[0], "renamed_ahead"),
                     (OTHERS[1], "sub_1234"), (TARGET, "renamed_again"),
                     (OTHERS[0], "renamed_ahead")]
            replies.append([answer(client.call("rename", {
                "address": hex(at), "name": new})) for at, new in steps])
        assert replies[0] == replies[1], replies
        assert [status for status, _, _ in replies[0]] == \
            ["ok", "error", "error", "ok", "ok"], replies[0]


if __name__ == "__main__":
    run(sys.argv[1])
    print("names prepare test passed")
