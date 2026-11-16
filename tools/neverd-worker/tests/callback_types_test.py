#!/usr/bin/env python3
"""Debug callback declarations survive both C backends and type edits."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from transport_test import Client


def ok(client, operation, payload):
    response = client.call(operation, payload, timeout=120)
    assert response["status"] == "ok", response
    return response["payload"]


def run(executable):
    clang, linker = shutil.which("clang"), shutil.which("ld.lld")
    if not clang or not linker:
        print("SKIP: requires clang and ld.lld")
        return 77
    views = syntax_checks = executions = 0
    for architecture in ("aarch64", "x86_64"):
        with tempfile.TemporaryDirectory(prefix="neverd-callback-types-") as directory:
            root = Path(directory)
            source, obj, image = (root / name for name in ("fixture.cpp", "fixture.o", "fixture.so"))
            source.write_text("""
namespace sample {
template<class T, unsigned N> struct Item { T value; };
using Record = Item<int, 2>;
enum Mode { First, Second };
int type_anchor(Record *record) { return record->value; }
}
using Scalar = int (*)(float);
using Factory = Scalar (*)(Scalar);
using Aggregate = sample::Record (*)(sample::Record, sample::Mode*);
using Observer = void (*)(sample::Record*, sample::Mode*);
using Variadic = int (*)(void*, ...);
using Table = Scalar[2];
using ReferenceCallback = sample::Record& (*)(sample::Record&);
struct CallbackHolder { ReferenceCallback value; };
CallbackHolder ReferenceHolder;
Scalar ScalarSlot;
Factory FactorySlot;
Aggregate AggregateSlot;
Observer ObserverSlot;
Variadic VariadicSlot;
Table *TableSlot;
extern "C" Scalar fetch_scalar() { return ScalarSlot; }
extern "C" Factory fetch_factory() { return FactorySlot; }
extern "C" Aggregate fetch_aggregate() { return AggregateSlot; }
extern "C" Observer fetch_observer() { return ObserverSlot; }
extern "C" Variadic fetch_variadic() { return VariadicSlot; }
extern "C" Table *fetch_table() { return TableSlot; }
extern "C" void store_scalar(Scalar value) { ScalarSlot = value; }
extern "C" CallbackHolder *fetch_reference_holder() { return &ReferenceHolder; }
""")
            functions = [
                ("fetch_scalar", "ScalarSlot"), ("fetch_factory", "FactorySlot"),
                ("fetch_aggregate", "AggregateSlot"), ("fetch_observer", "ObserverSlot"),
                ("fetch_variadic", "VariadicSlot"), ("fetch_table", "TableSlot"),
                ("store_scalar", "ScalarSlot"),
                ("fetch_reference_holder", "ReferenceHolder"),
            ]
            with source.open("a") as output:
                for index, lane in enumerate(("float", "double", "unsigned char", "long long")):
                    output.write(f"\nusing LaneVector{index} = {lane} __attribute__((vector_size(16)));\n"
                        f"using VectorCallback{index} = LaneVector{index} (*)(LaneVector{index});\n"
                        f"VectorCallback{index} VectorSlot{index};\n"
                        f'extern "C" VectorCallback{index} fetch_vector{index}() {{ return VectorSlot{index}; }}\n')
                    functions.append((f"fetch_vector{index}", f"VectorSlot{index}"))
            if architecture == "x86_64":
                with source.open("a") as output:
                    output.write("""
using PlatformCallback = int (__attribute__((ms_abi)) *)(int);
PlatformCallback PlatformSlot;
extern "C" PlatformCallback fetch_platform() { return PlatformSlot; }
""")
                functions.append(("fetch_platform", "PlatformSlot"))
            subprocess.run([clang, "--target=" + architecture + "-unknown-linux-gnu",
                            "-g", "-gdwarf-4", "-O1", "-fPIC", "-c", str(source), "-o", str(obj)], check=True)
            subprocess.run([linker, "-shared", "-Bsymbolic", "--no-undefined",
                            str(obj), "-o", str(image)], check=True)
            with Client(executable) as client:
                ok(client, "open", {"path": str(image), "analysis": False, "warm": False})
                for function, slot in functions:
                    address = ok(client, "resolve", {"query": function})["address"]
                    slot_address = ok(client, "resolve", {"query": slot})["address"]
                    for representation in ("c", "cpp", "llvmc"):
                        result = ok(client, "decompile", {"address": address,
                                    "representation": representation, "limit": 2048})
                        text = result["text"]
                        assert result["complete"] and text, (function, representation, result)
                        views += 1
                        if representation == "cpp":
                            continue
                        generated = root / (function + "-" + representation + ".c")
                        generated.write_text(text)
                        compiled = subprocess.run([clang, "-std=gnu11", "-fsyntax-only",
                            "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
                            str(generated)], capture_output=True, text=True)
                        assert compiled.returncode == 0, (architecture, function,
                            representation, compiled.stderr, text)
                        syntax_checks += 1
                        if function.startswith("fetch_vector"):
                            assert "vector_size(16)" in text, text
                        if function == "fetch_variadic" and representation == "c":
                            assert re.search(r"\(void\s*\*\s*,\s*\.\.\.\)", text), text
                        if function == "fetch_platform" and representation == "c":
                            assert "__attribute__((ms_abi))" in text, text
                        if function == "fetch_scalar" or function.startswith("fetch_vector"):
                            # Execute generated code against an independently
                            # defined callback. LLVM C keeps image backing;
                            # HighC names the global pointer directly.
                            setup = f"{slot} = callback;"
                            storage_definition = ""
                            if representation == "llvmc":
                                match = re.search(r"neverd.image: (0x[0-9A-Fa-f]+)"
                                    r" \(original loaded image storage\).*?extern uint8_t (\w+)\[(\d+)\]",
                                    text, re.S)
                                assert match, text
                                base, name, extent = match.groups()
                                offset = int(slot_address, 16) - int(base, 16)
                                storage_definition = f"_Alignas(4096) uint8_t {name}[{extent}];"
                                setup = ("uintptr_t pointer = (uintptr_t)&callback; "
                                         f"__builtin_memcpy({name} + {offset}, &pointer, 8);")
                            if function == "fetch_scalar":
                                callback = "static int callback(float value) { return (int)(value * 4.0f) + 7; }"
                                invoke = "int (*result)(float) = fetch_scalar(); return result(2.5f) != 17;"
                            else:
                                lane = ("float", "double", "unsigned char", "long long")[int(function[-1])]
                                callback = (f"typedef {lane} ExpectedVector __attribute__((vector_size(16)));\n"
                                    "static ExpectedVector callback(ExpectedVector value) { return value + value; }")
                                invoke = (f"ExpectedVector (*result)(ExpectedVector) = {function}(); "
                                    "ExpectedVector value = {0}; value[0] = 7; "
                                    "ExpectedVector doubled = result(value); "
                                    "return doubled[0] != 14 || doubled[1] != 0;")
                            generated.write_text(text + f"""
{storage_definition}
{callback}
int main(void) {{
  {setup}
  {invoke}
}}
""")
                            runnable = root / "check-callback"
                            subprocess.run([clang, "-std=gnu11", "-fno-strict-aliasing",
                                            "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
                                            str(generated), "-o", str(runnable)], check=True, capture_output=True)
                            subprocess.run([str(runnable)], check=True)
                            executions += 1
                # Y round-trips a variadic callback without dropping the
                # ellipsis in either its declaration or the refreshed view.
                address = ok(client, "resolve", {"query": "VariadicSlot"})["address"]
                query = {"address": address, "object_address": address}
                declaration = ok(client, "source_type", query)["type"]
                assert "..." in declaration, declaration
                ok(client, "source_type", {**query, "type": declaration})
                function = ok(client, "resolve", {"query": "fetch_variadic"})["address"]
                refreshed = ok(client, "decompile", {"address": function,
                    "representation": "c", "limit": 2048})["text"]
                assert "..." in refreshed, refreshed
                if architecture == "x86_64":
                    platform = ok(client, "resolve", {"query": "PlatformSlot"})["address"]
                    declared = ok(client, "source_type", {"address": platform,
                        "object_address": platform})["type"]
                    assert "ms_abi" in declared, declared
    print(f"callback declarations: {views} views, {syntax_checks} C syntax checks, "
          f"{executions} host executions; ARM64/x64 and Y passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1]))
