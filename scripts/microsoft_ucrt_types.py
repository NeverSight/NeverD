"""Prepare an exact UCRT export provider from pinned Microsoft inputs.

This module does not infer signatures from exports. Import libraries select
declarations from the target's headers and identify documented OS-state twins.
An optional, separate Wine provider supplies reviewed missing declarations.
All downloaded sources, notices and generated declarations remain local.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import shutil

import build_wdk_driver_fixtures as wdk


INPUTS = Path(__file__).with_name("MicrosoftUCRTInputs.json")
HEADERS = (
    "assert", "complex", "conio", "crtdbg", "ctype", "direct", "errno", "fenv",
    "float", "fpieee", "io", "inttypes", "locale", "malloc", "math", "mbctype",
    "mbstring", "new", "process", "search", "setjmp", "signal", "stdio", "stdlib",
    "string", "time", "uchar", "wchar", "wctype", "sys/stat", "sys/timeb",
    "sys/types", "sys/utime",
)
IMPORT_LIBRARIES = ("ucrt", "ucrt.osmode", "ucrt.osmode_permissive")
IMPORT_FORMATS = {"x86": "COFF-import-file-i386", "x64": "COFF-import-file-x86-64",
                  "arm64": "COFF-import-file-ARM64"}
DLL_FORMATS = {"x86": ("COFF-i386",), "x64": ("COFF-x86-64",),
               "arm64": ("COFF-ARM64", "COFF-ARM64X")}
ARM64X_DOCUMENTATION = "https://learn.microsoft.com/en-us/windows/arm/arm64x-pe"
OSMODE_DOCUMENTATION = (
    "https://learn.microsoft.com/cpp/c-runtime-library/global-state?view=msvc-170")
SOURCE_FUNCTIONS = {
    "__initialize_lconv_for_unsigned_char": "locale/lconv_unsigned_char_initialization.cpp",
    "__wcsncnt": "string/wcsncnt.cpp",
    "_assert": "startup/assert.cpp",
    "_cgets": "conio/cgets.cpp",
    "_cgetws": "conio/cgetws.cpp",
    "gets": "stdio/gets.cpp",
    "_getws": "stdio/gets.cpp",
}
CPP_FUNCTIONS = ("terminate", "set_terminate", "_get_terminate",
                 "__std_exception_copy", "__std_exception_destroy", "__std_type_info_name",
                 "__std_type_info_compare", "__std_type_info_hash", "unexpected",
                 "set_unexpected", "_get_unexpected", "_set_se_translator",
                 "__uncaught_exception", "__uncaught_exceptions",
                 "_CxxThrowException", "__CxxExceptionFilter", "__CxxDetectRethrow",
                 "__CxxQueryExceptionSize", "__CxxRegisterExceptionObject",
                 "__CxxUnregisterExceptionObject", "__RTDynamicCast", "__RTtypeid",
                 "__RTCastToVoid")
CPP_TYPEDEFS = ("terminate_handler", "unexpected_handler", "_se_translator_function")
CPP_RECORDS = ("__std_exception_data", "__std_type_info_data")


def c_declaration(text: str) -> str:
    """Project only the reviewed C-linkage declarations, not C++ objects/EH."""
    text = re.sub(r'^extern\s+"C"\s+', "", text.strip())
    text = re.sub(r"\s+throw\s*\(\s*\)$", "", text)
    text = re.sub(r"\s+noexcept(?:\s*\(\s*(?:true|false)\s*\))?$", "", text)
    text = re.sub(r"\bbool\b", "_Bool", text)
    # C++ empty parameters mean no arguments, unlike old-style C ().
    return re.sub(r"\(\s*\)", "(void)", text) + ";\n"


def source_declarations(root: Path) -> tuple[str, list[dict]]:
    declarations, evidence = [], []
    for name, relative in SOURCE_FUNCTIONS.items():
        path = root / relative
        text = path.read_text(encoding="utf-8-sig")
        pattern = (r'^extern\s+"C"\s+[^{};]+?\b' + re.escape(name) +
                   r'\s*\([^{};]*?\)(?:\s*throw\s*\(\s*\))?\s*\{')
        matches = list(re.finditer(pattern, text, re.M))
        if len(matches) != 1:
            raise ValueError(f"expected one official UCRT definition: {name}")
        match = matches[0]
        original = match[0].rstrip()[:-1].strip()
        declarations.append(c_declaration(original))
        evidence.append({"name": name, "file": relative, "sha256": wdk.digest(path),
                         "line": text.count("\n", 0, match.start()) + 1,
                         "declaration": original})
    return "".join(declarations), evidence


def global_declarations(ast: dict):
    # Deliberately do not enter classes, templates or namespaces: their
    # methods and similarly named declarations cannot supply these C APIs.
    for node in ast.get("inner", []):
        if node["kind"] == "LinkageSpecDecl":
            yield from global_declarations(node)
        else:
            yield node


def ast_declaration(source: bytes, node: dict) -> str:
    begin, end = node["range"]["begin"], node["range"]["end"]
    if "offset" not in begin or "offset" not in end:
        raise ValueError("UCRT declaration has no preprocessed source range")
    first, last = begin["offset"], end["offset"] + end["tokLen"]
    # Clang's typedef range may end at the name before its function suffix.
    if node["kind"] == "TypedefDecl":
        last = source.index(b";", last)
    if not 0 <= first < last <= len(source):
        raise ValueError("UCRT declaration source range is invalid")
    text = source[first:last].decode("utf-8")
    if any(char in text for char in "{};"):
        raise ValueError("UCRT bridge accepts declarations only")
    return text


def cpp_declarations(source: bytes, ast: dict) -> str:
    nodes = list(global_declarations(ast))

    def find(name, kind, complete=False):
        found = [node for node in nodes if node.get("name") == name and node["kind"] == kind
                 and (not complete or node.get("completeDefinition"))]
        if len(found) != 1:
            raise ValueError(f"expected one official UCRT declaration: {name}")
        return found[0]

    output = ["struct _EXCEPTION_POINTERS;\n",
              "typedef struct _ThrowInfo _ThrowInfo;\n"]
    output += [c_declaration(ast_declaration(source, find(name, "TypedefDecl")))
               for name in CPP_TYPEDEFS]
    find("__type_info_node", "CXXRecordDecl")
    output.append("typedef struct __type_info_node __type_info_node;\n")
    for name in CPP_RECORDS:
        record = find(name, "CXXRecordDecl", True)
        data = record.get("definitionData", {})
        if record.get("bases") or not data.get("isStandardLayout") or not data.get("isTrivial"):
            raise ValueError(f"UCRT C bridge requires a trivial standard-layout record: {name}")
        output.append(f"typedef struct {name} {{\n")
        fields = [node for node in record["inner"] if node["kind"] == "FieldDecl"]
        if not fields or any(field.get("isBitfield") for field in fields):
            raise ValueError(f"unsupported UCRT C bridge record fields: {name}")
        for field in fields:
            output.append(c_declaration(ast_declaration(source, field)))
        output.append(f"}} {name};\n")
    for name in CPP_FUNCTIONS:
        node = find(name, "FunctionDecl")
        symbol = node.get("mangledName", "")
        stdcall = ("stdcall" in node.get("type", {}).get("qualType", "") and
                   re.fullmatch("_" + re.escape(name) + r"@\d+", symbol))
        if symbol not in (name, "_" + name) and not stdcall:
            raise ValueError(f"UCRT bridge requires C linkage: {name}")
        output.append(c_declaration(ast_declaration(source, node)))
    return "".join(output)


def bridge_abis(ir: str, names=CPP_FUNCTIONS) -> dict:
    """Check compiler ABI types and conventions, including decorated stdcall.

    Address-taking keeps every declaration in IR. Exception specifications and
    optimization attributes are not C ABI types; parameter/return attributes
    such as zeroext are retained in the compared declaration.
    """
    declarations = {}
    for match in re.finditer(r'(?m)^declare (.+?) (@(?:"[^"\n]+"|[\w.$-]+))'
                             r'\(((?:[^()\n]|\([^()\n]*\))*)\)', ir):
        prefix, symbol, parameters = match.groups()
        declarations[symbol] = (prefix, parameters)
    result = {}
    for index, name in enumerate(names):
        binding = re.findall(r'(?m)^@neverd_ucrt_abi_' + str(index) +
                             r' = [^\n]*?global ptr (@(?:"[^"\n]+"|[\w.$-]+)),', ir)
        if len(binding) != 1 or binding[0] not in declarations:
            raise ValueError(f"UCRT bridge has no unique compiler ABI: {name}")
        result[name] = (binding[0], *declarations[binding[0]])
    return result


def import_names(text: str, architecture: str) -> list[dict]:
    """Read explicit export names, never decorated linker symbols or EC twins."""
    records = set()
    for block in re.split(r"(?m)^File: ", text)[1:]:
        fields = dict(re.findall(r"(?m)^([^:\n]+): (.*)$", block))
        if fields.get("Format") != IMPORT_FORMATS[architecture]:
            continue
        if fields.get("Type") != "code":
            continue
        name = fields.get("Export name")
        if not name:
            raise ValueError("llvm-readobj must report explicit COFF Export name fields")
        # This provider describes C exports. A future decorated C++ export
        # needs a different declaration identity, not a guessed C spelling.
        if not re.fullmatch(r"[A-Za-z_]\w*", name, re.ASCII):
            raise ValueError(f"unsupported UCRT export spelling: {name}")
        module = block.splitlines()[0].strip().lower()
        if not re.fullmatch(r"[a-z0-9_.-]+\.dll", module):
            raise ValueError("UCRT import member has no DLL identity")
        records.add((module, name))
    if not records:
        raise ValueError(f"no UCRT code imports for {architecture}")
    return [{"module": module, "export": name} for module, name in sorted(records)]


def select_exports(imports: list[dict], prototypes: list[str]) -> tuple[list[str], dict, list[str]]:
    declared = set(prototypes)
    selected, aliases, missing = set(), {}, set()
    for record in imports:
        name = record["export"]
        if name in declared:
            selected.add(name)
        elif name.startswith("_o_") and name[3:] in declared:
            aliases[name] = name[3:]
            selected.add(name)
        else:
            missing.add(name)
    return sorted(selected), dict(sorted(aliases.items())), sorted(missing)


def dll_exports(text: str, architecture: str) -> tuple[list[dict], list[dict]]:
    """Inventory the actual DLL, keeping non-executable exports out of APIs.

    An RVA in an executable section is a candidate, not a declaration or an
    ABI proof. Forwarders, data, and unmapped RVAs remain separately visible.
    """
    if len(re.findall(r"(?m)^File: ", text)) != 1:
        raise ValueError("expected one UCRT DLL inventory")
    # Microsoft documents the default ARM64X view as native ARM64. LLVM also
    # prints a transformed ARM64EC HybridObject; it is not this target's ABI.
    text = re.split(r"(?m)^HybridObject \{", text, maxsplit=1)[0]
    formats = re.findall(r"(?m)^Format: (.+)$", text)
    if len(formats) != 1 or formats[0] not in DLL_FORMATS[architecture]:
        raise ValueError("UCRT DLL architecture does not match the target")
    sections = []
    for block in re.findall(r"(?ms)^  Section \{\n(.*?)^  \}", text):
        fields = dict(re.findall(r"(?m)^    (\w+): ([^\n]+)$", block))
        start, size = int(fields["VirtualAddress"], 0), int(fields["VirtualSize"], 0)
        if start < 0 or size < 0 or start + size > 2**32:
            raise ValueError("invalid UCRT section extent")
        sections.append((start, start + size, "IMAGE_SCN_MEM_EXECUTE" in block))
    if not sections:
        raise ValueError("UCRT DLL has no section inventory")
    code, other, names = [], [], set()
    for block in re.findall(r"(?ms)^Export \{\n(.*?)^\}", text):
        fields = dict(re.findall(r"(?m)^  (\w+): ?([^\n]*)$", block))
        name = fields["Name"]
        if not name:
            continue
        if name in names:
            raise ValueError("duplicate UCRT DLL export name")
        names.add(name)
        address = int(fields["RVA"], 0)
        owners = [execute for start, end, execute in sections if start <= address < end]
        if len(owners) > 1:
            raise ValueError("ambiguous UCRT export section")
        row = {"module": "ucrtbase.dll", "export": name, "rva": address,
               "ordinal": int(fields["Ordinal"], 0)}
        (code if owners == [True] else other).append(row)
    if not code:
        raise ValueError("UCRT DLL has no executable named exports")
    return sorted(code, key=lambda row: row["export"]), sorted(other, key=lambda row: row["export"])


def prepare_compatibility(runtime: dict, output: Path, cache: Path) -> dict | None:
    """A separate provider, eligible only for names absent from Microsoft."""
    inventory = json.loads(INPUTS.read_text(encoding="utf-8"))["compatibility"]
    directory = output / "UCRT-inputs"
    declarations, evidence = [], []
    names = []
    for record in inventory["sources"]:
        path = wdk.download(cache, record["file"], record["url"], record["sha256"])
        text = path.read_text(encoding="utf-8")
        name = record["name"]
        matches = list(re.finditer(r"^\w+ CDECL " + re.escape(name) + r"\([^{};]*\)\s*\{", text, re.M))
        if len(matches) != 1:
            raise ValueError(f"expected one reviewed Wine definition: {name}")
        original = matches[0][0].rstrip()[:-1].strip()
        declarations.append(re.sub(r"\bCDECL\b", "__cdecl", original) + ";\n")
        names.append(name)
        evidence.append(dict(record, declaration=original,
                             line=text.count("\n", 0, matches[0].start()) + 1))
        # Retain the original source and its copyright/license notice outside
        # the repository. No implementation body enters the declaration unit.
        shutil.copy2(path, directory / path.name)
        notice = text[:text.index("*/") + 2]
        (output / "licenses" / (record["file"] + ".notice.txt")).write_text(notice + "\n", encoding="utf-8")
    notice = inventory["license"]
    path = wdk.download(cache, notice["file"], notice["url"], notice["sha256"])
    shutil.copy2(path, output / "licenses" / path.name)
    missing = runtime["metadata"]["unresolved_exports"]
    selected, aliases, remaining = select_exports([{"export": name} for name in missing], names)
    if not selected:
        return None
    for alias, original in aliases.items():
        declarations.append(f"__typeof__({original}) {alias};\n")
    source = directory / "UCRTCompatibility.c"
    source.write_text("#include <windows.h>\n#include <fpieee.h>\n" +
                      "".join(declarations), encoding="utf-8")
    return {"source": source, "arguments": runtime["arguments"], "prototypes": selected,
            "metadata": {"provider": inventory["provider"], "revision": inventory["revision"],
                         "sources": evidence, "license": notice, "aliases": aliases,
                         "alias_documentation": OSMODE_DOCUMENTATION,
                         "unresolved_exports": remaining}}


def prepare(output: Path, cache: Path, sdk: Path, sdk_include: Path,
            common: list[str], architecture: str, version: str, readobj: str,
            neverd: str, model: str, run, checked_types, verify_layouts) -> dict:
    """Return a source unit and complete acquisition/selection provenance."""
    inventory = json.loads(INPUTS.read_text(encoding="utf-8"))
    directory = output / "UCRT-inputs"
    directory.mkdir(exist_ok=True)
    packages = [inventory["vc_headers"], inventory["import_packages"][architecture]]
    downloaded = [wdk.download(cache, row["file"], row["url"], row["sha256"])
                  for row in packages]
    kit = directory / "kit"
    selections = [inventory["vc_include"] + "/", f"c/Include/{version}/ucrt/"]
    selections += [f"c/ucrt/{architecture}/{name}.lib" for name in IMPORT_LIBRARIES]
    dll_relative = f"c/Redist/{version}/ucrt/DLLs/{architecture}/ucrtbase.dll"
    selections.append(dll_relative)
    selections += sorted({f"c/Source/{version}/ucrt/{path}" for path in SOURCE_FUNCTIONS.values()})
    inputs = wdk.extract([sdk, *downloaded], kit, selections)
    notice = inventory["vc_license"]
    license_file = wdk.download(cache, notice["file"], notice["url"], notice["sha256"])
    shutil.copy2(license_file, output / "licenses" / license_file.name)
    overlay = directory / "headers-vfs.json"
    overlay.write_text(json.dumps({
        "version": 0, "case-sensitive": False,
        "roots": [wdk.overlay_directory(kit, True)],
    }, indent=2) + "\n", encoding="utf-8")
    arguments = common + ["-nostdinc", "-ivfsoverlay", str(overlay), "-D_CRT_RAND_S=1"]
    for include in [kit / inventory["vc_include"], kit / f"c/Include/{version}/ucrt",
                    sdk_include / "shared", sdk_include / "um"]:
        arguments += ["-I", str(include)]
    cpp = directory / "UCRT.cpp"
    cpp.write_text("#include <corecrt_terminate.h>\n#include <vcruntime_typeinfo.h>\n"
                   "#include <eh.h>\n"
                   # MSVC supplies this opaque compiler type to its EH header.
                   "struct _ThrowInfo;\n#include <ehdata_forceinclude.h>\n"
                   "#include <rttidata.h>\n", encoding="utf-8")
    cpp_input = directory / "UCRT.ii"
    commands = [arguments + ["-E", "-P", str(cpp)]]
    run(commands[-1], directory / "cpp-pre.log", cpp_input)
    commands.append(arguments + ["-std=c++14", "-fsyntax-only", "-Xclang", "-ast-dump=json",
                                 "-x", "c++", str(cpp_input)])
    ast = json.loads(run(commands[-1], directory / "cpp-ast.log", directory / "cpp-ast.json"))
    bridge = cpp_declarations(cpp_input.read_bytes(), ast)
    definitions, evidence = source_declarations(kit / f"c/Source/{version}/ucrt")
    source = directory / "UCRT.c"
    source.write_text("".join(f"#include <{name}.h>\n" for name in HEADERS) +
                      bridge + definitions + "\n".join(
                          row["declaration"] for row in inventory["documented_functions"].values()) +
                      "\n", encoding="utf-8")
    dump = ["-fsyntax-only", "-Xclang", "-fdump-record-layouts-complete"]
    cpp_layouts = run(arguments + ["-std=c++14"] + dump + [str(cpp)], directory / "cpp-layouts.log")
    c_layouts = run(arguments + dump + [str(source)], directory / "c-layouts.log")
    bridge_records = verify_layouts(cpp_layouts, c_layouts, required=list(CPP_RECORDS))
    if bridge_records != len(CPP_RECORDS):
        raise ValueError("UCRT C bridge did not verify every C++ record")
    references = "".join(f"__typeof__(&{name}) neverd_ucrt_abi_{i} = &{name};\n"
                          for i, name in enumerate(CPP_FUNCTIONS))
    compiled_abis = []
    for language, original in (("cpp", cpp), ("c", source)):
        probe = directory / ("bridge-abi." + language)
        probe.write_text(original.read_text(encoding="utf-8") +
                         ('extern "C" {\n' if language == "cpp" else "") + references +
                         ("}\n" if language == "cpp" else ""), encoding="utf-8")
        command = arguments + ["-S", "-emit-llvm", str(probe), "-o", "-"]
        commands.append(command)
        compiled_abis.append(bridge_abis(run(command, directory / (language + "-abi.log"),
                                            directory / (language + "-abi.ll"))))
    if compiled_abis[0] != compiled_abis[1]:
        raise ValueError("UCRT C bridge changes a compiler ABI")
    preprocessed = directory / "UCRT.pre.h"
    commands.append(arguments + ["-E", str(source)])
    run(commands[-1], directory / "pre.log", preprocessed)
    parsed = checked_types(neverd, preprocessed, directory / "parse.log", model)
    libraries, imports = [], []
    for name in IMPORT_LIBRARIES:
        path = kit / f"c/ucrt/{architecture}/{name}.lib"
        command = [readobj, "--coff-imports", str(path)]
        text = run(command, directory / (name + ".readobj.log"),
                   directory / (name + ".imports.txt"))
        records = import_names(text, architecture)
        libraries.append({"file": name + ".lib", "sha256": wdk.digest(path),
                          "imports": records})
        imports.extend(records)
        commands.append(command)
    dll = kit / dll_relative
    command = [readobj, "--coff-exports", "--sections", str(dll)]
    text = run(command, directory / "ucrtbase.readobj.log",
               directory / "ucrtbase.exports.txt")
    code_exports, nonexecutable_exports = dll_exports(text, architecture)
    imports.extend(code_exports)
    commands.append(command)
    selected, aliases, missing = select_exports(imports, parsed["prototypes"])
    if not selected:
        raise ValueError("UCRT imports matched no declared prototypes")
    with source.open("a", encoding="utf-8") as stream:
        for alias, original in aliases.items():
            stream.write(f"__typeof__({original}) {alias};\n")
    return {"source": source, "arguments": arguments, "prototypes": selected,
            "packages": [packages[0]],
            "target_packages": [dict(packages[1], architecture=architecture)],
            "metadata": {"inputs_definition_sha256": wdk.digest(INPUTS),
                         "inputs": inputs, "headers": HEADERS,
                         "source_declarations": evidence,
                         "documented_functions": inventory["documented_functions"],
                         "cpp_declarations": CPP_FUNCTIONS,
                         "cpp_typedefs": CPP_TYPEDEFS,
                         "cpp_preprocessed_sha256": wdk.digest(cpp_input),
                         "cpp_bridge_verified_records": bridge_records,
                         "cpp_bridge_verified_abis": compiled_abis[0],
                         "libraries": libraries, "aliases": aliases,
                         "dll": {"file": dll_relative, "sha256": wdk.digest(dll),
                                 "format": re.findall(r"(?m)^Format: (.+)$", text)[0],
                                 "view": "default; ARM64EC hybrid view excluded",
                                 "arm64x_documentation": ARM64X_DOCUMENTATION,
                                 "executable_named_exports": code_exports,
                                 "nonexecutable_exports": nonexecutable_exports},
                         "alias_documentation": OSMODE_DOCUMENTATION,
                         "unresolved_exports": missing,
                         "readobj": run([readobj, "--version"], directory / "readobj.log"),
                         "commands": commands}}
