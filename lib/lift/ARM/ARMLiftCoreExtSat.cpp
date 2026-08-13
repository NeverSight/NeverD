//===- ARMLiftCoreExtSat.cpp - ARM32 saturating and parallel arithmetic lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Saturating scalar arithmetic (QADD, QDADD, QSUB, QDSUB), the ARMv6
/// GE-setting lane-parallel add/subtract family, and SSAT/USAT.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftCoreExtSat(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                    const cs_arm &ARM) {
  switch (Insn->id) {
  // Saturating arithmetic (QADD/QSUB/QDADD/QDSUB).  Compute in 64 bits and
  // clamp to the signed 32-bit range, then truncate — no library/intrinsic
  // dependency, semantics exact.
  case ARM_INS_QADD:
  case ARM_INS_QDADD:
  case ARM_INS_QSUB:
  case ARM_INS_QDSUB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]); // Rm
    NdVar B = L.operandRead(S, ARM.operands[2]); // Rn
    bool IsSub = (Insn->id == ARM_INS_QSUB || Insn->id == ARM_INS_QDSUB);
    bool IsDouble = (Insn->id == ARM_INS_QDADD || Insn->id == ARM_INS_QDSUB);

    // Clamp a 64-bit signed value to [INT32_MIN, INT32_MAX], yield 4 bytes.
    auto sat32 = [&](NdVar Wide) -> NdVar {
      NdVar MaxC = NdVar::cst(0x7FFFFFFFull, 8);
      NdVar MinC = NdVar::cst(static_cast<uint64_t>(-(1LL << 31)), 8);
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, GtMax, {MaxC, Wide});
      NdVar C1 = S.makeTemp(8);
      S.emit(NdOp::SELECT, C1, {GtMax, MaxC, Wide});
      NdVar LtMin = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, LtMin, {C1, MinC});
      NdVar C2 = S.makeTemp(8);
      S.emit(NdOp::SELECT, C2, {LtMin, MinC, C1});
      NdVar Narrow = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Narrow, {C2, NdVar::cst(0, 4)});
      return Narrow;
    };

    NdVar WA = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, WA, {A});
    NdVar Addend = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, Addend, {B});
    if (IsDouble) {
      // doubled = SignedSat(2*Rn, 32), then sign-extend back to 64 bits.
      NdVar Dbl = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, Dbl, {Addend, Addend});
      NdVar DblSat = sat32(Dbl);
      Addend = S.makeTemp(8);
      S.emit(NdOp::INT_SEXT, Addend, {DblSat});
    }
    NdVar Sum = S.makeTemp(8);
    S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Sum, {WA, Addend});
    S.emit(NdOp::COPY, Dst, {sat32(Sum)});
    break;
  }
  // SIMD lane-parallel add/sub (ARMv6 GE-setting forms): 16-bit (2 lanes) or
  // 8-bit (4 lanes) packed in a 32-bit GPR, with plain / saturating (Q) /
  // halving (H) variants, plus the add-subtract-with-exchange forms.  ASX swaps
  // the second operand's halves with low=sub/high=add; SAX with
  // low=add/high=sub.
  case ARM_INS_QADD16:
  case ARM_INS_UQADD16:
  case ARM_INS_QADD8:
  case ARM_INS_UQADD8:
  case ARM_INS_QASX:
  case ARM_INS_UQASX:
  case ARM_INS_SADD16:
  case ARM_INS_UADD16:
  case ARM_INS_SADD8:
  case ARM_INS_UADD8:
  case ARM_INS_SASX:
  case ARM_INS_UASX:
  case ARM_INS_SHADD16:
  case ARM_INS_UHADD16:
  case ARM_INS_SHADD8:
  case ARM_INS_UHADD8:
  case ARM_INS_SHASX:
  case ARM_INS_UHASX:
  case ARM_INS_QSUB16:
  case ARM_INS_UQSUB16:
  case ARM_INS_QSUB8:
  case ARM_INS_UQSUB8:
  case ARM_INS_QSAX:
  case ARM_INS_UQSAX:
  case ARM_INS_SSUB16:
  case ARM_INS_USUB16:
  case ARM_INS_SSUB8:
  case ARM_INS_USUB8:
  case ARM_INS_SSAX:
  case ARM_INS_USAX:
  case ARM_INS_SHSUB16:
  case ARM_INS_UHSUB16:
  case ARM_INS_SHSUB8:
  case ARM_INS_UHSUB8:
  case ARM_INS_SHSAX:
  case ARM_INS_UHSAX: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto is = [&](std::initializer_list<int> L) {
      for (int V : L)
        if (static_cast<int>(Insn->id) == V)
          return true;
      return false;
    };
    bool Is8 =
        is({ARM_INS_QADD8, ARM_INS_UQADD8, ARM_INS_SADD8, ARM_INS_UADD8,
            ARM_INS_SHADD8, ARM_INS_UHADD8, ARM_INS_QSUB8, ARM_INS_UQSUB8,
            ARM_INS_SSUB8, ARM_INS_USUB8, ARM_INS_SHSUB8, ARM_INS_UHSUB8});
    bool IsUnsigned =
        is({ARM_INS_UQADD16, ARM_INS_UQADD8, ARM_INS_UQASX, ARM_INS_UADD16,
            ARM_INS_UADD8, ARM_INS_UASX, ARM_INS_UHADD16, ARM_INS_UHADD8,
            ARM_INS_UHASX, ARM_INS_UQSUB16, ARM_INS_UQSUB8, ARM_INS_UQSAX,
            ARM_INS_USUB16, ARM_INS_USUB8, ARM_INS_USAX, ARM_INS_UHSUB16,
            ARM_INS_UHSUB8, ARM_INS_UHSAX});
    bool IsSat =
        is({ARM_INS_QADD16, ARM_INS_UQADD16, ARM_INS_QADD8, ARM_INS_UQADD8,
            ARM_INS_QASX, ARM_INS_UQASX, ARM_INS_QSUB16, ARM_INS_UQSUB16,
            ARM_INS_QSUB8, ARM_INS_UQSUB8, ARM_INS_QSAX, ARM_INS_UQSAX});
    bool IsHalving =
        is({ARM_INS_SHADD16, ARM_INS_UHADD16, ARM_INS_SHADD8, ARM_INS_UHADD8,
            ARM_INS_SHASX, ARM_INS_UHASX, ARM_INS_SHSUB16, ARM_INS_UHSUB16,
            ARM_INS_SHSUB8, ARM_INS_UHSUB8, ARM_INS_SHSAX, ARM_INS_UHSAX});
    bool IsAsx = is({ARM_INS_QASX, ARM_INS_UQASX, ARM_INS_SASX, ARM_INS_UASX,
                     ARM_INS_SHASX, ARM_INS_UHASX});
    bool IsSax = is({ARM_INS_QSAX, ARM_INS_UQSAX, ARM_INS_SSAX, ARM_INS_USAX,
                     ARM_INS_SHSAX, ARM_INS_UHSAX});
    bool BaseSub =
        is({ARM_INS_QSUB16, ARM_INS_UQSUB16, ARM_INS_QSUB8, ARM_INS_UQSUB8,
            ARM_INS_SSUB16, ARM_INS_USUB16, ARM_INS_SSUB8, ARM_INS_USUB8,
            ARM_INS_SHSUB16, ARM_INS_UHSUB16, ARM_INS_SHSUB8, ARM_INS_UHSUB8});
    unsigned LaneBytes = Is8 ? 1 : 2;
    unsigned LaneBits = LaneBytes * 8;
    // Plain (non-saturating, non-halving) forms set the APSR.GE lane flags that
    // a following SEL consumes; the Q/H variants leave GE unchanged.
    bool SetsGE = !IsSat && !IsHalving;

    // Compute one lane: extend, add/sub in 32 bits, then
    // halve/saturate/truncate. When GEBase >= 0 also set the lane's GE flag(s)
    // from the ARM ARM rule.
    auto laneCompute = [&](NdVar LA, NdVar LB, bool Sub,
                           int GEBase) -> NdVar {
      NdVar EA = S.makeTemp(4);
      NdVar EB = S.makeTemp(4);
      NdOp Ext = IsUnsigned ? NdOp::INT_ZEXT : NdOp::INT_SEXT;
      S.emit(Ext, EA, {LA});
      S.emit(Ext, EB, {LB});
      NdVar Raw = S.makeTemp(4);
      S.emit(Sub ? NdOp::INT_SUB : NdOp::INT_ADD, Raw, {EA, EB});
      if (GEBase >= 0) {
        // unsigned add -> carry (Raw >= 2^n); unsigned sub -> no borrow
        // (EA >= EB); signed add/sub -> (Raw >= 0).  16-bit lanes duplicate the
        // flag across the two adjacent GE bits.
        NdVar Ge = S.makeTemp(1);
        if (IsUnsigned && Sub) {
          S.emit(NdOp::INT_LESSEQUAL, Ge, {EB, EA});
        } else if (IsUnsigned) {
          S.emit(NdOp::INT_LESSEQUAL, Ge,
                 {NdVar::cst(1ULL << LaneBits, 4), Raw});
        } else {
          NdVar Neg = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, Neg, {Raw, NdVar::cst(0, 4)});
          S.emit(NdOp::BOOL_NOT, Ge, {Neg});
        }
        S.emit(NdOp::COPY, NdVar::reg(armreg::GEFLAG(GEBase), 1), {Ge});
        if (LaneBytes == 2)
          S.emit(NdOp::COPY, NdVar::reg(armreg::GEFLAG(GEBase + 1), 1), {Ge});
      }
      NdVar Out = S.makeTemp(LaneBytes);
      if (IsHalving) {
        // The result is bits[LaneBits:1] of the signed intermediate, i.e. an
        // arithmetic shift right by one (keeps the sign for negative diffs).
        NdVar Half = S.makeTemp(4);
        S.emit(NdOp::INT_ASHR, Half, {Raw, NdVar::cst(1, 4)});
        S.emit(NdOp::SUBBYTES, Out, {Half, NdVar::cst(0, 4)});
      } else if (IsSat) {
        S.emitIntrinsic(IsUnsigned ? Intrinsic::ArmUsat : Intrinsic::ArmSsat,
                        Out, {Raw, NdVar::cst(LaneBits, 4)});
      } else {
        S.emit(NdOp::SUBBYTES, Out, {Raw, NdVar::cst(0, 4)});
      }
      return Out;
    };

    NdVar Acc = S.makeTemp(0);
    if (IsAsx || IsSax) {
      // 16-bit only: second operand halves swapped, per-lane op alternates.
      NdVar A0 = S.makeTemp(2), A1 = S.makeTemp(2);
      NdVar B0 = S.makeTemp(2), B1 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, A0, {A, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, A1, {A, NdVar::cst(2, 4)});
      S.emit(NdOp::SUBBYTES, B0, {B, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, B1, {B, NdVar::cst(2, 4)});
      // Low halfword -> GE[1:0], high halfword -> GE[3:2].
      NdVar Lo = laneCompute(A0, B1, /*Sub=*/IsAsx, SetsGE ? 0 : -1);
      NdVar Hi = laneCompute(A1, B0, /*Sub=*/IsSax, SetsGE ? 2 : -1);
      NdVar Next = S.makeTemp(4);
      S.emit(NdOp::CONCAT, Next, {Hi, Lo});
      Acc = Next;
    } else {
      unsigned NLanes = 4 / LaneBytes;
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LaneBytes), LB = S.makeTemp(LaneBytes);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LaneBytes, 4)});
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LaneBytes, 4)});
        NdVar Out =
            laneCompute(LA, LB, BaseSub, SetsGE ? (int)(I * LaneBytes) : -1);
        if (I == 0) {
          Acc = Out;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneBytes);
          S.emit(NdOp::CONCAT, Next, {Out, Acc});
          Acc = Next;
        }
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // SSAT/USAT — saturate to specified bit width.  Saturation is an exact clamp,
  // so lift it directly to compare+select instead of an intrinsic (the emitter
  // had no handler and fell back to a malformed `ssat` inline-asm string that
  // failed to reassemble).  SSAT clamps to the signed range [-2^(n-1),
  // 2^(n-1)-1]; USAT clamps the signed input to the unsigned range [0, 2^n-1].
  // The "16" variants apply the same clamp to each 16-bit half independently.
  case ARM_INS_SSAT:
  case ARM_INS_USAT:
  case ARM_INS_SSAT16:
  case ARM_INS_USAT16: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    uint32_t Sat = static_cast<uint32_t>(ARM.operands[1].imm);
    NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
    bool IsSigned = (Insn->id == ARM_INS_SSAT || Insn->id == ARM_INS_SSAT16);
    bool IsHalf = (Insn->id == ARM_INS_SSAT16 || Insn->id == ARM_INS_USAT16);

    // Clamp the `Width`-byte signed value in `In` to the saturation range and
    // write the result into `Out` (same width).
    // SSAT/USAT clamp.  Uses a single-SELECT pattern to avoid the fork's
    // InstCombine mis-fold on chained INT_SLESS+SELECT clamp.  Check
    // out-of-range in one go, then pick the saturation value based on sign.
    auto clamp = [&](NdVar In, NdVar Out, unsigned Width) {
      int64_t Max =
          IsSigned ? ((Sat >= 32) ? 0x7FFFFFFFLL : ((1LL << (Sat - 1)) - 1))
                   : ((Sat >= 32) ? 0xFFFFFFFFLL : ((1LL << Sat) - 1));
      int64_t Min =
          IsSigned ? ((Sat >= 32) ? -(1LL << 31) : -(1LL << (Sat - 1))) : 0;
      NdVar MaxC = NdVar::cst(static_cast<uint64_t>(Max), Width);
      NdVar MinC = NdVar::cst(static_cast<uint64_t>(Min), Width);
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, GtMax, {MaxC, In});
      NdVar LtMin = S.makeTemp(1);
      if (IsSigned)
        S.emit(NdOp::INT_SLESS, LtMin, {In, MinC});
      else
        S.emit(NdOp::INT_SLESS, LtMin, {In, NdVar::cst(0, Width)});
      NdVar OutOfRange = S.makeTemp(1);
      S.emit(NdOp::BOOL_OR, OutOfRange, {GtMax, LtMin});
      NdVar IsPos = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, Width), In});
      NdVar SatVal = S.makeTemp(Width);
      S.emit(NdOp::SELECT, SatVal, {IsPos, MaxC, MinC});
      S.emit(NdOp::SELECT, Out, {OutOfRange, SatVal, In});
    };

    if (IsHalf) {
      // Two independent 16-bit halves; sign-extend each to 32 bits, clamp, then
      // truncate back to 16 bits and reassemble.
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Half = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(H * 2, 4)});
        NdVar Wide = S.makeTemp(4);
        S.emit(NdOp::INT_SEXT, Wide, {Half});
        NdVar Clamped = S.makeTemp(4);
        clamp(Wide, Clamped, 4);
        NdVar Narrow = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Narrow, {Clamped, NdVar::cst(0, 4)});
        if (H == 0)
          Acc = Narrow;
        else {
          NdVar Next = S.makeTemp(4);
          S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      clamp(Src, Dst, Dst.Size > 0 ? Dst.Size : 4);
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
