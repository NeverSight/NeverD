#!/usr/bin/env python3
"""Extract fixed C declaration and export facts from a Darwin SDK.

Generation requires the SDK's libclang and PyYAML. The generated catalog is
self-contained and carries no SDK implementation or build-time dependency.
Declarations must agree between macOS and iOS preprocessing environments;
exports and reexports establish which exact library may supply each symbol.
Public inputs include Foundation, graphics, file and vector I/O attributes,
notifications, uniform type identifiers, logging and digests.
"""
import argparse
import ctypes
import json
from pathlib import Path
import tempfile

try:
    from .generate_objc_declarations import Clang, CXCursor, CXString, TARGETS, catalog_rows, framework_module_aliases
except ImportError:
    from generate_objc_declarations import Clang, CXCursor, CXString, TARGETS, catalog_rows, framework_module_aliases


class CXType(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_uint), ("data", ctypes.c_void_p * 2)]


class CDeclarations(Clang):
    def __init__(self, library):
        super().__init__(library)
        self.bind("clang_getCursorType", CXType, CXCursor)
        self.bind("clang_getFunctionTypeCallingConv", ctypes.c_uint, CXType)
        self.bind("clang_getCursorLinkage", ctypes.c_uint, CXCursor)
        self.bind("clang_Cursor_isFunctionInlined", ctypes.c_uint, CXCursor)
        self.bind("clang_Cursor_getMangling", CXString, CXCursor)

    def declaration(self, cursor):
        # Only an external function declaration can describe an import.
        if cursor.kind != 8 or self.clang_getCursorLinkage(cursor) != 4:
            return None
        name = self.string(self.clang_Cursor_getMangling(cursor))
        if not name.startswith("_"):
            return None
        function = self.clang_getCursorType(cursor)
        # Preserve negative evidence for variadic, non-prototyped, non-C and
        # inline declarations. Source emission does not reproduce inline code.
        encoding = ""
        if (function.kind == 111 and
                self.clang_getFunctionTypeCallingConv(function) == 1 and
                not self.clang_Cursor_isVariadic(cursor) and
                not self.clang_Cursor_isFunctionInlined(cursor)):
            encoding = self.string(self.clang_getDeclObjCTypeEncoding(cursor))
        return name[1:], encoding


def export_index(documents, target):
    """Resolve architecture-specific reexports to a fixed point, including cycles."""
    exports, dependencies = {}, {}
    for document in documents:
        if document.get("tbd-version") != 4:
            raise ValueError("only SDK TBD version 4 is supported")
        if target not in document.get("targets", []):
            continue
        module = document["install-name"]
        symbols = exports.setdefault(module, set())
        for group in document.get("exports", []) + document.get("reexports", []):
            if target in group.get("targets", []):
                symbols.update(group.get("symbols", []))
        for group in document.get("reexported-libraries", []):
            if target in group.get("targets", []):
                dependencies.setdefault(module, set()).update(group.get("libraries", []))
    changed = True
    while changed:
        changed = False
        for module, dependencies_for_module in dependencies.items():
            before = len(exports[module])
            for dependency in dependencies_for_module:
                exports[module].update(exports.get(dependency, ()))
            changed |= before != len(exports[module])
    result = {}
    for module, symbols in exports.items():
        for symbol in symbols:
            if symbol.startswith("_"):
                result.setdefault(symbol[1:], set()).update(framework_module_aliases(module))
    return result


def load_exports(sdk, extra_frameworks=(), targets=("arm64-macos", "x86_64-macos")):
    import yaml

    class TBDLoader(yaml.SafeLoader):
        pass

    TBDLoader.add_constructor(
        "!tapi-tbd", lambda loader, node: loader.construct_mapping(node, deep=True))
    # These public libraries and their SDK-listed reexports cover the headers
    # below. Other frameworks can supply declarations but cannot gain a binding
    # without their own export evidence.
    frameworks = sdk / "System/Library/Frameworks"
    paths = [sdk / "usr/lib/libSystem.tbd", sdk / "usr/lib/libobjc.tbd",
             frameworks / "Foundation.framework/Versions/C/Foundation.tbd",
             frameworks / "CoreFoundation.framework/Versions/A/CoreFoundation.tbd"]
    paths.extend(sorted((sdk / "usr/lib/system").glob("*.tbd")))
    for name in extra_frameworks:
        # Restrict requested declarations to one public SDK framework path.
        module = f"/System/Library/Frameworks/{name}.framework/Versions/A/{name}"
        if len(framework_module_aliases(module)) != 2:
            raise ValueError("invalid framework name")
        paths.append(frameworks / f"{name}.framework/{name}.tbd")
    documents = []
    for path in paths:
        documents.extend(yaml.load_all(path.read_text(), Loader=TBDLoader))
    return [export_index(documents, target) for target in targets]


def render(profiles, exports, version, compiler):
    lines = ["// clang-format off", "// Generated by scripts/generate_darwin_declarations.py.",
             f"// Compiler-derived C ABI and export facts: MacOSX SDK {version}.",
             f"// {compiler}", "// Target profiles: " + ", ".join(TARGETS),
             "// No SDK implementation is included. Null encodings supply no binding."]
    count = 0
    for name, arm, intel in catalog_rows(profiles):
        modules = ["|".join(sorted(index.get(name, ()))) for index in exports]
        if not any(modules):
            continue
        values = [name, arm, intel, *modules]
        lines.append("{" + ", ".join(
            "nullptr" if value is None else json.dumps(value)
            for value in values) + "},")
        count += 1
    lines.append("    // clang-format on")
    return "\n".join(lines) + "\n", count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--libclang", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    clang = CDeclarations(args.libclang)
    frameworks = ("CoreGraphics", "ImageIO")
    with tempfile.TemporaryDirectory(prefix="neverd-darwin-declarations-") as work:
        source = Path(work) / "declarations.m"
        source.write_text(
            "#import <Foundation/Foundation.h>\n#include <objc/runtime.h>\n"
            "#include <objc/objc-sync.h>\n#include <pthread.h>\n"
            "#include <dispatch/dispatch.h>\n#include <notify.h>\n"
            "#include <sys/uio.h>\n#include <sys/xattr.h>\n"
            "#include <os/log.h>\n#include <asl.h>\n"
            "#include <CommonCrypto/CommonDigest.h>\n"
            "#import <LaunchServices/UTType.h>\n" +
            "".join(f"#import <{name}/{name}.h>\n" for name in frameworks))
        nested_frameworks = (sdk / "System/Library/Frameworks/"
                             "CoreServices.framework/Frameworks")
        profiles = [clang.extract(source, sdk, target,
                                  ("-F", str(nested_frameworks)))
                    for target in TARGETS]
    version = json.loads((sdk / "SDKSettings.json").read_text())["Version"]
    exports = load_exports(sdk, frameworks)
    core_services = load_exports(sdk, ("CoreServices",))
    for common, extra in zip(exports, core_services):
        common["UTTypeConformsTo"] = extra["UTTypeConformsTo"]
    output, count = render(profiles, exports, version,
                           clang.string(clang.clang_getClangVersion()))
    if args.check:
        if args.output.read_text() != output:
            parser.error("generated catalog differs; regenerate with this SDK")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="w", dir=args.output.parent,
                                         delete=False) as temporary:
            temporary.write(output)
            staging = Path(temporary.name)
        try:
            staging.replace(args.output)
        finally:
            staging.unlink(missing_ok=True)
    print(f"verified {count} common C declaration rows with export evidence")


if __name__ == "__main__":
    main()
