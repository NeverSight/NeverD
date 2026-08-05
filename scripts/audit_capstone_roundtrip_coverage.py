#!/usr/bin/env python3
"""
Audit Capstone instruction roundtrip coverage.

Scans lifter source for all implemented `case *_INS_*` labels,
then scans roundtrip test files + gen script for coverage.

Usage:
    python3 scripts/audit_capstone_roundtrip_coverage.py [--verbose]

Output:
    Per-architecture coverage summary + uncovered instruction lists.
"""

import re
import sys
from pathlib import Path
from collections import defaultdict

PROJ = Path(__file__).parent.parent

# Patterns that match Capstone instruction IDs in lifter case labels
CASE_PAT = {
    "x86": re.compile(r"case\s+(X86_INS_\w+)"),
    "aarch64": re.compile(r"case\s+(AARCH64_INS_\w+)"),
    "arm32": re.compile(r"case\s+(ARM_INS_\w+)"),
}

LIFTER_DIRS = {
    "x86": PROJ / "lib/lift/X86",
    "aarch64": PROJ / "lib/lift/AArch64",
    "arm32": PROJ / "lib/lift/ARM",
}

SKIP_SUFFIXES = {
    "x86": {"X86_INS_INVALID", "X86_INS_ENDING"},
    "aarch64": {"AARCH64_INS_INVALID", "AARCH64_INS_ENDING"},
    "arm32": {"ARM_INS_INVALID", "ARM_INS_ENDING"},
}

TEST_DIR = PROJ / "unittests/semantic"
GEN_SCRIPT = PROJ / "scripts/gen_roundtrip_tests.py"


def extract_lifter_ids(arch: str) -> dict[str, set[str]]:
    """Extract implemented instruction IDs from lifter source, grouped by file."""
    pat = CASE_PAT[arch]
    result = defaultdict(set)
    for cpp in sorted(LIFTER_DIRS[arch].glob("*.cpp")):
        for line in cpp.read_text().splitlines():
            m = pat.search(line)
            if m:
                insn_id = m.group(1)
                if insn_id not in SKIP_SUFFIXES.get(arch, set()):
                    result[cpp.stem].add(insn_id)
    return result


def extract_gen_script_ids() -> dict[str, set[str]]:
    """Extract instruction IDs mapped in gen_roundtrip_tests.py."""
    result = {"x86": set(), "aarch64": set(), "arm32": set()}
    if not GEN_SCRIPT.exists():
        return result
    text = GEN_SCRIPT.read_text()
    for m in re.finditer(r'"(X86_INS_\w+)"', text):
        result["x86"].add(m.group(1))
    for m in re.finditer(r'"(AARCH64_INS_\w+)"', text):
        result["aarch64"].add(m.group(1))
    for m in re.finditer(r'"(ARM_INS_\w+)"', text):
        result["arm32"].add(m.group(1))
    return result


def extract_test_asm_mnemonics() -> dict[str, set[str]]:
    """Scan test .cpp files for inline asm mnemonics that map to instructions."""
    x86_mnemonics = set()
    a64_mnemonics = set()
    arm_mnemonics = set()

    x86_asm_pat = re.compile(r'__asm__\s*(?:volatile\s*)?\(\s*"([^"]+)"')
    builtin_pat = re.compile(r'__builtin_(\w+)')

    for cpp in sorted(TEST_DIR.glob("*.cpp")):
        if "Fixture" in cpp.name or cpp.name.endswith(".h"):
            continue
        text = cpp.read_text()

        is_x64 = "X64" in cpp.name or "X86" in cpp.name or "x64" in cpp.name
        is_a64 = "AArch64" in cpp.name or "A64" in cpp.name or "a64" in cpp.name
        is_arm = "ARM32" in cpp.name or "arm32" in cpp.name
        is_all = "AllPlatform" in cpp.name

        for m in x86_asm_pat.finditer(text):
            asm_str = m.group(1).split("\\n")[0].strip()
            mnem = asm_str.split()[0].rstrip("bwlqBWLQ") if asm_str else ""
            if mnem:
                if is_x64 or is_all:
                    x86_mnemonics.add(mnem.lower())
                if is_a64 or is_all:
                    a64_mnemonics.add(mnem.lower())
                if is_arm or is_all:
                    arm_mnemonics.add(mnem.lower())

    return {
        "x86": x86_mnemonics,
        "aarch64": a64_mnemonics,
        "arm32": arm_mnemonics,
    }


def mnemonic_from_id(insn_id: str) -> str:
    """Extract mnemonic from Capstone ID: X86_INS_ADD -> add"""
    parts = insn_id.split("_INS_")
    if len(parts) != 2:
        return ""
    return parts[1].lower()


def estimate_test_file_coverage(arch: str) -> set[str]:
    """Count unique RoundTripTC test names to estimate coverage depth."""
    pat = re.compile(r'\{"(\w+)"')
    names = set()
    prefix_map = {
        "x86": ["X64_", "X86_", "AllPlatform_"],
        "aarch64": ["AArch64_", "A64_", "AllPlatform_"],
        "arm32": ["ARM32_", "AllPlatform_"],
    }
    for cpp in sorted(TEST_DIR.glob("*.cpp")):
        if "Fixture" in cpp.name or cpp.name.endswith(".h"):
            continue
        matches_arch = any(cpp.name.startswith(p) for p in prefix_map[arch])
        if not matches_arch:
            continue
        if "RoundTrip" in cpp.name or "CExpr" in cpp.name or "RT" in cpp.name:
            for m in pat.finditer(cpp.read_text()):
                names.add(m.group(1))
    return names


def main():
    verbose = "--verbose" in sys.argv or "-v" in sys.argv

    print("=" * 70)
    print("NeverD Capstone RoundTrip Coverage Audit")
    print("=" * 70)

    gen_ids = extract_gen_script_ids()

    for arch in ["x86", "aarch64", "arm32"]:
        print(f"\n{'─' * 70}")
        print(f"  {arch.upper()}")
        print(f"{'─' * 70}")

        lifter_by_file = extract_lifter_ids(arch)
        all_lifter_ids = set()
        for ids in lifter_by_file.values():
            all_lifter_ids.update(ids)

        gen_covered = gen_ids[arch]
        test_names = estimate_test_file_coverage(arch)

        mnem_covered = set()
        for insn_id in all_lifter_ids:
            mnem = mnemonic_from_id(insn_id)
            for name in test_names:
                if mnem and (mnem in name.lower() or name.lower().startswith(mnem)):
                    mnem_covered.add(insn_id)
                    break

        all_covered = gen_covered | mnem_covered
        uncovered = all_lifter_ids - all_covered

        total = len(all_lifter_ids)
        covered = len(all_covered)
        gen_only = len(gen_covered)
        mnem_only = len(mnem_covered - gen_covered)

        print(f"\n  Lifter 已实现指令: {total}")
        print(f"  gen_roundtrip 显式映射: {gen_only}")
        print(f"  手写测试推测覆盖: {mnem_only}")
        print(f"  总覆盖 (下界): {covered}/{total} ({100*covered/total:.1f}%)")
        print(f"  未覆盖: {len(uncovered)}")

        if verbose or True:
            by_file = defaultdict(list)
            for insn_id in sorted(uncovered):
                for fname, ids in lifter_by_file.items():
                    if insn_id in ids:
                        by_file[fname].append(insn_id)
                        break

            if by_file:
                print(f"\n  未覆盖指令（按 lifter 文件分组）:")
                for fname in sorted(by_file.keys()):
                    ids = by_file[fname]
                    print(f"    {fname} ({len(ids)} 条):")
                    for i, insn_id in enumerate(ids):
                        if i < 20 or verbose:
                            print(f"      - {insn_id}")
                        elif i == 20:
                            print(f"      ... 还有 {len(ids) - 20} 条 (用 --verbose 查看全部)")
                            break

    print(f"\n{'=' * 70}")
    print("完成。用 --verbose 查看所有未覆盖指令的完整列表。")


if __name__ == "__main__":
    main()
