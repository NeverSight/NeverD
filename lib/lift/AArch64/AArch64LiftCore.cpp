//===- AArch64LiftCore.cpp - AArch64 core integer lifter dispatch ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the core integer instructions to the per-family handlers,
/// and defines the SIMD logical-immediate helper shared by the ORR and
/// BIC handlers.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

// SIMD logical-immediate (`orr vD.<T>, #imm{,lsl #s}` / `bic vD.<T>, #imm`):
// broadcast the modified immediate across every lane and OR / AND-NOT it into
// the (read-modified) destination register.  Capstone surfaces these as a
// 2-operand non-alias ORR/BIC, which previously fell through the register-form
// `op_count < 3` guard and silently became a no-op.
void emitSimdImmLogic(AArch64Lifter::LiftState &S, const cs_aarch64 &ARM64,
                      bool IsBic) {
  NdVar Dst = AArch64Lifter::operandWrite(ARM64.operands[0]);
  unsigned ElemSz = 0;
  switch (ARM64.operands[0].vas) {
  case AARCH64LAYOUT_VL_16B:
  case AARCH64LAYOUT_VL_8B:
    ElemSz = 1;
    break;
  case AARCH64LAYOUT_VL_8H:
  case AARCH64LAYOUT_VL_4H:
    ElemSz = 2;
    break;
  case AARCH64LAYOUT_VL_4S:
  case AARCH64LAYOUT_VL_2S:
    ElemSz = 4;
    break;
  case AARCH64LAYOUT_VL_2D:
    ElemSz = 8;
    break;
  default:
    break;
  }
  if (ElemSz == 0 || Dst.Size == 0 || Dst.Size < ElemSz) {
    // Unknown layout: keep the value rather than corrupt it.
    S.emit(NdOp::COPY, Dst, {NdVar::reg(Dst.Offset, Dst.Size)});
    return;
  }
  uint64_t Imm = static_cast<uint64_t>(ARM64.operands[1].imm);
  if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
    Imm <<= ARM64.operands[1].shift.value;
  unsigned NLanes = Dst.Size / ElemSz;
  NdVar Elem = NdVar::cst(Imm, ElemSz);
  NdVar Acc = Elem;
  for (unsigned I = 1; I < NLanes; ++I) {
    NdVar Next = S.makeTemp(Acc.Size + ElemSz);
    S.emit(NdOp::CONCAT, Next, {Elem, Acc});
    Acc = Next;
  }
  NdVar Bcast = S.makeTemp(Dst.Size);
  S.emit(NdOp::COPY, Bcast, {Acc});
  NdVar Cur = NdVar::reg(Dst.Offset, Dst.Size);
  if (IsBic) {
    NdVar NotB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotB, {Bcast});
    S.emit(NdOp::INT_AND, Dst, {Cur, NotB});
  } else {
    S.emit(NdOp::INT_OR, Dst, {Cur, Bcast});
  }
}

bool AArch64Lifter::liftCore(LiftState &S, const cs_insn *Insn,
                             const cs_aarch64 &ARM64) {
  return liftMove(*this, S, Insn, ARM64) || liftArith(*this, S, Insn, ARM64) ||
         liftShiftMulDiv(*this, S, Insn, ARM64) ||
         liftCondSel(*this, S, Insn, ARM64) ||
         liftBitfield(*this, S, Insn, ARM64) ||
         liftBitManip(*this, S, Insn, ARM64) ||
         liftRotate(*this, S, Insn, ARM64) ||
         liftMulLong(*this, S, Insn, ARM64) ||
         liftCondCompare(*this, S, Insn, ARM64);
}

} // namespace neverd
