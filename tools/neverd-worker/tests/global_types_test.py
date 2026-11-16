#!/usr/bin/env python3
"""Global declarations can be inspected and edited before any decompilation."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from transport_test import Client


def ok(client, operation, payload=None):
    response = client.call(operation, payload, timeout=120)
    assert response["status"] == "ok", response
    return response["payload"]


def run(executable):
    clang, linker = shutil.which("clang"), shutil.which("ld.lld")
    if not clang or not linker:
        print("SKIP: requires clang and ld.lld")
        return 77
    for architecture in ("aarch64", "x86_64"):
        with tempfile.TemporaryDirectory(prefix="neverd-global-types-") as directory:
            root = Path(directory)
            source, obj, image = (root / name for name in ("fixture.cpp", "fixture.o", "fixture.so"))
            source.write_text("""
struct Device { unsigned long long flags; unsigned long long value; };
Device *GlobalRoot;
template<class T> struct Handle { T *Object; unsigned long long Count; };
Handle<Device> WrappedRoot;
struct Owner { unsigned long long tag; Handle<Device> Reference; } GlobalOwner;
extern "C" { struct { unsigned long long value; } *AnonymousRoot __attribute__((used)); }
extern "C" unsigned long long read_value() { return GlobalRoot->value; }
extern "C" unsigned long long read_wrapped() { return WrappedRoot.Object->value; }
extern "C" unsigned long long read_interior() { return GlobalOwner.Reference.Object->value; }
""")
            subprocess.run([clang, "--target=" + architecture + "-unknown-linux-gnu",
                            "-g", "-gdwarf-4", "-O1", "-fPIC", "-c", str(source),
                            "-o", str(obj)], check=True)
            subprocess.run([linker, "-shared", "-Bsymbolic", "--no-undefined",
                            str(obj), "-o", str(image)], check=True)
            with Client(executable) as client:
                ok(client, "open", {"path": str(image), "analysis": False, "warm": False})
                address = ok(client, "resolve", {"query": "GlobalRoot"})["address"]
                function = ok(client, "resolve", {"query": "read_value"})["address"]
                anonymous = ok(client, "resolve", {"query": "AnonymousRoot"})["address"]
                query = {"address": address, "object_address": address}
                original = ok(client, "source_type", query)
                assert original["name"] == "GlobalRoot", original
                assert "Device" in original["type"] and "*" in original["type"], original
                assert original["bytes"] == 8 and not original["stated"], original

                def view():
                    return ok(client, "decompile", {"address": function,
                                                     "representation": "c", "limit": 2048})

                def edit(**attributes):
                    return ok(client, "code_edit", {**query, "kind": "type", **attributes})

                initial = view()["text"]
                assert "GlobalRoot->value" in initial, initial
                for symbol in ("read_wrapped", "read_interior"):
                    entry = ok(client, "resolve", {"query": symbol})["address"]
                    text = ok(client, "decompile", {"address": entry,
                              "representation": "c", "limit": 2048})["text"]
                    assert "->value" in text, (symbol, text)
                for bad in (
                    {**query, "type": "unsigned int"},
                    {**query, "type": "double"},
                    {"address": function, "object_address": function},
                    {"address": function, "object_address": address},
                    {"address": anonymous, "object_address": anonymous},
                    {"address": hex(int(address, 16) + 1), "object_address": hex(int(address, 16) + 1)},
                ):
                    response = client.call("source_type", bad)
                    assert response["status"] == "error", response
                edit(type="void *")
                assert ok(client, "source_type", query)["stated"]
                assert "GlobalRoot->value" not in view()["text"]
                ok(client, "undo")
                assert "GlobalRoot->value" in view()["text"]
                ok(client, "redo")
                assert "GlobalRoot->value" not in view()["text"]
                edit(reset=True)
                assert not ok(client, "source_type", query)["stated"]
                assert "GlobalRoot->value" in view()["text"]
                edit(type=original["type"])
                assert "GlobalRoot->value" in view()["text"]
            with Client(executable) as client:
                ok(client, "open", {"path": str(image), "analysis": False, "warm": False})
                restored = ok(client, "source_type", query)
                assert restored["stated"] and restored["type"] == original["type"], restored
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
