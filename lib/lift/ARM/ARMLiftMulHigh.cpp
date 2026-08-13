//===- ARMLiftMulHigh.cpp - ARM32 most-significant-word multiply lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The most-significant-word multiplies SMMUL{R}, SMMLA{R} and
/// SMMLS{R}, which keep the high half of the 64-bit product.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

/// Rounding constant the "R" variants (SMMULR/SMMLAR/SMMLSR) add to the 64-bit
/// intermediate before extracting the most-significant word; it rounds the
/// truncated high half to nearest and its carry must reach bit 32.
static constexpr uint64_t kSMMulRound = 0x80000000ULL;

bool liftMulHigh(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM) {
  switch (Insn->id) {
  // SMMUL{R}: Rd = (SInt(Rn)*SInt(Rm){ + kSMMulRound })[63:32].  Round on the
  // full 64-bit product so the rounding carry propagates into the high word.
  case ARM_INS_SMMUL:
  case ARM_INS_SMMULR: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    if (Insn->id == ARM_INS_SMMULR)
      S.emit(NdOp::INT_ADD, Prod64, {Prod64, NdVar::cst(kSMMulRound, 8)});
    S.emit(NdOp::SUBBYTES, Dst, {Prod64, NdVar::cst(4, 4)});
    break;
  }
  // SMMLA{R}: Rd = ((SInt(Ra)<<32) + SInt(Rn)*SInt(Rm){ + kSMMulRound
  // })[63:32]. Accumulate at full width so the rounding carry out of the low
  // half reaches bit 32.
  case ARM_INS_SMMLA:
  case ARM_INS_SMMLAR: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    NdVar CHi = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, CHi, {C});
    S.emit(NdOp::INT_LEFT, CHi, {CHi, NdVar::cst(32, 8)});
    NdVar Acc = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Acc, {CHi, Prod64});
    if (Insn->id == ARM_INS_SMMLAR)
      S.emit(NdOp::INT_ADD, Acc, {Acc, NdVar::cst(kSMMulRound, 8)});
    S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(4, 4)});
    break;
  }
  // SMMLS{R}: Rd = ((SInt(Ra)<<32) - SInt(Rn)*SInt(Rm){ + kSMMulRound
  // })[63:32]. The subtraction borrows out of the low half into bit 32, so it
  // must run at full 64-bit width (the old `Ra - prodHi` form dropped that
  // borrow).
  case ARM_INS_SMMLS:
  case ARM_INS_SMMLSR: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    NdVar CHi = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, CHi, {C});
    S.emit(NdOp::INT_LEFT, CHi, {CHi, NdVar::cst(32, 8)});
    NdVar Acc = S.makeTemp(8);
    S.emit(NdOp::INT_SUB, Acc, {CHi, Prod64});
    if (Insn->id == ARM_INS_SMMLSR)
      S.emit(NdOp::INT_ADD, Acc, {Acc, NdVar::cst(kSMMulRound, 8)});
    S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(4, 4)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
