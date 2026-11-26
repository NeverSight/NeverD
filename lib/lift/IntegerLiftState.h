//===- IntegerLiftState.h - Width-aware scalar LowIR operations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-instruction temporary values and integer memory operations shared by the
/// RISC-V and PowerPC lifters.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_INTEGERLIFTSTATE_H
#define NEVERD_LIB_LIFT_INTEGERLIFTSTATE_H

#include "neverd/lift/IntegerRegisters.h"
#include "neverd/lift/LiftCommon.h"

#include <vector>

namespace neverd {
struct IntegerLiftState : LiftStateBase {
  uint16_t Width;
  IntegerLiftState(va_t PC, uint16_t Size, std::vector<LowOp> &Ops,
                   uint16_t Width)
      : LiftStateBase(PC, Size, Ops), Width(Width) {}
  NdVar reg(unsigned Index) const {
    return NdVar::reg(integerreg::gpr(Index), Width);
  }
  NdVar imm(uint64_t Value) const {
    return NdVar::scalar(Width == 4 ? uint32_t(Value) : Value, Width);
  }
  NdVar op(NdOp Opcode, NdVar A) {
    NdVar Out = makeTemp(A.Size);
    emit(Opcode, Out, {A});
    return Out;
  }
  NdVar op(NdOp Opcode, NdVar A, NdVar B, uint16_t Bytes = 0) {
    NdVar Out = makeTemp(Bytes ? Bytes : A.Size);
    emit(Opcode, Out, {A, B});
    return Out;
  }
  NdVar resize(NdVar V, uint16_t Size, bool Signed = false) {
    if (V.Size == Size)
      return V;
    NdVar Out = makeTemp(Size);
    if (V.Size < Size)
      emit(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Out, {V});
    else
      emit(NdOp::SUBBYTES, Out, {V, NdVar::scalar(0, 4)});
    return Out;
  }
  NdVar select(NdVar Cond, NdVar Yes, NdVar No) {
    NdVar Out = makeTemp(Yes.Size);
    emit(NdOp::SELECT, Out, {Cond, Yes, No});
    return Out;
  }
  NdVar rotate(NdVar V, NdVar Shift, bool Right = false) {
    NdVar Mask = NdVar::scalar(V.Size * 8 - 1, V.Size);
    NdVar Count = op(NdOp::INT_AND, Shift, Mask);
    // Both shift counts must be in range, including when Count is zero.
    NdVar Inverse =
        op(NdOp::INT_AND, op(NdOp::INT_SUB, NdVar::scalar(0, V.Size), Count),
           Mask);
    return op(NdOp::INT_OR, op(NdOp::INT_LEFT, V, Right ? Inverse : Count),
              op(NdOp::INT_RIGHT, V, Right ? Count : Inverse));
  }
  NdVar byteSwap(NdVar V) {
    if (V.Size == 1)
      return V;
    const uint16_t Half = V.Size / 2;
    NdVar High = makeTemp(Half);
    emit(NdOp::SUBBYTES, High, {V, NdVar::scalar(Half, 4)});
    NdVar NewHigh = byteSwap(resize(V, Half));
    return op(NdOp::CONCAT, NewHigh, byteSwap(High), V.Size);
  }
  NdVar multiplyHigh(NdVar A, NdVar B, bool SignedA, bool SignedB) {
    const uint16_t Wide = 2 * A.Size;
    NdVar Product =
        op(NdOp::INT_MULT, resize(A, Wide, SignedA), resize(B, Wide, SignedB));
    NdVar High = makeTemp(A.Size);
    emit(NdOp::SUBBYTES, High, {Product, NdVar::scalar(A.Size, 4)});
    return High;
  }
  NdVar load(NdVar Address, uint16_t Bytes) {
    NdVar Out = makeTemp(Bytes);
    emit(NdOp::LOAD, Out, {Address});
    return Out;
  }
};
} // namespace neverd
#endif // NEVERD_LIB_LIFT_INTEGERLIFTSTATE_H
