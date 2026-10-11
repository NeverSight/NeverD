#!/usr/bin/env python3
"""Reconstruct PE32 bound/unbound catches in fixed and realigned source frames."""
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
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_realigned import require_test_result
    from .check_windows_registration_cxx_rewrite import code_owner
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_realigned import require_test_result
    from check_windows_registration_cxx_rewrite import code_owner
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import load_libraries

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_realigned_rewrite_driver.cpp"
EMITTER = ROOT / "unittests/lift/eh/WindowsRegistrationRealignedNativeTests.cpp"
DIRECT_EMITTER = ROOT / "unittests/lift/eh/WindowsRegistrationDirectNativeTests.cpp"
BASES = (0x400000, 0x18000000)
ROUTES = ("original", "product", "cli-section", "cli-inplace")
INCOMING_PROOFS = (
    "WindowsRegistrationIncoming.EntryRequiresObservedPhysicalWords",
    "WindowsRegistrationIncoming.ProjectsRegisterAndCalleeCleanupEntries",
    "WindowsRegistrationIncoming.RecoversStackHomesReadOnlyByCallbacks",
    "WindowsRegistrationIncoming.FailedProjectionRestoresMemoryAndDeclaration",
)
FORMS = {
    "value": "Value", "reference": "Reference", "unnamed-value": "UnnamedValue",
    "unnamed-reference": "UnnamedReference", "catch-all": "CatchAll",
    "unnamed-value-fixed": "UnnamedValueFixed",
    "unnamed-reference-fixed": "UnnamedReferenceFixed", "catch-all-fixed": "CatchAllFixed",
    "value-llvm-fixed": "ValueLLVMFixed",
    "value-large-llvm-fixed": "ValueLargeLLVMFixed",
    "reference-llvm-fixed": "ReferenceLLVMFixed",
    "unnamed-value-llvm-fixed": "UnnamedValueLLVMFixed",
    "unnamed-reference-llvm-fixed": "UnnamedReferenceLLVMFixed",
    "catch-all-llvm-fixed": "CatchAllLLVMFixed",
    "value-incoming-fixed": "ValueIncomingFixed",
    "reference-incoming-fixed": "ReferenceIncomingFixed",
    "catch-all-incoming-fixed": "CatchAllIncomingFixed",
    "catch-all-incoming-readonly-fixed": "CatchAllIncomingReadOnlyFixed",
}
KINDS = (*FORMS, "catch-all-unsigned", "catch-all-unsigned-fixed",
         "catch-all-unsigned-llvm-fixed")
CASES = (*KINDS, *(kind + "-control" for kind in KINDS))
OBSERVATION = re.compile(r"CALLBACK ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8})\r?\n")


def source_frame(case: str) -> str:
    if "-llvm-fixed" in case:
        return "fixed-displaced"
    return "direct" if case.removesuffix("-control").endswith("-fixed") else "realigned"


def proof_count(case: str) -> int:
    return {"direct": 1, "realigned": 3, "fixed-displaced": 4}[source_frame(case)]


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_installation(original: PE32, generated: PE32, receipt: dict, case: str) -> None:
    if receipt.get("schema") != 1 or \
            receipt.get("evidence") != "checked-realigned-source-reconstruction" or \
            receipt.get("base") != BASES[0] or original.base != BASES[0] or \
            generated.base != BASES[0] or \
            receipt.get("source_image_sha256") != hashlib.sha256(original.data).hexdigest() or \
            receipt.get("image_sha256") != hashlib.sha256(generated.data).hexdigest():
        raise ValueError("realigned installation lost its proved image identity")
    if receipt.get("source_frame") != source_frame(case):
        raise ValueError("source receipt lost its expected frame coordinate")
    if (receipt.get("incoming_reads"), receipt.get("incoming_writes")) != \
            ((3, 0 if "-readonly" in case else 2) if "-incoming" in case else (0, 0)):
        raise ValueError("source receipt lost its exact caller argument accesses")
    entry = original.entry(b"callback_parent")
    if entry != receipt["source_begin"] or generated.entry(b"callback_parent") != entry:
        raise ValueError("realigned installation changed the exported entry")
    at = generated.raw(entry, 5)
    target = (entry + 5 + struct.unpack_from("<i", generated.data, at + 1)[0]) & 0xffffffff
    if generated.data[at] != 0xe9 or target != receipt["generated_begin"]:
        raise ValueError("realigned trampoline lost its generated owner")
    code_owner(original, entry, receipt["source_end"], ".text")
    code_owner(generated, target, receipt["generated_end"], ".ndtext")


def observe(path: Path, case: str, route: str, receipt: dict, launcher: list[str],
            env: dict[str, str], timeout: float) -> dict:
    image = PE32(path.read_bytes())
    if image.base not in BASES or image.u16(image.optional + 70) & 0x40:
        raise ValueError("realigned runtime probe has no forced base")
    result = run_image(path, launcher, env, timeout)
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != int(case.endswith("-control")) or not match:
        raise ValueError("realigned runtime or negative control failed")
    value, caught, chain, iterations, caller = (int(v, 16) for v in match.groups())
    if (value, caught, chain, iterations) != (7, 18 if case.startswith("reference") else 7, 1, 4):
        raise ValueError("realigned catch value, reference effect or chain differs")
    owner = "source" if route == "original" else "generated"
    begin, end = receipt[owner + "_begin"], receipt[owner + "_end"]
    if not begin <= caller - image.base < end:
        raise ValueError("realigned throw caller is outside its exact owner")
    code_owner(image, begin, end, ".text" if route == "original" else ".ndtext")
    return {"image": path.name, "sha256": file_digest(path), "route": route,
            "base": image.base, "expected_exit": int(case.endswith("-control")),
            "caller_rva": caller - image.base, "runtime": result}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--patch-binary", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["WINEDEBUG"] = "-all"
    if args.wine_prefix:
        env.update(WINEARCH="win32", WINEPREFIX=str(args.wine_prefix.resolve()))
    report = {"schema": 1, "evidence": "realigned-source-reconstruction",
              "passed": False, "commands": [], "cases": []}

    def run(command: list[str | Path], extra: dict | None = None) -> None:
        command = list(map(str, command))
        result = subprocess.run(command, env=env | (extra or {}), cwd=out,
                                capture_output=True, text=True, errors="replace",
                                timeout=args.timeout, check=False)
        report["commands"].append({"command": command, "exit_code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("realigned source build, proof or installation failed")

    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if not compiler or not linker or os.name != "nt" and not wine:
            raise ValueError("compiler, linker or Wine unavailable")
        libraries, report["runtime_libraries"] = load_libraries(args.runtime_libs.resolve())
        report["source_sha256"] = file_digest(SOURCE)
        report["emitter_sha256"] = file_digest(EMITTER)
        report["direct_emitter_sha256"] = file_digest(DIRECT_EMITTER)
        test, patch = args.test_binary.resolve(), args.patch_binary.resolve()
        run([test, "--gtest_filter=WindowsRegistrationCatch.*",
             "--gtest_output=xml:" + str(out / "catch-projection.xml")])
        if require_test_result(out / "catch-projection.xml") != 1:
            raise ValueError("catch projection proof test missing")
        run([test, "--gtest_filter=" + ":".join(INCOMING_PROOFS),
             "--gtest_output=xml:" + str(out / "incoming-projection.xml")])
        if require_test_result(out / "incoming-projection.xml") != len(INCOMING_PROOFS):
            raise ValueError("caller argument entry/rollback proof tests missing")
        run([test, "--gtest_filter=WindowsRegistrationFixed.RuntimeOffsetsRequireTheCompleteLayout",
             "--gtest_output=xml:" + str(out / "fixed-projection.xml")])
        if require_test_result(out / "fixed-projection.xml") != 1:
            raise ValueError("fixed runtime coordinate proof test missing")
        for kind, test_name in FORMS.items():
            run([test, "--gtest_filter=WindowsRegistrationRealignedNative.Emits" + test_name + "Callback",
                 "--gtest_output=xml:" + str(out / (kind + "-emit.xml"))],
                {"NEVERD_REGISTRATION_REALIGNED_OBJECT": str(out / (kind + ".obj"))})
            if require_test_result(out / (kind + "-emit.xml")) != 1:
                raise ValueError("realigned emission test missing")
        report["objects"] = {kind: file_digest(out / (kind + ".obj"))
                             for kind in FORMS}
        for name in CASES:
            kind = name.removesuffix("-control")
            object_kind = kind.replace("-unsigned", "")
            case = out / name
            case.mkdir(exist_ok=True)
            run([compiler, "--target=i686-pc-windows-msvc", "-fms-extensions", "-fexceptions",
                 "-fcxx-exceptions", "-fno-omit-frame-pointer", "-O1",
                 "-DREFERENCE_CATCH=" + str(int(kind.startswith("reference"))),
                 "-DINCOMING_READ_ONLY=" + str(int("-readonly" in kind)),
                 "-DINCOMING_FRAME=" + str(int("-incoming" in kind)),
                 "-DUNSIGNED_THROW=" + str(int("-unsigned" in kind)),
                 "-DEXPECTED_RESULT=" + ("8" if name.endswith("-control") else "7"),
                 "-c", SOURCE, "-o", case / "driver.obj"])
            original = case / "original.exe"
            product = case / "product.exe"
            run([linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86", "/subsystem:console",
                 "/fixed:no", "/dynamicbase:no", "/out:" + str(original),
                 case / "driver.obj", out / (object_kind + ".obj"), *libraries])
            checks = "WindowsRegistrationRealignedNative.InputPE32ReconstructsTheSourceFrame"
            if proof_count(name) >= 3:
                checks += (":WindowsRegistrationHighCallback.InputPE32BindsCurrentCallbackRoots:"
                           "WindowsRegistrationHighCallback.InputPE32CollectsTheWholeCallbackCFG")
            if source_frame(name) == "fixed-displaced":
                checks += ":WindowsRegistrationFixed.InputPE32ProvesDisplacedFrame"
            run([test, "--gtest_filter=" + checks,
                 "--gtest_output=xml:" + str(case / "rewrite.xml")],
                {"NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32": str(original),
                 "NEVERD_REGISTRATION_REALIGNED_PE32": str(original),
                 "NEVERD_REGISTRATION_REALIGNED_OUTPUT_PE32": str(product),
                 "NEVERD_REGISTRATION_REALIGNED_RECEIPT": str(case / "contract.json"),
                 "NEVERD_REGISTRATION_OUTPUT_IR": str(case / "source.ll")})
            if require_test_result(case / "rewrite.xml") != proof_count(name):
                raise ValueError("realigned reconstruction or HighIR callback test missing")
            receipt = json.loads((case / "contract.json").read_text())
            validate_installation(PE32(original.read_bytes()), PE32(product.read_bytes()), receipt, name)
            for mode in ("section", "inplace"):
                run([patch, "patch", original, "--from-ir=" + str(case / "source.ll"),
                     "--mode=" + mode, "--no-opt", "-o", case / ("cli-" + mode + ".exe")])
                if (case / ("cli-" + mode + ".exe")).read_bytes() != product.read_bytes():
                    raise ValueError("CLI installation differs from the checked public transaction")
            record = {"case": name, "contract_sha256": file_digest(case / "contract.json"),
                      "ir_sha256": file_digest(case / "source.ll"), "images": []}
            report["cases"].append(record)
            for route in ROUTES:
                source = case / (route + ".exe")
                rebased = case / (route + "-rebased.exe")
                rebased.write_bytes(PE32(source.read_bytes()).rebase(BASES[1]))
                for image in (source, rebased):
                    record["images"].append(observe(image, name, route, receipt,
                                                    [wine] if wine else [], env, args.timeout))
            print("PASS realigned source " + name + ": 8 identical-contract executions", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, struct.error, subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    path = out / "realigned-rewrite.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(path), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
