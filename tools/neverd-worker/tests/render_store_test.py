#!/usr/bin/env python3
"""Renders kept between sessions (analysis_restore's render_store).

A session reads what an earlier one over the same inputs rendered from its first
request on, without asking the engine, and with a whole store solves no summaries
until a request asks for more than it answers.  Local names keep a store; any other
change to what the engine reads, and another build, do not.  Whatever a session cut
short or a disk damaged is a render not kept.
"""
from contextlib import ExitStack
from pathlib import Path
import os
import shutil
import sys
import tempfile
import time

from call_summaries_test import prefill
from local_name_cache_test import request
from render_ahead_test import done, latest, render_ahead, unfixtured
from source_cache_test import A, B, C, ok, source
from transport_test import BASE, Client

NAME = "summaries-ready-render-store-source-cache.bin"
COMPLETE_RECORD = 32 + len("complete")


def session(clients, executable, binary, store="write", **extra):
    owner = clients.enter_context(Client(executable))
    ok(owner, "open", {"path": str(binary), "analysis": False, "warm": False})
    snapshot = ok(owner, "analysis_snapshot")
    snapshot.update(call_summaries=True, render_ahead=True, **extra)
    if store:
        snapshot["render_store"] = store
    replica = clients.enter_context(Client(executable))
    ok(replica, "analysis_restore", snapshot)
    return owner, replica


def summaries(client, accept, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        message = client.next(lambda m: m.get("type") == "heartbeat",
                              timeout=max(0.1, deadline - time.monotonic()))
        state = ((message.get("background") or {}).get("call_summaries") or {}).get("state")
        if accept(state):
            return state


def files(binary):
    return sorted((Path(str(binary) + ".neverd") / "local" / "render-ahead").rglob("*.ndrs"))


def end(*clients):
    for client in clients:
        client.close()


def asks_engine_first(client):
    """The first page this session's engine writes is for this view: everything
    so far came from the store."""
    view = source(client, A, representation="c")
    assert view["fixture_pages"] == 1, view
    assert view["fixture_prepares"] == 1, view


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-render-store-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / NAME
        binary.write_bytes(b"render store fixture")

        # A session renders, and keeps what it renders on disk.
        owner, first = session(clients, executable, binary)
        state = render_ahead(first, done)
        total = state["total"]
        assert total > 3 and state["rendered"] == total and not state["failed"], state
        # Each function and the mark of a job that reached them all.
        assert state["kept"] == total + 1, state
        assert len(files(binary)) == 1
        fresh = {address: unfixtured(source(first, address)) for address in (A, B, C)}
        assert render_ahead(first, lambda s: s["hits"] == 3)
        end(first, owner)

        # The next reads them from its first request, and solves nothing.
        owner, second = session(clients, executable, binary)
        for address in (A, B, C):
            assert unfixtured(source(second, address)) == fresh[address]
        state = render_ahead(second, lambda s: s and s["hits"] == 3)
        assert done(state) and not state["rendered"] and state["kept"] == total + 1, state
        assert summaries(second, lambda s: s == "deferred") == "deferred"
        # A graph reads no summary.
        ok(second, "cfg", {"address": A})
        time.sleep(1.5)
        assert summaries(second, lambda s: True) == "deferred"
        asks_engine_first(second)
        # This view reads the engine, whose summaries come now.  A store that
        # answers every function is not rendered again for them.
        assert summaries(second, lambda s: s == "ready") == "ready"
        state = latest(second)
        assert done(state) and not state["rendered"], state
        assert len(files(binary)) == 1

        # A rename of a local redecorates: the store still reads.
        edit = request(source(owner), "renamed_value")
        assert ok(owner, "code_edit", edit)["local_names_only"]
        current = source(owner)
        end(second, owner)
        owner, renamed = session(clients, executable, binary)
        view = source(renamed, A)
        assert "renamed_value = 1" in view["text"], view
        assert view["text"] == current["text"], (view, current)
        assert unfixtured(source(renamed, B)) == fresh[B]
        asks_engine_first(renamed)
        end(renamed, owner)

        # A comment and a function's name take effect in a running replica with
        # its renders, and a later session keeps reading them.
        owner, running = session(clients, executable, binary)
        assert unfixtured(source(running, B)) == fresh[B]
        ok(owner, "rename", {"address": C, "name": "renamed_function"})
        anchor = source(owner, B)["rows"][0]["code_anchor"]
        ok(owner, "code_edit", {"address": B, "representation": "source", "kind": "comment",
                                "line": 0, "anchor": anchor, "text": "a running note"})
        ok(owner, "annotation_set", {"address": B, "text": "an image note"})
        ok(owner, "save")
        assert ok(running, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        view = source(running, B)
        assert "a running note" in view["text"], view
        # Other functions and the renamed one still read the store.
        source(running, A)
        renamed_view = source(running, C)
        assert render_ahead(running, lambda s: s and s["hits"] == 3)
        direct = source(owner, C)
        assert renamed_view["text"] == direct["text"], (renamed_view, direct)
        assert renamed_view["function_identity"] == direct["function_identity"], renamed_view
        assert renamed_view["function_identity"]["name"] == "renamed_function", renamed_view
        asks_engine_first(running)
        end(running, owner)
        owner, next_session = session(clients, executable, binary)
        assert "a running note" in source(next_session, B)["text"]
        # The replica's own revision counts the updates it took.
        kept = {**unfixtured(source(next_session, C)), "revision": renamed_view["revision"]}
        assert kept == unfixtured(renamed_view)
        assert summaries(next_session, lambda s: s == "deferred") == "deferred"
        asks_engine_first(next_session)
        end(next_session, owner)

        # What a function was called before a name was taken away is the engine's
        # to say, so its kept page does not answer.
        unnaming = root / ("unnaming-" + NAME)
        unnaming.write_bytes(b"render store fixture")
        owner = clients.enter_context(Client(executable))
        ok(owner, "open", {"path": str(unnaming), "analysis": False, "warm": False})
        ok(owner, "rename", {"address": C, "name": "named_for_now"})
        end(owner)
        owner, named = session(clients, executable, unnaming)
        state = render_ahead(named, done)
        kept_name = source(named, C)
        assert kept_name["function_identity"]["name_origin"] == "user", kept_name
        assert kept_name["fixture_pages"] <= state["total"], kept_name
        ok(owner, "undo")
        assert ok(named, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        original_name = source(named, C)
        direct = source(owner, C)
        assert original_name["function_identity"] == direct["function_identity"], original_name
        assert original_name["fixture_pages"] > state["total"], original_name
        assert original_name["text"] == direct["text"], (original_name, direct)
        # The other functions' renders stay.
        other = source(named, A)
        assert other["fixture_pages"] <= state["total"], other
        end(named, owner)

        # A session's job that started over keeps nothing further, and neither
        # reads nor changes the store from then on.
        cut = root / ("cut-" + NAME)
        cut.write_bytes(b"render store fixture")
        owner, over = session(clients, executable, cut)
        state = render_ahead(over, done)
        kept = state["kept"]
        size = files(cut)[0].stat().st_size
        ok(over, "prepare_call_summaries")
        render_ahead(over, lambda s: s and s["state"] == "pending" and not s["rendered"])
        state = render_ahead(over, done)
        assert state["rendered"] == total and state["kept"] == kept, state
        view = source(over, C)
        assert view["fixture_pages"] > total, view
        assert [path.stat().st_size for path in files(cut)] == [size]
        end(over, owner)

        # A session that ended part of the way through a record, past the mark
        # of a whole job: the next reads the rest at once, and renders only what
        # that leaves out.
        path = files(cut)[0]
        with path.open("r+b") as store:
            store.truncate(size - COMPLETE_RECORD - 10)
        owner, resumed = session(clients, executable, cut)
        view = source(resumed, A)
        assert render_ahead(resumed, lambda s: s and s["hits"] == 1)
        state = render_ahead(resumed, done)
        assert state["rendered"] == 1 and state["kept"] == total + 1, state
        assert len(files(cut)) == 2
        end(resumed, owner)

        # A damaged record is a render not kept: the visit reads the engine.
        damaged = root / ("damaged-" + NAME)
        damaged.write_bytes(b"render store fixture")
        owner, writer = session(clients, executable, damaged)
        state = render_ahead(writer, done)
        rows = []
        while (page := ok(writer, "functions", {"offset": len(rows), "limit": 512}))["items"]:
            rows += page["items"]
        whole = {row["address"]: unfixtured(source(writer, row["address"])) for row in rows[:16]}
        end(writer, owner)
        path = files(damaged)[0]
        data = bytearray(path.read_bytes())
        first_data = len(b"NDRS1\n") + 32 + int.from_bytes(data[10:14], "little") + 8
        data[first_data + 4] ^= 0x55
        path.write_bytes(data)
        owner, reader = session(clients, executable, damaged)
        assert [unfixtured(source(reader, address)) for address in whole] == list(whole.values())
        state = render_ahead(reader, lambda s: s and s["hits"] >= 15)
        assert state["hits"] == 15, state
        end(reader, owner)

        # A function that fails to render is kept as that: a later session does
        # not try it, and its visit reports what the engine says.
        failing = root / ("render-fails-" + NAME)
        failing.write_bytes(b"render store fixture")
        owner, renderer = session(clients, executable, failing)
        state = render_ahead(renderer, done)
        assert state["failed"] == 1 and state["rendered"] == total - 1, state
        assert state["kept"] == total + 1, state
        end(renderer, owner)
        owner, visitor = session(clients, executable, failing)
        state = render_ahead(visitor, done)
        assert not state["rendered"] and not state["failed"], state
        assert summaries(visitor, lambda s: s == "deferred") == "deferred"
        reply = visitor.call("decompile", {"address": B, "representation": "source",
                                           "offset": 0, "limit": 4})
        assert reply["status"] == "error", reply
        assert "fixture render failure" in reply["error"]["message"], reply
        time.sleep(1.5)
        assert summaries(visitor, lambda s: True) == "deferred"
        end(visitor, owner)

        # A function visited before the job reaches it and before renders settle
        # is rendered too: its document answers this session alone.
        visited = root / ("slow-prefill-" + NAME)
        visited.write_bytes(b"render store fixture")
        owner, early = session(clients, executable, visited)
        heartbeat = lambda m: m.get("type") == "heartbeat"
        ready = False
        while not ready:
            # Requests hold the job off; a slow machine only renders it sooner.
            source(early, A)
            try:
                while True:
                    message = early.next(heartbeat, timeout=0.2)
                    summary = (message.get("background") or {}).get("call_summaries") or {}
                    ready = (summary.get("prefill") or {}).get("state") == "done"
            except TimeoutError:
                pass
        state = render_ahead(early, done)
        assert state["kept"] == total + 1, state
        end(early, owner)

        # A function the job leaves to its visit costs the next session nothing,
        # and that session reads it without starting the summaries.
        sized = root / ("render-sizes-" + NAME)
        sized.write_bytes(b"render store fixture")
        owner, visiting = session(clients, executable, sized)
        state = render_ahead(visiting, done)
        rows = []
        while (page := ok(visiting, "functions", {"offset": len(rows), "limit": 512}))["items"]:
            rows += page["items"]
        assert state["total"] == len(rows) - 2, state
        large = rows[-1]["address"]
        rendered = unfixtured(source(visiting, large))
        state = latest(visiting)
        assert state["kept"] == len(rows) - 2 + 2, state
        end(visiting, owner)
        owner, again = session(clients, executable, sized)
        assert unfixtured(source(again, large)) == rendered
        assert summaries(again, lambda s: s == "deferred") == "deferred"
        asks_engine_first(again)
        end(again, owner)

        # Reading alone: no file, for an instance that holds nothing of the
        # project, and renders in memory.
        reading = root / ("reading-" + NAME)
        reading.write_bytes(b"render store fixture")
        owner, memory = session(clients, executable, reading, store="read")
        state = render_ahead(memory, done)
        assert state["rendered"] == total and not state["kept"], state
        assert not files(reading)
        assert source(memory, A)["fixture_pages"] <= total
        assert render_ahead(memory, lambda s: s["hits"] == 1)
        end(memory, owner)

        # A session reads only what it was told to.
        owner, unasked = session(clients, executable, binary, store=None)
        state = render_ahead(unasked, lambda s: s is not None)
        assert not state["kept"], state
        assert summaries(unasked, lambda s: s not in (None, "pending", "deferred")) != "deferred"
        end(unasked, owner)

        # Malformed requests fail clearly.
        owner = clients.enter_context(Client(executable))
        ok(owner, "open", {"path": str(binary), "analysis": False, "warm": False})
        snapshot = ok(owner, "analysis_snapshot")
        bad = clients.enter_context(Client(executable))
        snapshot["render_store"] = "sometimes"
        assert bad.call("analysis_restore", snapshot)["status"] == "error"
        end(bad)

        # A change to what the engine reads starts a store of its own, whose
        # first render removes the one that no longer applies.
        ok(owner, "function_delete", {"address": C})
        end(owner)
        before = files(binary)
        owner, edited = session(clients, executable, binary)
        state = render_ahead(edited, done)
        assert state["rendered"] == state["total"] == total - 1, state
        assert state["kept"] == total, state
        after = files(binary)
        assert after[0].parent != before[0].parent and {p.parent for p in after} == {after[0].parent}, (before, after)

        # So does a new name of data, which pages spell, in a running replica.
        # Its renders go, and what it renders now goes to the new store.
        data = hex(int(BASE, 16) + 4)
        ok(owner, "rename", {"address": data, "name": "renamed_data"})
        assert ok(edited, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        state = render_ahead(edited, lambda s: s and s["state"] == "pending" and not s["rendered"] and not s["kept"])
        state = render_ahead(edited, done)
        assert state["rendered"] == state["total"] and state["kept"] == state["total"] + 1, state
        changed = files(binary)
        assert changed[0].parent != after[0].parent and {p.parent for p in changed} == {changed[0].parent}, (after, changed)
        end(edited, owner)

        # Another build of the worker or engine reads none of it.
        built = root / executable.name
        shutil.copy2(executable, built)
        rebuilt = root / ("rebuilt-" + NAME)
        rebuilt.write_bytes(b"render store fixture")
        owner, older = session(clients, str(built), rebuilt)
        state = render_ahead(older, done)
        assert state["kept"] == total + 1, state
        end(older, owner)
        now = time.time() + 10
        os.utime(built, (now, now))
        owner, newer = session(clients, str(built), rebuilt)
        state = render_ahead(newer, done)
        assert state["rendered"] == total, state
        end(newer, owner)


if __name__ == "__main__":
    run(Path(sys.argv[1]))
