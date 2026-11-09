#!/usr/bin/env python3
"""A function rendered under the declaration a Set Type dialog read answers the
view after the user states that declaration, and no view before.

The dialog has its replica render the function as the declaration would make it
read (prepare_function with "declaration"); the replica gives the function's
declaration back.  Until the owner states it, the function reads as it did.  Once
the replica takes the owner's state, its view reads that render, as a fresh
replica reads the function.  Another declaration, summaries solved since, or
other inputs of the store leave the render unread.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

from kept_reads_test import fresh_page, page, sessions
from pe_listing_facts_test import fixture as functions_fixture
from render_ahead_test import done, render_ahead
from source_variable_types_test import ok

TARGET, CALLER = 0x140001040, 0x140001100


def declared(name):
    return f"long long sub_140001100(long long {name})"


def hits(replica):
    return render_ahead(replica, lambda state: state is not None)["declared_hits"]


def prepare(client, name):
    return ok(client, "prepare_function",
              {"address": hex(CALLER), "declaration": declared(name)})


def state(owner, replica, name):
    ok(owner, "function_edit", {"address": hex(CALLER),
                                "attributes": {"type": declared(name)}})
    assert ok(replica, "analysis_update_names",
              ok(owner, "analysis_snapshot"))["updated"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-declared-render-") as directory, \
            ExitStack() as clients:
        typed = Path(directory) / "typed.dll"
        typed.write_bytes(functions_fixture())
        owner, replica = sessions(clients, executable, typed)

        # Rendered before the replica solved the image's call summaries: a view
        # after them reads them, and so does not read the render.
        reply = prepare(replica, "early_argument")
        assert reply["prepared"] and reply["rendered"], reply
        start = render_ahead(replica, done)["declared_hits"]
        state(owner, replica, "early_argument")
        shown = page(replica, CALLER)
        assert "early_argument" in shown["text"], shown["text"]
        assert hits(replica) == start
        assert shown == fresh_page(executable, owner, CALLER)

        # The dialog reads a declaration; the replica renders under it.
        reply = prepare(replica, "renamed_argument")
        assert reply["prepared"] and reply["rendered"], reply
        # Until the user states it, the function reads as it did.
        assert page(replica, CALLER) == shown
        assert hits(replica) == start
        # Stated, the view reads the render, as a fresh replica reads it.
        state(owner, replica, "renamed_argument")
        stated = page(replica, CALLER)
        assert "renamed_argument" in stated["text"], stated["text"]
        assert hits(replica) == start + 1
        assert stated == fresh_page(executable, owner, CALLER)

        # A declaration other than the one rendered leaves it unread.
        assert prepare(replica, "rendered_argument")["rendered"]
        state(owner, replica, "other_argument")
        other = page(replica, CALLER)
        assert "other_argument" in other["text"], other["text"]
        assert hits(replica) == start + 1
        assert other == fresh_page(executable, owner, CALLER)

        # A dialog given up: rendered, never stated, never read.
        assert prepare(replica, "cancelled_argument")["rendered"]
        assert page(replica, CALLER) == other
        assert hits(replica) == start + 1

        # A type the store's inputs take since leaves it unread too.
        assert prepare(replica, "typed_argument")["rendered"]
        ok(owner, "local_types_declare",
           {"language": "c", "source": "struct declared_render { int value; };"})
        state(owner, replica, "typed_argument")
        typed_page = page(replica, CALLER)
        assert "typed_argument" in typed_page["text"], typed_page["text"]
        assert hits(replica) == start + 1
        assert typed_page == fresh_page(executable, owner, CALLER)

        # A declaration the function has renders nothing, nor does an owner.
        reply = prepare(replica, "typed_argument")
        assert not reply["prepared"] and not reply["rendered"], reply
        reply = prepare(owner, "owner_argument")
        assert not reply["prepared"] and not reply["rendered"], reply
        # The function the caller calls reads as a fresh replica's throughout.
        assert page(replica, TARGET) == fresh_page(executable, owner, TARGET)


if __name__ == "__main__":
    run(sys.argv[1])
    print("declared render test passed")
