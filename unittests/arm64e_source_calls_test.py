"""Compile explicit arm64e value calls through the public SDK and both C routes.

This checks declaration admission and source projection. Raw variadic cases
also execute against independently compiled va_arg callbacks on Apple arm64.
It does not execute PAC or authenticate a supplied prototype.
"""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess


FIXTURES = json.loads(
    Path(__file__).with_name("Arm64eSourceCallFixtures.def").read_text()
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument(FIXTURES["cleanup"]["keep_option"], action="store_true",
                        help=FIXTURES["cleanup"]["keep_help"])
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    assembly = FIXTURES["assembly_prefix"]
    for case in FIXTURES["cases"]:
        variadic = "variadic_types" in case
        assembly += FIXTURES[case.get("entry", "assembly_variadic_entry" if variadic else
                             "assembly_entry")].format(name=case["name"])
        assembly += FIXTURES["arguments"][case["arguments"]]
        assembly += FIXTURES[case.get("call", "call")]
        assembly += FIXTURES["results"][case["result"]]
        assembly += FIXTURES[case.get("exit", "assembly_variadic_exit" if variadic else
                             "assembly_exit")]
    assembly += FIXTURES["assembly_suffix"]
    original = args.artifacts / "calls.S"
    binary = args.artifacts / "calls.dylib"
    products = [binary]
    original.write_text(assembly)
    subprocess.run(
        [args.compiler, "-target", FIXTURES["triple"], "-dynamiclib",
         "-nostdlib", "-lSystem", str(original), "-o", str(binary)],
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

    native = [platform.system(), platform.machine()] == FIXTURES["native_host"]

    def execute_unwind(case, source, route, prototype, parameter_types):
        if not case.get("unwind"):
            return 0
        policy = FIXTURES["unwind"]
        harness = args.artifacts / policy["harness"].format(name=case["name"], route=route)
        harness.write_text(policy["source"].format(prototype=prototype, name=case["name"],
                                                   target=parameter_types[0],
                                                   modifier=parameter_types[1],
                                                   storage=parameter_types[2]))
        for optimization in FIXTURES["optimizations"]:
            stem = policy["stem"].format(name=case["name"], route=route, optimization=optimization)
            executable = args.artifacts / stem
            obj = executable.with_suffix(".o")
            commands = [
                [args.compiler, "-target", FIXTURES["native_triple"],
                 *FIXTURES["native_flags"], optimization, "-c", str(source), "-o", str(obj)],
                [args.compiler, "-target", FIXTURES["native_triple"],
                 *policy["flags"], optimization, str(harness), str(obj), "-o", str(executable)],
                [str(executable)],
            ]
            with (args.artifacts / (stem + ".log")).open("w") as log:
                for command in commands:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
                    assert result.returncode == 0, (stem, result.returncode)
            products.extend((obj, executable))
        summary.append({"name": case["name"], "route": route,
                        "native_unwind_executions": len(FIXTURES["optimizations"]),
                        "signed_unwind_qualified": False})
        return len(FIXTURES["optimizations"])

    def execute_variadic(case, source, route):
        if "callback" not in case or not native:
            return 0
        values = [*case.get("native_header_arguments",
                            FIXTURES["native_header_arguments"]), *case["inputs"]]
        parameter_types = [FIXTURES["native_parameter"]] * len(values)
        prototype = FIXTURES["native_prototype"].format(
            name=case["name"], parameters=", ".join(
                parameter_types[len(FIXTURES["native_header_arguments"]):]))
        if source != original:
            signature = re.search(FIXTURES["native_signature"].format(
                name=re.escape(case["name"])), source.read_text())
            assert signature, source
            prototype = signature[1] + ";\n"
            parameter_types = [re.sub(FIXTURES["native_parameter_name"], "", p).strip()
                               for p in signature[2].split(",")]
            assert len(parameter_types) == len(values), prototype
            assert all(t in FIXTURES["native_entry_types"] for t in parameter_types), prototype
        arguments = ", ".join(FIXTURES["native_argument"].format(type=t, value=value)
                              for t, value in zip(parameter_types, values))
        harness = args.artifacts / FIXTURES["native_callback_file"].format(
            name=case["name"])
        harness.write_text(
            FIXTURES["native_header"] + FIXTURES["declarations"] + "\n" +
            prototype +
            case["callback"] + FIXTURES["native_main"].format(
                name=case["name"], arguments=arguments,
                expected=case["expected"]))
        for optimization in FIXTURES["optimizations"]:
            stem = case["name"] + "-" + route + optimization
            executable = args.artifacts / stem
            compiled = subprocess.run(
                [args.compiler, "-target", FIXTURES["native_triple"],
                 *FIXTURES["native_flags"], optimization, str(source),
                 str(harness), "-o", str(executable)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True)
            (args.artifacts / (stem + ".log")).write_text(compiled.stdout)
            assert compiled.returncode == 0, (stem, compiled.stdout)
            executed = subprocess.run([str(executable)], stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True)
            assert executed.returncode == 0, (stem, executed.returncode,
                                              executed.stdout)
            products.append(executable)
        execute_unwind(case, source, route, prototype, parameter_types)
        return len(FIXTURES["optimizations"])

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
                "variadic_types": case.get("variadic_types", []),
            }]}
            if "callback" in case:
                summary.append({"name": case["name"], "route": "original",
                                "native_executions": execute_variadic(
                                    case, original, "original"),
                                "native_execution_available": native})
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
                    obj = args.artifacts / (stem + optimization + ".o")
                    compiled = subprocess.run(
                        [args.compiler, "-target", FIXTURES["triple"],
                         *FIXTURES["compile_flags"], optimization, "-c",
                         str(source), "-o", str(obj)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True,
                    )
                    (args.artifacts / (stem + optimization + ".log")).write_text(
                        compiled.stdout)
                    assert compiled.returncode == 0, (stem, compiled.stdout)
                    products.append(obj)
                summary.append({"name": case["name"], "route": route,
                                "applied": True,
                                "native_executions": execute_variadic(
                                    case, source, route),
                                "source_sha256": hashlib.sha256(
                                    source.read_bytes()).hexdigest()})
        address = symbols[FIXTURES["cases"][0]["name"]]
        page = view(address, FIXTURES["routes"][0])
        for declaration in FIXTURES["refused"]:
            edit = ({"declaration": declaration} if isinstance(declaration, str)
                    else declaration)
            request = {"call_types": [{
                "id": page["source_calls"][0]["id"],
                "snapshot": page["call_snapshot"],
                **edit,
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
    product_records = {
        path.relative_to(args.artifacts).as_posix(): {
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "bytes": path.stat().st_size,
        }
        for path in products
    }
    (args.artifacts / FIXTURES["cleanup"]["manifest"]).write_text(
        json.dumps({"retained": args.keep_products, "products": product_records},
                   indent=2) + "\n")
    if not args.keep_products:
        for path in products:
            path.unlink()
    print(json.dumps({"compiled_source_routes":
                      len(FIXTURES["cases"]) * len(FIXTURES["routes"]),
                      "refused_routes":
                      len(FIXTURES["refused"]) * len(FIXTURES["routes"])}))


if __name__ == "__main__":
    main()
