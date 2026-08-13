//===- TargetRegInfoX86.cpp - x86 register description tables ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-64 and i386 register description tables: sub-register relationships,
/// parameter/callee-save/return register sequences and the two TargetRegInfo
/// instances built from them.
///
//===----------------------------------------------------------------------===//

#include "TargetRegInfoDetail.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// x86-64 register tables
//===----------------------------------------------------------------------===//

// x86-64 sub-register relationships (RAX→EAX/AX/AL/AH, etc.)
// ByteOffset: 0 = low byte, 1 = second byte (AH/CH/DH/BH)
// WriteZeroExtends: only 32-bit writes zero-extend to 64-bit
#define X64_GPR_SUBREGS(WIDE, NARROW_H)                                        \
  {WIDE, 8, WIDE, 4, 0, true},      /* EAX→RAX (zext) */                       \
      {WIDE, 8, WIDE, 2, 0, false}, /* AX→RAX */                               \
      {WIDE, 8, WIDE, 1, 0, false}, /* AL→RAX */                               \
      {WIDE, 4, WIDE, 2, 0, false}, /* AX→EAX */                               \
      {WIDE, 4, WIDE, 1, 0, false}, /* AL→EAX */                               \
      {WIDE, 2, WIDE, 1, 0, false}, /* AL→AX */

#define X64_GPR_SUBREGS_H(WIDE)                                                \
  X64_GPR_SUBREGS(WIDE, true){WIDE, 8, WIDE + 1, 1, 1, false}, /* AH→RAX */    \
      {WIDE, 4, WIDE + 1, 1, 1, false},                        /* AH→EAX */    \
      {WIDE, 2, WIDE + 1, 1, 1, false},                        /* AH→AX */

#define X64_GPR_SUBREGS_NO_H(WIDE) X64_GPR_SUBREGS(WIDE, false)

#define X64_XMM_SUBREGS(N)                                                     \
  {                                                                            \
      x86reg::XMM##N, 32, x86reg::XMM##N, 16, 0,                               \
      false}, /* XMM (low 128) of YMM */                                       \
      {x86reg::XMM##N, 32, x86reg::XMM##N, 8, 0, false}, /* low 64 of YMM */   \
      {x86reg::XMM##N, 32, x86reg::XMM##N, 4, 0, false}, /* low 32 of YMM */   \
      {x86reg::XMM##N, 16, x86reg::XMM##N, 8, 0, false}, /* low 64 of XMM */   \
      {x86reg::XMM##N, 16, x86reg::XMM##N, 4, 0, false}, /* low 32 of XMM */   \
  {                                                                            \
    x86reg::XMM##N, 8, x86reg::XMM##N, 4, 0, false                             \
  } /* low 32 of low 64 */

static const SubRegEntry X64SubRegs[] = {
    X64_GPR_SUBREGS_H(x86reg::RAX) X64_GPR_SUBREGS_H(x86reg::RCX)
        X64_GPR_SUBREGS_H(x86reg::RDX) X64_GPR_SUBREGS_H(x86reg::RBX)
            X64_GPR_SUBREGS_NO_H(x86reg::RSP) X64_GPR_SUBREGS_NO_H(x86reg::RBP)
                X64_GPR_SUBREGS_NO_H(x86reg::RSI) X64_GPR_SUBREGS_NO_H(
                    x86reg::RDI) X64_GPR_SUBREGS_NO_H(x86reg::R8)
                    X64_GPR_SUBREGS_NO_H(x86reg::R9)
                        X64_GPR_SUBREGS_NO_H(x86reg::R10)
                            X64_GPR_SUBREGS_NO_H(x86reg::R11)
                                X64_GPR_SUBREGS_NO_H(x86reg::R12)
                                    X64_GPR_SUBREGS_NO_H(x86reg::R13)
                                        X64_GPR_SUBREGS_NO_H(x86reg::R14)
                                            X64_GPR_SUBREGS_NO_H(x86reg::R15)
                                                X64_XMM_SUBREGS(0),
    X64_XMM_SUBREGS(1),
    X64_XMM_SUBREGS(2),
    X64_XMM_SUBREGS(3),
    X64_XMM_SUBREGS(4),
    X64_XMM_SUBREGS(5),
    X64_XMM_SUBREGS(6),
    X64_XMM_SUBREGS(7),
    X64_XMM_SUBREGS(8),
    X64_XMM_SUBREGS(9),
    X64_XMM_SUBREGS(10),
    X64_XMM_SUBREGS(11),
    X64_XMM_SUBREGS(12),
    X64_XMM_SUBREGS(13),
    X64_XMM_SUBREGS(14),
    X64_XMM_SUBREGS(15),
};

#undef X64_GPR_SUBREGS
#undef X64_GPR_SUBREGS_H
#undef X64_GPR_SUBREGS_NO_H

static const uint64_t X64SysVParams[] = {
    x86reg::RDI, x86reg::RSI, x86reg::RDX, x86reg::RCX, x86reg::R8, x86reg::R9,
};

static const uint64_t X64Win64Params[] = {
    x86reg::RCX,
    x86reg::RDX,
    x86reg::R8,
    x86reg::R9,
};

static const uint64_t X64FPParams[] = {
    x86reg::XMM0, x86reg::XMM1, x86reg::XMM2, x86reg::XMM3,
    x86reg::XMM4, x86reg::XMM5, x86reg::XMM6, x86reg::XMM7,
};

static const uint64_t X64CalleeSave[] = {
    x86reg::RBX, x86reg::RBP, x86reg::R12,
    x86reg::R13, x86reg::R14, x86reg::R15,
};

TargetRegInfo X64RegInfo = {
    Arch::X64,
    x86reg::RSP,
    x86reg::RBP,
    0,
    8,
    8,
    X64SysVParams,
    X64Win64Params,
    X64FPParams,
    x86reg::RAX,
    x86reg::RDX,
    x86reg::XMM0,
    X64CalleeSave,
    x86reg::CF,
    x86reg::ZF,
    x86reg::SF,
    x86reg::OF,
    x86reg::PF,
    x86reg::DF,
    x86reg::CF,
    x86reg::DF,
    x86reg::XMM0,
    32,
    16,
    CondCode::ULT,
    getX86RegName,
};

//===----------------------------------------------------------------------===//
// x86 (32-bit) register tables
//===----------------------------------------------------------------------===//

// clang/GCC pass the first two integer arguments of an internal (static,
// address-not-taken) i386 function in ECX and EDX, fastcall-style, spilling the
// rest to the stack.  External cdecl functions take every argument on the stack
// and are recovered by detectCdeclStackParams; a live-in ECX/EDX is the signal
// that selects the register convention.
static const uint64_t X86FastcallParams[] = {x86reg::RCX, x86reg::RDX};

static const uint64_t X86CalleeSave[] = {
    x86reg::RBX,
    x86reg::RBP,
    x86reg::RSI,
    x86reg::RDI,
};

TargetRegInfo X86RegInfo = {
    Arch::X86,
    x86reg::RSP,
    x86reg::RBP,
    0,
    4,
    4,
    X86FastcallParams,
    {},
    X64FPParams,
    x86reg::RAX,
    x86reg::RDX,
    x86reg::XMM0,
    X86CalleeSave,
    x86reg::CF,
    x86reg::ZF,
    x86reg::SF,
    x86reg::OF,
    x86reg::PF,
    x86reg::DF,
    x86reg::CF,
    x86reg::DF,
    x86reg::XMM0,
    32,
    8,
    CondCode::ULT,
    getX86RegName,
};

// Return-register sequences for multi-register struct-by-value returns, in ABI
// order.  x86-64 SysV: INTEGER eightbytes -> RAX,RDX; SSE eightbytes ->
// XMM0,XMM1.  AArch64: non-HFA aggregate -> X0,X1; HFA -> V0..V3.  i386/ARM32
// return such aggregates through the sret pointer (or x87 / soft-float pair),
// so they expose only the integer pair for the wide-int path; no FP sequence.
static const uint64_t X64IntReturnRegs[] = {x86reg::RAX, x86reg::RDX};
static const uint64_t X64FPReturnRegs[] = {x86reg::XMM0, x86reg::XMM1};

void initX86RegInfoTables() {
  X64RegInfo.SubRegs = X64SubRegs;
  X86RegInfo.SubRegs = X64SubRegs;

  X64RegInfo.MinInsnAlign = limits::kMinInsnAlignX86;
  X86RegInfo.MinInsnAlign = limits::kMinInsnAlignX86;

  X64RegInfo.IntReturnRegs = X64IntReturnRegs;
  X64RegInfo.FPReturnRegs = X64FPReturnRegs;
  X86RegInfo.IntReturnRegs = X64IntReturnRegs;
}

} // namespace neverd
