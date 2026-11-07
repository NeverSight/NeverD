#!/usr/bin/env python3
"""A type nothing has read leaves everything kept as it is.

Only the declarations of types new to the library, which no other type, function
declaration, prototype or spelling reads, count as unread.  The views after each read
as a fresh replica's do, and what earlier sessions kept holds for the state after one.
"""
from pathlib import Path
import sys
import tempfile
import time

from listing_facts_test import ADDRESSES
from parameter_type_propagation_test import ENTRY, fixture, ok, source
from pe_listing_facts_test import fixture as functions_fixture
from render_ahead_test import done, render_ahead
from transport_test import Client

REPRESENTATIONS = ("c", "llvmc", "low")


def declare(client, text):
    return ok(client, "local_types_declare", {"language": "c", "source": text})


def kept_folder(path):
    return next(iter(path.parent.glob(path.name + "*/local/render-ahead")), None)


def stores(path):
    root = kept_folder(path)
    return sorted(p.name for p in root.iterdir()) if root else []


def settled(client, timeout=60):
    """The summaries the job reads come first, and what is read ahead under them."""
    deadline = time.monotonic() + timeout
    while True:
        message = client.next(lambda m: m.get("type") == "heartbeat",
                              timeout=max(0.1, deadline - time.monotonic()))
        state = (message.get("background") or {}).get("call_summaries") or {}
        if state.get("state") == "unavailable" or (state.get("prefill") or {}).get("state") in ("done", "unavailable"):
            return state


def source_of(client, representation, address=ADDRESSES[1]):
    return ok(client, "decompile", {"address": hex(address), "offset": 0, "limit": 200,
                                    "representation": representation})


def kept_views(client):
    views = [source_of(client, "source", address) for address in ADDRESSES]
    for view in views:
        for moving in ("revision", "project_id"):
            view.pop(moving, None)
    return views


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-unread-types-") as directory:
        path = Path(directory) / "types.exe"
        binary, _ = fixture()
        path.write_bytes(binary)
        with Client(executable) as owner, Client(executable) as replica:
            ok(owner, "open", {"path": str(path), "analysis": False})
            ok(replica, "analysis_restore", ok(owner, "analysis_snapshot"))
            for rep in REPRESENTATIONS:
                source(replica, rep)

            def same_as_fresh(step):
                assert ok(replica, "analysis_update_names",
                          ok(owner, "analysis_snapshot"))["updated"], step
                views = {rep: source(replica, rep) for rep in REPRESENTATIONS}
                with Client(executable) as fresh:
                    ok(fresh, "analysis_restore", ok(owner, "analysis_snapshot"))
                    for rep in REPRESENTATIONS:
                        assert source(fresh, rep) == views[rep], (step, rep)
                return views

            original = same_as_fresh("nothing")
            # A new type nothing names, and one that only names a name yet unknown.
            reply = declare(owner, "struct FRESH { unsigned long long first; };")
            assert reply["unread"] and not reply["decompilation"], reply
            assert same_as_fresh("new") == original
            reply = declare(owner, "struct HOLDER { struct LATER *later; };")
            assert reply["unread"], reply
            assert same_as_fresh("names a later one") == original
            # A type another names, or one already known, is read.
            reply = declare(owner, "struct LATER { unsigned long long inner; };")
            assert not reply["unread"], reply
            same_as_fresh("named")
            reply = declare(owner, "struct FRESH { unsigned long long second; };")
            assert not reply["unread"], reply
            same_as_fresh("replaced")
            # A type another still names, taken away and declared again, is read.
            reply = declare(owner, "struct USER { struct GOING *going; };")
            assert reply["unread"], reply
            same_as_fresh("names one to go")
            declare(owner, "struct GOING { unsigned long long inner; };")
            same_as_fresh("defined")
            removed = ok(owner, "local_types_forget", {"names": ["GOING"]})
            assert removed["usedBy"] == ["USER"], removed
            same_as_fresh("gone")
            reply = declare(owner, "struct GOING { unsigned long long inner; };")
            assert not reply["unread"], reply
            same_as_fresh("back")
            # An enumerator gives way to a type of its name.
            reply = declare(owner, "enum CHOICE { CLASHING = 1, OTHER = 2 };")
            assert reply["unread"], reply
            same_as_fresh("new enumerators")
            reply = declare(owner, "struct CLASHING { unsigned long long inner; };")
            assert not reply["unread"], reply
            same_as_fresh("takes an enumerator's name")
            # A prototype reaches analysis with the types it names.
            reply = declare(owner, "int prototyped(struct LATER *value);")
            assert not reply["unread"], reply
            same_as_fresh("prototype")
            # A function's declaration reads the new type as a new context does.
            reply = declare(owner, "struct APPLIED { unsigned long long cookie; "
                                   "unsigned long long value; unsigned long long slots[2]; };")
            assert reply["unread"], reply
            same_as_fresh("to be applied")
            source(owner, "c")
            declared = ok(owner, "function_type", {"address": hex(ENTRY), "parameter": 0,
                                                   "type": "struct APPLIED *arg0"})
            ok(owner, "function_edit", {"address": hex(ENTRY),
                                        "attributes": {"type": declared["declaration"]}})
            views = same_as_fresh("applied")
            assert "->value" in views["c"], views["c"]
            # A declaration that names a type taken away reads again when it comes
            # back, so that type is read.
            removed = ok(owner, "local_types_forget", {"names": ["APPLIED"]})
            assert removed["removed"] == ["APPLIED"], removed
            same_as_fresh("taken away")
            reply = declare(owner, "struct APPLIED { unsigned long long cookie; "
                                   "unsigned long long value; unsigned long long slots[2]; };")
            assert not reply["unread"], reply
            views = same_as_fresh("back")
            assert "->value" in views["c"], views["c"]
            # Undo takes the types away again.
            ok(owner, "undo")
            ok(owner, "undo")
            ok(owner, "undo")
            same_as_fresh("not applied")
            ok(owner, "undo")
            same_as_fresh("undeclared")

        # What is kept stands for the state after a type nothing reads.
        kept = Path(directory) / "kept.dll"
        kept.write_bytes(functions_fixture())
        with Client(executable) as owner, Client(executable) as replica:
            ok(owner, "open", {"path": str(kept), "analysis": False, "warm": False,
                               "write_behind": True})
            restore = ok(owner, "analysis_snapshot")
            restore.update(call_summaries=True, render_ahead=True, render_store="write")
            ok(replica, "analysis_restore", restore)
            state = render_ahead(replica, done)
            assert state["rendered"] and state["kept"] == state["total"] + 1, state
            before = kept_views(replica)
            renders = stores(kept)
            assert len(renders) == 1, renders
            reply = declare(owner, "struct KEPT_FRESH { unsigned long long first; };")
            assert reply["unread"], reply
            assert ok(owner, "kept_renders")["complete"]
            # Whether or not its file has been written by then.
            ok(owner, "analysis_snapshot")
            assert ok(owner, "kept_renders")["complete"]
            assert len(stores(kept)) == 2 and renders[0] in stores(kept), stores(kept)
            assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
            # Heartbeats from before the update may still come first.
            after = render_ahead(replica, lambda s: done(s) and not s["rendered"])
            assert after["kept"] == state["kept"], (state, after)
            assert kept_views(replica) == before
        # A later session finds them, and writing them keeps what leads to them.
        with Client(executable) as owner, Client(executable) as replica:
            ok(owner, "open", {"path": str(kept), "analysis": False, "warm": False,
                               "write_behind": True})
            assert ok(owner, "kept_renders")["complete"]
            restore = ok(owner, "analysis_snapshot")
            restore.update(call_summaries=True, render_ahead=True, render_store="write")
            ok(replica, "analysis_restore", restore)
            state = render_ahead(replica, done)
            assert not state["rendered"], state
            assert kept_views(replica) == before
            # A view no session rendered writes to what the later state reads.
            folder = next(p for p in kept_folder(kept).iterdir() if p.is_dir())
            written = len(list(folder.iterdir()))
            source_of(replica, "c")
            settled(replica)
            source_of(replica, "c", ADDRESSES[2])
            assert len(list(folder.iterdir())) > written, list(folder.iterdir())
            assert len(stores(kept)) == 2, stores(kept)
            # A type a render reads moves to a store of its own.
            reply = declare(owner, "struct KEPT_FRESH { unsigned long long second; };")
            assert not reply["unread"], reply
            assert not ok(owner, "kept_renders")["complete"]
            assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
            state = render_ahead(replica, lambda s: done(s) and s["rendered"])
            names = stores(kept)
            assert not [name for name in names if name.endswith(".same")], names
    print("a type nothing has read leaves what is kept, and reads as a fresh session does")


if __name__ == "__main__":
    run(sys.argv[1])
