#!/usr/bin/env python3
"""Execute and relift a compiler-generated PE32 realigned catch callback.

This is a generated frame/analysis probe, not source-rewrite evidence. Both
preferred and forced bases must pass; a mismatched-result control must fail.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import image_digest, run_image
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_eh import image_digest, run_image
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import load_libraries

SOURCE = (Path(__file__).resolve().parents[1] / "unittests/lift/eh/fixtures/"
          "registration_realigned_driver.cpp")


def require_test_result(path: Path) -> int:
    root = ET.parse(path).getroot()
    count = int(root.get("tests", "0"))
    if count <= 0 or any(int(root.get(key, "0"))
                        for key in ("failures", "errors", "disabled")) or \
            root.findall(".//skipped"):
        raise ValueError("realigned callback checks failed or skipped")
    return count


def validate_decompilation(text: str, language: str) -> None:
    if "highir.structured_regions=1, fallback_regions=0" not in text or \
            text.count("= __neverd_x86_callback_esp(0x") != 1:
        raise ValueError("public output lost the checked callback body or entry input")
    calls = re.findall(r"^\s*callback_increment\((.*)\);$", text, re.M)
    if len(calls) != 1 or not calls[0].strip():
        raise ValueError("public output lost the callback's object argument")
    depth = 0
    for char in calls[0]:
        depth += (char == "(") - (char == ")")
        if depth < 0 or (char == "," and depth == 0):
            raise ValueError("public output mistook a private spill for an argument")
    if depth:
        raise ValueError("public output has an unbalanced call expression")
    if language == "c" and ("Native x86 callback @" not in text or
                            "goto L_x86_eh_after_" not in text):
        raise ValueError("C output lost the separate native callback entry")
    if language == "cpp" and "catch (...)" not in text:
        raise ValueError("C++ output lost the catch clause")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--neverd", type=Path,
                        help="public CLI; defaults to the test binary's sibling")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True,
                        help="native x86 MSVC libraries captured by the source-fixture job")
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["WINEDEBUG"] = "-all"
    if args.wine_prefix:
        env["WINEARCH"] = "win32"
        env["WINEPREFIX"] = str(args.wine_prefix.resolve())
    report = {"schema": 1, "evidence": "generated-realigned-callback-analysis",
              "passed": False, "commands": [], "images": []}

    def run(command: list[str], extra: dict | None = None) -> None:
        result = subprocess.run(command, env=env | (extra or {}), cwd=out,
                                capture_output=True, text=True, errors="replace",
                                timeout=args.timeout, check=False)
        report["commands"].append({"command": command, "exit_code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("realigned probe build or analysis failed")

    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if not compiler or not linker or (os.name != "nt" and not wine):
            raise ValueError("required compiler, linker or Wine is unavailable")
        libraries, report["runtime_libraries"] = load_libraries(args.runtime_libs.resolve())
        binary = str(args.test_binary.resolve())
        neverd = (args.neverd or args.test_binary.with_name(
            "neverd.exe" if os.name == "nt" else "neverd")).resolve()
        if not neverd.is_file():
            raise ValueError("the public neverd CLI is required for callback output checks")
        run([binary, "--gtest_filter=WindowsRegistrationRealigned.EmitsIndependentCallbackFrame:"
             "WindowsRegistrationRealigned.RuntimeRootsKeepInvocationIdentity:"
             "WindowsRegistrationHighCallback.EntryIdentityIncludesTheInvocation",
             "--gtest_output=xml:" + str(out / "emit.xml")],
            {"NEVERD_REGISTRATION_REALIGNED_OBJECT": str(out / "frame.obj")})
        if require_test_result(out / "emit.xml") != 3:
            raise ValueError("frame emission or runtime-root identity check is missing")
        report["source_sha256"] = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
        report["object_sha256"] = hashlib.sha256((out / "frame.obj").read_bytes()).hexdigest()
        for control in (False, True):
            name = "wrong-result" if control else "probe"
            run([compiler, "--target=i686-pc-windows-msvc", "-fms-extensions",
                 "-fexceptions", "-fcxx-exceptions", "-fno-omit-frame-pointer", "-O1",
                 "-DEXPECTED_RESULT=" + ("8" if control else "7"), "-c", str(SOURCE),
                 "-o", str(out / (name + ".obj"))])
            image = out / (name + ".exe")
            run([linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86",
                 "/subsystem:console", "/fixed:no", "/dynamicbase:no", "/out:" + str(image),
                 str(out / (name + ".obj")), str(out / "frame.obj"),
                 *map(str, libraries)])
            rebased = out / (name + "-rebased.exe")
            rebased.write_bytes(PE32(image.read_bytes()).rebase(0x18000000))
            for path in (image, rebased):
                runtime = run_image(path, [wine] if wine else [], env, args.timeout)
                record = {"image": path.name, "sha256": image_digest(path),
                          "expected_exit": int(control), "runtime": runtime}
                report["images"].append(record)
                if runtime.get("exit_code") != int(control):
                    raise ValueError("callback frame runtime/control mismatch")
                if not control:
                    xml = out / (path.stem + ".xml")
                    run([binary, "--gtest_filter=WindowsRegistrationRealigned.InputPE32RecoversTheCallbackContract:"
                         "WindowsRegistrationHighCallback.InputPE32BindsCurrentCallbackRoots:"
                         "WindowsRegistrationHighCallback.InputPE32CollectsTheWholeCallbackCFG",
                         "--gtest_output=xml:" + str(xml)],
                        {"NEVERD_REGISTRATION_REALIGNED_PE32": str(path)})
                    record["analysis_tests"] = require_test_result(xml)
                    pe = PE32(path.read_bytes())
                    entry = pe.base + pe.entry(b"callback_parent")
                    record["decompilation"] = {}
                    for language in ("c", "cpp"):
                        source = out / (path.stem + ".decompiled." + language)
                        run([str(neverd), "decompile", str(path), "--func=" + hex(entry),
                             "--language=" + language, "-o", str(source)])
                        validate_decompilation(source.read_text(), language)
                        if language == "c":
                            run([compiler, "-x", "c", "-std=c11", "-fsyntax-only",
                                 "-Werror=implicit-function-declaration", str(source)])
                        record["decompilation"][language] = {
                            "source": source.name,
                            "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                            "syntax_checked": language == "c"}
        report["passed"] = True
    except (OSError, ValueError, struct.error, subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    (out / "realigned-callback.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(out / "realigned-callback.json"),
                      "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
