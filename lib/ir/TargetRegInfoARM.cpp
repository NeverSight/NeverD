//===- TargetRegInfoARM.cpp - ARM32 register description tables ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 register description tables: sub-register relationships,
/// parameter/callee-save/return register sequences and the TargetRegInfo
/// instance built from them.
///
//===----------------------------------------------------------------------===//

#include "TargetRegInfoDetail.h"

#include "neverd/Limits.h"
#include "neverd/lift/ARMRegs.h"

namespace neverd {

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

TargetRegInfo ARMRegInfo = {
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
static const uint64_t ARMIntReturnRegs[] = {armreg::R0, armreg::R1};

void initARMRegInfoTables() {
  ARMRegInfo.SubRegs = ARMSubRegs;

  ARMRegInfo.MinInsnAlign = limits::kMinInsnAlignARM;

  // AArch64 returns floating point in V0/D0, and ARM's -O2 internal
  // convention for static functions returns it in D0 (both vector
  // registers), like x86-64's XMM0 — so the FP return is modeled in the FP
  // return register on every target. (ARM's softfp ABI returns it in R0:R1
  // for *external* functions; recovering that is a separate concern, not
  // exercised by intra-module calls.) Auto-declared external callees need
  // variadic prototypes (ARM/AArch64 variadic and non-variadic calling
  // conventions differ).
  ARMRegInfo.UnknownExternIsVarArg = true;

  ARMRegInfo.IntReturnRegs = ARMIntReturnRegs;
}

} // namespace neverd
