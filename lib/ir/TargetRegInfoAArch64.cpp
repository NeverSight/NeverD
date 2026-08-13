//===- TargetRegInfoAArch64.cpp - AArch64 register description tables ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 register description tables: sub-register relationships,
/// parameter/callee-save/return register sequences and the TargetRegInfo
/// instance built from them.
///
//===----------------------------------------------------------------------===//

#include "TargetRegInfoDetail.h"

#include "neverd/Limits.h"
#include "neverd/lift/AArch64Regs.h"

namespace neverd {

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

TargetRegInfo A64RegInfo = {
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

// Return-register sequences for multi-register struct-by-value returns, in ABI
// order.  x86-64 SysV: INTEGER eightbytes -> RAX,RDX; SSE eightbytes ->
// XMM0,XMM1.  AArch64: non-HFA aggregate -> X0,X1; HFA -> V0..V3.  i386/ARM32
// return such aggregates through the sret pointer (or x87 / soft-float pair),
// so they expose only the integer pair for the wide-int path; no FP sequence.
static const uint64_t A64IntReturnRegs[] = {a64reg::X0, a64reg::X1};
static const uint64_t A64FPReturnRegs[] = {a64reg::V(0), a64reg::V(1),
                                           a64reg::V(2), a64reg::V(3)};

void initAArch64RegInfoTables() {
  A64RegInfo.SubRegs = A64SubRegs;

  A64RegInfo.MinInsnAlign = limits::kMinInsnAlignAArch64;

  // AArch64 returns floating point in V0/D0, and ARM's -O2 internal
  // convention for static functions returns it in D0 (both vector
  // registers), like x86-64's XMM0 — so the FP return is modeled in the FP
  // return register on every target. (ARM's softfp ABI returns it in R0:R1
  // for *external* functions; recovering that is a separate concern, not
  // exercised by intra-module calls.) Auto-declared external callees need
  // variadic prototypes (ARM/AArch64 variadic and non-variadic calling
  // conventions differ).
  A64RegInfo.UnknownExternIsVarArg = true;

  A64RegInfo.IntReturnRegs = A64IntReturnRegs;
  A64RegInfo.FPReturnRegs = A64FPReturnRegs;
}

} // namespace neverd
