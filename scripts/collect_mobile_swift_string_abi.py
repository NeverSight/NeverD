#!/usr/bin/env python3
"""Retain compiler evidence for an exact Swift String boolean import.

Fixed device/simulator acquisition on macOS Actions. No application code or
recovery engine is executed, and no declaration is installed by this script.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

SDK_VERSION = "26.5"
TARGETS = (("iphoneos", "arm64-apple-ios18.0"),
           ("iphonesimulator", "arm64-apple-ios18.0-simulator"))
SYMBOL = "$ss27_stringCompareWithSmolCheck__9expectingSbs11_StringGutsV_ADs01_G16ComparisonResultOtF"
TYPES = ("i64", "ptr", "i64", "ptr", "i8")
MAX_BYTES = 16 * 1024 * 1024
SWIFT_SOURCE = """@inline(never) public func equalStrings(_ lhs: String, _ rhs: String) -> Bool { lhs == rhs }
@inline(never) public func lessStrings(_ lhs: String, _ rhs: String) -> Bool { lhs < rhs }
"""
C_SOURCE = """#include <stdint.h>
extern _Bool neverd_compare(uint64_t, void *, uint64_t, void *, uint8_t)
  __asm__(\"_""" + SYMBOL + """\") __attribute__((swiftcall));
uint8_t compare_byte(uint64_t a, void *b, uint64_t c, void *d, uint8_t e) {
  return neverd_compare(a, b, c, d, e);
}
"""
PREFIX_SYMBOL = "$sSS9hasPrefixySbSSF"
PREFIX_TYPES = ("i64", "ptr", "i64", "ptr")
PREFIX_SWIFT_SOURCE = """@inline(never) public func prefixStrings(_ value: String, _ prefix: String) -> Bool { value.hasPrefix(prefix) }
"""
PREFIX_C_SOURCE = """#include <stdint.h>
extern _Bool neverd_has_prefix(uint64_t, void *, uint64_t, void *)
  __asm__(\"_""" + PREFIX_SYMBOL + """\") __attribute__((swiftcall));
uint8_t prefix_byte(uint64_t a, void *b, uint64_t c, void *d) {
  return neverd_has_prefix(a, b, c, d);
}
"""


def probe_inputs(probe):
    if probe == "comparison":
        return SYMBOL, TYPES, SWIFT_SOURCE, C_SOURCE
    if probe == "prefix":
        return PREFIX_SYMBOL, PREFIX_TYPES, PREFIX_SWIFT_SOURCE, PREFIX_C_SOURCE
    raise ValueError("unknown fixed Swift String probe")


def validate_ir(text, language, target=None, probe="comparison"):
    """Require the actual fixed call and declaration, including the i1 result."""
    symbol, types, _, _ = probe_inputs(probe)
    if language not in {"swift", "c"} or len(text.encode()) > MAX_BYTES:
        raise ValueError("invalid IR profile or size")
    if target is not None:
        triples = re.findall(r'^target triple = "([^"]+)"$', text, re.M)
        # Clang canonicalizes the deployment version to 18.0.0; Swift may not.
        expected = {target, target.replace("ios18.0", "ios18.0.0")}
        if len(triples) != 1 or triples[0] not in expected:
            raise ValueError("compiler IR has a different target triple")
    name = symbol if language == "swift" else r"\01_" + symbol
    quoted = re.escape('"' + name + '"')
    decls = re.findall(r'^declare swiftcc i1 @' + quoted +
                       r'\(([^\n]*)\)[^\n]*$', text, re.M)
    if len(decls) != 1:
        raise ValueError("exact swiftcc i1 declaration is absent or duplicated")
    parameters = [re.fullmatch(r'(i64|ptr|i8)(?: noundef)?', value.strip())
                  for value in decls[0].split(',')]
    if any(value is None for value in parameters) or tuple(
            value.group(1) for value in parameters) != types:
        raise ValueError("declaration has a different or hidden argument ABI")
    calls = re.findall(r'(%[A-Za-z0-9._-]+) = (?:tail )?call swiftcc i1 @' + quoted +
                       r'\(([^\n]*)\)', text)
    if len(calls) != (2 if language == "swift" and probe == "comparison" else 1):
        raise ValueError("probe did not call the exact declaration")
    modes = set()
    for result, call in calls:
        parameters = [re.fullmatch(r'(i64|ptr|i8)(?: noundef)? (%[A-Za-z0-9._-]+|[0-9]+)',
                                   value.strip()) for value in call.split(',')]
        if any(value is None for value in parameters) or tuple(
                value.group(1) for value in parameters) != types:
            raise ValueError("call arguments disagree with the declaration")
        if any(not value.group(2).startswith('%') for value in parameters[:4]):
            raise ValueError("probe does not pass its dynamic String arguments")
        if probe == "comparison":
            modes.add(parameters[-1].group(2))
    if language == "swift" and probe == "comparison" and modes != {"0", "1"}:
        raise ValueError("equality and ordering modes were not independently observed")
    if language == "c":
        normalized = re.findall(r'(%[A-Za-z0-9._-]+) = zext i1 ' +
                                re.escape(calls[0][0]) + r' to i8\b', text)
        if len(normalized) != 1 or not re.search(
                r'\bret i8 ' + re.escape(normalized[0]) + r'(?:\s|$)', text):
            raise ValueError("C probe does not return its normalized one-bit result")
    return {"calling_convention": "swiftcc", "return": "i1",
            "parameters": list(types), "call_count": len(calls),
            "modes": sorted(modes) if language == "swift" else []}


def retained_file(path, root=None, nonempty=True):
    resolved = path.resolve(strict=True)
    if (root is not None and not resolved.is_relative_to(root.resolve(strict=True))) or not resolved.is_file():
        raise ValueError("evidence file is outside its declared root")
    size = resolved.stat().st_size
    if size > MAX_BYTES or (nonempty and not size):
        raise ValueError("evidence file is empty or exceeds its budget")
    data = resolved.read_bytes()
    if len(data) != size:
        raise ValueError("evidence file changed during retention")
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def tool_identity(path, developer):
    resolved = Path(path).resolve(strict=True)
    if not resolved.is_relative_to(developer) or not resolved.is_file():
        raise ValueError("compiler is outside the selected Xcode")
    size = resolved.stat().st_size
    if not 0 < size <= 1024 * 1024 * 1024:
        raise ValueError("compiler identity exceeds its budget")
    digest, count = hashlib.sha256(), 0
    with resolved.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
            count += len(block)
            if count > size:
                raise ValueError("compiler changed while hashing")
    if count != size:
        raise ValueError("compiler changed while hashing")
    return {"path": path, "resolved_path": str(resolved), "size": size,
            "sha256": digest.hexdigest()}


def collect(output, probe="comparison"):
    symbol, _, swift_source, c_source = probe_inputs(probe)
    if os.environ.get("GITHUB_ACTIONS") != "true" or sys.platform != "darwin":
        raise RuntimeError("Swift SDK evidence must run on macOS GitHub Actions")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    evidence = {"schema_version": 1, "scope": "swift-string-call-abi-only",
                "status": "incomplete", "consumer_commit": os.environ.get("CONSUMER_COMMIT"),
                "probe": probe, "symbol": symbol, "sdk_version": SDK_VERSION, "commands": [], "profiles": []}
    end = time.monotonic() + 480

    def command(name, argv):
        out, err = output / (name + ".stdout"), output / (name + ".stderr")
        record = {"name": name, "argv": [str(x) for x in argv], "exitcode": None}
        evidence["commands"].append(record)
        environment = os.environ.copy()
        for key in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                    "SDKROOT", "SWIFT_EXEC", "SWIFT_DRIVER_SWIFT_FRONTEND_EXEC"):
            environment.pop(key, None)
        try:
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Swift SDK evidence exceeded its deadline")
            with out.open("wb") as stdout, err.open("wb") as stderr:
                process = subprocess.run(argv, cwd=output, env=environment, stdout=stdout,
                                         stderr=stderr, timeout=min(120, remaining), check=False)
            record["exitcode"] = process.returncode
            if process.returncode:
                raise RuntimeError(f"{name} exited {process.returncode}; see retained logs")
        except Exception as error:
            record["error"] = str(error)
            raise
        finally:
            for key, path in (("stdout", out), ("stderr", err)):
                if path.exists():
                    try:
                        record[key] = {"path": path.name, **retained_file(path, output, False)}
                    except Exception as error:
                        record.setdefault("retention_errors", []).append(str(error))
        if record.get("retention_errors"):
            raise ValueError("command evidence could not be completely retained")
        return out.read_text().strip()

    def copy_evidence(source, root, name):
        identity = retained_file(source, root)
        target = output / name
        target.write_bytes(source.read_bytes())
        if retained_file(target, output) != identity:
            raise ValueError("evidence changed during retention")
        return {"source_path": str(source), "path": name, **identity}

    try:
        commit = evidence["consumer_commit"]
        if not isinstance(commit, str) or not re.fullmatch(r'[0-9a-f]{40}', commit):
            raise ValueError("consumer commit must be an exact Git revision")
        developer = Path(os.environ["DEVELOPER_DIR"]).resolve(strict=True)
        if not developer.is_dir():
            raise ValueError("selected Xcode developer directory is absent")
        evidence["developer_dir"] = str(developer)
        evidence["xcode_version"] = command("xcode-version", ["xcodebuild", "-version"])
        if not evidence["xcode_version"].startswith("Xcode " + SDK_VERSION + "\n"):
            raise ValueError("unexpected Xcode version")
        for sdk, target in TARGETS:
            profile = {"sdk": sdk, "target": target, "status": "incomplete", "files": []}
            evidence["profiles"].append(profile)
            prefix = ["xcrun", "--toolchain", "XcodeDefault", "--sdk", sdk]
            version = command(sdk + "-version", [*prefix, "--show-sdk-version"])
            if version != SDK_VERSION:
                raise ValueError("unexpected SDK version")
            root = Path(command(sdk + "-path", [*prefix, "--show-sdk-path"])).resolve(strict=True)
            if not root.is_relative_to(developer):
                raise ValueError("SDK is outside the selected Xcode")
            profile["sdk_root"] = str(root)
            profile["files"].append(copy_evidence(root / "SDKSettings.json", root, sdk + "-SDKSettings.json"))
            profile["files"].append(copy_evidence(root / "usr/lib/swift/libswiftCore.tbd", root, sdk + "-libswiftCore.tbd"))
            swift, clang = (command(sdk + "-" + tool + "-path", [*prefix, "--find", tool])
                            for tool in ("swiftc", "clang"))
            profile["tools"] = [tool_identity(path, developer) for path in (swift, clang)]
            profile["swift_version"] = command(sdk + "-swift-version", [swift, "--version"])
            profile["clang_version"] = command(sdk + "-clang-version", [clang, "--version"])
            for language, source, suffix, compiler in (("swift", swift_source, ".swift", swift),
                                                        ("c", c_source, ".c", clang)):
                name = sdk + "-" + language
                path = output / (name + suffix)
                path.write_text(source)
                profile["files"].append({"path": path.name, **retained_file(path, output)})
                args = ([compiler, "-O", "-module-name", "NeverDStringABI", "-target", target, "-sdk", root]
                        if language == "swift" else [compiler, "-O2", "-x", "c", "-std=gnu11", "-Werror",
                                                      "-target", target, "-isysroot", root])
                ir = output / (name + ".ll")
                assembly = output / (name + ".s")
                emit_ir = ["-emit-ir"] if language == "swift" else ["-S", "-emit-llvm"]
                command(name + "-ir", [*args, *emit_ir, path, "-o", ir])
                profile["files"].append({"path": ir.name, **retained_file(ir, output)})
                profile[language + "_abi"] = validate_ir(ir.read_text(), language, target, probe)
                command(name + "-assembly", [*args, "-S", path, "-o", assembly])
                profile["files"].append({"path": assembly.name, **retained_file(assembly, output)})
            profile["status"] = "complete"
        evidence["status"] = "complete"
    except Exception as error:
        evidence["status"] = "failed"
        evidence["error"] = str(error)
    finally:
        # Retain partial compiler outputs even when a compiler or validator failed.
        evidence["retained_files"] = []
        for path in sorted(output.iterdir()):
            if path.is_file() and path.name != "manifest.json":
                try:
                    evidence["retained_files"].append({"path": path.name, **retained_file(path, output, False)})
                except Exception as error:
                    evidence["status"] = "failed"
                    evidence.setdefault("retention_errors", []).append({"path": path.name, "error": str(error)})
        (output / "manifest.json").write_text(json.dumps(evidence, indent=2) + "\n")
    return evidence["status"] == "complete"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--probe", choices=("comparison", "prefix"), default="comparison")
    args = parser.parse_args()
    return 0 if collect(args.output, args.probe) else 1


if __name__ == "__main__":
    raise SystemExit(main())
