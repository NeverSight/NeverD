"""Authenticate the WDK's RDBSS C declarations against its C++ interface.

WDK 26100 reuses the create-flags typedef for the low-I/O enum only in C.
Keep the original headers intact, repair that one preprocessed declaration,
and require original C++ compiler contracts before publishing the provider.
"""

from pathlib import Path
import re

import build_wdk_driver_fixtures as wdk
import microsoft_ucrt_types as ucrt


NO_ARGUMENTS = {
    "RxInitializeDispatcher": "NTSTATUS",
    "RxInitializeRxTimer": "NTSTATUS __stdcall",
    "RxTearDownDispatcher": "NTSTATUS",
}
COMPANION_FUNCTIONS = {
    "DuplicateConnectionInformation", "DuplicateTransportAddress", "GET_ALREADY_PREFIXED_NAME",
}


def bridge_enum_typedefs(c_source: str, cpp_source: str) -> str:
    pattern = re.compile(
        r"typedef enum\s*\{([^{}]*)\}\s*"
        r"(RX_CONTEXT_CREATE_FLAGS|RX_CONTEXT_LOWIO_FLAGS)\s*;")
    original = list(pattern.finditer(cpp_source))
    current = list(pattern.finditer(c_source))
    expected = {item[2]: re.sub(r"\s+", "", item[1]) for item in original}
    if (len(original) != 2 or len(expected) != 2 or len(current) != 2
            or len(set(expected.values())) != 2):
        raise ValueError("RDBSS requires two distinct official flag enums")
    replacements, seen = [], set()
    for item in current:
        body = re.sub(r"\s+", "", item[1])
        names = [name for name, value in expected.items() if body == value]
        if len(names) != 1 or names[0] in seen:
            raise ValueError("RDBSS C/C++ flag enum values differ")
        name = names[0]
        seen.add(name)
        if item[2] != name:
            if (item[2], name) != ("RX_CONTEXT_CREATE_FLAGS", "RX_CONTEXT_LOWIO_FLAGS"):
                raise ValueError("unrecognized RDBSS C flag typedef conflict")
            replacements.append((item.start(2), item.end(2), name))
    for begin, end, name in reversed(replacements):
        c_source = c_source[:begin] + name + c_source[end:]
    return c_source


def prepare(source: Path, preprocessed: Path, arguments: list[str],
            directory: Path, run) -> dict:
    cpp = directory / "RDBSS-original.cpp"
    cpp.write_text('extern "C" {\n' + source.read_text(encoding="utf-8") + '}\n',
                   encoding="utf-8")
    cpp_arguments = arguments + ["-x", "c++", "-std=c++17"]
    original = directory / "RDBSS-original-cpp.h"
    command = cpp_arguments + ["-E", str(cpp)]
    run(command, directory / "RDBSS-cpp-pre.log", original)
    c_source = preprocessed.read_text(encoding="utf-8")
    repaired = bridge_enum_typedefs(c_source, original.read_text(encoding="utf-8"))
    retained = directory / "RDBSS-original-c.h"
    retained.write_text(c_source, encoding="utf-8")
    # C's empty parentheses are not a prototype. Every redeclaration is
    # authenticated below against the unchanged C++ header's complete type.
    repaired += "\n" + "".join(f"{result} {name}(void);\n"
                                for name, result in NO_ARGUMENTS.items())
    preprocessed.write_text(repaired, encoding="utf-8")
    return {"source": str(cpp), "source_sha256": wdk.digest(cpp),
            "arguments": cpp_arguments, "commands": [command],
            "original_c_sha256": wdk.digest(retained),
            "original_cpp_sha256": wdk.digest(original),
            "enum_typedef": "RX_CONTEXT_LOWIO_FLAGS",
            "no_argument_functions": list(NO_ARGUMENTS)}


def verify_abis(proof: dict, preprocessed: Path, arguments: list[str],
                directory: Path, prototypes: list[str], run) -> None:
    names = sorted(name for name in prototypes
                   if name.startswith(("Rx", "_Rx", "__Rx")) or name in COMPANION_FUNCTIONS)
    if not names or not set(NO_ARGUMENTS) <= set(names):
        raise ValueError("RDBSS interface declarations are incomplete")
    aliases = [f"neverd_rdbss_contract_{index}" for index in range(len(names))]
    references = "".join(
        f"extern __typeof__({name}) {alias};\n"
        f"__typeof__(&{alias}) neverd_ucrt_abi_{index} = &{alias};\n"
        for index, (name, alias) in enumerate(zip(names, aliases)))
    contracts, inputs = [], []
    # A type-only external alias also covers inline SDK routines without
    # comparing their implementation or LLVM linkage with a declaration.
    for language, include, compiler in (
            ("cpp", Path(proof["source"]), proof["arguments"]),
            ("c", preprocessed, arguments)):
        source = directory / ("RDBSS-abi-" + language + ".c")
        body = references if language == "c" else 'extern "C" {\n' + references + '}\n'
        source.write_text('#include "' + include.as_posix() + '"\n' + body,
                          encoding="utf-8")
        ir = source.with_suffix(".ll")
        command = compiler + ["-S", "-emit-llvm", "-O0", str(source), "-o", "-"]
        text = run(command, source.with_suffix(".log"), ir)
        contracts.append(ucrt.bridge_abis(text, aliases))
        proof["commands"].append(command)
        inputs.append({"source": str(source), "sha256": wdk.digest(source),
                       "ir": str(ir), "ir_sha256": wdk.digest(ir)})
    differences = [name for name, alias in zip(names, aliases)
                   if contracts[0][alias] != contracts[1][alias]]
    if differences:
        raise ValueError("RDBSS C/C++ compiler ABI differs: " + ", ".join(differences))
    proof.update(verified_functions=names, compiler_inputs=inputs,
                 c_declaration_sha256=wdk.digest(preprocessed))
