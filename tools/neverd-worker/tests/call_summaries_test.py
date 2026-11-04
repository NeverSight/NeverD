#!/usr/bin/env python3
"""A replica's idle-time call-summary solve (analysis_restore's call_summaries)
and the explicit prepare_call_summaries request.

The replica warms every function a page at a time, solves, and reports each
state in its heartbeats; an owner or a replica restored without the option
reports none.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import time

from transport_test import Client


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def summaries(client, accept, timeout=30):
    deadline = time.monotonic() + timeout
    seen = []
    while True:
        message = client.next(lambda m: m.get("type") == "heartbeat",
                              timeout=max(0.1, deadline - time.monotonic()))
        state = ((message.get("background") or {}).get("call_summaries") or {}).get("state")
        seen.append(state)
        if accept(state):
            return state, seen


def prefill(client, accept, timeout=30):
    deadline = time.monotonic() + timeout
    seen = []
    while True:
        message = client.next(lambda m: m.get("type") == "heartbeat",
                              timeout=max(0.1, deadline - time.monotonic()))
        summaries = (message.get("background") or {}).get("call_summaries") or {}
        state = (summaries.get("prefill") or {}).get("state")
        seen.append((summaries.get("state"), state))
        if accept(state):
            return state, seen


def replica(clients, executable, owner, **extra):
    snapshot = ok(owner, "analysis_snapshot")
    snapshot.update(extra)
    client = clients.enter_context(Client(executable))
    ok(client, "analysis_restore", snapshot)
    return client


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-summaries-") as directory, ExitStack() as clients:
        root = Path(directory)
        for name in ("summaries-ready.bin", "summaries-discover.bin",
                     "summaries-unavailable.bin", "summaries-fail.bin"):
            (root / name).write_bytes(b"\0" * 64)

        owner = clients.enter_context(Client(executable))
        ok(owner, "open", {"path": str(root / "summaries-ready.bin"), "analysis": False})
        # Nothing asked for them: no state at all.
        state, _ = summaries(owner, lambda s: True)
        assert state is None, state
        assert summaries(replica(clients, executable, owner), lambda s: True)[0] is None

        # Restored with the option, the replica warms in idle time, then solves.
        solving = replica(clients, executable, owner, call_summaries=True)
        state, seen = summaries(solving, lambda s: s not in ("pending", "preparing", "solving"))
        assert state == "ready", seen
        assert "preparing" in seen, seen
        # Then, while idle, it reads ahead the callees its decompiles read,
        # only once the summaries are ready.
        state, seen = prefill(solving, lambda s: s in ("done", "unavailable"))
        assert state == "done", seen
        assert all(s == "ready" for s, p in seen if p is not None), seen
        # Its decompiles keep working as before, and an explicit request
        # answers at once.
        assert ok(solving, "prepare_call_summaries") == {"available": True, "ready": True}
        bad = solving.call("prepare_call_summaries", {"now": True})
        assert bad["status"] == "error" and bad["error"]["code"] == "invalid_request", bad

        # The replica discovers the image's functions before it solves their
        # summaries, which a later discovery would retire: summaries-discover
        # warms only once its discovery added a function, which the replica
        # then lists and its owner, which does not analyze, does not.
        ok(owner, "open", {"path": str(root / "summaries-discover.bin"), "analysis": False})
        listed = ok(owner, "functions", {"offset": 0, "limit": 64})["total"]
        discovering = replica(clients, executable, owner, call_summaries=True)
        state, seen = summaries(discovering, lambda s: s not in ("pending", "preparing", "solving"))
        assert state == "ready", seen
        assert ok(discovering, "functions", {"offset": 0, "limit": 64})["total"] == listed + 1

        # An image without them ends unavailable at its first page, as one
        # that is not x86-64; a failed solve too, with the engine's message.
        for name, message in (("summaries-unavailable.bin", None),
                              ("summaries-fail.bin", "fixture")):
            ok(owner, "open", {"path": str(root / name), "analysis": False})
            other = replica(clients, executable, owner, call_summaries=True)
            state, seen = summaries(other, lambda s: s not in ("pending", "preparing", "solving"))
            assert state == "unavailable", (name, seen)
            reply = other.call("prepare_call_summaries")
            if message:
                assert reply["status"] == "error" and message in reply["error"]["message"], reply
            else:
                assert reply["status"] == "ok" and reply["payload"] == {"available": False,
                                                                         "ready": False}, reply
        print("call summaries: off by default, replica idle solve after discovery, callee "
              "read-ahead, explicit request, unavailable, failure")


if __name__ == "__main__":
    run(sys.argv[1])
