#!/usr/bin/env python3
"""Original C++ virtual calls keep dispatch, name links and qualified xrefs."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from transport_test import Client


def ok(client, operation, payload=None):
    response = client.call(operation, payload, timeout=120)
    assert response["status"] == "ok", response
    return response["payload"]


def run(executable, target="aarch64-unknown-linux-gnu", scenario="all"):
    assert scenario in {"all", "member"}, scenario
    clang, linker = shutil.which("clang"), shutil.which("ld.lld")
    if not clang or not linker:
        print("SKIP: requires clang and ld.lld")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-virtual-calls-") as directory:
        root = Path(directory)
        source, obj, image = (root / name for name in ("fixture.cpp", "fixture.o", "fixture.so"))
        source.write_text("""
struct Device { virtual unsigned long long value(); };
unsigned long long Device::value() { return 7; }
extern "C" unsigned long long unrelated(unsigned long long input) { return input + 1; }
extern "C" unsigned long long dispatch(Device *object) {
  return object->value();
}
struct Leaf { unsigned long long payload; };
struct View { Leaf *cursor; };
struct Extended : Device { View *view; virtual unsigned long long extra(); };
unsigned long long Extended::extra() { return 19; }
struct Holder { Device *object; unsigned tag; };
__attribute__((noinline)) void observe(Holder *holder) {
  asm volatile("" : : "r"(holder) : "memory");
}
extern "C" unsigned long long dispatch_member(Holder *holder) {
  if (holder->tag != 7) return 0;
  Extended *child = static_cast<Extended *>(holder->object);
  if (!child) return 0;
  observe(holder);
  return child->extra();
}
extern "C" unsigned long long read_members(Holder *holder) {
  if (holder->tag != 7) return 0;
  Extended *child = static_cast<Extended *>(holder->object);
  if (!child) return 0;
  observe(holder);
  return child->view->cursor->payload;
}
extern "C" void write_member(Holder *holder, View *replacement) {
  if (holder->tag != 7) return;
  Extended *child = static_cast<Extended *>(holder->object);
  if (!child) return;
  observe(holder);
  child->view = replacement;
}
""")
        subprocess.run([clang, "--target=" + target, "-g", "-gdwarf-4",
                        "-O1", "-fPIC", "-fno-rtti", "-fno-exceptions",
                        "-fno-optimize-sibling-calls", "-c", str(source), "-o", str(obj)], check=True)
        # Protect the table for every AArch64 page granule, including 64 KiB.
        subprocess.run([linker, "-shared", "-Bsymbolic", "--no-undefined",
                        "-z", "common-page-size=65536", "-z", "max-page-size=65536",
                        str(obj), "-o", str(image)], check=True)
        with Client(executable) as client:
            ok(client, "open", {"path": str(image), "analysis": False, "warm": False})
            address = ok(client, "resolve", {"query": "dispatch"})["address"]

            def decompile(**options):
                return ok(client, "decompile", {"address": address, "representation": "cpp",
                                                "limit": 2048, "if_none_match": "", **options})

            def xrefs(target, direction="to"):
                return ok(client, "xrefs", {"address": target, "direction": direction})["items"]

            member = ok(client, "resolve", {"query": "dispatch_member"})["address"]
            for name, fields in (("read_members", ("view", "cursor", "payload")),
                                 ("write_member", ("view",))):
                entry = ok(client, "resolve", {"query": name})["address"]
                members = ok(client, "decompile", {"address": entry,
                                                   "representation": "cpp", "limit": 2048})
                body = members["text"].split("neverd.entry:")[-1]
                for field in fields:
                    assert "->" + field in body, (name, field, body)
            member_view = ok(client, "decompile", {"address": member,
                                                    "representation": "cpp", "limit": 2048})
            member_calls = member_view["virtual_calls"]
            assert len(member_calls) == 1, (member_calls, member_view["text"])
            assert member_calls[0]["receiver_type"] == "Extended", member_calls
            assert any(n["symbol"] == "_ZN8Extended5extraEv"
                       and n.get("address") == member_calls[0]["to"]
                       for n in member_view["source_names"]), member_view["source_names"]
            assert any(r["kind"] == "vcall" and r["from"] == member_calls[0]["from"]
                       for r in xrefs(member_calls[0]["to"]))
            # An explicit Y type overrides the scoped debug declaration.
            variable = next(v for v in member_view["source_variables"]
                            if not v["parameter"] and not v["ambiguous"]
                            and re.search(r"\b" + re.escape(v["name"])
                                          + r"\s*=\s*holder->object;", member_view["text"]))
            edit = {"address": member, "representation": "cpp", "kind": "type",
                    "variable_id": variable["id"],
                    "snapshot": member_view["variable_snapshot"], "type": "void *"}
            ok(client, "code_edit", edit)
            untyped_member = ok(client, "decompile", {"address": member,
                                                      "representation": "cpp", "limit": 2048})
            assert not untyped_member["virtual_calls"], untyped_member["text"]
            assert not [r for r in xrefs(member_calls[0]["to"]) if r["kind"] == "vcall"]
            ok(client, "undo")
            restored_member = ok(client, "decompile", {"address": member,
                                                       "representation": "cpp", "limit": 2048})
            assert restored_member["virtual_calls"] == member_calls, restored_member["virtual_calls"]

            if scenario == "member":
                return 0

            # The GUI renders in a read-only replica but queries xrefs in the
            # project owner. Transfer only an accepted result at its revision.
            with Client(executable) as replica:
                ok(replica, "analysis_restore", ok(client, "analysis_snapshot"))
                remote = ok(replica, "decompile", {"address": address,
                                                   "representation": "cpp", "limit": 2048})
                remote_calls = remote["virtual_calls"]
                assert len(remote_calls) == 1, remote_calls
                assert not [r for r in xrefs(remote_calls[0]["to"]) if r["kind"] == "vcall"]
                revision = client.call("metadata")["revision"]
                publication = {"address": address, "virtual_calls": remote_calls}
                published = client.call("analysis_virtual_calls", publication, revision=revision)
                assert published["status"] == "ok" and published["revision"] == revision, published
                assert len([r for r in xrefs(remote_calls[0]["to"]) if r["kind"] == "vcall"]) == 1

            view = decompile()
            calls = view["virtual_calls"]
            if len(calls) != 1:
                high = ok(client, "decompile", {"address": address,
                                                "representation": "high", "limit": 100})
                raise AssertionError((calls, view["text"], high["text"]))
            call = calls[0]
            assert call["resolution"] == "static_type" and call["receiver_type"] == "Device"
            names = [n for n in view["source_names"] if n["symbol"] == "_ZN6Device5valueEv"]
            assert any(n.get("address") == call["to"] for n in names), names
            incoming = [r for r in xrefs(call["to"]) if r["kind"] == "vcall"]
            assert len(incoming) == 1 and incoming[0]["from"] == call["from"], incoming
            assert incoming[0]["dispatch"] == "virtual" and incoming[0]["resolution"] == "static_type"
            assert any(r["kind"] == "vslot" and r["from"] == call["from"] for r in xrefs(call["slot"]))
            outgoing = xrefs(call["from"], "from")
            assert {r["kind"] for r in outgoing} >= {"vcall", "vslot"}, outgoing
            cached = decompile(refresh=True)
            assert len([r for r in xrefs(call["to"]) if r["kind"] == "vcall"]) == 1
            ok(client, "decompile", {"address": call["to"],
                                     "representation": "cpp", "limit": 2048})
            assert len([r for r in xrefs(call["to"]) if r["kind"] == "vcall"]) == 1
            unrelated = ok(client, "resolve", {"query": "unrelated"})["address"]
            ok(client, "function_edit", {"address": unrelated, "attributes": {
                "type": "unsigned long long unrelated(unsigned long long renamed)"}})
            restored = decompile()
            assert restored["etag"] == cached["etag"], (restored["etag"], cached["etag"])
            assert len([r for r in xrefs(call["to"]) if r["kind"] == "vcall"]) == 1
            ok(client, "function_edit", {"address": address, "attributes": {
                "type": "unsigned long long dispatch(void *object)"}})
            assert not [r for r in xrefs(call["to"]) if r["kind"] == "vcall"]
            stale = client.call("analysis_virtual_calls", publication, revision=revision)
            assert stale["status"] == "error" and stale["error"]["code"] == "stale_revision", stale
            assert not [r for r in xrefs(call["to"]) if r["kind"] == "vcall"]
            untyped = decompile()
            assert not untyped["virtual_calls"], untyped["virtual_calls"]
    return 0


if __name__ == "__main__":
    sys.exit(run(*sys.argv[1:]))
