#!/usr/bin/env python3
"""Completed source documents survive function switches, but never stale edits.

The mock reports actual preparation/page calls, so these checks do not rely on
timing or a client cache. Its graph and IR entry points also reject reading a
different function's restricted pipeline.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

from transport_test import BASE, Client


A = BASE
B = hex(int(BASE, 16) + 16)
C = hex(int(BASE, 16) + 32)


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def source(client, address=A, *, representation="source", offset=0, limit=4):
    return ok(client, "decompile", {"address": address,
                                    "representation": representation,
                                    "offset": offset, "limit": limit})


def reused(actual, original):
    assert actual["fixture_prepares"] == original["fixture_prepares"], actual
    assert actual["fixture_pages"] == original["fixture_pages"], actual
    assert actual["text"] == original["text"], (actual, original)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-source-cache-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / "source-cache.bin"
        binary.write_bytes(b"source cache fixture")
        client = clients.enter_context(Client(executable))
        ok(client, "open", {"path": str(binary), "analysis": False})
        first = source(client)
        assert first["fixture_prepares"] == 1 and first["fixture_pages"] == 1
        assert first["fixture_renders"] == 0, first
        second = source(client, B)
        assert second["fixture_prepares"] == 2
        reused(source(client), first)
        late = source(client, offset=2, limit=2)
        assert late["fixture_prepares"] == 1 and late["fixture_pages"] == 1
        assert late["text"] == "  return value;\n}\n"
        assert late["rows"][0]["addresses"] == [hex(int(A, 16) + 2)]

        # A source cache hit must not label the mutable B pipeline as A.
        # The mock refuses this graph call unless A is really prepared again.
        ok(client, "cfg", {"address": A})
        third = source(client, C)
        assert third["fixture_prepares"] == 4, third
        reused(source(client), first)
        low = source(client, representation="low")
        assert low["fixture_prepares"] == 5, low
        reused(source(client), first)
        explicit_c = source(client, representation="c")
        assert explicit_c["representation"] == "c"
        assert explicit_c["fixture_renders"] == 0, explicit_c
        assert explicit_c["fixture_pages"] > first["fixture_pages"]
        reused(source(client), first)

        # Image comments and renames invalidate every previously kept source,
        # including A after another function became the active pipeline.
        source(client, B)
        ok(client, "annotation_set", {"address": hex(int(A, 16) + 2),
                                       "text": "current source comment"})
        commented = source(client)
        assert commented["fixture_pages"] > first["fixture_pages"]
        assert "current source comment" in commented["text"], commented
        source(client, B)
        reused(source(client), commented)
        ok(client, "save")
        ok(client, "rename", {"address": A, "name": "renamed_source"})
        renamed = source(client)
        assert "int renamed_source(void)" in renamed["text"], renamed
        assert "current source comment" in renamed["text"]
        source(client, B)
        reused(source(client), renamed)

        # Editing the cached A document after B validates A's own target.
        ok(client, "code_edit", {"address": A, "representation": "source",
                                  "kind": "comment", "line": 0,
                                  "anchor": renamed["rows"][0]["code_anchor"],
                                  "text": "local source note"})
        edited = source(client)
        assert "local source note" in edited["text"], edited
        source(client, B)
        reused(source(client), edited)
        ok(client, "undo")
        undone = source(client)
        assert "local source note" not in undone["text"]
        source(client, B)
        reused(source(client), undone)

        # A fresh replica rebuilds its own documents from the owner's current
        # snapshot, then keeps them across function switches without writes.
        snapshot = ok(client, "analysis_snapshot")
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", snapshot)
        restored = source(replica)
        assert restored["fixture_prepares"] == 1
        assert restored["text"] == undone["text"]
        source(replica, B)
        reused(source(replica), restored)

        # Opening another project with the same addresses retires old text.
        other_dir = root / "other"
        other_dir.mkdir()
        other = other_dir / binary.name
        other.write_bytes(b"another source cache fixture")
        ok(client, "open", {"path": str(other), "analysis": False})
        clean = source(client)
        assert clean["fixture_prepares"] == 1
        assert "renamed_source" not in clean["text"]
        assert "current source comment" not in clean["text"]

        # Count-bounded LRU: touch A after filling eight slots, then introduce
        # the ninth. A survives; the older B must be generated again.
        entries = [hex(int(A, 16) + index * 16) for index in range(9)]
        previous_b = source(client, entries[1])
        for entry in entries[2:8]:
            source(client, entry)
        reused(source(client), clean)
        source(client, entries[8])
        reused(source(client), clean)
        rebuilt_b = source(client, B)
        assert rebuilt_b["fixture_prepares"] > previous_b["fixture_prepares"]
        assert rebuilt_b["fixture_pages"] > previous_b["fixture_pages"]

        # The byte budget applies before the eight-document count limit.
        # Each document includes thousands of row objects as well as text.
        big = root / "source-cache-budget.bin"
        big.write_bytes(b"source cache byte budget fixture")
        bounded = clients.enter_context(Client(executable))
        ok(bounded, "open", {"path": str(big), "analysis": False})
        large = source(bounded, limit=1)
        reused(source(bounded, limit=1), large)
        for entry in entries[1:4]:
            source(bounded, entry, limit=1)
        rebuilt = source(bounded, limit=1)
        assert rebuilt["fixture_prepares"] > large["fixture_prepares"], rebuilt
        assert rebuilt["fixture_pages"] > large["fixture_pages"], rebuilt
        assert rebuilt["text"] == large["text"]

        # Switching pipelines can fail after discarding the preceding IR.
        # A graph request for A must really prepare A again; B then retries
        # successfully instead of being remembered as already prepared.
        fail_binary = root / "source-cache-prepare-fail.bin"
        fail_binary.write_bytes(b"prepare failure fixture")
        retry = clients.enter_context(Client(executable))
        ok(retry, "open", {"path": str(fail_binary), "analysis": False})
        prepared_a = source(retry)
        assert prepared_a["fixture_prepares"] == 1
        failure = retry.call("decompile", {"address": B,
                                            "representation": "source"})
        assert failure["status"] == "error", failure
        assert "fixture preparation failed" in str(failure), failure
        ok(retry, "cfg", {"address": A})
        prepared_b = source(retry, B)
        assert prepared_b["fixture_prepares"] == 4, prepared_b
        assert prepared_b["fixture_renders"] == 0, prepared_b
        reused(source(retry), prepared_a)

        # VM preparation advances the worker revision and rebuilds aliases.
        # Cache the finished document under that new context on its first
        # request, so even the first repeat does not query the backend again.
        for arch in ("evm", "sbf"):
            vm_binary = root / f"source-cache-{arch}.bin"
            vm_binary.write_bytes(b"whole-program source cache fixture")
            vm = clients.enter_context(Client(executable))
            ok(vm, "open", {"path": str(vm_binary), "analysis": False})
            vm_first = source(vm)
            assert vm_first["fixture_prepares"] == 1, vm_first
            assert vm_first["fixture_pages"] == 1, vm_first
            vm_repeat = source(vm)
            reused(vm_repeat, vm_first)
            assert vm_repeat["revision"] == vm_first["revision"]
            vm_second = source(vm, B)
            assert vm_second["fixture_prepares"] == 1, vm_second
            assert vm_second["fixture_pages"] == 2, vm_second
            reused(source(vm), vm_first)


if __name__ == "__main__":
    run(sys.argv[1])
    print("Source document reuse, edits, pipeline ownership and LRU budgets passed")
