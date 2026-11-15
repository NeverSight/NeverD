#!/usr/bin/env python3
"""Catalog template types survive Y edits, field views and virtual xrefs."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from transport_test import Client


def ok(client, operation, payload=None):
    reply = client.call(operation, payload, timeout=120)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def run(executable):
    clang, linker = shutil.which("clang"), shutil.which("ld.lld")
    if not clang or not linker:
        print("SKIP: requires clang and ld.lld")
        return 77
    for architecture in ("aarch64", "x86_64"):
        with tempfile.TemporaryDirectory(prefix="neverd-template-types-") as directory:
            root = Path(directory)
            source, obj, image = (root / name for name in ("fixture.cpp", "fixture.o", "fixture.so"))
            source.write_text("""
namespace sample {
template<class A, class B> struct Pair { A first; B second; };
template<class T, int N> struct Box {
  virtual unsigned long long get();
  T payload;
};
template<class T, int N> unsigned long long Box<T, N>::get() { return N; }
using Item = Box<Pair<int, unsigned int>, 3>;
template struct Box<Pair<int, unsigned int>, 3>;
template<class T> struct Holder { T *pointer; };
}
void *GlobalRoot;
sample::Item *TypedRoot;
extern "C" unsigned read_global() {
  return static_cast<sample::Item *>(GlobalRoot)->payload.second;
}
extern "C" unsigned long long dispatch_global() {
  return static_cast<sample::Item *>(GlobalRoot)->get();
}
extern "C" unsigned long long dispatch(unsigned long long, void *opaque) {
  return static_cast<sample::Item *>(opaque)->get();
}
extern "C" unsigned long long typed_dispatch(unsigned long long, sample::Item *object) {
  return object->get();
}
extern "C" unsigned long long nested_dispatch(sample::Holder<sample::Item> *holder) {
  return holder->pointer->get();
}
extern "C" __attribute__((noinline)) void *obtain() { return GlobalRoot; }
extern "C" unsigned long long local_dispatch() {
  auto *object = static_cast<sample::Item *>(obtain());
  return object->get() + object->payload.second;
}
""")
            subprocess.run([clang, "--target=" + architecture + "-unknown-linux-gnu",
                            "-g", "-gdwarf-4", "-O1", "-fPIC", "-fno-rtti", "-fno-exceptions",
                            "-fno-optimize-sibling-calls", "-c", str(source), "-o", str(obj)], check=True)
            subprocess.run([linker, "-shared", "-Bsymbolic", "--no-undefined",
                            "-z", "common-page-size=65536", "-z", "max-page-size=65536",
                            str(obj), "-o", str(image)], check=True)
            with Client(executable) as client:
                ok(client, "open", {"path": str(image), "analysis": False, "warm": False})
                def resolve(name):
                    return ok(client, "resolve", {"query": name})["address"]
                addresses = {name: resolve(name) for name in (
                    "GlobalRoot", "TypedRoot", "read_global", "dispatch_global", "dispatch",
                    "typed_dispatch", "nested_dispatch", "local_dispatch")}
                def view(name, representation="cpp"):
                    return ok(client, "decompile", {"address": addresses[name],
                              "representation": representation, "limit": 2048})
                def xrefs(target):
                    return [row for row in ok(client, "xrefs", {"address": target,
                            "direction": "to"})["items"] if row["kind"] == "vcall"]
                def virtual(name):
                    result = view(name)
                    calls = result["virtual_calls"]
                    assert len(calls) == 1, (name, calls, result["text"])
                    assert "sample::Box<" in calls[0]["receiver_type"], calls
                    assert "sample::" in result["text"], result["text"]
                    assert any(row["from"] == calls[0]["from"] for row in xrefs(calls[0]["to"]))
                    return calls[0]

                typed_query = {"address": addresses["TypedRoot"], "object_address": addresses["TypedRoot"]}
                declared = ok(client, "source_type", typed_query)["type"]
                assert "sample::Box<" in declared, declared
                ok(client, "source_type", {**typed_query, "type": declared})
                # Pasting the complete existing declaration, and changing its
                # whitespace, must select the same exact instantiation.
                proposed = "::sample :: Box < sample::Pair<int,unsigned int>,3 > *"
                query = {"address": addresses["GlobalRoot"], "object_address": addresses["GlobalRoot"]}
                assert "payload.second" not in view("read_global")["text"]
                assert not view("dispatch_global")["virtual_calls"]
                ok(client, "code_edit", {**query, "kind": "type", "type": proposed})
                for representation in ("c", "cpp", "llvmc"):
                    text = view("read_global", representation)["text"]
                    assert "payload.second" in text, (representation, text)
                    if representation != "cpp":
                        # Execute the generated scalar C on the host. The
                        # expected layout/value comes from the C++ fixture,
                        # independently of NeverD's displayed field names.
                        generated = root / (representation + ".c")
                        runnable = root / (representation + ".exe")
                        if representation == "c":
                            harness = """
int main(void) {
  __typeof__(*GlobalRoot) object = {0};
  object.payload.first = 19;
  object.payload.second = 117;
  GlobalRoot = &object;
  return read_global() != 117 || sizeof(object) != 16 ||
         __builtin_offsetof(__typeof__(object), payload.second) != 12;
}
"""
                        else:
                            # LLVM C retains the original image storage.
                            # Populate its pointer slot, then read independent
                            # bytes at the fixture's known member offset.
                            storage = re.search(r"neverd.image: (0x[0-9A-Fa-f]+)"
                                r" \(original loaded image storage\).*?extern uint8_t (\w+)\[(\d+)\]",
                                text, re.S)
                            assert storage, text
                            base, name, extent = storage.groups()
                            offset = int(addresses["GlobalRoot"], 16) - int(base, 16)
                            harness = f"""
_Alignas(4096) uint8_t {name}[{extent}];
int main(void) {{
  _Alignas(8) unsigned char object[16] = {{0}};
  unsigned value = 117;
  uintptr_t pointer = (uintptr_t)object;
  __builtin_memcpy(object + 12, &value, 4);
  __builtin_memcpy({name} + {offset}, &pointer, 8);
  return read_global() != 117;
}}
"""
                        generated.write_text(text + harness)
                        subprocess.run([clang, "-std=gnu11", "-fno-strict-aliasing",
                                        "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
                                        str(generated), "-o", str(runnable)], check=True)
                        subprocess.run([str(runnable)], check=True)
                call = virtual("dispatch_global")
                dispatch_source = root / "dispatch.c"
                dispatch_binary = root / "dispatch.exe"
                dispatch_source.write_text(view("dispatch_global", "c")["text"] + """
static unsigned long long first(void *object) { return object ? 17 : 0; }
static unsigned long long second(void *object) { return object ? 91 : 0; }
int main(void) {
  void *one[] = {(void *)first}, *two[] = {(void *)second};
  __typeof__(*GlobalRoot) object = {0};
  object._vptr_Box = (void *)one;
  GlobalRoot = &object;
  if (dispatch_global() != 17) return 1;
  object._vptr_Box = (void *)two;
  return dispatch_global() != 91;
}
""")
                subprocess.run([clang, "-std=gnu11", "-fno-strict-aliasing",
                                str(dispatch_source), "-o", str(dispatch_binary)], check=True)
                subprocess.run([str(dispatch_binary)], check=True)
                ok(client, "undo")
                assert not any(row["from"] == call["from"] for row in xrefs(call["to"]))
                assert not view("dispatch_global")["virtual_calls"]
                assert "payload.second" not in view("read_global")["text"]
                ok(client, "redo")
                virtual("dispatch_global")
                # Original debug parameter types and the Y function editor
                # keep the same complete name as global declarations.
                virtual("typed_dispatch")
                current = ok(client, "function_type", {"address": addresses["typed_dispatch"]})
                assert not current.get("problems"), current
                assert "sample::" in current["parameters"][1]["type"], current
                assert not view("dispatch")["virtual_calls"]
                ok(client, "function_edit", {"address": addresses["dispatch"], "attributes": {
                    "type": "unsigned long long dispatch(unsigned long long ignored, "
                            + proposed + "object)"}})
                virtual("dispatch")
                virtual("nested_dispatch")
                # Retype an actual recovered call-result local using its
                # source snapshot; both members and dispatch must follow it.
                before = view("local_dispatch", "c")
                candidates = [row for row in before["source_variables"]
                              if not row.get("parameter") and not row.get("address")
                              and not row["ambiguous"] and row["bytes"] == 8]
                assert candidates, before
                chosen = next(row for row in candidates if re.search(
                    r"\b" + re.escape(row["name"]) + r"\s*=\s*[^;\n]*\bobtain\(", before["text"]))
                request = {"address": addresses["local_dispatch"], "representation": "c",
                           "kind": "type", "variable_id": chosen["id"],
                           "snapshot": before["variable_snapshot"], "type": proposed}
                ok(client, "code_edit", request)
                after = view("local_dispatch", "c")
                assert "payload.second" in after["text"], after["text"]
                assert len(after["virtual_calls"]) == 1, after["text"]
                for scalar in ("void *", "unsigned long long"):
                    ok(client, "code_edit", {**request, "type": scalar})
                    untyped = view("local_dispatch", "c")
                    assert "payload.second" not in untyped["text"] and not untyped["virtual_calls"]
                    ok(client, "undo")
                    assert len(view("local_dispatch", "c")["virtual_calls"]) == 1
                # A malformed or absent specialization never changes state.
                for bad in ("sample::Box<sample::Pair<int,unsigned int>,4>*",
                            "sample::Box<sample::Pair<int,unsigned int>,3>>*",
                            "sample::Box<sample::Pair<int,unsigned int>,3>"):
                    response = client.call("code_edit", {**query, "kind": "type", "type": bad})
                    assert response["status"] == "error", response
                virtual("dispatch_global")
                # The analysis replica must see the accepted types and
                # publish the same targets at this revision.
                with Client(executable) as replica:
                    ok(replica, "analysis_restore", ok(client, "analysis_snapshot"))
                    other = ok(replica, "decompile", {"address": addresses["dispatch_global"],
                               "representation": "cpp", "limit": 2048})
                    assert other["virtual_calls"] == view("dispatch_global")["virtual_calls"]
            with Client(executable) as client:
                ok(client, "open", {"path": str(image), "analysis": False, "warm": False})
                assert ok(client, "source_type", query)["stated"]
                virtual("dispatch_global")
                virtual("dispatch")
                assert len(view("local_dispatch", "c")["virtual_calls"]) == 1
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
