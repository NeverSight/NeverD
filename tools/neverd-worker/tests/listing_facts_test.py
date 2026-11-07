#!/usr/bin/env python3
"""What a job keeps of a function's first look answers later listings.

A first look asks the engine for the function's recovered instructions and stack
frame.  An owner reads what a replica's job kept of them, from the same session on,
and lists what asking the engine lists.  Function names keep the facts; a change to
what the engine reads, and damage, send a look back to the engine.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import time

from pe_listing_facts_test import BASE, ENTRY, BODY, fixture, ok
from render_ahead_test import done, render_ahead
from transport_test import Client

ADDRESSES = (ENTRY, BODY, BASE + 0x1100, BASE + 0x1140)
DATA = BASE + 0x3000


def listed(client, addresses=ADDRESSES):
    return [ok(client, "listing", {"address": hex(address), "before": 0,
                                   "after": 35})["lines"]
            for address in addresses]


def facts(client):
    return ok(client, "metadata")["listing_facts"]


def owner_of(clients, worker, binary, **options):
    owner = clients.enter_context(Client(worker))
    ok(owner, "open", {"path": str(binary), "analysis": False, "warm": False, **options})
    return owner


def replica_of(clients, worker, owner):
    replica = clients.enter_context(Client(worker))
    snapshot = ok(owner, "analysis_snapshot")
    snapshot.update(call_summaries=True, render_ahead=True, render_store="write")
    ok(replica, "analysis_restore", snapshot)
    return replica


def correct_frame(client):
    flow = ok(client, "stack_frame", {"address": hex(BODY)})
    frame = {"snapshot": flow["snapshot"], "variables": [],
             "points": [{"address": hex(BODY), "after": -40}]}
    ok(client, "code_edit", {"address": hex(BODY), "representation": "c",
                             "kind": "stack_frame", "stack_frame": frame})
    return listed(client, ADDRESSES[1:2])


def stores(binary):
    return sorted((Path(str(binary) + ".neverd") / "local" / "listing-facts").rglob("*.ndrs"))


def run(worker):
    with tempfile.TemporaryDirectory(prefix="neverd-listing-facts-") as directory, ExitStack() as clients:
        root = Path(directory)
        plain = root / "plain.dll"
        plain.write_bytes(fixture())
        plain_owner = owner_of(clients, worker, plain, browse_cache=False)
        reference = listed(plain_owner)
        assert any("rsp" in str(page) for page in reference), reference
        # A frame the user corrected is another answer from the engine.
        corrected = correct_frame(plain_owner)
        plain_owner.close()

        # An owner takes up what its replica keeps, in the same session.
        binary = root / "facts.dll"
        binary.write_bytes(fixture())
        owner = owner_of(clients, worker, binary, browse_cache=False)
        # Its store is not read while the browse cache is off.
        assert listed(owner) == reference
        assert facts(owner) == {"kept": 0, "bytes": 0, "hits": 0, "misses": 0}, facts(owner)
        owner.close()
        owner = owner_of(clients, worker, binary)
        assert listed(owner, ADDRESSES[:1]) == reference[:1]
        before = facts(owner)
        assert before["hits"] == 0 and before["kept"] == 0, before
        replica = replica_of(clients, worker, owner)
        render_ahead(replica, done)
        kept = facts(replica)
        assert kept["kept"] >= 2 and kept["bytes"] > 0, kept
        assert stores(binary), "the job keeps facts on disk"
        time.sleep(1.1)  # kListingFactsRefreshMilliseconds
        assert listed(owner, ADDRESSES[1:2]) == reference[1:2]
        taken_up = facts(owner)
        assert taken_up["hits"] >= 2 and taken_up["kept"] == kept["kept"], (before, taken_up)
        replica.close()
        owner.close()

        # A later session reads them from its first look on.
        later = owner_of(clients, worker, binary)
        assert listed(later, ADDRESSES[2:]) == reference[2:]
        state = facts(later)
        assert state["hits"] >= 1 and state["kept"] == kept["kept"], state
        # A function's name is not among what they read.
        ok(later, "rename", {"address": hex(BODY), "name": "renamed_body"})
        hits = facts(later)["hits"]
        again = listed(later, ADDRESSES[1:2])
        assert "renamed_body" in str(again), again
        renamed = facts(later)
        assert renamed["hits"] > hits and renamed["kept"] == kept["kept"], renamed
        ok(later, "undo")
        later.close()

        # What the engine reads is: a name for data starts a new set.
        named = owner_of(clients, worker, binary)
        assert listed(named, ADDRESSES[1:2]) == reference[1:2]
        state = facts(named)
        assert state["hits"] >= 2 and state["kept"] == kept["kept"], state
        ok(named, "rename", {"address": hex(DATA), "name": "named_data"})
        listed(named, ADDRESSES[2:])
        state = facts(named)
        assert state["kept"] == 0 and state["misses"] > 0, state
        ok(named, "undo")
        named.close()

        # A frame the user corrected asks the engine, under what corrects it.
        edited = owner_of(clients, worker, binary)
        assert correct_frame(edited) == corrected
        ok(edited, "undo")
        edited.close()

        # So does another image, whatever its name and time.
        binary_bytes = binary.read_bytes()
        changed_bytes = bytearray(binary_bytes)
        changed_bytes[0x360] ^= 1
        binary.write_bytes(changed_bytes)
        other = owner_of(clients, worker, binary)
        listed(other, ADDRESSES[1:2])
        state = facts(other)
        assert state["hits"] == 0 and state["kept"] == 0, state
        other.close()
        binary.write_bytes(binary_bytes)

        # A store another session is still writing: what it has written answers,
        # up to a record cut short, and the rest once it is there.
        files = stores(binary)
        assert len(files) == 1, files
        whole = files[0].read_bytes()
        files[0].write_bytes(whole[:len(whole) // 2])
        reading = owner_of(clients, worker, binary)
        listed(reading, ADDRESSES[:1])
        partial = facts(reading)
        assert 0 < partial["kept"] < kept["kept"], (partial, kept)
        with files[0].open("ab") as output:
            output.write(whole[len(whole) // 2:])
        time.sleep(1.1)  # kListingFactsRefreshMilliseconds
        listed(reading, ADDRESSES[1:])
        assert facts(reading)["kept"] == kept["kept"], facts(reading)
        reading.close()

        # Damage is a fact not kept: the look asks the engine.
        files = stores(binary)
        assert files
        for file in files:
            data = bytearray(file.read_bytes())
            for at in range(64, len(data), 16):
                data[at] ^= 0xff
            file.write_bytes(data)
        damaged = owner_of(clients, worker, binary)
        assert listed(damaged) == reference
        state = facts(damaged)
        assert state["hits"] == 0 and state["misses"] > 0, state
        damaged.close()
    print("Listing facts kept by a job, read by later listings, and refused when damaged passed")


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
