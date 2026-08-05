#!/usr/bin/env python3
"""
Auto-generate semantic roundtrip test cases by scanning lifter source code
for all implemented capstone instruction IDs.

For each instruction, generates a C wrapper function with inline asm
that exercises the instruction, along with the GoogleTest registration.

Usage:
    python3 scripts/gen_roundtrip_tests.py

Output:
    unittests/semantic/X64_AutoRoundTripTests.cpp
    unittests/semantic/AArch64_AutoRoundTripTests.cpp
    unittests/semantic/ARM32_AutoRoundTripTests.cpp
"""

import os
import re
import sys
from pathlib import Path
from collections import defaultdict

PROJ_ROOT = Path(__file__).parent.parent

# ============================================================================
# Step 1: Extract all implemented capstone instruction IDs from lifter source
# ============================================================================

def extract_insn_ids(lift_dir: Path, prefix: str) -> dict[str, list[str]]:
    """Extract case XXX_INS_YYY labels grouped by source file."""
    pat = re.compile(rf"case\s+({prefix}_INS_\w+)")
    result = defaultdict(set)
    for cpp in sorted(lift_dir.glob("*.cpp")):
        fname = cpp.stem
        for line in cpp.read_text().splitlines():
            m = pat.search(line)
            if m:
                result[fname].add(m.group(1))
    return {k: sorted(v) for k, v in result.items()}


# ============================================================================
# Step 2: Map instruction IDs to C wrapper functions
# ============================================================================

# Map entry: (asm_template, operand_count, category, args[, extra_flags[, opt_level]])
# Template placeholders: %0=dst, %1=src1, %2=src2
X86_INSN_MAP = {
    # Core ALU
    "X86_INS_ADD":      ("addq %1, %0",     2, "alu_binary",   [100, 50]),
    "X86_INS_SUB":      ("subq %1, %0",     2, "alu_binary",   [100, 30]),
    "X86_INS_AND":      ("andq %1, %0",     2, "alu_binary",   [0xFF00, 0x0FF0]),
    "X86_INS_OR":       ("orq %1, %0",      2, "alu_binary",   [0xF0, 0x0F]),
    "X86_INS_XOR":      ("xorq %1, %0",     2, "alu_binary",   [0xFF, 0x55]),
    "X86_INS_NOT":      ("notq %0",         1, "alu_unary",    [0xFF]),
    "X86_INS_NEG":      ("negq %0",         1, "alu_unary",    [42]),
    "X86_INS_INC":      ("incq %0",         1, "alu_unary",    [99]),
    "X86_INS_DEC":      ("decq %0",         1, "alu_unary",    [100]),
    "X86_INS_IMUL":     ("imulq %1, %0",    2, "alu_binary",   [6, 7]),
    "X86_INS_BSWAP":    ("bswapq %0",       1, "alu_unary",    [0x0102030405060708]),
    "X86_INS_SHL":      ("shlq $3, %0",     1, "shift_imm",    [5]),
    "X86_INS_SAL":      ("salq $3, %0",     1, "shift_imm",    [5]),
    "X86_INS_SHR":      ("shrq $2, %0",     1, "shift_imm",    [100]),
    "X86_INS_SAR":      ("sarq $2, %0",     1, "shift_imm",    [0xFFFFFFFFFFFFFF00]),
    "X86_INS_ROL":      ("rolq $4, %0",     1, "shift_imm",    [0xF]),
    "X86_INS_ROR":      ("rorq $4, %0",     1, "shift_imm",    [0xF0]),
    "X86_INS_BSF":      ("bsfq %1, %0",     2, "out_in",       [0x80]),
    "X86_INS_BSR":      ("bsrq %1, %0",     2, "out_in",       [0x80]),
    "X86_INS_ADC":      ("stc\\n\\tadcq %1, %0", 2, "alu_binary", [10, 20]),
    "X86_INS_SBB":      ("stc\\n\\tsbbq %1, %0", 2, "alu_binary", [100, 30]),
    "X86_INS_BT":       ("btq $3, %1\\n\\tsetc %b0", 2, "bt_setc", [0x8]),
    "X86_INS_BTS":      ("btsq $5, %0",     1, "alu_unary",    [0]),
    "X86_INS_BTR":      ("btrq $3, %0",     1, "alu_unary",    [0xFF]),
    "X86_INS_BTC":      ("btcq $3, %0",     1, "alu_unary",    [0]),
    "X86_INS_SHLD":     ("shldq $4, %1, %0", 2, "alu_binary", [0xF0, 0x0F]),
    "X86_INS_SHRD":     ("shrdq $4, %1, %0", 2, "alu_binary", [0xF0, 0x0F00000000000000]),
    "X86_INS_RCL":      ("clc\\n\\trclq $1, %0", 1, "shift_imm",    [0xF]),
    "X86_INS_RCR":      ("clc\\n\\trcrq $1, %0", 1, "shift_imm",    [0xF0]),

    "X86_INS_MOVZX":    ("movzbl %b1, %k0", 2, "out_in",       [0xFF42]),
    "X86_INS_MOVSX":    ("movsbl %b1, %k0", 2, "out_in",       [0x80]),
    "X86_INS_MOVSXD":   ("movslq %k1, %0",  2, "out_in",       [0xFFFFFFFF]),
    "X86_INS_CDQ":      ("cdq",             0, "custom",       []),
    "X86_INS_CQO":      ("cqo",             0, "custom",       []),
    "X86_INS_CDQE":     ("cdqe",            0, "custom",       []),
    "X86_INS_CBW":      ("cbw",             0, "custom",       []),
    "X86_INS_CWD":      ("cwd",             0, "custom",       []),
    "X86_INS_CWDE":     ("cwde",            0, "custom",       []),

    # BMI
    "X86_INS_ANDN":     ("andnq %2, %1, %0",  3, "alu_ternary_x86", [0xFF, 0x1234]),
    "X86_INS_BEXTR":    ("bextrq %2, %1, %0", 3, "alu_ternary_x86", [0xABCDEF, 0x0810]),
    "X86_INS_BLSI":     ("blsiq %1, %0",      2, "out_in",          [0x3C]),
    "X86_INS_BLSMSK":   ("blsmskq %1, %0",    2, "out_in",          [0x3C]),
    "X86_INS_BLSR":     ("blsrq %1, %0",      2, "out_in",          [0x3C]),
    "X86_INS_BZHI":     ("bzhiq %2, %1, %0",  3, "alu_ternary_x86", [0xFFFFFFFF, 16]),
    "X86_INS_PDEP":     ("pdepq %2, %1, %0",  3, "alu_ternary_x86", [0x12345678, 0xFF]),
    "X86_INS_PEXT":     ("pextq %2, %1, %0",  3, "alu_ternary_x86", [0x12345678, 0xFF]),
    "X86_INS_SARX":     ("sarxq %2, %1, %0",  3, "alu_ternary_x86", [0xFFFFFFFF80000000, 4]),
    "X86_INS_SHLX":     ("shlxq %2, %1, %0",  3, "alu_ternary_x86", [1, 10]),
    "X86_INS_SHRX":     ("shrxq %2, %1, %0",  3, "alu_ternary_x86", [0x100, 2]),
    "X86_INS_RORX":     ("rorxq $4, %1, %0",  2, "out_in",          [0xF0]),
    "X86_INS_MULX":     ("mulxq %2, %0, %1",  3, "alu_ternary_x86", [6, 7]),

    # Misc: XCHG, LEA
    "X86_INS_XCHG":     ("xchgq %0, %1",  2, "alu_binary",    [1, 2]),

    # Flag instructions
    "X86_INS_CLC":      ("clc",  0, "custom", []),
    "X86_INS_STC":      ("stc",  0, "custom", []),
    "X86_INS_CMC":      ("cmc",  0, "custom", []),
    "X86_INS_CLD":      ("cld",  0, "custom", []),
    "X86_INS_STD":      ("std",  0, "custom", []),

    # LAHF/SAHF
    "X86_INS_LAHF":     ("lahf", 0, "custom", []),
    "X86_INS_SAHF":     ("sahf", 0, "custom", []),

    # NOP
    "X86_INS_NOP":      ("nop",  0, "custom", []),

    # CMPXCHG
    "X86_INS_CMPXCHG":  ("cmpxchgq %1, (%0)", 2, "custom", []),

    # ADX
    "X86_INS_ADCX":     ("clc\\n\\tadcx %1, %0", 2, "alu_binary", [10, 20]),
    "X86_INS_ADOX":     ("clc\\n\\tadox %1, %0", 2, "alu_binary", [10, 20]),

    # CMOV variants (all test via cmp+cmov)
    "X86_INS_CMOVAE":   ("cmpq $50, %0\\n\\tcmovaeq %1, %0", 2, "alu_binary", [100, 42]),
    "X86_INS_CMOVB":    ("cmpq $50, %0\\n\\tcmovbq %1, %0",  2, "alu_binary", [30, 42]),
    "X86_INS_CMOVBE":   ("cmpq $50, %0\\n\\tcmovbeq %1, %0", 2, "alu_binary", [50, 42]),
    "X86_INS_CMOVGE":   ("cmpq $50, %0\\n\\tcmovgeq %1, %0", 2, "alu_binary", [100, 42]),
    "X86_INS_CMOVL":    ("cmpq $50, %0\\n\\tcmovlq %1, %0",  2, "alu_binary", [30, 42]),
    "X86_INS_CMOVLE":   ("cmpq $50, %0\\n\\tcmovleq %1, %0", 2, "alu_binary", [50, 42]),
    "X86_INS_CMOVNO":   ("cmpq $50, %0\\n\\tcmovnoq %1, %0", 2, "alu_binary", [100, 42]),
    "X86_INS_CMOVNP":   ("cmpq $50, %0\\n\\tcmovnpq %1, %0", 2, "alu_binary", [100, 42]),
    "X86_INS_CMOVNS":   ("cmpq $50, %0\\n\\tcmovnsq %1, %0", 2, "alu_binary", [100, 42]),
    "X86_INS_CMOVO":    ("addq %1, %0\\n\\tcmovoq %1, %0",   2, "alu_binary", [0x7FFFFFFFFFFFFFFF, 1]),
    "X86_INS_CMOVP":    ("cmpq $50, %0\\n\\tcmovpq %1, %0",  2, "alu_binary", [100, 42]),
    "X86_INS_CMOVS":    ("cmpq $50, %0\\n\\tcmovsq %1, %0",  2, "alu_binary", [100, 42]),

    # SETcc variants
    "X86_INS_SETAE":    ("cmpq $50, %1\\n\\tsetae %b0",  2, "bt_setc", [100]),
    # SETO needs careful overflow setup — tested via C expression instead
    "X86_INS_SETNO":    ("cmpq $50, %1\\n\\tsetno %b0",  2, "bt_setc", [100]),
    "X86_INS_SETP":     ("cmpq $50, %1\\n\\tsetp %b0",   2, "bt_setc", [100]),
    "X86_INS_SETNP":    ("cmpq $50, %1\\n\\tsetnp %b0",  2, "bt_setc", [100]),
    "X86_INS_SETS":     ("cmpq $50, %1\\n\\tsets %b0",    2, "bt_setc", [100]),
    "X86_INS_SETNS":    ("cmpq $50, %1\\n\\tsetns %b0",  2, "bt_setc", [100]),

    # PUSH/POP (stack)
    "X86_INS_PUSH":     ("pushq %0\\n\\tpopq %0", 1, "alu_unary", [42]),
    "X86_INS_PUSHFQ":   ("pushfq\\n\\tpopfq", 0, "custom", []),
    "X86_INS_POPFQ":    ("pushfq\\n\\tpopfq", 0, "custom", []),

    # Missing CMOV variants
    "X86_INS_CMOVA":    ("cmpq $50, %0\\n\\tcmovaq %1, %0",  2, "alu_binary", [100, 42]),
    "X86_INS_CMOVE":    ("cmpq $50, %0\\n\\tcmoveq %1, %0",  2, "alu_binary", [50, 42]),
    "X86_INS_CMOVG":    ("cmpq $50, %0\\n\\tcmovgq %1, %0",  2, "alu_binary", [100, 42]),
    "X86_INS_CMOVNE":   ("cmpq $50, %0\\n\\tcmovneq %1, %0", 2, "alu_binary", [100, 42]),

    # Missing SETcc
    "X86_INS_SETA":     ("cmpq $50, %1\\n\\tseta %b0",   2, "bt_setc", [100]),
    "X86_INS_SETB":     ("cmpq $50, %1\\n\\tsetb %b0",   2, "bt_setc", [30]),
    "X86_INS_SETBE":    ("cmpq $50, %1\\n\\tsetbe %b0",  2, "bt_setc", [50]),
    "X86_INS_SETE":     ("cmpq $50, %1\\n\\tsete %b0",   2, "bt_setc", [50]),
    "X86_INS_SETG":     ("cmpq $50, %1\\n\\tsetg %b0",   2, "bt_setc", [100]),
    "X86_INS_SETGE":    ("cmpq $50, %1\\n\\tsetge %b0",  2, "bt_setc", [50]),
    "X86_INS_SETL":     ("cmpq $50, %1\\n\\tsetl %b0",   2, "bt_setc", [30]),
    "X86_INS_SETLE":    ("cmpq $50, %1\\n\\tsetle %b0",  2, "bt_setc", [50]),
    "X86_INS_SETNE":    ("cmpq $50, %1\\n\\tsetne %b0",  2, "bt_setc", [100]),

    # TEST instruction
    "X86_INS_TEST":     ("testq %0, %1\\n\\tsete %b0", 2, "bt_setc", [0xFF]),

    # XADD
    "X86_INS_XADD":     ("xaddq %1, %0", 2, "alu_binary", [10, 20]),

    # TZCNT/LZCNT (BMI1)
    "X86_INS_TZCNT":    ("tzcntq %1, %0", 2, "out_in", [0x80]),
    "X86_INS_LZCNT":    ("lzcntq %1, %0", 2, "out_in", [0x80]),
    "X86_INS_POPCNT":   ("popcntq %1, %0", 2, "out_in", [0xFF]),

    # Core ALU not previously in auto-gen map
    "X86_INS_LEA":      ("", 0, "x86_lea",       [100, 10]),
    "X86_INS_CMP":      ("", 0, "x86_cmp",       [50, 50]),
    "X86_INS_MUL":      ("", 0, "x86_mul",       [6, 7]),
    "X86_INS_DIV":      ("", 0, "x86_div",       [7]),
    "X86_INS_IDIV":     ("", 0, "x86_idiv",      [7]),
    "X86_INS_MOV":      ("movq %1, %0", 2, "out_in", [42]),
    "X86_INS_MOVABS":   ("movabsq $0x1234567890ABCDEF, %0", 1, "out_zero", []),

    # SSE scalar FP (bitcast long args; ExtraFlags=-msse2)
    "X86_INS_ADDSS":    ("addss %1, %0", 2, "sse_ss_bin", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_SUBSS":    ("subss %1, %0", 2, "sse_ss_bin", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_MULSS":    ("mulss %1, %0", 2, "sse_ss_bin", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_DIVSS":    ("divss %1, %0", 2, "sse_ss_bin", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_SQRTSS":   ("sqrtss %1, %0", 1, "sse_ss_una", [0x40A00000], "-msse"),
    "X86_INS_MINSS":    ("minss %1, %0", 2, "sse_ss_bin", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_MAXSS":    ("maxss %1, %0", 2, "sse_ss_bin", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_ADDSD":    ("addsd %1, %0", 2, "sse_sd_bin", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_SUBSD":    ("subsd %1, %0", 2, "sse_sd_bin", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_MULSD":    ("mulsd %1, %0", 2, "sse_sd_bin", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_DIVSD":    ("divsd %1, %0", 2, "sse_sd_bin", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_SQRTSD":   ("sqrtsd %1, %0", 1, "sse_sd_una", [0x4014000000000000], "-msse2"),
    "X86_INS_MINSD":    ("minsd %1, %0", 2, "sse_sd_bin", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_MAXSD":    ("maxsd %1, %0", 2, "sse_sd_bin", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_CVTSS2SD": ("cvtss2sd %1, %0", 1, "sse_ss2sd", [0x40A00000], "-msse2"),
    "X86_INS_CVTSD2SS": ("cvtsd2ss %1, %0", 1, "sse_sd2ss", [0x4014000000000000], "-msse2"),
    "X86_INS_CVTSI2SS": ("", 0, "sse_cvtsi2ss", [5], "-msse"),
    "X86_INS_CVTSI2SD": ("", 0, "sse_cvtsi2sd", [5], "-msse2"),
    "X86_INS_CVTTSS2SI":("", 0, "sse_cvttss2si", [0x40A00000], "-msse"),
    "X86_INS_CVTTSD2SI":("", 0, "sse_cvttsd2si", [0x4014000000000000], "-msse2"),

    # SSE bitwise (lane-wise on full xmm)
    "X86_INS_ANDPS":    ("andps %1, %0", 2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse"),
    "X86_INS_ORPS":     ("orps %1, %0",  2, "sse_xmm_bin", [0x00000000FFFFFFFF, 0xFFFFFFFF00000000], "-msse"),
    "X86_INS_XORPS":    ("xorps %1, %0", 2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse"),
    "X86_INS_ANDPD":    ("andpd %1, %0", 2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse2"),
    "X86_INS_ORPD":     ("orpd %1, %0",  2, "sse_xmm_bin", [0x00000000FFFFFFFF, 0xFFFFFFFF00000000], "-msse2"),
    "X86_INS_XORPD":    ("xorpd %1, %0", 2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse2"),

    # x87 scalar (80-bit stack via inline asm on double bitcast).  The FP work is
    # entirely in inline asm with memory operands, so no -mfpmath=387 is needed
    # (and on x86_64 that flag is rejected: "the '387' unit is not supported").
    # Binary ops use the m64 form (faddl/fsubl/fmull/fdivl) so only one x87 stack
    # slot is live -- the pop-form FADDP/etc. have known lifter stack-tracking
    # issues.  asm_tmpl is the bare mnemonic; the x87_bin template appends "l %2".
    "X86_INS_FADD":     ("fadd", 0, "x87_bin", [0x4014000000000000, 0x4008000000000000]),
    "X86_INS_FSUB":     ("fsub", 0, "x87_bin", [0x4014000000000000, 0x4008000000000000]),
    "X86_INS_FMUL":     ("fmul", 0, "x87_bin", [0x4014000000000000, 0x4008000000000000]),
    "X86_INS_FDIV":     ("fdiv", 0, "x87_bin", [0x4014000000000000, 0x4000000000000000]),
    "X86_INS_FSQRT":    ("fsqrt", 0, "x87_una", [0x4014000000000000]),
    "X86_INS_FABS":     ("fabs", 0, "x87_una", [0xC014000000000000]),
    "X86_INS_FCHS":     ("fchs", 0, "x87_una", [0xC014000000000000]),

    "X86_INS_MOVSS":    ("movss %1, %0", 2, "sse_ss_mov", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_MOVSD":    ("movsd %1, %0", 2, "sse_sd_mov", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_UCOMISS":  ("ucomiss %%xmm0, %1\\n\\tsete %b0", 2, "sse_ss_cmp", [0x40A00000, 0x40A00000], "-msse"),
    "X86_INS_UCOMISD":  ("ucomisd %%xmm0, %1\\n\\tsete %b0", 2, "sse_sd_cmp", [0x4014000000000000, 0x4014000000000000], "-msse2"),
    "X86_INS_COMISS":   ("comiss %%xmm0, %1\\n\\tseta %b0", 2, "sse_ss_cmp", [0x40A00000, 0x40400000], "-msse"),
    "X86_INS_COMISD":   ("comisd %%xmm0, %1\\n\\tseta %b0", 2, "sse_sd_cmp", [0x4014000000000000, 0x4008000000000000], "-msse2"),
    "X86_INS_RSQRTSS":  ("rsqrtss %1, %0", 1, "sse_ss_una", [0x40A00000], "-msse"),
    "X86_INS_RCPSS":    ("rcpss %1, %0", 1, "sse_ss_una", [0x40A00000], "-msse"),

    "X86_INS_PADDD":    ("paddd %1, %0", 2, "sse_xmm_bin", [0x0000000200000001, 0x0000000400000003], "-msse2"),
    "X86_INS_PSUBD":    ("psubd %1, %0", 2, "sse_xmm_bin", [0x0000000600000005, 0x0000000200000001], "-msse2"),
    "X86_INS_PAND":     ("pand %1, %0",  2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse2"),
    "X86_INS_POR":      ("por %1, %0",   2, "sse_xmm_bin", [0x00000000FFFFFFFF, 0xFFFFFFFF00000000], "-msse2"),
    "X86_INS_PXOR":     ("pxor %1, %0",  2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse2"),
    "X86_INS_PANDN":    ("pandn %1, %0", 2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse2"),
    "X86_INS_PCMPEQD":  ("pcmpeqd %1, %0", 2, "sse_xmm_bin", [0x0000000200000001, 0x0000000200000001], "-msse2"),
    "X86_INS_PSLLD":    ("pslld $2, %0", 1, "sse_xmm_shift", [0x0000000200000001], "-msse2"),
    "X86_INS_PSRLD":    ("psrld $1, %0", 1, "sse_xmm_shift", [0x0000000600000004], "-msse2"),
    "X86_INS_PSRAD":    ("psrad $1, %0", 1, "sse_xmm_shift", [0xFFFFFFF800000004], "-msse2"),
    "X86_INS_PABSD":    ("pabsd %1, %0", 1, "sse_xmm_una", [0xFFFFFFF800000004], "-mssse3"),
    "X86_INS_PMINUD":   ("pminud %1, %0", 2, "sse_xmm_bin", [0x0000000600000005, 0x0000000400000003], "-msse4.1"),
    "X86_INS_PMAXUD":   ("pmaxud %1, %0", 2, "sse_xmm_bin", [0x0000000600000005, 0x0000000400000003], "-msse4.1"),
    "X86_INS_MOVAPS":   ("movaps %0, %1", 2, "sse_xmm_mov", [0x0000000200000001, 0x0000000400000003], "-msse"),
    "X86_INS_MOVUPS":   ("movups %0, %1", 2, "sse_xmm_mov", [0x0000000200000001, 0x0000000400000003], "-msse"),
    "X86_INS_ANDNPS":   ("andnps %1, %0", 2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse"),
    "X86_INS_ANDNPD":   ("andnpd %1, %0", 2, "sse_xmm_bin", [0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF], "-msse2"),
}

# AArch64 instruction → (asm_template, operand_count, category)
A64_INSN_MAP = {
    "AARCH64_INS_ADD":  ("add %0, %1, %2",   3, "alu_ternary",  [50, 30]),
    "AARCH64_INS_SUB":  ("sub %0, %1, %2",   3, "alu_ternary",  [100, 30]),
    "AARCH64_INS_AND":  ("and %0, %1, %2",   3, "alu_ternary",  [0xFF00, 0x0FF0]),
    "AARCH64_INS_ORR":  ("orr %0, %1, %2",   3, "alu_ternary",  [0xF0, 0x0F]),
    "AARCH64_INS_EOR":  ("eor %0, %1, %2",   3, "alu_ternary",  [0xFF, 0x55]),
    "AARCH64_INS_NEG":  ("neg %0, %1",       2, "out_in",       [42]),
    "AARCH64_INS_MUL":  ("mul %0, %1, %2",   3, "alu_ternary",  [6, 7]),
    "AARCH64_INS_UDIV": ("udiv %0, %1, %2",  3, "alu_ternary",  [100, 7]),
    "AARCH64_INS_SDIV": ("sdiv %0, %1, %2",  3, "alu_ternary",  [100, 7]),
    "AARCH64_INS_LSL":  ("lsl %0, %1, #4",   2, "out_in",       [1]),
    "AARCH64_INS_LSR":  ("lsr %0, %1, #2",   2, "out_in",       [100]),
    "AARCH64_INS_ASR":  ("asr %0, %1, #2",   2, "out_in",       [0xFFFFFFFFFFFFFF00]),
    "AARCH64_INS_CLZ":  ("clz %0, %1",       2, "out_in",       [0x100]),
    "AARCH64_INS_RBIT": ("rbit %0, %1",      2, "out_in",       [1]),
    "AARCH64_INS_REV":  ("rev %0, %1",       2, "out_in",       [0x0102030405060708]),
    "AARCH64_INS_MADD": ("madd %0, %1, %2, %3", 4, "alu_quad", [6, 7, 10]),
    "AARCH64_INS_MSUB": ("msub %0, %1, %2, %3", 4, "alu_quad", [6, 7, 100]),
    "AARCH64_INS_BIC":  ("bic %0, %1, %2",   3, "alu_ternary",  [0xFFFF, 0xFF]),
    "AARCH64_INS_ORN":  ("orn %0, %1, %2",   3, "alu_ternary",  [0xF0, 0x0F]),
    "AARCH64_INS_EON":  ("eon %0, %1, %2",   3, "alu_ternary",  [0xFF, 0x55]),
    "AARCH64_INS_REV16":("rev16 %0, %1",     2, "out_in",       [0x0102030405060708]),
    "AARCH64_INS_REV32":("rev32 %0, %1",     2, "out_in",       [0x0102030405060708]),
    "AARCH64_INS_CLS":  ("cls %0, %1",       2, "out_in",       [0xFFFFFFFFFFFFFF00]),
    "AARCH64_INS_SMULL":("smull %0, %w1, %w2", 3, "alu_ternary", [6, 7]),
    "AARCH64_INS_UMULL":("umull %0, %w1, %w2", 3, "alu_ternary", [6, 7]),
    "AARCH64_INS_UMULH":("umulh %0, %1, %2",  3, "alu_ternary", [0x100000000, 0x100000000]),
    "AARCH64_INS_SMULH":("smulh %0, %1, %2",  3, "alu_ternary", [0x100000000, 0x100000000]),
    "AARCH64_INS_ADDS": ("adds %0, %1, %2",   3, "alu_ternary", [50, 30]),
    "AARCH64_INS_SUBS": ("subs %0, %1, %2",   3, "alu_ternary", [100, 30]),
    "AARCH64_INS_ANDS": ("ands %0, %1, %2",   3, "alu_ternary", [0xFF00, 0x0FF0]),
    "AARCH64_INS_BICS": ("bics %0, %1, %2",   3, "alu_ternary", [0xFFFF, 0xFF]),
    "AARCH64_INS_ROR":  ("ror %0, %1, %2",    3, "alu_ternary", [0xF0, 4]),
    "AARCH64_INS_EXTR": ("extr %0, %1, %2, #4", 3, "alu_ternary", [0xF0, 0x0F]),
    "AARCH64_INS_CSEL": ("cmp %1, %2\\n\\tcsel %0, %1, %2, eq", 3, "alu_ternary", [42, 42]),
    "AARCH64_INS_CSINC":("cmp %1, %2\\n\\tcsinc %0, %1, %2, eq", 3, "alu_ternary", [42, 42]),
    "AARCH64_INS_CSINV":("cmp %1, %2\\n\\tcsinv %0, %1, %2, ne", 3, "alu_ternary", [42, 43]),
    "AARCH64_INS_CSNEG":("cmp %1, %2\\n\\tcsneg %0, %1, %2, ne", 3, "alu_ternary", [42, 43]),
    "AARCH64_INS_SXTW": ("sxtw %0, %w1",     2, "out_in",       [0xFFFFFFFF]),
    "AARCH64_INS_SXTB": ("sxtb %0, %w1",     2, "out_in",       [0x80]),
    "AARCH64_INS_SXTH": ("sxth %0, %w1",     2, "out_in",       [0x8000]),
    "AARCH64_INS_UXTB": ("uxtb %w0, %w1",    2, "out_in",       [0xABCD]),
    "AARCH64_INS_UXTH": ("uxth %w0, %w1",    2, "out_in",       [0xABCDEF]),
    "AARCH64_INS_SMADDL":("smaddl %0, %w1, %w2, %3", 4, "alu_quad", [6, 7, 10]),
    "AARCH64_INS_UMADDL":("umaddl %0, %w1, %w2, %3", 4, "alu_quad", [6, 7, 10]),
    "AARCH64_INS_MOVZ": ("movz %0, #0x1234",  1, "out_zero",    []),
    "AARCH64_INS_MOVN": ("movn %0, #0",       1, "out_zero",    []),

    # ADC/SBC with carry
    "AARCH64_INS_ADC":  ("adds xzr, xzr, xzr\\n\\tadc %0, %1, %2", 3, "alu_ternary", [50, 30]),
    "AARCH64_INS_ADCS": ("adds xzr, xzr, xzr\\n\\tadcs %0, %1, %2", 3, "alu_ternary", [50, 30]),

    # CCMN/CCMP — complex multi-instruction, tested via C expression instead

    # MVN
    "AARCH64_INS_MVN":  ("mvn %0, %1",       2, "out_in",       [0xFF]),

    # EON/BIC/ORN
    "AARCH64_INS_EON":  ("eon %0, %1, %2",   3, "alu_ternary",  [0xFF, 0x55]),

    # ADDS/SUBS (flag-setting)
    "AARCH64_INS_ADDS": ("adds %0, %1, %2",  3, "alu_ternary",  [100, 50]),
    "AARCH64_INS_SUBS": ("subs %0, %1, %2",  3, "alu_ternary",  [100, 30]),

    # ROR
    "AARCH64_INS_ROR":  ("ror %0, %1, #4",   2, "out_in",       [0xF0]),

    # REV16/REV32
    "AARCH64_INS_REV16":("rev16 %0, %1",     2, "out_in",       [0x0102030405060708]),
    "AARCH64_INS_REV32":("rev32 %0, %1",     2, "out_in",       [0x0102030405060708]),

    # EXTR
    "AARCH64_INS_EXTR": ("extr %0, %1, %2, #16", 3, "alu_ternary", [0xAAAA0000, 0x0000BBBB]),

    # CSEL/CSINC/CSINV/CSNEG
    "AARCH64_INS_CSEL": ("cmp %1, %2\\n\\tcsel %0, %1, %2, gt", 3, "alu_ternary", [100, 50]),
    "AARCH64_INS_CSINC":("cmp %1, %2\\n\\tcsinc %0, %1, %2, gt", 3, "alu_ternary", [100, 50]),
    "AARCH64_INS_CSINV":("cmp %1, %2\\n\\tcsinv %0, %1, %2, gt", 3, "alu_ternary", [100, 50]),
    "AARCH64_INS_CSNEG":("cmp %1, %2\\n\\tcsneg %0, %1, %2, gt", 3, "alu_ternary", [100, 50]),

    # SMULL/UMULL (scalar widening)
    "AARCH64_INS_SMULL":("smull %0, %w1, %w2", 3, "alu_ternary", [0xFFFFFFFF, 2]),
    "AARCH64_INS_UMULL":("umull %0, %w1, %w2", 3, "alu_ternary", [0xFFFFFFFF, 2]),

    # SMADDL/UMADDL/SMSUBL
    "AARCH64_INS_SMADDL":("smaddl %0, %w1, %w2, %3", 4, "alu_quad", [6, 7, 10]),
    "AARCH64_INS_SMSUBL":("smsubl %0, %w1, %w2, %3", 4, "alu_quad", [6, 7, 100]),

    # CLS
    "AARCH64_INS_CLS":  ("cls %0, %1",       2, "out_in",       [0xFF]),

    # CCMN/CCMP tested via C expression

    # SBFM/UBFM aliases (SXTB etc.) tested via dedicated roundtrip tests

    "AARCH64_INS_MOV":  ("mov %0, %1",       2, "out_in",       [42]),
    "AARCH64_INS_CMP":  ("", 0, "a64_cmp",    [50, 50]),
    "AARCH64_INS_MOVK": ("movz %0, #0x1234\\n\\tmovk %0, #0x5678, lsl #16", 1, "out_zero", []),

    # The scalar integer ABS (abs Rd, Rn on general registers) is FEAT_CSSC
    # (ARMv8.9); default clang rejects it ("instruction requires: cssc") and the
    # bundled Unicorn cannot execute it, so it is not round-trip testable here.
    "AARCH64_INS_TST":  ("tst %1, %2\\n\\tcset %0, eq", 3, "alu_ternary", [0xFF, 0x0F]),
    "AARCH64_INS_CMN":  ("cmn %1, %2\\n\\tcset %0, eq", 3, "alu_ternary", [50, 0xFFFFFFCE]),
    "AARCH64_INS_NGC":  ("ngc %0, %1",       2, "out_in",       [42]),
    "AARCH64_INS_NGCS": ("ngcs %0, %1",      2, "out_in",       [42]),
    "AARCH64_INS_UBFM": ("ubfm %0, %1, #4, #11", 2, "out_in",   [0xABCD]),
    "AARCH64_INS_SBFM": ("sbfm %0, %1, #4, #11", 2, "out_in",   [0xABCD]),
    "AARCH64_INS_BFM":  ("bfm %0, %1, #8, #15", 2, "out_in",   [0xFFFF0000]),
    "AARCH64_INS_UBFX": ("ubfx %0, %1, #4, #8", 2, "out_in",   [0xABCD]),
    "AARCH64_INS_SBFX": ("sbfx %0, %1, #4, #8", 2, "out_in",   [0xABCD]),
    "AARCH64_INS_BFXIL":("bfxil %0, %1, #8, #8", 2, "alu_binary_a64", [0xFFFF0000, 0xAB]),
    "AARCH64_INS_SBFIZ":("sbfiz %0, %1, #8, #8", 2, "out_in",   [0x80]),
    "AARCH64_INS_UBFIZ":("ubfiz %0, %1, #8, #8", 2, "out_in",   [0xAB]),
    "AARCH64_INS_ASRV": ("asrv %0, %1, %2",   3, "alu_ternary",  [0xFFFFFFFFFFFFFF00, 2]),
    "AARCH64_INS_LSLV": ("lslv %0, %1, %2",   3, "alu_ternary",  [5, 3]),
    "AARCH64_INS_LSRV": ("lsrv %0, %1, %2",   3, "alu_ternary",  [100, 2]),
    "AARCH64_INS_RORV": ("rorv %0, %1, %2",   3, "alu_ternary",  [0xF0, 4]),
    "AARCH64_INS_ADDW": ("add %w0, %w1, %w2", 3, "alu_ternary",  [50, 30]),
    "AARCH64_INS_SUBW": ("sub %w0, %w1, %w2", 3, "alu_ternary",  [100, 30]),
    "AARCH64_INS_MULW": ("mul %w0, %w1, %w2", 3, "alu_ternary",  [6, 7]),
    "AARCH64_INS_UDIVW":("udiv %w0, %w1, %w2", 3, "alu_ternary", [100, 7]),
    "AARCH64_INS_SDIVW":("sdiv %w0, %w1, %w2", 3, "alu_ternary", [100, 7]),
    "AARCH64_INS_MNEG": ("mneg %0, %1",       2, "out_in",       [42]),
    "AARCH64_INS_ANDS": ("ands %w0, %w1, %w2", 3, "alu_ternary",  [0xFF00, 0x0FF0]),
}

# ARM32 instruction → (asm_template, operand_count, category)
ARM32_INSN_MAP = {
    "ARM_INS_ADD":      ("add %0, %1, %2",   3, "alu_ternary",  [50, 30]),
    "ARM_INS_SUB":      ("sub %0, %1, %2",   3, "alu_ternary",  [100, 30]),
    "ARM_INS_AND":      ("and %0, %1, %2",   3, "alu_ternary",  [0xFF00, 0x0FF0]),
    "ARM_INS_ORR":      ("orr %0, %1, %2",   3, "alu_ternary",  [0xF0, 0x0F]),
    "ARM_INS_EOR":      ("eor %0, %1, %2",   3, "alu_ternary",  [0xFF, 0x55]),
    "ARM_INS_MVN":      ("mvn %0, %1",       2, "out_in",       [0]),
    "ARM_INS_MUL":      ("mul %0, %1, %2",   3, "alu_ternary",  [6, 7]),
    "ARM_INS_MLA":      ("mla %0, %1, %2, %3", 4, "alu_quad",  [6, 7, 10]),
    "ARM_INS_SDIV":     ("sdiv %0, %1, %2",  3, "alu_ternary",  [100, 7]),
    "ARM_INS_UDIV":     ("udiv %0, %1, %2",  3, "alu_ternary",  [100, 7]),
    "ARM_INS_LSL":      ("lsl %0, %1, #3",   2, "out_in",       [5]),
    "ARM_INS_LSR":      ("lsr %0, %1, #2",   2, "out_in",       [100]),
    "ARM_INS_ASR":      ("asr %0, %1, #2",   2, "out_in",       [0xFFFFFF00]),
    "ARM_INS_ROR":      ("ror %0, %1, #4",   2, "out_in",       [0xF0]),
    "ARM_INS_REV":      ("rev %0, %1",       2, "out_in",       [0x01020304]),
    "ARM_INS_REV16":    ("rev16 %0, %1",     2, "out_in",       [0x01020304]),
    "ARM_INS_UXTB":     ("uxtb %0, %1",      2, "out_in",       [0xABCD]),
    "ARM_INS_SXTB":     ("sxtb %0, %1",      2, "out_in",       [0x80]),
    "ARM_INS_UXTH":     ("uxth %0, %1",      2, "out_in",       [0xABCDEF]),
    "ARM_INS_SXTH":     ("sxth %0, %1",      2, "out_in",       [0x8000]),
    "ARM_INS_BIC":      ("bic %0, %1, %2",   3, "alu_ternary",  [0xFFFF, 0xFF]),
    "ARM_INS_MLS":      ("mls %0, %1, %2, %3", 4, "alu_quad",  [6, 7, 100]),
    "ARM_INS_SMULL":    ("smull %0, %1, %2, %3", 4, "alu_quad_pair", [6, 7]),
    "ARM_INS_UMULL":    ("umull %0, %1, %2, %3", 4, "alu_quad_pair", [6, 7]),
    "ARM_INS_RSB":      ("rsb %0, %1, %2",  3, "alu_ternary",  [30, 100]),
    "ARM_INS_RBIT":     ("rbit %0, %1",      2, "out_in",       [1]),
    "ARM_INS_REVSH":    ("revsh %0, %1",     2, "out_in",       [0x0102]),
    "ARM_INS_UXTAH":    ("uxtah %0, %1, %2", 3, "alu_ternary", [100, 0x1234]),
    "ARM_INS_SXTAH":    ("sxtah %0, %1, %2", 3, "alu_ternary", [100, 0x8000]),
    "ARM_INS_UXTAB":    ("uxtab %0, %1, %2", 3, "alu_ternary", [100, 0xAB]),
    "ARM_INS_SXTAB":    ("sxtab %0, %1, %2", 3, "alu_ternary", [100, 0x80]),
    "ARM_INS_PKH":      ("pkhbt %0, %1, %2", 3, "alu_ternary", [0xFFFF0000, 0x0000FFFF]),

    # ADC/SBC with carry
    "ARM_INS_ADC":      ("adds %0, %1, #0\\n\\tadc %0, %1, %2", 3, "alu_ternary", [50, 30]),
    "ARM_INS_SBC":      ("adds %0, %1, #0\\n\\tsbc %0, %1, %2", 3, "alu_ternary", [100, 30]),

    # MOV/MVN
    "ARM_INS_MOV":      ("mov %0, %1",       2, "out_in",       [42]),
    "ARM_INS_MOVW":     ("movw %0, #0x1234", 1, "out_zero",     []),
    "ARM_INS_MOVT":     ("movw %0, #0\\n\\tmovt %0, #0x5678", 1, "out_zero", []),

    # CMN/TST/TEQ
    "ARM_INS_CMN":      ("cmn %1, %2\\n\\tmoveq %0, #1\\n\\tmovne %0, #0", 3, "alu_ternary", [50, 0xFFFFFFCE]),
    "ARM_INS_TST":      ("tst %1, %2\\n\\tmoveq %0, #1\\n\\tmovne %0, #0", 3, "alu_ternary", [0xFF, 0x0F]),
    "ARM_INS_TEQ":      ("teq %1, %2\\n\\tmoveq %0, #1\\n\\tmovne %0, #0", 3, "alu_ternary", [42, 42]),

    # CLZ
    "ARM_INS_CLZ":      ("clz %0, %1",       2, "out_in",       [0x100]),

    # SUBS (flag-setting)
    "ARM_INS_SUBS":     ("subs %0, %1, %2",  3, "alu_ternary",  [100, 30]),

    # BFC/BFI
    "ARM_INS_BFC":      ("bfc %0, #4, #8",    1, "alu_unary",    [0xFFFFFFFF]),
    "ARM_INS_BFI":      ("bfi %0, %1, #8, #8", 2, "alu_binary", [0xFFFF0000, 0xAB]),

    # REV/REV16/RBIT
    "ARM_INS_REV":      ("rev %0, %1",        2, "out_in",       [0x01020304]),
    "ARM_INS_REV16":    ("rev16 %0, %1",      2, "out_in",       [0x01020304]),
    "ARM_INS_RBIT":     ("rbit %0, %1",       2, "out_in",       [1]),

    # UMULL/SMULL — need separate RdLo/RdHi, use widening mul C expression instead
    # Tested via C expression roundtrip (c_umull etc.) in ARM32_ExtendedCExprTests.cpp

    # UMLAL/SMLAL — same constraint, tested via C expression roundtrip

    # MLA
    "ARM_INS_MLA":      ("mla %0, %1, %2, %3", 4, "alu_quad",   [6, 7, 10]),
    "ARM_INS_MLS":      ("mls %0, %1, %2, %3", 4, "alu_quad",   [6, 7, 100]),

    # ADDS (flag-setting)
    "ARM_INS_ADDS":     ("adds %0, %1, %2",   3, "alu_ternary", [100, 50]),

    # CMP — conditional execution wrapping bug in eliminateFlags, tested via C expr
    # "ARM_INS_CMP":  needs fix in MedFlags SELECT+ZFLAG resolution
    # CMP flag-setting is tested through SUBS+branch and other patterns

    # UBFX/SBFX
    "ARM_INS_UBFX":     ("ubfx %0, %1, #4, #8", 2, "out_in",    [0xABCD]),
    "ARM_INS_SBFX":     ("sbfx %0, %1, #4, #8", 2, "out_in",    [0xABCD]),

    # MOVS
    "ARM_INS_MVN":      ("mvn %0, %1",        2, "out_in",       [0xFF]),

    # ORN needs Thumb2 mode, skip for ARM mode
    # "ARM_INS_ORN":    ("orn %0, %1, %2",   3, "alu_ternary",  [0xF0, 0x0F]),

    # CSEL/CSINC/CSINV/CSNEG are AArch64 (A64) conditional-select instructions;
    # they do not exist in the A32/T32 (ARM32) encoding and clang rejects them
    # ("invalid instruction").  ARM32 uses conditional execution (e.g. movgt/
    # movle) instead, covered via the CMN/TST/TEQ moveq/movne cases.
    "ARM_INS_ASR":      ("asr %0, %1, %2",   3, "alu_ternary",  [0xFFFFFF00, 2]),
    "ARM_INS_LSL":      ("lsl %0, %1, %2",   3, "alu_ternary",  [5, 3]),
    "ARM_INS_LSR":      ("lsr %0, %1, %2",   3, "alu_ternary",  [100, 2]),
    "ARM_INS_ROR":      ("ror %0, %1, %2",   3, "alu_ternary",  [0xF0, 4]),
    "ARM_INS_SXTB":     ("sxtb %0, %1",      2, "out_in",       [0x80]),
    "ARM_INS_UXTB":     ("uxtb %0, %1",      2, "out_in",       [0xABCD]),
    "ARM_INS_SXTH":     ("sxth %0, %1",      2, "out_in",       [0x8000]),
    "ARM_INS_UXTH":     ("uxth %0, %1",      2, "out_in",       [0xABCDEF]),
}


def parse_map_entry(entry: tuple) -> tuple:
    """Normalize map entry to (asm, nops, category, args, extra_flags, opt_level)."""
    if len(entry) < 4:
        raise ValueError(f"bad map entry: {entry}")
    asm, nops, category, args = entry[:4]
    extra = entry[4] if len(entry) > 4 else ""
    opt = entry[5] if len(entry) > 5 else 0
    return asm, nops, category, args, extra, opt


def gen_c_wrapper(insn_id: str, arch: str, insn_map: dict) -> tuple | None:
    """Generate a C wrapper function for an instruction."""
    if insn_id not in insn_map:
        return None

    asm_tmpl, nops, category, args, extra_flags, opt_level = parse_map_entry(
        insn_map[insn_id])
    safe_name = insn_id.split("_INS_")[1].lower()
    typ = "long" if arch != "arm32" else "int"

    if category == "alu_binary":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+r"(a) : "r"(b));\n'
            f'  return a;\n'
            f'}}\n'
        )
    elif category == "alu_unary" or category == "shift_imm":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+r"(a));\n'
            f'  return a;\n'
            f'}}\n'
        )
    elif category == "out_in":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=r"(r) : "r"(a));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "alu_ternary":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=r"(r) : "r"(a), "r"(b));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "alu_quad":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b, {typ} c) {{\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=r"(r) : "r"(a), "r"(b), "r"(c));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "bt_setc":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  {typ} r = 0;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+r"(r) : "r"(a));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "alu_ternary_x86":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=r"(r) : "r"(a), "r"(b));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "out_zero":
        src = (
            f'{typ} rt_{safe_name}(void) {{\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=r"(r));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "x86_lea":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("lea (%1,%2,4), %%rax" : "=a"(r) : "r"(a), "r"(b));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "x86_cmp":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  {typ} r = 0;\n'
            f'  __asm__ volatile ("cmp %%rdi, %%rsi\\n\\tsete %%al\\n\\tmovzbq %%al, %%rax"\n'
            f'                   : "=a"(r) : "D"(a), "S"(b) : "cc");\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "x86_mul":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  __asm__ volatile ("mulq %%rsi" : "+a"(a) : "S"(b) : "cc");\n'
            f'  return a;\n'
            f'}}\n'
        )
    elif category == "x86_div":
        src = (
            f'{typ} rt_{safe_name}({typ} b) {{\n'
            f'  {typ} a = 100, rdx = 0;\n'
            f'  __asm__ volatile ("xor %%rdx, %%rdx; divq %%rsi"\n'
            f'                   : "+a"(a) : "S"(b) : "rdx", "cc");\n'
            f'  return a;\n'
            f'}}\n'
        )
    elif category == "x86_idiv":
        src = (
            f'{typ} rt_{safe_name}({typ} b) {{\n'
            f'  {typ} a = 100, rdx = 0;\n'
            f'  __asm__ volatile ("cqo; idivq %%rsi"\n'
            f'                   : "+a"(a) : "S"(b) : "rdx", "cc");\n'
            f'  return a;\n'
            f'}}\n'
        )
    elif category == "a64_cmp":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("cmp %1, %2\\n\\tcset %0, eq" : "=r"(r) : "r"(a), "r"(b));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "alu_binary_a64":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  {typ} r = a;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+r"(r) : "r"(b));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_sd_bin":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  double af, bf;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  __builtin_memcpy(&bf, &b, 8);\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+x"(af) : "x"(bf));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &af, 8);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_sd_una":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  double af;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+x"(af));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &af, 8);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_ss_bin":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  float af, bf;\n'
            f'  __builtin_memcpy(&af, &a, 4);\n'
            f'  __builtin_memcpy(&bf, &b, 4);\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+x"(af) : "x"(bf));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &af, 4);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_ss_una":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  float af;\n'
            f'  __builtin_memcpy(&af, &a, 4);\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+x"(af));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &af, 4);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_xmm_bin":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  long long af = (long long)a, bf = (long long)b;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+x"(af) : "x"(bf));\n'
            f'  return (long)af;\n'
            f'}}\n'
        )
    elif category == "sse_ss2sd":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  float af;\n'
            f'  __builtin_memcpy(&af, &a, 4);\n'
            f'  double df;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=x"(df) : "x"(af));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &df, 8);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_sd2ss":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  double af;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  float sf;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=x"(sf) : "x"(af));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &sf, 4);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_cvtsi2ss":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  float sf;\n'
            f'  __asm__ volatile ("cvtsi2ss %1, %0" : "=x"(sf) : "r"(a));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &sf, 4);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_cvtsi2sd":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  double df;\n'
            f'  __asm__ volatile ("cvtsi2sd %1, %0" : "=x"(df) : "r"(a));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &df, 8);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_cvttss2si":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  float af;\n'
            f'  __builtin_memcpy(&af, &a, 4);\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("cvttss2si %1, %0" : "=r"(r) : "x"(af));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_cvttsd2si":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  double af;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  {typ} r;\n'
            f'  __asm__ volatile ("cvttsd2si %1, %0" : "=r"(r) : "x"(af));\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "x87_bin":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  double af, bf, rf;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  __builtin_memcpy(&bf, &b, 8);\n'
            f'  __asm__ volatile ("fldl %1; {asm_tmpl}l %2; fstpl %0"\n'
            f'                   : "=m"(rf) : "m"(af), "m"(bf) : "st");\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &rf, 8);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "x87_una":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  double af, rf;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  __asm__ volatile ("fldl %1; {asm_tmpl}; fstpl %0"\n'
            f'                   : "=m"(rf) : "m"(af) : "st");\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &rf, 8);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_ss_mov":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  float af, bf;\n'
            f'  __builtin_memcpy(&af, &a, 4);\n'
            f'  __builtin_memcpy(&bf, &b, 4);\n'
            f'  __asm__ volatile ("movss %1, %0" : "+x"(af) : "x"(bf));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &af, 4);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_sd_mov":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  double af, bf;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  __builtin_memcpy(&bf, &b, 8);\n'
            f'  __asm__ volatile ("movsd %1, %0" : "+x"(af) : "x"(bf));\n'
            f'  {typ} r;\n'
            f'  __builtin_memcpy(&r, &af, 8);\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_ss_cmp":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  float af, bf;\n'
            f'  __builtin_memcpy(&af, &a, 4);\n'
            f'  __builtin_memcpy(&bf, &b, 4);\n'
            f'  {typ} r = 0;\n'
            f'  __asm__ volatile ("movss %3, %%xmm0; {asm_tmpl}"\n'
            f'                   : "+r"(r) : "x"(bf), "r"((int)0), "x"(af) : "cc");\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_sd_cmp":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  double af, bf;\n'
            f'  __builtin_memcpy(&af, &a, 8);\n'
            f'  __builtin_memcpy(&bf, &b, 8);\n'
            f'  {typ} r = 0;\n'
            f'  __asm__ volatile ("movsd %3, %%xmm0; {asm_tmpl}"\n'
            f'                   : "+r"(r) : "x"(bf), "r"((int)0), "x"(af) : "cc");\n'
            f'  return r;\n'
            f'}}\n'
        )
    elif category == "sse_xmm_shift":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  long long af = (long long)a;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "+x"(af));\n'
            f'  return (long)af;\n'
            f'}}\n'
        )
    elif category == "sse_xmm_una":
        src = (
            f'{typ} rt_{safe_name}({typ} a) {{\n'
            f'  long long af = (long long)a, out;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=x"(out) : "x"(af));\n'
            f'  return (long)out;\n'
            f'}}\n'
        )
    elif category == "sse_xmm_mov":
        src = (
            f'{typ} rt_{safe_name}({typ} a, {typ} b) {{\n'
            f'  long long bf = (long long)b, out;\n'
            f'  __asm__ volatile ("{asm_tmpl}" : "=x"(out) : "x"(bf));\n'
            f'  return (long)out;\n'
            f'}}\n'
        )
    elif category == "custom":
        return None
    else:
        return None

    return src, args, extra_flags, opt_level


def format_args(args: list[int]) -> str:
    parts = []
    for a in args:
        if a > 0xFFFFFFFF:
            parts.append(f"0x{a:X}ULL")
        else:
            parts.append(str(a))
    return "{" + ", ".join(parts) + "}"


def gen_test_file(arch_name: str, test_class: str, insn_map: dict,
                  all_ids: dict[str, list[str]], output_path: Path):
    """Generate a test .cpp file for all implemented instructions."""
    tests = []
    implemented = set()
    for ids in all_ids.values():
        implemented.update(ids)

    for insn_id in sorted(implemented):
        result = gen_c_wrapper(insn_id, arch_name, insn_map)
        if result is None:
            continue
        src, args, extra_flags, opt_level = result
        safe_name = insn_id.split("_INS_")[1].lower()
        tests.append((safe_name, src, args, extra_flags, opt_level))

    if not tests:
        return

    mapped = len(tests)
    total = len(implemented)
    print(f"  {arch_name}: {mapped}/{total} instructions mapped to roundtrip tests")

    with open(output_path, "w") as f:
        f.write(f'//===- {output_path.name} - Auto-generated roundtrip tests --*- C++ -*-===//\n')
        f.write(f'//\n')
        f.write(f'// AUTO-GENERATED by scripts/gen_roundtrip_tests.py\n')
        f.write(f'// {mapped} instructions mapped out of {total} total {arch_name} capstone IDs.\n')
        f.write(f'//\n')
        f.write(f'//===----------------------------------------------------------------------===//\n\n')
        f.write(f'#include "SemanticRoundTripFixture.h"\n\n')
        f.write(f'TEST_P({test_class}, AutoLiftVerify) {{\n')
        if arch_name == "x86_64":
            f.write(f'  roundTripX64(GetParam());\n')
        elif arch_name == "aarch64":
            f.write(f'  roundTripAArch64(GetParam());\n')
        else:
            f.write(f'  roundTripARM32(GetParam());\n')
        f.write(f'}}\n\n')
        f.write(f'// clang-format off\n\n')
        f.write(f'static const std::vector<RoundTripTC> kAuto{test_class} = {{\n')

        for safe_name, src, args, extra_flags, opt_level in tests:
            c_str = src.replace("\\", "\\\\").replace('"', '\\"').replace("\n", '\\n"\n   "')
            args_str = format_args(args)
            f.write(f'  {{"{safe_name}",\n')
            f.write(f'   "{c_str}",\n')
            f.write(f'   {args_str}, "AutoRT"')
            if opt_level or extra_flags:
                f.write(f', {opt_level}, "{extra_flags}"')
            f.write('},\n\n')

        f.write(f'}};\n\n')
        f.write(f'// clang-format on\n\n')
        f.write(f'INSTANTIATE_TEST_SUITE_P(AutoRT, {test_class},\n')
        f.write(f'                         ::testing::ValuesIn(kAuto{test_class}), rtTCName);\n')


def main():
    print("=== NeverD Roundtrip Test Generator ===\n")

    # Extract instruction IDs
    x86_ids = extract_insn_ids(PROJ_ROOT / "lib/lift/X86", "X86")
    a64_ids = extract_insn_ids(PROJ_ROOT / "lib/lift/AArch64", "AARCH64")
    arm_ids = extract_insn_ids(PROJ_ROOT / "lib/lift/ARM", "ARM")

    x86_total = len(set(i for ids in x86_ids.values() for i in ids))
    a64_total = len(set(i for ids in a64_ids.values() for i in ids))
    arm_total = len(set(i for ids in arm_ids.values() for i in ids))
    print(f"Found: x86={x86_total}, AArch64={a64_total}, ARM32={arm_total}")
    print(f"Total: {x86_total + a64_total + arm_total}\n")

    out_dir = PROJ_ROOT / "unittests/semantic"

    gen_test_file("x86_64", "X64RoundTrip", X86_INSN_MAP, x86_ids,
                  out_dir / "X64_AutoRoundTripTests.cpp")
    gen_test_file("aarch64", "AArch64RoundTrip", A64_INSN_MAP, a64_ids,
                  out_dir / "AArch64_AutoRoundTripTests.cpp")
    gen_test_file("arm32", "ARM32RoundTrip", ARM32_INSN_MAP, arm_ids,
                  out_dir / "ARM32_AutoRoundTripTests.cpp")

    print(f"\nGenerated test files in {out_dir}/")
    print("Add the new .cpp files to CMakeLists.txt and rebuild.")


if __name__ == "__main__":
    main()
