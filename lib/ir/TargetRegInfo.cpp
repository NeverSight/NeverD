//===- TargetRegInfo.cpp - Architecture register information ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"

#include "neverd/Limits.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/ARMRegs.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// CondCode helpers
//===----------------------------------------------------------------------===//

CondCode invertCond(CondCode CC) {
  switch (CC) {
  case CondCode::EQ:
    return CondCode::NE;
  case CondCode::NE:
    return CondCode::EQ;
  case CondCode::SLT:
    return CondCode::SGE;
  case CondCode::SLE:
    return CondCode::SGT;
  case CondCode::SGT:
    return CondCode::SLE;
  case CondCode::SGE:
    return CondCode::SLT;
  case CondCode::ULT:
    return CondCode::UGE;
  case CondCode::ULE:
    return CondCode::UGT;
  case CondCode::UGT:
    return CondCode::ULE;
  case CondCode::UGE:
    return CondCode::ULT;
  default:
    return CondCode::Invalid;
  }
}

NdOp condToOpcode(CondCode CC) {
  switch (CC) {
  case CondCode::EQ:
    return NdOp::INT_EQUAL;
  case CondCode::NE:
    return NdOp::INT_NOTEQUAL;
  case CondCode::SLT:
  case CondCode::SGT:
    return NdOp::INT_SLESS;
  case CondCode::SLE:
  case CondCode::SGE:
    return NdOp::INT_SLESSEQUAL;
  case CondCode::ULT:
  case CondCode::UGT:
    return NdOp::INT_LESS;
  case CondCode::ULE:
  case CondCode::UGE:
    return NdOp::INT_LESSEQUAL;
  case CondCode::VS:
    return NdOp::INT_SOVF;
  default:
    return NdOp::NOP;
  }
}

bool condSwapsOperands(CondCode CC) {
  return CC == CondCode::SGT || CC == CondCode::SGE || CC == CondCode::UGT ||
         CC == CondCode::UGE;
}

CondCode TargetRegInfo::singleFlagCond(uint64_t FlagOff, bool Inverted) const {
  CondCode Base = CondCode::Invalid;
  if (FlagOff == FlagCF)
    Base = CfCondCode;
  else if (FlagOff == FlagZF)
    Base = CondCode::EQ;
  else if (FlagOff == FlagNF)
    Base = CondCode::SLT;
  else if (FlagOff == FlagVF)
    Base = CondCode::VS;
  else
    return CondCode::Invalid;
  return Inverted ? invertCond(Base) : Base;
}

//===----------------------------------------------------------------------===//
// Sub-register query implementations
//===----------------------------------------------------------------------===//

bool TargetRegInfo::isSubRegOf(uint64_t NarrowOff, uint16_t NarrowSz,
                               uint64_t WideOff, uint16_t WideSz) const {
  if (NarrowOff == WideOff && NarrowSz < WideSz)
    return true;
  for (const auto &E : SubRegs) {
    if (E.WideRegOff == WideOff && E.WideSize == WideSz &&
        E.NarrowRegOff == NarrowOff && E.NarrowSize == NarrowSz)
      return true;
  }
  return false;
}

bool TargetRegInfo::writeZeroExtends(uint64_t RegOff, uint16_t Size) const {
  for (const auto &E : SubRegs) {
    if (E.NarrowRegOff == RegOff && E.NarrowSize == Size && E.WriteZeroExtends)
      return true;
  }
  return false;
}

int TargetRegInfo::subRegByteOffset(uint64_t NarrowOff, uint16_t NarrowSz,
                                    uint64_t WideOff, uint16_t WideSz) const {
  if (NarrowOff == WideOff && NarrowSz < WideSz)
    return 0;
  for (const auto &E : SubRegs) {
    if (E.WideRegOff == WideOff && E.WideSize == WideSz &&
        E.NarrowRegOff == NarrowOff && E.NarrowSize == NarrowSz)
      return E.ByteOffset;
  }
  return -1;
}

std::pair<uint64_t, uint16_t> TargetRegInfo::findWideReg(uint64_t RegOff,
                                                         uint16_t Size) const {
  uint64_t BestOff = RegOff;
  uint16_t BestSz = Size;
  for (const auto &E : SubRegs) {
    if (E.NarrowRegOff == RegOff && E.NarrowSize == Size &&
        E.WideSize > BestSz) {
      BestOff = E.WideRegOff;
      BestSz = E.WideSize;
    }
  }
  if (BestSz == Size && Size < FullRegWidth && RegOff % FullRegWidth == 0) {
    BestOff = RegOff;
    BestSz = FullRegWidth;
  }
  return {BestOff, BestSz};
}

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

// AArch64: W registers are the lower 32 bits of X registers (zext on write),
// and Sn/Dn are the lower 32/64 bits of Qn/Vn (zext on write).
#define A64_GPR_SUBREG(N) {a64reg::X##N, 8, a64reg::X##N, 4, 0, true}
#define A64_VFP_SUBREGS(N)                                                     \
  {a64reg::V(N), 16, a64reg::V(N), 8, 0, true},     /* Dn→Qn */                \
      {a64reg::V(N), 16, a64reg::V(N), 4, 0, true}, /* Sn→Qn */                \
      {a64reg::V(N), 16, a64reg::V(N), 2, 0, true}, /* Hn→Qn */                \
      {a64reg::V(N), 16, a64reg::V(N), 1, 0, true}, /* Bn→Qn */                \
      {a64reg::V(N), 8, a64reg::V(N), 4, 0, true},  /* Sn→Dn */                \
      {a64reg::V(N), 8, a64reg::V(N), 2, 0, true},  /* Hn→Dn */                \
      {a64reg::V(N), 8, a64reg::V(N), 1, 0, true},  /* Bn→Dn */                \
      {a64reg::V(N), 4, a64reg::V(N), 2, 0, true},  /* Hn→Sn */                \
      {a64reg::V(N), 4, a64reg::V(N), 1, 0, true},  /* Bn→Sn */                \
  {                                                                            \
    a64reg::V(N), 2, a64reg::V(N), 1, 0, true                                  \
  } /* Bn→Hn */

static const SubRegEntry A64SubRegs[] = {
    A64_GPR_SUBREG(0),   A64_GPR_SUBREG(1),   A64_GPR_SUBREG(2),
    A64_GPR_SUBREG(3),   A64_GPR_SUBREG(4),   A64_GPR_SUBREG(5),
    A64_GPR_SUBREG(6),   A64_GPR_SUBREG(7),   A64_GPR_SUBREG(8),
    A64_GPR_SUBREG(9),   A64_GPR_SUBREG(10),  A64_GPR_SUBREG(11),
    A64_GPR_SUBREG(12),  A64_GPR_SUBREG(13),  A64_GPR_SUBREG(14),
    A64_GPR_SUBREG(15),  A64_GPR_SUBREG(16),  A64_GPR_SUBREG(17),
    A64_GPR_SUBREG(18),  A64_GPR_SUBREG(19),  A64_GPR_SUBREG(20),
    A64_GPR_SUBREG(21),  A64_GPR_SUBREG(22),  A64_GPR_SUBREG(23),
    A64_GPR_SUBREG(24),  A64_GPR_SUBREG(25),  A64_GPR_SUBREG(26),
    A64_GPR_SUBREG(27),  A64_GPR_SUBREG(28),  A64_GPR_SUBREG(29),
    A64_GPR_SUBREG(30),  A64_VFP_SUBREGS(0),  A64_VFP_SUBREGS(1),
    A64_VFP_SUBREGS(2),  A64_VFP_SUBREGS(3),  A64_VFP_SUBREGS(4),
    A64_VFP_SUBREGS(5),  A64_VFP_SUBREGS(6),  A64_VFP_SUBREGS(7),
    A64_VFP_SUBREGS(8),  A64_VFP_SUBREGS(9),  A64_VFP_SUBREGS(10),
    A64_VFP_SUBREGS(11), A64_VFP_SUBREGS(12), A64_VFP_SUBREGS(13),
    A64_VFP_SUBREGS(14), A64_VFP_SUBREGS(15), A64_VFP_SUBREGS(16),
    A64_VFP_SUBREGS(17), A64_VFP_SUBREGS(18), A64_VFP_SUBREGS(19),
    A64_VFP_SUBREGS(20), A64_VFP_SUBREGS(21), A64_VFP_SUBREGS(22),
    A64_VFP_SUBREGS(23), A64_VFP_SUBREGS(24), A64_VFP_SUBREGS(25),
    A64_VFP_SUBREGS(26), A64_VFP_SUBREGS(27), A64_VFP_SUBREGS(28),
    A64_VFP_SUBREGS(29), A64_VFP_SUBREGS(30), A64_VFP_SUBREGS(31),
};
#undef A64_GPR_SUBREG
#undef A64_VFP_SUBREGS

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

static TargetRegInfo X64RegInfo = {
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
    x86reg::RBX, x86reg::RBP, x86reg::RSI, x86reg::RDI,
};

static TargetRegInfo X86RegInfo = {
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

//===----------------------------------------------------------------------===//
// AArch64 register tables
//===----------------------------------------------------------------------===//

static const uint64_t A64Params[] = {
    a64reg::X0, a64reg::X1, a64reg::X2, a64reg::X3,
    a64reg::X4, a64reg::X5, a64reg::X6, a64reg::X7,
};

static const uint64_t A64CalleeSave[] = {
    a64reg::X19, a64reg::X20, a64reg::X21, a64reg::X22,
    a64reg::X23, a64reg::X24, a64reg::X25, a64reg::X26,
    a64reg::X27, a64reg::X28, a64reg::X29,
};

static const uint64_t A64FPParams[] = {
    a64reg::V(0), a64reg::V(1), a64reg::V(2), a64reg::V(3),
    a64reg::V(4), a64reg::V(5), a64reg::V(6), a64reg::V(7),
};

static TargetRegInfo A64RegInfo = {
    Arch::AArch64,
    a64reg::SP,
    a64reg::X29,
    a64reg::X30,
    8,
    8,
    A64Params,
    {},
    A64FPParams,
    a64reg::X0,
    0,
    a64reg::V(0),
    A64CalleeSave,
    a64reg::CFLAG,
    a64reg::ZFLAG,
    a64reg::NFLAG,
    a64reg::VFLAG,
    0,
    0,
    a64reg::NFLAG,
    a64reg::VFLAG,
    a64reg::V(0),
    16,
    32,
    CondCode::UGE,
    getAArch64RegName,
};

//===----------------------------------------------------------------------===//
// ARM (32-bit) register tables
//===----------------------------------------------------------------------===//

static const uint64_t ARMParams[] = {
    armreg::R0,
    armreg::R1,
    armreg::R2,
    armreg::R3,
};

static const uint64_t ARMCalleeSave[] = {
    armreg::R4, armreg::R5, armreg::R6,  armreg::R7,
    armreg::R8, armreg::R9, armreg::R10, armreg::R11,
};

// clang's -O2 internal convention for ARM static functions passes
// floating-point arguments in the VFP D registers (D0-D7), like AAPCS-VFP, even
// under the soft float ABI.  (The standard softfp ABI passes them in the
// integer registers; a live-in D-register self-copy is the signal that selects
// the VFP convention.)
static const uint64_t ARMFPParams[] = {
    armreg::D(0), armreg::D(1), armreg::D(2), armreg::D(3),
    armreg::D(4), armreg::D(5), armreg::D(6), armreg::D(7),
};

static TargetRegInfo ARMRegInfo = {
    Arch::ARM,
    armreg::SP,
    armreg::R11,
    armreg::LR,
    4,
    4,
    ARMParams,
    {},
    ARMFPParams,
    armreg::R0,
    armreg::R1,
    armreg::D(0),
    ARMCalleeSave,
    armreg::CFLAG,
    armreg::ZFLAG,
    armreg::NFLAG,
    armreg::VFLAG,
    0,
    0,
    armreg::NFLAG,
    armreg::VFLAG,
    armreg::D(0),
    8,
    16,
    CondCode::UGE,
    getARMRegName,
};

//===----------------------------------------------------------------------===//
// Fallback
//===----------------------------------------------------------------------===//

static const TargetRegInfo UnknownRegInfo = {};

// ARM32 sub-register relationships:
// D(2N) and D(2N+1) are the low/high 64-bit halves of Q(N).
// S(2N) is the low 32 bits of D(N); S(2N+1) is the high 32 bits of D(N).
// Q(N) is stored at the same offset as D(2N) with size 16.
// Use integer constants to avoid constexpr function issues in array
// initializers.
#define ARM_D_(N) (0x100 + (N) * 8)

// Q -> {two D halves} + {four S lanes}.  The S lanes are essential because
// clang lowers a scalar `(int)w[i]` reduction to per-lane `vcvt.s32.f32 sX, sY`
// where sY addresses lane 1/2/3 of a Q register written by a NEON vcvt. Without
// these entries the high-lane S reads cannot resolve to the producing Q and the
// optimizer folds them to undef/0 (VectorAlgo8 arm32 fmla/fdiv).
#define ARM_DQ_SUBREGS(N)                                                      \
  {ARM_D_(2 * (N)), 16, ARM_D_(2 * (N)), 8, 0, false},                         \
      {ARM_D_(2 * (N)), 16, ARM_D_(2 * (N) + 1), 8, 8, false},                 \
      {ARM_D_(2 * (N)), 16, ARM_D_(2 * (N)) + 0, 4, 0, false},                 \
      {ARM_D_(2 * (N)), 16, ARM_D_(2 * (N)) + 4, 4, 4, false},                 \
      {ARM_D_(2 * (N)), 16, ARM_D_(2 * (N)) + 8, 4, 8, false},                 \
      {ARM_D_(2 * (N)), 16, ARM_D_(2 * (N)) + 12, 4, 12, false},

// D -> {two S halves}: S(2N) is the low word, S(2N+1) the high word of D(N).
#define ARM_SD_SUBREGS(N)                                                      \
  {ARM_D_(N), 8, ARM_D_(N) + 0, 4, 0, false},                                  \
      {ARM_D_(N), 8, ARM_D_(N) + 4, 4, 4, false},

static const SubRegEntry ARMSubRegs[] = {
    ARM_DQ_SUBREGS(0) ARM_DQ_SUBREGS(1) ARM_DQ_SUBREGS(2) ARM_DQ_SUBREGS(
        3) ARM_DQ_SUBREGS(4) ARM_DQ_SUBREGS(5) ARM_DQ_SUBREGS(6)
        ARM_DQ_SUBREGS(7) ARM_DQ_SUBREGS(8) ARM_DQ_SUBREGS(9) ARM_DQ_SUBREGS(
            10) ARM_DQ_SUBREGS(11) ARM_DQ_SUBREGS(12) ARM_DQ_SUBREGS(13)
            ARM_DQ_SUBREGS(14) ARM_DQ_SUBREGS(15) ARM_SD_SUBREGS(
                0) ARM_SD_SUBREGS(1) ARM_SD_SUBREGS(2) ARM_SD_SUBREGS(3)
                ARM_SD_SUBREGS(4) ARM_SD_SUBREGS(5) ARM_SD_SUBREGS(
                    6) ARM_SD_SUBREGS(7) ARM_SD_SUBREGS(8) ARM_SD_SUBREGS(9)
                    ARM_SD_SUBREGS(10) ARM_SD_SUBREGS(11) ARM_SD_SUBREGS(
                        12) ARM_SD_SUBREGS(13) ARM_SD_SUBREGS(14)
                        ARM_SD_SUBREGS(15) ARM_SD_SUBREGS(16) ARM_SD_SUBREGS(
                            17) ARM_SD_SUBREGS(18) ARM_SD_SUBREGS(19)
                            ARM_SD_SUBREGS(20) ARM_SD_SUBREGS(21)
                                ARM_SD_SUBREGS(22) ARM_SD_SUBREGS(23)
                                    ARM_SD_SUBREGS(24) ARM_SD_SUBREGS(25)
                                        ARM_SD_SUBREGS(26) ARM_SD_SUBREGS(27)
                                            ARM_SD_SUBREGS(28)
                                                ARM_SD_SUBREGS(29)
                                                    ARM_SD_SUBREGS(30)
                                                        ARM_SD_SUBREGS(31)};
#undef ARM_D_
#undef ARM_DQ_SUBREGS
#undef ARM_SD_SUBREGS

// Return-register sequences for multi-register struct-by-value returns, in ABI
// order.  x86-64 SysV: INTEGER eightbytes -> RAX,RDX; SSE eightbytes ->
// XMM0,XMM1.  AArch64: non-HFA aggregate -> X0,X1; HFA -> V0..V3.  i386/ARM32
// return such aggregates through the sret pointer (or x87 / soft-float pair),
// so they expose only the integer pair for the wide-int path; no FP sequence.
static const uint64_t X64IntReturnRegs[] = {x86reg::RAX, x86reg::RDX};
static const uint64_t X64FPReturnRegs[] = {x86reg::XMM0, x86reg::XMM1};
static const uint64_t A64IntReturnRegs[] = {a64reg::X0, a64reg::X1};
static const uint64_t A64FPReturnRegs[] = {a64reg::V(0), a64reg::V(1),
                                           a64reg::V(2), a64reg::V(3)};
static const uint64_t ARMIntReturnRegs[] = {armreg::R0, armreg::R1};

static void initSubRegs() {
  // The pipeline's first getTargetRegInfo() call comes from CFGBuilder inside
  // the parallel buildLowIR phase (nothing on the path before it — decoder init
  // and function detection — touches this), so several worker threads race to
  // initialize.  A plain `bool` flag let a losing thread see the flag already
  // set while the stores below were still in flight and read a zero
  // MinInsnAlign or an empty SubRegs table; function-local static
  // initialization blocks the other threads until the lambda completes.
  [[maybe_unused]] static const bool Initialized = [] {
    // NOTE: these RegInfo objects must be non-const.  Assigning SubRegs through
    // a const_cast on a genuinely `const` object is UB and the store may be
    // dropped under optimization, leaving SubRegs empty (which silently broke
    // high-byte registers AH/BH/CH/DH and other table-driven sub-reg lookups).
    X64RegInfo.SubRegs = X64SubRegs;
    X86RegInfo.SubRegs = X64SubRegs;
    A64RegInfo.SubRegs = A64SubRegs;
    ARMRegInfo.SubRegs = ARMSubRegs;

    X64RegInfo.MinInsnAlign = limits::kMinInsnAlignX86;
    X86RegInfo.MinInsnAlign = limits::kMinInsnAlignX86;
    A64RegInfo.MinInsnAlign = limits::kMinInsnAlignAArch64;
    ARMRegInfo.MinInsnAlign = limits::kMinInsnAlignARM;

    // AArch64 returns floating point in V0/D0, and ARM's -O2 internal
    // convention for static functions returns it in D0 (both vector
    // registers), like x86-64's XMM0 — so the FP return is modeled in the FP
    // return register on every target. (ARM's softfp ABI returns it in R0:R1
    // for *external* functions; recovering that is a separate concern, not
    // exercised by intra-module calls.) Auto-declared external callees need
    // variadic prototypes (ARM/AArch64 variadic and non-variadic calling
    // conventions differ).
    A64RegInfo.UnknownExternIsVarArg = true;
    ARMRegInfo.UnknownExternIsVarArg = true;

    X64RegInfo.IntReturnRegs = X64IntReturnRegs;
    X64RegInfo.FPReturnRegs = X64FPReturnRegs;
    X86RegInfo.IntReturnRegs = X64IntReturnRegs;
    A64RegInfo.IntReturnRegs = A64IntReturnRegs;
    A64RegInfo.FPReturnRegs = A64FPReturnRegs;
    ARMRegInfo.IntReturnRegs = ARMIntReturnRegs;
    return true;
  }();
}

const TargetRegInfo &getTargetRegInfo(Arch TheArch) {
  initSubRegs();
  switch (TheArch) {
  case Arch::X64:
    return X64RegInfo;
  case Arch::X86:
    return X86RegInfo;
  case Arch::AArch64:
    return A64RegInfo;
  case Arch::ARM:
    return ARMRegInfo;
  default:
    return UnknownRegInfo;
  }
}

//===----------------------------------------------------------------------===//
// regToArgIdx
//===----------------------------------------------------------------------===//

bool TargetRegInfo::isX87StackReg(uint64_t RegOff) const {
  if (TheArch != Arch::X86 && TheArch != Arch::X64)
    return false;
  return RegOff >= x86reg::ST0 && RegOff <= x86reg::ST7 &&
         (RegOff - x86reg::ST0) % x86reg::FPURegStride == 0;
}

bool TargetRegInfo::isCallPreserved(uint64_t RegOff, uint16_t Size) const {
  if (isCalleeSaveReg(RegOff))
    return true;

  // AAPCS-VFP preserves d8-d15.  S-register views and aligned Q4-Q7 views are
  // preserved exactly when their complete byte range is inside that bank.
  if (TheArch == Arch::ARM) {
    constexpr uint64_t PreservedBegin = armreg::D(8);
    constexpr uint64_t PreservedEnd = armreg::D(16);
    return RegOff >= PreservedBegin && RegOff < PreservedEnd &&
           Size <= PreservedEnd - RegOff;
  }

  // AAPCS64 requires callees to preserve only the low 64 bits of v8-v15.
  // Treating the whole Q register as callee-saved would retain an upper half
  // the callee may overwrite; treating it as volatile loses valid D-register
  // values, which clang commonly uses to save HFA results across calls.
  if (TheArch != Arch::AArch64 || Size > 8 || !isVectorReg(RegOff))
    return false;
  unsigned VecIdx = static_cast<unsigned>((RegOff - VecRegBase) / VecRegStride);
  return VecIdx >= 8 && VecIdx <= 15;
}

uint16_t TargetRegInfo::callPreservedPrefixSize(uint64_t RegOff,
                                                uint16_t Size) const {
  if (isCallPreserved(RegOff, Size))
    return Size;
  if (TheArch != Arch::AArch64 || Size <= 8 || !isVectorReg(RegOff))
    return 0;
  unsigned VecIdx = static_cast<unsigned>((RegOff - VecRegBase) / VecRegStride);
  return VecIdx >= 8 && VecIdx <= 15 ? 8 : 0;
}

uint64_t TargetRegInfo::indirectResultReg() const {
  // AArch64 AAPCS returns a >16-byte aggregate through the buffer pointed to by
  // the indirect-result register x8.  x86-64 (RDI) and ARM (r0) use an ordinary
  // argument register, recovered through the normal argument path.
  return TheArch == Arch::AArch64 ? a64reg::X8 : 0;
}

int TargetRegInfo::regToArgIdx(uint64_t RegOff) const {
  for (size_t I = 0; I < IntParamRegs.size(); ++I)
    if (IntParamRegs[I] == RegOff)
      return static_cast<int>(I);
  if (TheArch == Arch::AArch64 || TheArch == Arch::ARM) {
    // Stride-based: param regs are contiguous at FullRegWidth intervals
    if (RegOff <= IntParamRegs.back() && RegOff % FullRegWidth == 0)
      return static_cast<int>(RegOff / FullRegWidth);
  }
  return -1;
}

int TargetRegInfo::regToArgIdx(uint64_t RegOff, bool IsWin64) const {
  if (IsWin64 && !Win64ParamRegs.empty()) {
    for (size_t I = 0; I < Win64ParamRegs.size(); ++I)
      if (Win64ParamRegs[I] == RegOff)
        return static_cast<int>(I);
    for (size_t I = 0; I < FPParamRegs.size() && I < Win64ParamRegs.size(); ++I)
      if (FPParamRegs[I] == RegOff)
        return static_cast<int>(I);
    return -1;
  }
  int Idx = regToArgIdx(RegOff);
  if (Idx >= 0)
    return Idx;
  for (size_t I = 0; I < FPParamRegs.size(); ++I)
    if (FPParamRegs[I] == RegOff)
      return static_cast<int>(IntParamRegs.size() + I);
  return -1;
}

} // namespace neverd
