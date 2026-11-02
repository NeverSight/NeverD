//===- X86LiftLegacySSEFloat.cpp - x86 baseline SSE compare and float lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Baseline (non-VEX) SSE/SSE2 packed integer compares,
/// ordered/unordered scalar float compares, scalar and
/// packed float arithmetic, scalar float-to-integer conversion,
/// and the MXCSR load/store.  Legacy CVTSI2SS/SD live in liftCoreMove.
///
//===----------------------------------------------------------------------===//

#include "X86FPArithState.h"
#include "X86FPComiState.h"
#include "X86LiftDetail.h"
#include "X86ScalarFPConversion.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftLegacySSEFloat(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // SIMD scalar/packed compares
  // ========================================================================
  case X86_INS_PCMPEQB:
  case X86_INS_PCMPEQW:
  case X86_INS_PCMPEQD:
  case X86_INS_PCMPGTB:
  case X86_INS_PCMPGTW:
  case X86_INS_PCMPGTD: {
    if (X86.op_count < 2)
      break;
    // A register compared with itself reads none of its lanes: each equals
    // itself and exceeds none, so `pcmpeqd x, x` sets every bit (a vector of
    // ones, as compilers make it) and `pcmpgtd x, x` clears them.
    if (isSameRegisterOperand(X86.operands[0], X86.operands[1])) {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      const bool Greater = InsnId == X86_INS_PCMPGTB ||
                           InsnId == X86_INS_PCMPGTW ||
                           InsnId == X86_INS_PCMPGTD;
      S.emit(NdOp::COPY, Dst, {emitByteSplat(S, Dst.Size, Greater ? 0 : 0xFF)});
      break;
    }
    NdVar Lhs = L.operandRead(S, X86.operands[0]);
    NdVar Rhs = L.operandRead(S, X86.operands[1]);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_PCMPEQB:
    case X86_INS_PCMPGTB:
      LaneSz = 1;
      break;
    case X86_INS_PCMPEQW:
    case X86_INS_PCMPGTW:
      LaneSz = 2;
      break;
    default:
      LaneSz = 4;
      break;
    }
    bool IsGT = (InsnId == X86_INS_PCMPGTB || InsnId == X86_INS_PCMPGTW ||
                 InsnId == X86_INS_PCMPGTD);
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildCmpHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {Lhs, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Rhs, NdVar::cst(Off, 4)});
        NdVar Cmp = S.makeTemp(1);
        if (IsGT)
          S.emit(NdOp::INT_SLESS, Cmp, {Lb, La});
        else
          S.emit(NdOp::INT_EQUAL, Cmp, {La, Lb});
        NdVar Mask = S.makeTemp(LaneSz);
        uint64_t AllOnes = (LaneSz == 8) ? 0xFFFFFFFFFFFFFFFFULL
                                         : ((1ULL << (LaneSz * 8)) - 1);
        S.emit(NdOp::SELECT, Mask,
               {Cmp, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Mask;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Mask, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoH = BuildCmpHalf(0);
    NdVar HiH = BuildCmpHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiH, LoH});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_COMISS:
  case X86_INS_COMISD:
  case X86_INS_UCOMISS:
  case X86_INS_UCOMISD:
    return liftFPComiState(
        L, S, Insn, X86, InsnId == X86_INS_COMISD || InsnId == X86_INS_UCOMISD,
        InsnId == X86_INS_UCOMISS || InsnId == X86_INS_UCOMISD, false);

  // ========================================================================
  // SSE scalar float arithmetic
  // ========================================================================
  case X86_INS_ADDSS:
  case X86_INS_ADDSD:
  case X86_INS_ADDPS:
  case X86_INS_ADDPD:
  case X86_INS_SUBSS:
  case X86_INS_SUBSD:
  case X86_INS_SUBPS:
  case X86_INS_SUBPD:
  case X86_INS_MULSS:
  case X86_INS_MULSD:
  case X86_INS_MULPS:
  case X86_INS_MULPD:
  case X86_INS_DIVSS:
  case X86_INS_DIVSD:
  case X86_INS_DIVPS:
  case X86_INS_DIVPD: {
    return liftFPArithState(L, S, Insn, X86);
  }

  // ========================================================================
  // SSE scalar conversions (int->FP CVTSI2SS/SD: liftCoreMove)
  // ========================================================================
  case X86_INS_CVTSS2SI:
  case X86_INS_CVTSD2SI: {
    return liftScalarFPIntegerState(
        L, S, Insn, X86, Insn->id == X86_INS_CVTSS2SI ? 4 : 8, false, false);
  }
  case X86_INS_CVTTSS2SI:
  case X86_INS_CVTTSD2SI: {
    return liftScalarFPIntegerState(
        L, S, Insn, X86, Insn->id == X86_INS_CVTTSS2SI ? 4 : 8, true, false);
  }

  // ========================================================================
  // MXCSR (SSE control/status register)
  // ========================================================================
  case X86_INS_LDMXCSR:
  case X86_INS_STMXCSR: {
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(InsnId == X86_INS_LDMXCSR ? Intrinsic::Ldmxcsr
                                                         : Intrinsic::Stmxcsr,
                               X86.operands[0]))
      return false;
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
