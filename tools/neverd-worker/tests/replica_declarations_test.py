#!/usr/bin/env python3
"""A running replica takes a declaration in place and reads as a new one does.

A function's declaration and the project's types reach only what the decompiler
reads, and a note on a function nothing at all, so neither restores a replica.  After
each edit its views read as those of a replica restored afresh, and the owner's
listing as it does after a reload.  What analysis reads is still refused.
"""
from pathlib import Path
import sys
import tempfile

from parameter_type_propagation_test import ENTRY, fixture, ok, source
from transport_test import Client

REPRESENTATIONS = ("c", "llvmc", "low")


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-replica-declarations-") as directory:
        path = Path(directory) / "declarations.exe"
        binary, _ = fixture()
        path.write_bytes(binary)
        with Client(executable) as owner, Client(executable) as replica:
            ok(owner, "open", {"path": str(path), "analysis": False})
            ok(replica, "analysis_restore", ok(owner, "analysis_snapshot"))
            original = {rep: source(replica, rep) for rep in REPRESENTATIONS}

            def takes_it(step, reads):
                assert ok(replica, "analysis_update_names",
                          ok(owner, "analysis_snapshot"))["updated"], step
                views = {rep: source(replica, rep) for rep in REPRESENTATIONS}
                assert reads(views["c"]), (step, views["c"])
                with Client(executable) as fresh:
                    ok(fresh, "analysis_restore", ok(owner, "analysis_snapshot"))
                    for rep in REPRESENTATIONS:
                        assert source(fresh, rep) == views[rep], (step, rep)
                        assert source(owner, rep) == views[rep], (step, rep)
                page = {"address": hex(ENTRY), "before": 0, "after": 24}
                shown = ok(owner, "listing", page)["lines"]
                return views, shown

            # Types no declaration names yet.
            ok(owner, "local_types_declare", {"language": "c", "source":
               "struct CONTEXT { unsigned long long cookie; "
               "unsigned long long value; unsigned long long slots[2]; }; "
               "struct OTHER_CONTEXT { unsigned long long cookie; "
               "unsigned long long payload; unsigned long long items[2]; };"})
            views, _ = takes_it("types", lambda text: "->value" not in text)
            assert views["low"] == original["low"]
            # A note reads nowhere.
            ok(owner, "function_edit", {"address": hex(ENTRY),
                                        "attributes": {"color": "#204060"}})
            kept, _ = takes_it("note", lambda text: "->value" not in text)
            assert kept == views
            # A declaration that names one.
            declaration = ok(owner, "function_type", {
                "address": hex(ENTRY), "parameter": 0, "type": "CONTEXT *arg0"})
            ok(owner, "function_edit", {"address": hex(ENTRY),
                                        "attributes": {"type": declaration["declaration"]}})
            _, declared = takes_it("declaration", lambda text: "->value" in text)
            ok(owner, "reload")
            assert ok(owner, "listing", {"address": hex(ENTRY), "before": 0,
                                         "after": 24})["lines"] == declared
            ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))
            changed = ok(owner, "function_type", {
                "address": hex(ENTRY), "parameter": 0, "type": "OTHER_CONTEXT *arg0"})
            ok(owner, "function_edit", {"address": hex(ENTRY),
                                        "attributes": {"type": changed["declaration"]}})
            takes_it("another declaration", lambda text: "->payload" in text)
            ok(owner, "undo")
            takes_it("undone", lambda text: "->value" in text)
            ok(owner, "redo")
            takes_it("redone", lambda text: "->payload" in text)
            # What analysis reads starts over.
            ok(owner, "function_edit", {"address": hex(ENTRY),
                                        "attributes": {"noreturn": True}})
            refused = replica.call("analysis_update_names", ok(owner, "analysis_snapshot"))
            assert refused["error"]["code"] == "stale_snapshot", refused
    print("a running replica takes declarations and notes as a new one reads them")


if __name__ == "__main__":
    run(sys.argv[1])
