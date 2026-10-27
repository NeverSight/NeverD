#!/usr/bin/env python3
"""Build, reconstruct and execute a real SEH3/EH4 PE32 function, including rebasing.

The runtime caller PC must belong to the installed generated section. Original
and rewritten images must both restore FS:[0] after repeated exception dispatch.
This is deliberately separate from original-corpus and callback-only evidence.
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
import sys
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_frame import KERNEL32, MSVCRT, undecorate_kernel32_imports
    from .windows_registration_runtime import load_runtime, validate_runtime
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_frame import KERNEL32, MSVCRT, undecorate_kernel32_imports
    from windows_registration_runtime import load_runtime, validate_runtime

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/lift/eh/fixtures"
OBSERVATION = re.compile(
    r"neverd-registration-rewrite: value=(\d+) caller=([0-9a-fA-F]{8}) "
    r"entry=([0-9a-fA-F]{8}) chain=(\d+) iterations=(\d+) trace=(\d+)"
    r"(?: fp=([0-9a-fA-F]{8}))?\s*")
CONTROL_FP_MODES = ("output-word", "null-query", "precision-24", "precision-53",
                    "precision-64")
CASES = {"filter": (0, 0), "nested-finally": (1, 123),
         "continue-search": (2, 123), "continue-execution": (3, 1),
         "normal-finally": (4, 2), "cdecl-parameter": (5, 7),
         "cdecl-parameter-write": (6, 177), "exception-code": (7, 7),
         "exception-flags": (8, 7)}
CASES.update({"eh4-" + name: value for name, value in tuple(CASES.items())})
CASES.update({"eh4-gs-" + name: CASES[name] for name in
              ("filter", "nested-finally", "continue-search", "continue-execution",
               "normal-finally", "cdecl-parameter", "cdecl-parameter-write",
               "exception-code", "exception-flags")})
CASES.update({"longjmp-finally": (9, 23), "longjmp-zero": (10, 23),
              "longjmp-filter": (11, 3), "longjmp-parameter": (12, 23),
              "longjmp-callback": (13, 23),
              "longjmp-unwind-callback": (14, 23),
              "longjmp-callback-zero": (15, 23),
              "longjmp-search-filter": (16, 23),
              "longjmp-search-filter-zero": (17, 23),
              "longjmp-retired-scope": (18, 23),
              "longjmp-retired-inner-scope": (19, 23),
              "longjmp-callee": (20, 23),
              "longjmp-returning-callee": (21, 23),
              "longjmp-scalar-callee": (22, 23),
              "longjmp-runtime-raise": (23, 23),
              "longjmp-control-fp": (24, 23),
              "longjmp-runtime-continue": (25, 23),
              "longjmp-runtime-finally": (26, 23),
              "longjmp-runtime-ordinary-finally": (27, 23),
              "longjmp-nested-finally-call": (28, 243),
              "longjmp-nested-finally-unwind": (29, 243),
              "longjmp-state-activate-call": (30, 243),
              "longjmp-state-activate-unwind": (31, 243),
              "longjmp-state-retire": (32, 23),
              "longjmp-state-sentinel": (33, 23),
              "longjmp-state-byte": (34, 243),
              "longjmp-state-word": (35, 243),
              "longjmp-filter-correlated-state": (36, 2345),
              "longjmp-filter-shared-state": (37, 2345),
              "longjmp-filter-captured-cursor": (38, 2345),
              "longjmp-filter-negative-disposition": (39, 2345),
              "longjmp-filter-skipped-scope": (40, 2345),
              "longjmp-filter-two-resume-mappings": (41, 2345),
              "longjmp-filter-callback-activation": (42, 2345),
              "longjmp-state-byte-high-bits": (43, 2),
              "longjmp-finally-incidental-result": (44, 2),
              "longjmp-deep-scope-chain": (45, 2),
              "longjmp-shared-parent-call": (46, 2),
              "shared-parent-throwing-call": (47, 2),
              "longjmp-private-parent-call": (48, 2),
              "longjmp-private-parent-graph": (49, 2),
              "longjmp-handler-selected": (53, 23),
              "longjmp-handler-cleanup": (54, 236),
              "longjmp-handler-captured-cursor": (55, 234),
              "longjmp-handler-nested-finally": (56, 23),
              "longjmp-handler-mixed-resumption": (57, 23),
              "longjmp-handler-cleanup-jump": (58, 236),
              "longjmp-handler-nested-cleanup-jump": (59, 2367),
              "longjmp-handler-cleanup-value": (60, 236),
              "longjmp-handler-nested-cleanup-value": (61, 2367),
              "longjmp-handler-cleanup-zero": (62, 236),
              "longjmp-handler-nested-cleanup-zero": (63, 2367),
              "longjmp-handler-cleanup-dynamic-value": (64, 236),
              "longjmp-handler-nested-cleanup-dynamic-value": (65, 2367),
              "longjmp-handler-cleanup-other-capture": (66, 236),
              "longjmp-handler-nested-cleanup-other-capture": (67, 2367),
              "longjmp-search-filter-jump": (68, 23),
              "longjmp-nested-search-filter-jump": (69, 263),
              "longjmp-search-filter-other-capture": (70, 23),
              "longjmp-nested-search-filter-other-capture": (71, 263),
              "longjmp-search-filter-mixed-jump": (72, 23),
              "longjmp-nested-search-filter-mixed-jump": (73, 263),
              "longjmp-search-filter-selected-jump": (74, 23),
              "longjmp-nested-search-filter-selected-jump": (75, 263),
              "longjmp-finally-register-loop": (76, 6662345),
              "longjmp-finally-frame-loop": (77, 6662345),
              "longjmp-finally-private-loop": (78, 6662345),
              "longjmp-filter-register-loop": (79, 2666345),
              "longjmp-filter-frame-loop": (80, 2666345),
              "longjmp-filter-private-loop": (81, 2666345)})
PARENT_EXIT_CASES = {
    "finally-parent-jump": (82, 23),
    "finally-parent-unwind": (83, 23),
    "finally-parent-select": (84, 24),
    "finally-parent-mixed-return": (85, 23),
    "finally-parent-reenter-unwind": (86, 234),
    "finally-parent-reenter-normal": (87, 234),
    "finally-parent-register-input": (88, 23),
    "finally-parent-only": (89, 23),
    "finally-parent-only-unwind": (90, 23),
}
CASES.update({prefix + name: value for prefix in ("", "eh4-", "eh4-gs-")
              for name, value in PARENT_EXIT_CASES.items()})
EH4_JUMP_CASES = ("longjmp-finally", "longjmp-zero", "longjmp-filter",
                  "longjmp-parameter", "longjmp-callback",
                  "longjmp-unwind-callback", "longjmp-callback-zero",
                  "longjmp-search-filter", "longjmp-search-filter-zero",
                  "longjmp-retired-scope", "longjmp-retired-inner-scope",
                  "longjmp-callee", "longjmp-returning-callee",
                  "longjmp-scalar-callee", "longjmp-filter-correlated-state",
                  "longjmp-filter-shared-state", "longjmp-filter-captured-cursor",
                  "longjmp-filter-negative-disposition", "longjmp-filter-skipped-scope",
                  "longjmp-filter-two-resume-mappings", "longjmp-filter-callback-activation",
                  "longjmp-handler-selected", "longjmp-handler-cleanup",
                  "longjmp-handler-captured-cursor", "longjmp-handler-nested-finally",
                  "longjmp-handler-mixed-resumption", "longjmp-handler-cleanup-jump",
                  "longjmp-handler-nested-cleanup-jump", "longjmp-handler-cleanup-value",
                  "longjmp-handler-nested-cleanup-value", "longjmp-handler-cleanup-zero",
                  "longjmp-handler-nested-cleanup-zero",
                  "longjmp-handler-cleanup-dynamic-value",
                  "longjmp-handler-nested-cleanup-dynamic-value",
                  "longjmp-handler-cleanup-other-capture",
                  "longjmp-handler-nested-cleanup-other-capture",
                  "longjmp-search-filter-jump",
                  "longjmp-nested-search-filter-jump",
                  "longjmp-search-filter-other-capture",
                  "longjmp-nested-search-filter-other-capture",
                  "longjmp-search-filter-mixed-jump",
                  "longjmp-nested-search-filter-mixed-jump",
                  "longjmp-search-filter-selected-jump",
                  "longjmp-nested-search-filter-selected-jump",
                  "longjmp-finally-register-loop",
                  "longjmp-finally-frame-loop",
                  "longjmp-finally-private-loop",
                  "longjmp-filter-register-loop",
                  "longjmp-filter-frame-loop",
                  "longjmp-filter-private-loop")
CASES.update({prefix + name: CASES[name]
              for prefix in ("eh4-", "eh4-gs-") for name in EH4_JUMP_CASES})
CASES.update({prefix + name: (number, 23)
              for prefix in ("eh4-", "eh4-gs-")
              for number, name in ((50, "longjmp-private-parent-call"),
                                   (51, "longjmp-private-parent-graph"))})
CASES.update({prefix + "longjmp-multistate-capture": (52, 23)
              for prefix in ("", "eh4-", "eh4-gs-")})
CASES.update({prefix + name: (number, 23)
              for prefix in ("eh4-", "eh4-gs-")
              for number, name in ((20, "longjmp-eh4-callee"),
                                   (21, "longjmp-eh4-returning-callee"),
                                   (22, "longjmp-eh4-scalar-callee"))})
CASES.update({name + "-mixed-gs": value for name, value in tuple(CASES.items())
              if "-eh4-" in name})



class PE32:
    def __init__(self, data: bytes | bytearray):
        self.data = data
        if len(data) < 64 or data[:2] != b"MZ":
            raise ValueError("missing DOS header")
        pe = self.u32(0x3c)
        if data[pe:pe + 4] != b"PE\0\0" or self.u16(pe + 4) != 0x14c:
            raise ValueError("expected i386 PE32")
        self.optional = pe + 24
        if self.u16(self.optional) != 0x10b:
            raise ValueError("expected PE32 optional header")
        self.base = self.u32(self.optional + 28)
        self.sections = []
        for index in range(self.u16(pe + 6)):
            offset = self.optional + self.u16(pe + 20) + 40 * index
            name = bytes(data[offset:offset + 8]).rstrip(b"\0").decode("ascii")
            virtual_size, rva, raw_size, raw = struct.unpack_from("<4I", data, offset + 8)
            self.sections.append((name, rva, virtual_size, raw, raw_size))

    def u16(self, offset: int) -> int:
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.data, offset)[0]

    def raw(self, rva: int, size: int = 4) -> int:
        owners = [raw + rva - start for _, start, _, raw, count in self.sections
                  if start <= rva and rva - start + size <= count]
        if len(owners) != 1 or owners[0] + size > len(self.data):
            raise ValueError("RVA has no unique raw-backed owner")
        return owners[0]

    def directory(self, index: int) -> tuple[int, int]:
        return struct.unpack_from("<2I", self.data, self.optional + 96 + 8 * index)

    def entry(self, name: bytes = b"registration_entry") -> int:
        export, _ = self.directory(0)
        table = self.raw(export, 40)
        functions, names, ordinals = struct.unpack_from("<3I", self.data, table + 28)
        for index in range(self.u32(table + 24)):
            name_offset = self.raw(self.u32(self.raw(names + 4 * index)), 1)
            end = self.data.index(0, name_offset)
            if self.data[name_offset:end] == name:
                ordinal = self.u16(self.raw(ordinals + 2 * index, 2))
                if ordinal >= self.u32(table + 20):
                    raise ValueError("export ordinal is out of bounds")
                return self.u32(self.raw(functions + 4 * ordinal))
        raise ValueError(f"required export is missing: {name!r}")

    def relocation_fields(self) -> set[int]:
        reloc, size = self.directory(5)
        if not reloc or not size:
            raise ValueError("probe has no relocation contract")
        cursor, fields = 0, set()
        while cursor < size:
            page, block_size = struct.unpack_from(
                "<2I", self.data, self.raw(reloc + cursor, 8))
            if page & 0xfff or block_size < 8 or block_size % 4 or cursor + block_size > size:
                raise ValueError("invalid relocation block")
            for index in range((block_size - 8) // 2):
                item = self.u16(self.raw(reloc + cursor + 8 + 2 * index, 2))
                if item >> 12 == 0:
                    continue
                target = page + (item & 0xfff)
                if item >> 12 != 3 or target in fields:
                    raise ValueError("unsupported or duplicate HIGHLOW relocation")
                self.raw(target)
                fields.add(target)
            cursor += block_size
        if not fields:
            raise ValueError("probe has no HIGHLOW fields")
        return fields

    def rebase(self, new_base: int) -> bytes:
        data = bytearray(self.data)
        if new_base % 0x10000 or not 0 < new_base <= 0xffffffff:
            raise ValueError("rebasing requires relocations and an aligned image base")
        delta = new_base - self.base
        for target in self.relocation_fields():
            offset = self.raw(target)
            struct.pack_into("<I", data, offset, (self.u32(offset) + delta) & 0xffffffff)
        struct.pack_into("<I", data, self.optional + 28, new_base)
        struct.pack_into("<I", data, self.optional + 64, 0)
        return bytes(data)


def observe(image: Path, generated: bool, launcher: list[str],
            environment: dict[str, str], timeout: float, expected_trace: int,
            control_fp_mode: str = "output-word") -> dict:
    pe = PE32(image.read_bytes())
    config_rva, config_size = pe.directory(10)
    config = pe.raw(config_rva, config_size)
    if config_size < 92 or pe.u32(config) < 92 or pe.u32(config + 88) != 0x400:
        raise ValueError("probe lost its declared Guard CF contract")
    table, count = pe.u32(config + 80), pe.u32(config + 84)
    fields = pe.relocation_fields()
    if generated:
        if not count or table < pe.base or config_rva + 80 not in fields:
            raise ValueError("new Guard CF table has no HIGHLOW pointer owner")
        pe.raw(table - pe.base, count * 4)
    elif table or count or config_rva + 80 in fields:
        raise ValueError("original Guard CF table must be empty and unrelocated")
    result = run_image(image, launcher, environment, timeout)
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != 0 or not match:
        raise ValueError(f"exception execution failed: {result}")
    value, caller, entry, chain, iterations, trace = (int(v, 16 if i in (1, 2) else 10)
                                              for i, v in enumerate(match.groups()[:6]))
    control_word = int(match[7], 16) if match[7] is not None else None
    if (control_word is None) != (control_fp_mode == "output-word"):
        raise ValueError("floating-point control observation is missing or unexpected")
    precision = {"precision-24": 0x20000, "precision-53": 0x10000,
                 "precision-64": 0}.get(control_fp_mode)
    if precision is not None and control_word & 0x30000 != precision:
        raise ValueError("native floating-point precision differs from the source request")
    runtime_base = entry - pe.entry()
    if ((value, chain, iterations, trace) != (7, 1, 4, expected_trace) or
            runtime_base != pe.base):
        raise ValueError("exception result, chain restoration or forced base differs")
    owners = [name for name, start, size, _, _ in pe.sections
              if start <= caller - runtime_base < start + size]
    if owners != ([".ndtext"] if generated else [".text"]):
        raise ValueError("runtime caller PC does not prove the required source/generated path")
    if generated:
        source = pe.raw(pe.entry(), 5)
        displacement = struct.unpack_from("<i", pe.data, source + 1)[0]
        target = pe.entry() + 5 + displacement
        if pe.data[source] != 0xe9 or not any(
                name == ".ndtext" and start <= target < start + size
                for name, start, size, _, _ in pe.sections):
            raise ValueError("source entry does not redirect into generated code")
    return {"image": str(image), "sha256": hashlib.sha256(pe.data).hexdigest(),
            "generated": generated, "runtime_base": runtime_base,
            "guard_cf_count": count, "guard_cf_pointer_relocated": config_rva + 80 in fields,
            "caller_rva": caller - runtime_base, "value": value,
            "chain_restored": bool(chain), "iterations": iterations,
            "trace": trace, "control_fp_word": control_word, "runtime": result}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--patch-binary", type=Path,
                        help="execute CLI section/inplace paths from IR and EXE, with and without optimization")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--runtime-root", type=Path,
                        help="captured Microsoft x86 CRT directory with runtime.json")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--case", choices=CASES, default="filter")
    parser.add_argument("--shared-seh4", action="store_true",
                        help="use the authored shared SEH4 entry and exit helpers")
    parser.add_argument("--parent-pop", type=int, choices=(0, 4, 16, 20), default=0,
                        help="use a stdcall parent with this RET cleanup; callbacks remain cdecl")
    parser.add_argument("--startup-info", action="store_true",
                        help="compare a private STARTUPINFOW output with an independent runtime observation")
    parser.add_argument("--heap-termination", action="store_true",
                        help="retain the exact process heap terminate-on-corruption request")
    parser.add_argument("--synchronization", action="store_true",
                        help="observe native atomic success, failure, return values and Sleep")
    parser.add_argument("--thread-stack-base", action="store_true",
                        help="compare the source thread-block StackBase read with an independent observer")
    parser.add_argument("--control-fp-mode", choices=CONTROL_FP_MODES,
                        default="output-word",
                        help="use a private output word, null query or independently observed x87 precision setting")
    parser.add_argument("--control-fp-veneer", action="store_true",
                        help="call _controlfp_s through an authored near CALL and FF25 import veneer")
    parser.add_argument("--observe-callee-result", action="store_true",
                        help="use the returning authored helper's EAX as the longjmp value")
    parser.add_argument("--state-write", choices=("literal", "register", "bitwise"),
                        default="literal", help="source encoding of ordinary SEH state writes")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    case, expected_trace = CASES[args.case]
    eh4 = args.case.startswith("eh4-")
    gs = args.case.startswith("eh4-gs-")
    helper_gs = gs != args.case.endswith("-mixed-gs")
    jump = "longjmp-" in args.case
    if args.observe_callee_result and case != 21:
        parser.error("--observe-callee-result requires a returning-callee case")
    if args.shared_seh4 and (not eh4 or gs or case not in range(9)):
        parser.error("--shared-seh4 requires an ordinary EH4 case without GS")
    if args.parent_pop and case not in (0, 4, 5):
        parser.error("--parent-pop requires a filter, normal-finally or cdecl-parameter case")
    if args.startup_info and case not in (0, 4):
        parser.error("--startup-info requires a filter or normal-finally case")
    if args.heap_termination and case not in (0, 4):
        parser.error("--heap-termination requires a filter or normal-finally case")
    if args.synchronization and case not in (0, 4):
        parser.error("--synchronization requires a filter or normal-finally case")
    if args.thread_stack_base and case not in (0, 4):
        parser.error("--thread-stack-base requires a filter or normal-finally case")
    if args.control_fp_mode != "output-word" and case != 24:
        parser.error("--control-fp-mode requires longjmp-control-fp")
    if args.control_fp_veneer and (case != 24 or eh4):
        parser.error("--control-fp-veneer requires the SEH3 longjmp-control-fp case")
    if args.state_write != "literal" and case not in (*range(9), 43, 44):
        parser.error("--state-write requires an ordinary filter/finally or shared byte-state case")
    if args.state_write == "bitwise" and case in (1, 2, 4):
        parser.error("--state-write bitwise requires a zero-activation case")
    report = {"schema": 2, "evidence": f"reconstructed-source-registration-{'seh4' if eh4 else 'seh3'}",
              "case": args.case, "expected_trace": expected_trace,
              "shared_seh4": args.shared_seh4,
              "parent_pop": args.parent_pop,
              "startup_info": args.startup_info,
              "heap_termination": args.heap_termination,
              "synchronization": args.synchronization,
              "thread_stack_base": args.thread_stack_base,
              "control_fp_mode": args.control_fp_mode,
              "control_fp_veneer": args.control_fp_veneer,
              "observe_callee_result": args.observe_callee_result,
              "state_write": args.state_write,
              "passed": False, "steps": [], "observations": []}
    environment = os.environ.copy()
    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        if not compiler or not linker:
            raise ValueError("cross-target clang and lld-link are required")
        launcher = []
        if os.name != "nt":
            wine = args.wine or shutil.which("wine") or shutil.which("wine64-stable")
            if not wine:
                raise ValueError("Wine is unavailable; reconstructed source was not executed")
            launcher = [wine]
            prefix = (args.wine_prefix or output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEDEBUG="-all")
        commands = []
        runtime_exports = "_except_handler3\n"
        if eh4:
            runtime_exports += "_except_handler4_common\n"
        if jump:
            runtime_exports += "_setjmp3\n_longjmpex\n"
            runtime_exports += "_seh_longjmp_unwind4\n" if eh4 else "_seh_longjmp_unwind\n"
        definitions = [("kernel32", KERNEL32 +
                        ("_GetStartupInfoW@4\n" if args.startup_info else "") +
                        ("_HeapSetInformation@16\n" if args.heap_termination else "") +
                        ("_InterlockedCompareExchange@12\n_Sleep@4\n" if args.synchronization else ""))]
        if args.runtime_root:
            runtime, report["seh_runtime"] = load_runtime(args.runtime_root.resolve())
            shutil.copy2(runtime, output / runtime.name)
            validate_runtime(output, report["seh_runtime"])
            environment["WINEDLLOVERRIDES"] = "vcruntime140=n"
            definitions.extend((("msvcrt", "LIBRARY msvcrt.dll\nEXPORTS\nprintf\n"),
                                ("vcruntime", "LIBRARY vcruntime140.dll\nEXPORTS\n" +
                                 runtime_exports)))
        else:
            if case == 19 and launcher:
                raise ValueError("retired inner scopes require --runtime-root: Wine's builtin CRT uses a different unwind bound")
            definitions.append(("msvcrt", MSVCRT.replace("_except_handler3\n", "") +
                                runtime_exports))
        for name, definition in definitions:
            if name == "msvcrt" and case == 24:
                definition += "_controlfp_s\n"
            (output / f"{name}.def").write_text(definition, encoding="utf-8")
            commands.append([linker, "/lib", f"/def:{output / (name + '.def')}",
                             "/machine:x86", f"/out:{output / (name + '.lib')}"])
        if case in {value[0] for value in PARENT_EXIT_CASES.values()}:
            sources = ["registration_parent_exit.s"]
            if eh4:
                sources.append("registration_eh4_runtime.c")
        elif 76 <= case <= 81:
            sources = ["registration_callback_loop.s"]
            if eh4:
                sources.append("registration_eh4_runtime.c")
        else:
            sources = (["registration_multistate_jump.s"] +
                       (["registration_eh4_runtime.c"] if eh4 else []) if case == 52
                       else ["registration_filter_state_eh4.s" if 36 <= case <= 42 or 53 <= case <= 75 else
                             "registration_jump_eh4.s" if jump else "registration_seh4.s",
                        "registration_eh4_runtime.c"] if eh4
                       else ["registration_shared_calls.s" if case == 47 else
                             "registration_deep_scopes.s" if case in (45, 46, 48, 49) else
                             "registration_state_dispatch.s" if case in (43, 44) else
                             "registration_filter_state.s" if case >= 36 else
                             "registration_state.s" if case >= 30 else
                             "registration_jump.s" if jump else "registration_seh3.s"])
        if args.shared_seh4:
            sources = ["registration_seh4_shared.s", "registration_eh4_runtime.c"]
        sources.append("registration_observer.c")
        if case in (49, 51):
            sources.append("registration_private_graph.s")
        if case in (20, 21, 22):
            sources.append("registration_jump_callee_eh4.s" if "-eh4-" in args.case
                           else "registration_jump_callee.s")
        for source in sources:
            compile_command = [compiler, "--target=i686-pc-windows-msvc", "-O0",
                             f"-DREGISTRATION_CASE={case}",
                             f"-DREGISTRATION_PARENT_POP={args.parent_pop}",
                             f"-DREGISTRATION_STARTUP_INFO={int(args.startup_info)}",
                             f"-DREGISTRATION_HEAP_TERMINATION={int(args.heap_termination)}",
                             f"-DREGISTRATION_SYNCHRONIZATION={int(args.synchronization)}",
                             f"-DREGISTRATION_THREAD_STACK_BASE={int(args.thread_stack_base)}",
                             f"-DREGISTRATION_CONTROL_FP_MODE={CONTROL_FP_MODES.index(args.control_fp_mode)}",
                             f"-DREGISTRATION_CONTROL_FP_VENEER={int(args.control_fp_veneer)}",
                             f"-DREGISTRATION_OBSERVE_CALLEE_RESULT={int(args.observe_callee_result)}",
                             f"-DREGISTRATION_STATE_WRITE={('literal', 'register', 'bitwise').index(args.state_write)}",
                             f"-DREGISTRATION_LOOP_EH4={int(eh4)}",
                             f"-DREGISTRATION_PARENT_EH4={int(eh4)}",
                             "-DREGISTRATION_FORWARD_ONLY=1", f"-DREGISTRATION_EXPECT_GS={int(gs)}",
                             f"-DREGISTRATION_HELPER_GS={int(helper_gs)}",
                             *([f"-DREGISTRATION_MULTISTATE_EH4={int(eh4)}"] if case == 52 else []),
                             *( ["-x", "assembler-with-cpp"] if source.endswith(".s") else []),
                             "-fno-stack-protector", "-c", str(FIXTURES / source),
                             "-o", str(output / (source + ".obj"))]
            commands.append(compile_command)
            if case == 86 and source == "registration_parent_exit.s":
                commands.append([
                    *[str(output / "unrepaired-parent.obj")
                      if argument == str(output / (source + ".obj")) else argument
                      for argument in compile_command],
                    "-DREGISTRATION_PARENT_RESTORE_CHAIN=0"])
        original, patched = output / "original.exe", output / "patched.exe"
        for stale in (original, patched, output / "product-patched.exe",
                      output / "collision-patched.exe",
                      output / "rewrite.xml"):
            stale.unlink(missing_ok=True)
        link_command = [linker, "/entry:mainCRTStartup", "/nodefaultlib",
                         "/machine:x86", "/subsystem:console", "/dynamicbase:no",
                         "/export:registration_entry",
                         *([f"/alternatename:_registration_entry@{args.parent_pop}=_registration_entry"]
                           if args.parent_pop else []),
                         *( ["/export:registration_eh4_personality=_except_handler4"] if eh4 else []),
                         f"/out:{original}",
                         *(str(output / (name + ".obj")) for name in sources),
                         *(str(output / (name + ".lib")) for name, _ in definitions)]
        commands.append(link_command)
        if case == 86:
            # Never execute this control: its second exception follows the
            # abandoned CRT unwind guard. Admission must reject the source.
            unrepaired = output / "unrepaired-parent.exe"
            replacements = {
                f"/out:{original}": f"/out:{unrepaired}",
                str(output / "registration_parent_exit.s.obj"):
                    str(output / "unrepaired-parent.obj"),
            }
            commands.append([replacements.get(argument, argument)
                             for argument in link_command])
            environment["NEVERD_SEH_UNREPAIRED_PARENT_INPUT_PE32"] = str(unrepaired)
            commands.append([
                str(args.test_binary.resolve()),
                "--gtest_filter=RegistrationSEHParentExit.InputPE32RejectsAnAbandonedUnwindGuard",
                f"--gtest_output=xml:{output / 'unrepaired-parent.xml'}"])
        environment.update(NEVERD_REGISTRATION_INPUT_PE32=str(original),
                           NEVERD_REGISTRATION_OUTPUT_PE32=str(patched),
                           NEVERD_REGISTRATION_OUTPUT_IR=str(output / "registration.ll"))
        commands.append([str(args.test_binary.resolve()),
                         "--gtest_filter=WindowsRegistrationNative.InputPE32PreservesItsCheckedSourceContract",
                         f"--gtest_output=xml:{output / 'rewrite.xml'}"])
        for index, command in enumerate(commands):
            result = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, errors="replace", timeout=args.timeout)
            report["steps"].append({"command": command, "exit_code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
            if result.returncode:
                raise ValueError(f"fixture build or reconstruction failed: {command[0]}")
            if index == 0:
                undecorate_kernel32_imports(
                    output / "kernel32.lib",
                    ((b"_GetStartupInfoW@4",) if args.startup_info else ()) +
                    ((b"_HeapSetInformation@16",) if args.heap_termination else ()) +
                    ((b"_InterlockedCompareExchange@12", b"_Sleep@4") if args.synchronization else ()))
        tests = ET.parse(output / "rewrite.xml").getroot()
        if int(tests.get("tests", "0")) != 1 or tests.findall(".//skipped"):
            raise ValueError("source reconstruction was not executed")
        if case == 86:
            control = ET.parse(output / "unrepaired-parent.xml").getroot()
            if int(control.get("tests", "0")) != 1 or control.findall(".//skipped"):
                raise ValueError("unrepaired parent-chain control was not executed")
        images = [(original, False), (patched, True),
                  (output / "product-patched.exe", True),
                  (output / "collision-patched.exe", True)]
        if args.patch_binary:
            for mode in ("section", "inplace"):
                inputs = (("ir", [f"--from-ir={output / 'registration.ll'}", "--no-opt"]),
                          ("exe-no-opt", ["--max-func=1", "--no-debug", "--no-opt"]),
                          ("exe-opt", ["--max-func=1", "--no-debug"]))
                for source, options in inputs:
                    destination = output / f"cli-{mode}-{source}.exe"
                    destination.unlink(missing_ok=True)
                    command = [str(args.patch_binary.resolve()), "patch", str(original),
                               *options, f"--mode={mode}", "-o", str(destination)]
                    result = subprocess.run(command, env=environment, capture_output=True,
                                            text=True, errors="replace", timeout=args.timeout)
                    report["steps"].append({"command": command, "exit_code": result.returncode,
                                            "stdout": result.stdout, "stderr": result.stderr})
                    if result.returncode or not destination.is_file():
                        raise ValueError(f"CLI {mode} {source} reconstruction failed")
                    images.append((destination, True))
        for image, generated in images:
            report["observations"].append(observe(image, generated, launcher,
                                                 environment, args.timeout, expected_trace,
                                                 args.control_fp_mode))
            rebased = image.with_name(image.stem + "-rebased.exe")
            rebased.write_bytes(PE32(image.read_bytes()).rebase(0x18000000))
            report["observations"].append(observe(rebased, generated, launcher,
                                                 environment, args.timeout, expected_trace,
                                                 args.control_fp_mode))
        report["passed"] = True
    except (OSError, ValueError, struct.error, ET.ParseError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "registration-rewrite.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} reconstructed source: {destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
