"""Authenticate and install a second PE32 generation with ordered cleanups."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess

if __package__:
    from .check_windows_registration_rewrite import PE32
    from .check_windows_registration_realigned import require_test_result
    from .windows_registration_image import code_owner, safe_handlers, jump_target
else:
    from check_windows_registration_rewrite import PE32
    from check_windows_registration_realigned import require_test_result
    from windows_registration_image import code_owner, safe_handlers, jump_target

ROOT = Path(__file__).resolve().parents[1]
RELIFT_LABELS = ("relifted", "relift-cli-section", "relift-cli-inplace")
PROOFS = tuple(ROOT / "unittests/lift/eh" / name for name in (
    "WindowsRegistrationNativeTests.cpp", "RegistrationReliftTests.cpp",
    "RegistrationSourceReceiptTestUtils.cpp"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def proof_digests():
    return {path.name: digest(path) for path in PROOFS}


def validate_cleanup_installation(first: PE32, second: PE32, receipt: dict,
                                  contract: dict) -> None:
    if first.base != 0x400000 or second.base != first.base or \
            receipt.get("schema") != 1 or \
            receipt.get("evidence") != "checked-realigned-source-reconstruction" or \
            receipt.get("source_frame") != "realigned" or \
            receipt.get("base") != first.base or \
            receipt.get("source_image_sha256") != hashlib.sha256(first.data).hexdigest() or \
            receipt.get("image_sha256") != hashlib.sha256(second.data).hexdigest() or \
            (receipt.get("entry_registers"), receipt.get("entry_pop")) != (1, 0) or \
            (receipt.get("cleanup_actions"), receipt.get("cleanup_calls")) != (2, 2) or \
            (receipt.get("incoming_reads"), receipt.get("incoming_writes")) != (0, 0):
        raise ValueError("cleanup re-lift lost its image, frame, ABI or ordered action proof")
    if receipt["source_image_sha256"] != contract["image_sha256"] or \
            receipt["source_begin"] != contract["generated_code_begin_rva"] or \
            receipt["source_end"] != contract["generated_owner_end_rva"] or \
            not receipt["source_begin"] < contract["generated_code_end_rva"] <= receipt["source_end"]:
        raise ValueError("cleanup re-lift no longer owns the first generated callback closure")
    entry = first.entry(b"registration_cxx_probe")
    if second.entry(b"registration_cxx_probe") != entry or \
            entry != contract["source_entry_rva"] or \
            jump_target(first, entry) != receipt["source_begin"] or \
            jump_target(second, entry) != receipt["source_begin"] or \
            jump_target(second, receipt["source_begin"]) != receipt["generated_begin"] or \
            receipt["generated_begin"] < receipt["source_end"]:
        raise ValueError("cleanup re-lift changed the two-generation trampoline chain")
    code_owner(first, receipt["source_begin"], receipt["source_end"], ".ndtext")
    code_owner(second, receipt["generated_begin"], receipt["generated_end"], ".ndtext")
    _, old = safe_handlers(first)
    config, new = safe_handlers(second)
    handler, info = receipt["generated_handler"], receipt["generated_func_info"]
    if handler in old or new != sorted(old + [handler]):
        raise ValueError("cleanup re-lift changed the retained SafeSEH closure")
    code_owner(second, handler, handler + 10, ".ndtext")
    offset = second.raw(handler, 10)
    if second.data[offset] != 0xb8 or second.data[offset + 5] != 0xe9 or \
            second.u32(offset + 1) != second.base + info or \
            second.u32(second.raw(info, 36)) != 0x19930522 or \
            not {config, handler + 1} <= second.relocation_fields():
        raise ValueError("cleanup re-lift lost its relocated FuncInfo handler")


def validate_cleanup_artifacts(parent, first_contract, case):
    if case.get("relift_contract_sha256") != digest(parent / "relift-contract.json") or \
            case.get("relift_ir_sha256") != digest(parent / "relift.ll") or \
            require_test_result(parent / "relift.xml") != 1:
        raise ValueError("cleanup re-lift has no current executed source proof")
    receipt = json.loads((parent / "relift-contract.json").read_text())
    first = PE32((parent / "product-patched.exe").read_bytes())
    second = PE32((parent / "relifted.exe").read_bytes())
    validate_cleanup_installation(first, second, receipt, first_contract)
    return receipt, second


def reconstruct_cleanup(test, patch, parent, first_contract, environment,
                        timeout, steps):
    def run(command, extra=None):
        command = list(map(str, command))
        result = subprocess.run(command, env=environment | (extra or {}),
                                capture_output=True, text=True, errors="replace",
                                timeout=timeout, check=False)
        steps.append({"command": command, "exit_code": result.returncode,
                      "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("cleanup re-lift source proof or installation failed")

    run([test, "--gtest_filter=RegistrationRelift.InputPE32ReconstructsGeneratedFunction",
         "--gtest_output=xml:" + str(parent / "relift.xml")],
        {"NEVERD_REGISTRATION_RELIFT_PE32": str(parent / "product-patched.exe"),
         "NEVERD_REGISTRATION_RELIFT_ENTRY": hex(0x400000 + first_contract["generated_code_begin_rva"]),
         "NEVERD_REGISTRATION_ENTRY_POP": "0", "NEVERD_REGISTRATION_ENTRY_REGISTERS": "1",
         "NEVERD_REGISTRATION_RELIFT_OUTPUT": str(parent / "relifted.exe"),
         "NEVERD_REGISTRATION_REALIGNED_RECEIPT": str(parent / "relift-contract.json"),
         "NEVERD_REGISTRATION_OUTPUT_IR": str(parent / "relift.ll")})
    case = {"relift_contract_sha256": digest(parent / "relift-contract.json"),
            "relift_ir_sha256": digest(parent / "relift.ll")}
    receipt, second = validate_cleanup_artifacts(parent, first_contract, case)
    for mode in ("section", "inplace"):
        output = parent / ("relift-cli-" + mode + ".exe")
        run([patch, "patch", parent / "product-patched.exe",
             "--from-ir=" + str(parent / "relift.ll"), "--mode=" + mode,
             "--no-opt", "-o", output])
        if output.read_bytes() != second.data:
            raise ValueError("cleanup re-lift CLI differs from the checked transaction")
    return receipt, second, case
