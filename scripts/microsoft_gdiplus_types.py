"""Project the Microsoft GDI+ flat C API from its C++ SDK declarations."""
from __future__ import annotations

import json
from pathlib import Path
import re

import build_wdk_driver_fixtures as wdk
import microsoft_ucrt_types as ucrt


# The SDK exposes private graphics objects as class pointers. These public
# data records additionally have directly addressable fields in the flat API.
DATA_RECORDS = {
    "BitmapData", "CharacterRange", "ColorMatrix", "ColorPalette",
    "GdiplusStartupInput", "GdiplusStartupOutput", "ImageCodecInfo",
    "PathData", "Point", "PointF", "PropertyItem", "Rect", "RectF",
}


def declarations(source: Path, compiler: list[str], directory: Path, run):
    """Inventory the flat exports independently of any generated header."""
    command = compiler + ["-fsyntax-only", "-Xclang", "-ast-dump=json",
                          "-Xclang", "-ast-dump-filter=Gdiplus", str(source)]
    dump = run(command, directory / "GDIPlus-ast.log")
    decoder = json.JSONDecoder()
    offset = 0
    types, functions = {}, {}

    def collect(node):
        name = node.get("name", "")
        kind = node["kind"]
        if kind == "LinkageSpecDecl" or (kind == "NamespaceDecl" and
                                        name in ("Gdiplus", "DllExports")):
            for child in node.get("inner", ()):
                collect(child)
        elif kind == "FunctionDecl" and name.startswith("Gdip"):
            # Namespace helpers with C++ linkage are not flat API entry points.
            decorated = node.get("mangledName", "")
            if decorated in (name, "_" + name) or re.fullmatch("_" + re.escape(name) + r"@\d+", decorated):
                functions[name] = node
        elif name and kind in ("EnumDecl", "CXXRecordDecl", "TypedefDecl"):
            if kind == "CXXRecordDecl" and not node.get("completeDefinition") and name in types:
                return
            if kind == "TypedefDecl" and name in types and node["type"]["qualType"] in (
                    "struct " + name, "class " + name, "enum " + name):
                return
            types[name] = node

    while offset < len(dump):
        if dump[offset].isspace():
            offset += 1
            continue
        node, offset = decoder.raw_decode(dump, offset)
        if node["kind"] == "NamespaceDecl" and node.get("name") == "Gdiplus":
            collect(node)
    if not {"GdipCreatePath", "GdipCreateEffect", "GdiplusStartup"} <= functions.keys():
        raise ValueError("Microsoft GDI+ flat SDK declarations are incomplete")
    return types, functions, command


def parameters(function):
    return [p for p in function.get("inner", ()) if p["kind"] == "ParmVarDecl"]


def supported(functions):
    # ISO C parameter types do not preserve the alignment, non-null and
    # dereferenceability attributes of a C++ lvalue reference in compiler IR.
    return sorted(name for name, fn in functions.items()
                  if all("&" not in p["type"]["qualType"] for p in parameters(fn)))


def import_library(output, cache, architecture, readobj):
    package = json.loads(ucrt.INPUTS.read_text(encoding="utf-8"))["import_packages"][architecture]
    archive = wdk.download(cache, package["file"], package["url"], package["sha256"])
    kit = output / "GDIPlus-imports"
    member = f"c/um/{architecture}/gdiplus.lib"
    wdk.extract([archive], kit, [member])
    library = kit / member
    return {"architecture": architecture, "library": str(library), "sha256": wdk.digest(library),
            "package": dict(package, architecture=architecture),
            "command": [readobj, "--coff-imports", str(library)]}


def imported_functions(functions, inputs, directory, run):
    """The DLL name and linker spelling both come from the target SDK import library."""
    if wdk.digest(Path(inputs["library"])) != inputs["sha256"]:
        raise ValueError("Microsoft GDI+ import library changed")
    fmt = ucrt.IMPORT_FORMATS[inputs["architecture"]]
    text = run(inputs["command"], directory / "GDIPlus-imports.log")
    imports = {}
    for block in re.split(r"(?m)^File: ", text)[1:]:
        if block.splitlines()[0].strip().lower() != "gdiplus.dll":
            continue
        fields = dict(re.findall(r"(?m)^([^:\n]+): (.*)$", block))
        if fields.get("Format") != fmt or fields.get("Type") != "code":
            continue
        name = fields.get("Export name")
        if not name:
            raise ValueError("GDI+ import member has no export name")
        imports.setdefault(name, set()).update(re.findall(r"(?m)^Symbol: (\S+)$", block))
    if not {"GdipCreatePath", "GdiplusStartup"} <= imports.keys():
        raise ValueError("GDI+ import inventory is incomplete")
    for name in functions.keys() & imports.keys():
        if functions[name]["mangledName"] not in imports[name]:
            raise ValueError(f"GDI+ compiler linkage differs from the SDK import library: {name}")
    return {name: fn for name, fn in functions.items() if name in imports}


def enum_members(node):
    previous = -1
    for member in node.get("inner", ()):
        if member["kind"] != "EnumConstantDecl":
            continue
        children = list(member.get("inner", ()))
        if children:
            while children and children[-1]["kind"] != "ConstantExpr":
                children.extend(children.pop().get("inner", ()))
            if not children or "value" not in children[-1]:
                raise ValueError("unevaluated GDI+ enumerator " + member["name"])
            previous = int(children[-1]["value"])
        else:
            previous += 1
        yield member["name"], previous


class CProjection:
    def __init__(self, types):
        self.types = types
        self.definitions = {}
        self.pending = set()
        self.assertions = []
        self.incomplete = []

    def spelling(self, text):
        def replace(match):
            name = match[0]
            if name not in self.types:
                return name
            self.define(name)
            return "Gdiplus_" + name
        return re.sub(r"\b[A-Za-z_]\w*\b", replace, text.replace("Gdiplus::", ""))

    def record(self, name, node, alias):
        if name not in DATA_RECORDS:
            self.incomplete.append(name)
            return f"typedef struct {alias} {alias};"
        if not node.get("definitionData", {}).get("isStandardLayout") or node.get("bases"):
            raise ValueError("GDI+ record cannot be projected as C: " + name)
        fields = []
        for field in node.get("inner", ()):
            if field["kind"] != "FieldDecl":
                continue
            key = field.get("name")
            if not key or field.get("isBitfield"):
                raise ValueError("unsupported field in GDI+ data record " + name)
            spelling = self.spelling(field["type"]["qualType"])
            base, _, array = spelling.partition("[")
            fields.append(f"    {base.rstrip()} {key}" + ("[" + array if array else "") + ";")
            self.assertions += [
                f"__builtin_offsetof(Gdiplus::{name}, {key}) == __builtin_offsetof({alias}, {key})",
                f"sizeof(Gdiplus::{name}::{key}) == sizeof({alias}::{key})",
            ]
        if not fields:
            raise ValueError("empty public GDI+ data record " + name)
        self.assertions += [f"sizeof(Gdiplus::{name}) == sizeof({alias})",
                            f"alignof(Gdiplus::{name}) == alignof({alias})"]
        return f"typedef struct {alias} {{\n" + "\n".join(fields) + f"\n}} {alias};"

    def define(self, name):
        if name in self.definitions:
            return
        if name in self.pending:
            raise ValueError("cyclic GDI+ typedef " + name)
        self.pending.add(name)
        node = self.types[name]
        alias = "Gdiplus_" + name
        if node["kind"] == "CXXRecordDecl":
            line = self.record(name, node, alias)
        elif node["kind"] == "EnumDecl":
            values = list(enum_members(node))
            if not values:
                raise ValueError("incomplete GDI+ enum " + name)
            entries = ",\n".join(f"    {alias}_{member} = {v}" for member, v in values)
            line = f"typedef enum {alias} {{\n{entries}\n}} {alias};"
            self.assertions.append(
                f"__is_same(__underlying_type(Gdiplus::{name}), __underlying_type({alias}))")
            self.assertions += [f"(long long)Gdiplus::{name}::{m} == (long long){alias}_{m}"
                                for m, _ in values]
        else:
            original = node["type"]["qualType"]
            if original == "struct " + name:
                self.incomplete.append(name)
                line = f"typedef struct {alias} {alias};"
            else:
                spelling = self.spelling(original)
                target = (spelling.replace("(*)", "(*" + alias + ")", 1)
                          if "(*)" in spelling else spelling + " " + alias)
                line = "typedef " + target + ";"
        self.definitions[name] = line
        self.pending.remove(name)

    def function(self, name, node):
        signature = node["type"]["qualType"]
        if "__attribute__((stdcall))" in signature:
            convention = "__stdcall"
        elif "__attribute__" not in signature or signature.endswith("__attribute__((cdecl))"):
            convention = "__cdecl"
        else:
            raise ValueError("unrecognized GDI+ SDK calling convention: " + name)
        result = self.spelling(signature.split("(", 1)[0].strip())
        args = [self.spelling(v["type"]["qualType"]) + " " + (v.get("name") or f"argument{i}")
                for i, v in enumerate(parameters(node))]
        return f"{result} {convention} {name}({', '.join(args) or 'void'});"


def prepare(source: Path, compiler: list[str], directory: Path, inputs, run):
    original = directory / "GDIPlus-sdk.cpp"
    original.write_text("#include <windows.h>\n#include <objidl.h>\n#include <gdiplus.h>\n"
                        "using namespace Gdiplus;\nusing namespace Gdiplus::DllExports;\n",
                        encoding="utf-8")
    cpp = compiler + ["-x", "c++", "-std=c++17"]
    # One x86 declaration lacks WINGDIPAPI. Use the SDK library's actual
    # callee-pops profile and check every decorated symbol, including this one.
    if inputs["architecture"] == "x86":
        cpp += ["-Xclang", "-fdefault-calling-conv=stdcall"]
    types, functions, ast = declarations(original, cpp, directory, run)
    imported = imported_functions(functions, inputs, directory, run)
    names = supported(imported)
    projection = CProjection(types)
    entries = [projection.function(name, functions[name]) for name in names]
    callbacks = sorted(name for name in projection.definitions
                       if types[name]["kind"] == "TypedefDecl" and
                       "(*)" in types[name]["type"].get("desugaredQualType",
                                                       types[name]["type"]["qualType"]))
    definitions = directory / "GDIPlus-types.h"
    definitions.write_text(source.read_text(encoding="utf-8") +
                           "\n".join(projection.definitions.values()) + "\n", encoding="utf-8")
    source.write_text('#include "GDIPlus-types.h"\n' + "\n".join(entries) + "\n",
                      encoding="utf-8")
    layouts = directory / "GDIPlus-original-types.cpp"
    layouts.write_text('#include "GDIPlus-sdk.cpp"\n#include "GDIPlus-types.h"\n' +
                       "".join(f"static_assert({test});\n" for test in projection.assertions),
                       encoding="utf-8")
    command = cpp + ["-fsyntax-only", str(layouts)]
    run(command, directory / "GDIPlus-original-types.log")
    return {"source": str(original), "source_sha256": wdk.digest(original),
            "arguments": cpp, "commands": [ast, inputs["command"], command],
            "import_library": inputs, "projected_functions": names,
            "callback_types": callbacks,
            "reference_parameters": sorted(imported.keys() - set(names)),
            "declarations_without_import": sorted(functions.keys() - imported.keys()),
            "incomplete_records": sorted(projection.incomplete), "layout_proof": str(layouts),
            "layout_proof_sha256": wdk.digest(layouts)}


def verify_abis(proof, header, compiler, directory, prototypes, run):
    names = proof["projected_functions"]
    if not set(names) <= set(prototypes):
        raise ValueError("published GDI+ function inventory is incomplete")
    refs = "".join(f"__typeof__(&{name}) neverd_ucrt_abi_{i} = &{name};\n"
                   for i, name in enumerate(names))
    callbacks = proof["callback_types"]
    aliases = ["neverd_gdiplus_callback_" + name for name in callbacks]
    contracts = []
    for language, include, arguments, prefix in (
            ("cpp", proof["source"], proof["arguments"], "Gdiplus::"),
            ("c", header, compiler, "Gdiplus_")):
        body = refs + "".join(f"extern __typeof__(*({prefix}{name})0) {alias};\n"
                              f"__typeof__(&{alias}) neverd_ucrt_abi_{i} = &{alias};\n"
                              for i, (name, alias) in enumerate(zip(callbacks, aliases), len(names)))
        if language == "cpp":
            body = 'extern "C" {\n' + body + '}\n'
        source = directory / ("GDIPlus-abi-" + language + ".c")
        source.write_text('#include "' + Path(include).as_posix() + '"\n' + body, encoding="utf-8")
        command = arguments + ["-S", "-emit-llvm", "-O0", str(source), "-o", "-"]
        ir = run(command, source.with_suffix(".log"), source.with_suffix(".ll"))
        contracts.append(ucrt.bridge_abis(ir, names + aliases))
        proof["commands"].append(command)
    differences = [name for name in names + aliases if contracts[0][name] != contracts[1][name]]
    if differences:
        raise ValueError("GDI+ C/C++ compiler ABI differs: " + ", ".join(differences))
    proof["verified_functions"] = list(names)
    proof["c_declaration_sha256"] = wdk.digest(header)
