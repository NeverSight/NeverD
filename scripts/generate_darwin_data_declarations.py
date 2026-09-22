#!/usr/bin/env python3
"""Extract common external, non-TLS data declarations and SDK export facts.

The generated catalog records linker identities, not object contents or SDK
implementations. Generation requires Clang, libclang and PyYAML; consumers do not.
"""
import argparse
import ctypes
import json
from pathlib import Path
import re
import subprocess
import tempfile

try:
    from .generate_darwin_declarations import CDeclarations, load_exports
    from .generate_objc_declarations import CXCursor, TARGETS, catalog_rows
except ImportError:
    from generate_darwin_declarations import CDeclarations, load_exports
    from generate_objc_declarations import CXCursor, TARGETS, catalog_rows


class DataDeclarations(CDeclarations):
    def __init__(self, library):
        super().__init__(library)
        self.bind("clang_getCursorTLSKind", ctypes.c_uint, CXCursor)

    def declaration(self, cursor):
        if cursor.kind != 9 or self.clang_getCursorLinkage(cursor) != 4:
            return None
        name = self.string(self.clang_Cursor_getMangling(cursor))
        if not name.startswith("_"):
            return None
        # A TLS symbol requires thread-specific access and cannot use an
        # ordinary external storage address. Keep its negative evidence.
        return name[1:], "data" if self.clang_getCursorTLSKind(cursor) == 0 else ""


LITERAL_PROBES = {
    "neverd_literal_array": "@[]",
    "neverd_literal_dictionary": "@{}",
    "neverd_literal_true": "@YES",
    "neverd_literal_false": "@NO",
}

LEGACY_LITERAL_PROBES = {
    "neverd_legacy_literal_array": "@[]",
    "neverd_legacy_literal_dictionary": "@{}",
}


def literal_storage_declarations(ir):
    """Read only direct addresses of external non-TLS data in compiler output."""
    globals_by_name = {}
    for name, declaration in re.findall(r"^@([A-Za-z_][A-Za-z_0-9]*) = (.*)$", ir, re.M):
        globals_by_name.setdefault(name, []).append(declaration)
    result = {}
    for probe in LITERAL_PROBES:
        bodies = re.findall(r"^define [^\n]*@" + probe +
                            r"\(\)[^\n]*\{\n(.*?)^\}", ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError(f"missing or ambiguous literal probe: {probe}")
        body = [line.strip() for line in bodies[0].splitlines()
                if line.strip() and not line.lstrip().startswith(";")]
        if body and body[0] == "entry:":
            body.pop(0)
        returned = re.fullmatch(r"ret ptr @([A-Za-z_][A-Za-z_0-9]*)",
                                body[0]) if len(body) == 1 else None
        declarations = globals_by_name.get(returned[1], []) if returned else []
        if len(declarations) != 1 or not re.fullmatch(
                r"external global ptr(?: #[0-9]+)?(?:, align [0-9]+)?",
                declarations[0]):
            raise ValueError(f"literal probe does not return external data: {probe}")
        result[returned[1]] = {"data"}
    return result


def legacy_literal_storage_declarations(ir):
    """Read the external storage loaded by legacy empty collection literals."""
    globals_by_name = {}
    for name, declaration in re.findall(r"^@([A-Za-z_][A-Za-z_0-9]*) = (.*)$", ir, re.M):
        globals_by_name.setdefault(name, []).append(declaration)
    result = {}
    for probe in LEGACY_LITERAL_PROBES:
        bodies = re.findall(r"^define [^\n]*@" + probe +
                            r"\(\)[^\n]*\{\n(.*?)^\}", ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError(f"missing or ambiguous legacy literal probe: {probe}")
        loads = re.findall(
            r"^\s*(%[A-Za-z_0-9.]+) = load ptr, ptr @([A-Za-z_][A-Za-z_0-9]*),",
            bodies[0], re.M)
        returned = [(value, name) for value, name in loads
                    if re.search(r"^\s*ret ptr " + re.escape(value) + r"\s*$",
                                 bodies[0], re.M)]
        if len(returned) != 1:
            raise ValueError(f"legacy literal probe has no unique storage load: {probe}")
        declarations = globals_by_name.get(returned[0][1], [])
        if len(declarations) != 1 or not re.fullmatch(
                r"external (?:local_unnamed_addr )?global ptr"
                r"(?: #[0-9]+)?(?:, align [0-9]+)?",
                declarations[0]):
            raise ValueError(f"legacy literal probe does not load external data: {probe}")
        result[returned[0][1]] = {"data"}
    return result


def compile_literal_storage(compiler, sdk, target):
    source = "#import <Foundation/Foundation.h>\n" + "".join(
        f"id {name}(void) {{ return {value}; }}\n"
        for name, value in LITERAL_PROBES.items())
    result = subprocess.run(
        [str(compiler), "-x", "objective-c", "-target", target, "-isysroot", str(sdk),
         "-O2", "-g0", "-fobjc-arc", "-fobjc-constant-literals", "-S", "-emit-llvm",
         "-o", "-", "-"], input=source, text=True, capture_output=True, timeout=60,
        check=True)
    return literal_storage_declarations(result.stdout)


def compile_legacy_literal_storage(compiler, sdk, target):
    source = "#import <Foundation/Foundation.h>\n" + "".join(
        f"id {name}(void) {{ return {value}; }}\n"
        for name, value in LEGACY_LITERAL_PROBES.items())
    runtime = "macosx-10.14" if "-macos" in target else "ios-12.0"
    result = subprocess.run(
        [str(compiler), "-x", "objective-c", "-target", target, "-isysroot", str(sdk),
         "-O2", "-g0", "-fobjc-arc", f"-fobjc-runtime={runtime}", "-S",
         "-emit-llvm", "-o", "-", "-"], input=source, text=True,
        capture_output=True, timeout=60, check=True)
    return legacy_literal_storage_declarations(result.stdout)


def render(profiles, exports, version, compiler, literal_compiler=None):
    lines = ["// clang-format off",
             "// Generated by scripts/generate_darwin_data_declarations.py.",
             f"// Compiler-derived external data/export facts: MacOSX SDK {version}.",
             f"// {compiler}", "// Target profiles: " + ", ".join(TARGETS),
             "// No SDK implementation or object contents are included."]
    if literal_compiler:
        lines.append(f"// Literal storage identities emitted by {literal_compiler}.")
    count = 0
    for name, arm, intel in catalog_rows(profiles):
        if not any(name in index for index in exports):
            continue
        modules = ["|".join(sorted(index.get(name, ()))) if kind == "data" else ""
                   for kind, index in zip((arm, intel), exports)]
        lines.append("{" + ", ".join(json.dumps(x) for x in (name, *modules)) + "},")
        count += 1
    lines.append("    // clang-format on")
    return "\n".join(lines) + "\n", count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--libclang", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True,
                        help="compiler used to extract built-in literal storage identities")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    clang = DataDeclarations(args.libclang)
    frameworks = ("CoreData", "CoreGraphics", "ImageIO", "CoreSpotlight", "QuartzCore",
                  "CoreImage")
    with tempfile.TemporaryDirectory(prefix="neverd-darwin-data-") as work:
        source = Path(work) / "declarations.m"
        source.write_text(
            "#import <Foundation/Foundation.h>\n#include <objc/runtime.h>\n"
            "#include <objc/objc-sync.h>\n#include <pthread.h>\n"
            "#include <dispatch/dispatch.h>\n#include <os/log.h>\n"
            "#import <LaunchServices/UTType.h>\n"
            "#import <LaunchServices/UTCoreTypes.h>\n" +
            # CALayer supplies the public layer constants without pulling in
            # OpenGLES headers absent from the command-line-tools SDK.
            "".join(f"#import <{name}/{'CALayer' if name == 'QuartzCore' else name}.h>\n"
                    for name in frameworks if name != "CoreImage") +
            # These public CoreImage headers are complete on every target.
            # The umbrella also imports CIContext, whose iOS OpenGLES headers
            # are absent from the command-line-tools SDK.
            "#import <CoreImage/CIDetector.h>\n#import <CoreImage/CIFilter.h>\n"
            "#import <CoreImage/CIImage.h>\n")
        nested_frameworks = (sdk / "System/Library/Frameworks/"
                             "CoreServices.framework/Frameworks")
        profiles = [clang.extract(source, sdk, target,
                                  ("-F", str(nested_frameworks)))
                    for target in TARGETS]
        for profile, target in zip(profiles, TARGETS):
            for name, declarations in compile_literal_storage(args.clang, sdk, target).items():
                profile.setdefault(name, set()).update(declarations)
            for name, declarations in compile_legacy_literal_storage(
                    args.clang, sdk, target).items():
                profile.setdefault(name, set()).update(declarations)
    compiler_version = subprocess.run(
        [str(args.clang), "--version"], text=True, capture_output=True,
        timeout=30, check=True).stdout.splitlines()[0]
    version_match = re.search(
        r"(?:Apple )?clang version [A-Za-z_0-9.]+(?: \(clang-[0-9.]+\))?",
        compiler_version)
    if not version_match:
        raise ValueError("unrecognized literal compiler version")
    version = json.loads((sdk / "SDKSettings.json").read_text())["Version"]
    exports = load_exports(sdk, frameworks)
    core_services = load_exports(sdk, ("CoreServices",))
    for common, extra in zip(exports, core_services):
        for name in ("kUTTagClassFilenameExtension", "kUTTypeImage"):
            common[name] = extra[name]
    output, count = render(profiles, exports, version,
                           clang.string(clang.clang_getClangVersion()), version_match[0])
    if args.check:
        if args.output.read_text() != output:
            parser.error("generated data catalog differs; regenerate with this SDK")
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
    print(f"verified {count} external data declaration rows with export evidence")


if __name__ == "__main__":
    main()
