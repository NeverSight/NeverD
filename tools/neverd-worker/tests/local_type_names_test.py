#!/usr/bin/env python3
"""Idle time reads what a first declaration of local types checks its names
against (neverd_local_types_prepare), once the session has read its types.

A view reads the program's local types; the idle step after it reads their
names, which metadata counts.  A declaration then reads as it does in a worker
that read nothing ahead.
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

TARGET = 0x140001040
DECLARED = "struct prepared_names { int value; };"
# What the engine says of the declaration; the projects differ by path.
ENGINE_FIELDS = ("types", "prototypes", "problems", "unread", "decompilation")


def prepared(client):
    return ok(client, "metadata")["local_type_names_prepared"]


def opened(clients, executable, path):
    client = clients.enter_context(Client(executable))
    ok(client, "open", {"path": str(path), "analysis": False, "warm": False,
                        "write_behind": True})
    return client


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-local-type-names-") as directory, \
            ExitStack() as clients:
        root = Path(directory)
        replies = []
        for name, wait in (("ahead.dll", True), ("plain.dll", False)):
            path = root / name
            path.write_bytes(functions_fixture())
            client = opened(clients, executable, path)
            page(client, TARGET)
            if wait:
                # The idle step after the view reads the names, once requests
                # pause for longer than the worker's grace period (150 ms).
                deadline = time.monotonic() + 30
                while prepared(client) == 0:
                    assert time.monotonic() < deadline, "names never read ahead"
                    time.sleep(0.3)
            reply = ok(client, "local_types_declare",
                       {"language": "c", "source": DECLARED})
            replies.append({key: reply.get(key) for key in ENGINE_FIELDS})
        assert replies[0] == replies[1], replies
        assert replies[0]["types"] == ["prepared_names"], replies[0]


if __name__ == "__main__":
    run(sys.argv[1])
    print("local type names test passed")
