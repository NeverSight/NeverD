#!/usr/bin/env python3
"""Idle-time function preparation (open's warm option) and its restarts.

warm-slow.bin makes each preparation step take a while, so heartbeats show a
pass running, and a change to the project starting it over.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import time

from transport_test import BASE, Client


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def warm_state(client, accept, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        message = client.next(lambda m: m.get("type") == "heartbeat",
                              timeout=max(0.1, deadline - time.monotonic()))
        warm = (message.get("background") or {}).get("warm")
        if warm is not None and accept(warm):
            return warm


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-warm-") as directory, ExitStack() as clients:
        root = Path(directory)
        for name in ("plain.bin", "warm-slow.bin", "warm-full.bin", "warm-unavailable.bin"):
            (root / name).write_bytes(b"\0" * 64)

        client = Client(executable)
        clients.callback(client.close)
        ok(client, "open", {"path": str(root / "plain.bin")})
        assert warm_state(client, lambda w: True)["state"] == "off"

        ok(client, "open", {"path": str(root / "warm-slow.bin"), "warm": True})
        running = warm_state(client, lambda w: w["state"] == "running")
        done = warm_state(client, lambda w: w["state"] == "complete")
        assert 0 < running["prepared"] < done["prepared"], (running, done)
        # A change to the project may drop what the engine prepared, so the
        # preparation starts over from the first function.
        ok(client, "annotation_set", {"address": BASE, "text": "changed"})
        again = warm_state(client, lambda w: w["state"] in ("pending", "running"))
        assert again["prepared"] < done["prepared"], (again, done)
        assert warm_state(client, lambda w: w["state"] == "complete") == done

        ok(client, "open", {"path": str(root / "warm-full.bin"), "warm": True})
        full = warm_state(client, lambda w: w["state"] in ("complete", "full"))
        assert full == {"state": "full", "prepared": 8}, full

        ok(client, "open", {"path": str(root / "warm-unavailable.bin"), "warm": True})
        unavailable = warm_state(client, lambda w: w["state"] not in ("pending", "running"))
        assert unavailable["state"] == "unavailable", unavailable
        print("warm functions: off by default, running and complete, restart on change, full, unavailable")


if __name__ == "__main__":
    run(sys.argv[1])
