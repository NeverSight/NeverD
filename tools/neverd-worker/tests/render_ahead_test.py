#!/usr/bin/env python3
"""A replica's idle-time renders (analysis_restore's render_ahead).

Once its summaries and read-ahead are done the replica renders each function's
engine pages, smallest first, and a first visit builds its document from them
without asking the engine, as the mock's page and preparation counters show.
The visit reads as one that asked the engine does, a rename keeps the renders,
and whatever forgets kept documents forgets them.  The job leaves a reader's
requests and an expensive prepared function alone.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import time

from call_summaries_test import prefill
from local_name_cache_test import request
from source_cache_test import A, B, C, ok, source
from transport_test import BASE, Client


def render_ahead(client, accept, timeout=60):
    deadline = time.monotonic() + timeout
    while True:
        message = client.next(lambda m: m.get("type") == "heartbeat",
                              timeout=max(0.1, deadline - time.monotonic()))
        state = (message.get("background") or {}).get("render_ahead")
        if accept(state):
            return state


def latest(client, timeout=30):
    """The job's state in a heartbeat sent after this call began."""
    heartbeat = lambda m: m.get("type") == "heartbeat"
    try:
        while True:
            client.next(heartbeat, timeout=0.02)
    except TimeoutError:
        pass
    message = client.next(heartbeat, timeout=timeout)
    return (message.get("background") or {}).get("render_ahead")


def queued(client):
    """The job's states in the heartbeats received so far, oldest first."""
    states = []
    try:
        while True:
            message = client.next(lambda m: m.get("type") == "heartbeat", timeout=0.02)
            states.append((message.get("background") or {}).get("render_ahead"))
    except TimeoutError:
        return states


def done(state):
    return state is not None and state["state"] == "done"


def replica(clients, executable, owner, **extra):
    snapshot = ok(owner, "analysis_snapshot")
    snapshot.update(extra)
    client = clients.enter_context(Client(executable))
    ok(client, "analysis_restore", snapshot)
    return client


def unfixtured(view):
    """A view without the mock's counters, which say which call answered."""
    return {key: value for key, value in view.items()
            if not key.startswith("fixture_")}


def open_owner(clients, executable, binary):
    binary.write_bytes(b"render ahead fixture")
    owner = clients.enter_context(Client(executable))
    ok(owner, "open", {"path": str(binary), "analysis": False, "warm": False})
    return owner


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-render-ahead-") as directory, ExitStack() as clients:
        root = Path(directory)
        owner = open_owner(clients, executable, root / "summaries-ready-source-cache.bin")

        # Nothing asked for it: no state, and a visit asks the engine.
        plain = replica(clients, executable, owner, call_summaries=True)
        prefill(plain, lambda state: state == "done")
        assert render_ahead(plain, lambda state: True) is None
        answered = {address: source(plain, address) for address in (A, B, C)}

        ahead = replica(clients, executable, owner, call_summaries=True, render_ahead=True)
        finished = render_ahead(ahead, done)
        total = finished["total"]
        assert total > 3, finished
        assert finished["rendered"] == total and not finished["failed"], finished
        assert 0 < finished["bytes"] < finished["raw_bytes"], finished
        # Every engine page and preparation so far was the job's, one a function.
        for address in (A, B, C):
            view = source(ahead, address)
            assert view["fixture_pages"] <= total, view
            assert view["fixture_prepares"] <= total, view
            # Whole-payload identity with the visit that asked the engine.
            assert unfixtured(view) == unfixtured(answered[address]), (view, answered[address])
            again = source(ahead, address)
            assert again["fixture_pages"] == view["fixture_pages"]
            assert unfixtured(again) == unfixtured(view)
        # The next page the engine writes is the job's total plus one: those
        # visits asked it for none, and a view the job did not render does.
        other = source(ahead, A, representation="c")
        assert other["fixture_pages"] == total + 1, other
        assert render_ahead(ahead, lambda state: state and state["hits"] == 3)["rendered"] == total

        # A rename of a local redecorates a function nobody has visited from
        # its kept pages, and leaves them and the job alone.
        unvisited = hex(int(BASE, 16) + 48)
        edit = request(source(owner, unvisited), "renamed_value") | {"address": unvisited}
        assert ok(owner, "code_edit", edit)["local_names_only"]
        current = source(owner, unvisited)
        assert ok(ahead, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        renamed = source(ahead, unvisited)
        assert "renamed_value = 1" in renamed["text"], renamed
        assert renamed["text"] == current["text"], (renamed, current)
        assert renamed["fixture_pages"] <= total, renamed
        state = render_ahead(ahead, lambda state: state and state["hits"] == 4)
        assert done(state) and state["rendered"] == total, state
        # Nothing the engine prepared changed, so nothing starts over: long
        # enough for the summaries to be solved again and drop these renders.
        time.sleep(3)
        state = latest(ahead)
        assert done(state) and state["rendered"] == total, state
        fifth = hex(int(BASE, 16) + 64)
        assert source(ahead, fifth)["fixture_pages"] <= total

        # What forgets kept documents forgets the renders, and the job runs
        # again; while a reader keeps asking, it waits for them to pause.
        ok(ahead, "prepare_call_summaries")
        last = time.monotonic()
        deadline = last + 9
        slowest = 0.0
        seen = []
        while time.monotonic() < deadline:
            source(ahead, A)
            now = time.monotonic()
            slowest = max(slowest, now - last)
            last = now
            seen += queued(ahead)
            # Past the worker's idle grace, so idle work runs, and short of
            # the job's wait for a pause: the pace of a reader going through
            # functions, whom it must never hold up.
            time.sleep(1.5)
        # Only a pause this machine imposed on the requests lets the job run,
        # or even list what it will render.  A heartbeat can still carry the
        # state from before the restart.
        if slowest < 1.9:
            counts = [(state["rendered"], state["total"]) for state in seen if state]
            assert (0, 0) in counts, (seen, slowest)
            assert not any(map(any, counts[counts.index((0, 0)):])), (seen, slowest)
        again = render_ahead(ahead, done)
        assert again["rendered"] >= total - 1, again
        rendered = source(ahead, B)
        assert rendered["fixture_pages"] > total + 1, rendered

        # An expensive function a request prepared stays prepared while the
        # reader may yet ask for its graph, once the wait for a pause is over.
        slow_owner = open_owner(clients, executable,
                                root / "summaries-ready-slow-prepare-source-cache.bin")
        slow = replica(clients, executable, slow_owner, call_summaries=True, render_ahead=True)
        prefill(slow, lambda state: state == "done")
        # The check needs both visits to prepare, before the job runs again.
        for _ in range(5):
            ok(slow, "prepare_call_summaries")
            source(slow, B)
            prepared = source(slow, C)
            before = latest(slow)
            if before["rendered"] == 0:
                break
        else:
            raise AssertionError("the machine never let two requests pass within a second")
        time.sleep(3.5)
        after = latest(slow)
        assert after["rendered"] == 0, (before, after)
        assert after["state"] == "pending", after
        assert source(slow, C, representation="c")["fixture_prepares"] == prepared["fixture_prepares"]

        # The smallest function first.
        sized_owner = open_owner(clients, executable,
                                 root / "summaries-ready-render-sizes-source-cache.bin")
        sized = replica(clients, executable, sized_owner, call_summaries=True, render_ahead=True)
        render_ahead(sized, done)
        rows = []
        while (page := ok(sized, "functions", {"offset": len(rows), "limit": 512}))["items"]:
            rows += page["items"]
        # The engine knows no size for the last function but one; the listing's
        # reaches the next function.
        unsized = rows[-2]
        order = sorted((row for row in rows if row is not unsized),
                       key=lambda row: (row["size"], int(row["address"], 16)))
        assert len({row["size"] for row in order}) > 1, order
        pages = [source(sized, row["address"])["fixture_pages"] for row in order[:12]]
        assert pages == sorted(pages), (pages, order[:12])
        # A function too large to render without holding up a request is left
        # to the visit, which asks the engine, and so is one that could be any
        # size.
        state = latest(sized)
        assert state["total"] == len(rows) - 2, (state, len(rows))
        assert order[-1]["size"] == 1024 * 1024, order[-1]
        huge = source(sized, order[-1]["address"])
        assert huge["fixture_pages"] > state["rendered"], (huge, state)
        unknown = source(sized, unsized["address"])
        assert unknown["fixture_pages"] > huge["fixture_pages"], (unknown, huge)

        # A larger function waits for a longer pause, in proportion to its size
        # and not in steps of it: 2 s and 900/1024 of 5 s here.
        late_owner = open_owner(clients, executable,
                                root / "summaries-ready-render-late-source-cache.bin")
        late = replica(clients, executable, late_owner, call_summaries=True, render_ahead=True)
        prefill(late, lambda state: state == "done")
        ok(late, "prepare_call_summaries")
        asked = time.monotonic()
        state = render_ahead(late, lambda state: state and state["rendered"] == state["total"] - 1
                             and state["state"] == "pending")
        wait = asked + 5 - time.monotonic()
        # Only a machine too slow to reach the others in time says nothing here.
        if wait > 0:
            time.sleep(wait)
            state = latest(late)
            if time.monotonic() - asked < 6:
                assert state["state"] == "pending", state
                assert state["rendered"] == state["total"] - 1, state
        render_ahead(late, done)


if __name__ == "__main__":
    run(sys.argv[1])
