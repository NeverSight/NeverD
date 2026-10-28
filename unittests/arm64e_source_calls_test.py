"""Compile explicit arm64e value calls through the public SDK and both C routes.

This checks declaration admission and source projection. It does not execute
PAC, authenticate a supplied prototype, or qualify native Apple arm64e.
"""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess


FIXTURES = json.loads(
    Path(__file__).with_name("Arm64eSourceCallFixtures.def").read_text()
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    assembly = FIXTURES["assembly_prefix"]
    for case in FIXTURES["cases"]:
        assembly += FIXTURES["assembly_entry"].format(name=case["name"])
        assembly += FIXTURES["arguments"][case["arguments"]]
        assembly += FIXTURES["call"]
        assembly += FIXTURES["results"][case["result"]]
        assembly += FIXTURES["assembly_exit"]
    source = args.artifacts / "calls.S"
    binary = args.artifacts / "calls.dylib"
    source.write_text(assembly)
    subprocess.run(
        [args.compiler, "-target", FIXTURES["triple"], "-dynamiclib",
         "-nostdlib", "-lSystem", str(source), "-o", str(binary)],
        check=True,
    )

    library = ctypes.CDLL(str(args.library.resolve()))
    pointer = ctypes.c_void_p
    library.neverd_session_create.restype = pointer
    library.neverd_session_destroy.argtypes = [pointer]
    library.neverd_last_error.argtypes = [pointer]
    library.neverd_last_error.restype = ctypes.c_char_p
    library.neverd_session_load.argtypes = [pointer, ctypes.c_char_p]
    library.neverd_session_load.restype = ctypes.c_int
    library.neverd_session_analyze.argtypes = [pointer]
    library.neverd_session_analyze.restype = ctypes.c_int
    library.neverd_local_types_declare.argtypes = [
        pointer, ctypes.c_char_p, ctypes.c_char_p]
    library.neverd_local_types_declare.restype = pointer
    library.neverd_func_count.argtypes = [pointer]
    library.neverd_func_count.restype = ctypes.c_int
    library.neverd_func_name.argtypes = [pointer, ctypes.c_int]
    library.neverd_func_name.restype = pointer
    library.neverd_func_entry.argtypes = [pointer, ctypes.c_int]
    library.neverd_func_entry.restype = ctypes.c_uint64
    library.neverd_ir_view_ex_json.argtypes = [
        pointer, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_size_t,
        ctypes.c_size_t, ctypes.c_char_p]
    library.neverd_ir_view_ex_json.restype = pointer
    library.neverd_free_string.argtypes = [pointer]
    session = library.neverd_session_create()
    assert session
    summary = []

    def take(value):
        assert value, library.neverd_last_error(session)
        try:
            return ctypes.string_at(value).decode()
        finally:
            library.neverd_free_string(value)

    def view(address, route, request=None):
        return json.loads(take(library.neverd_ir_view_ex_json(
            session, address, route.encode(), 0, FIXTURES["page_size"],
            json.dumps(request).encode() if request else None)))

    try:
        assert library.neverd_session_load(session, str(binary).encode()) == 1, (
            library.neverd_last_error(session))
        declared = json.loads(take(library.neverd_local_types_declare(
            session, FIXTURES["declarations"].encode(), None)))
        assert not declared.get("problems"), declared
        assert library.neverd_session_analyze(session) == 1, (
            library.neverd_last_error(session))
        symbols = {}
        for index in range(library.neverd_func_count(session)):
            name = take(library.neverd_func_name(session, index)).lstrip("_")
            symbols[name] = library.neverd_func_entry(session, index)
        for case in FIXTURES["cases"]:
            address = symbols[case["name"]]
            page = view(address, FIXTURES["routes"][0])
            assert "error" not in page, page
            assert len(page["source_calls"]) == 1, page
            request = {"call_types": [{
                "id": page["source_calls"][0]["id"],
                "snapshot": page["call_snapshot"],
                "declaration": case["declaration"],
            }]}
            for route in FIXTURES["routes"]:
                result = view(address, route, request)
                stem = case["name"] + "-" + route
                (args.artifacts / (stem + ".json")).write_text(
                    json.dumps(result, indent=2) + "\n")
                assert "error" not in result, result
                contracts = result["call_type_results"]
                assert len(contracts) == 1 and contracts[0]["applied"], result
                source = args.artifacts / (stem + ".c")
                source.write_text(result["text"])
                for optimization in FIXTURES["optimizations"]:
                    compiled = subprocess.run(
                        [args.compiler, "-target", FIXTURES["triple"],
                         *FIXTURES["compile_flags"], optimization, "-c",
                         str(source), "-o", str(args.artifacts / (
                             stem + optimization + ".o"))],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True,
                    )
                    (args.artifacts / (stem + optimization + ".log")).write_text(
                        compiled.stdout)
                    assert compiled.returncode == 0, (stem, compiled.stdout)
                summary.append({"name": case["name"], "route": route,
                                "applied": True,
                                "source_sha256": hashlib.sha256(
                                    source.read_bytes()).hexdigest()})
        address = symbols[FIXTURES["cases"][0]["name"]]
        page = view(address, FIXTURES["routes"][0])
        for declaration in FIXTURES["refused"]:
            request = {"call_types": [{
                "id": page["source_calls"][0]["id"],
                "snapshot": page["call_snapshot"],
                "declaration": declaration,
            }]}
            for route in FIXTURES["routes"]:
                result = view(address, route, request)
                assert "error" not in result, result
                contracts = result["call_type_results"]
                assert len(contracts) == 1 and not contracts[0]["applied"], result
                summary.append({"declaration": declaration, "route": route,
                                "applied": False,
                                "contract": contracts[0]})
    finally:
        library.neverd_session_destroy(session)
        (args.artifacts / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"compiled_source_routes":
                      len(FIXTURES["cases"]) * len(FIXTURES["routes"]),
                      "refused_routes":
                      len(FIXTURES["refused"]) * len(FIXTURES["routes"])}))


if __name__ == "__main__":
    main()
