//===- LowScalarWideDivision.h - Exact wide division lanes ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Narrow an unsigned division only when its complete dividend fits the lane.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_LOW_LOWSCALARWIDEDIVISION_H
#define NEVERD_LIB_IR_LOW_LOWSCALARWIDEDIVISION_H

#include "neverd/ir/NdOps.h"
#include "neverd/symbolic/SymExpr.h"

namespace neverd {

inline symbolic::SymRef narrowUnsignedDivision(symbolic::SymContext &Ctx,
                                               NdOp Opcode,
                                               symbolic::SymRef Left,
                                               symbolic::SymRef Right) {
  using symbolic::SymOp;
  if ((Opcode != NdOp::INT_DIV && Opcode != NdOp::INT_REM) || !Left || !Right ||
      Ctx.width(Left) != 128 || Ctx.width(Right) != 128 ||
      Ctx.op(Left) != SymOp::ZExt || Ctx.numOperands(Left) != 1)
    return {};
  const auto Dividend = Ctx.operand(Left, 0);
  const auto Width = Ctx.width(Dividend);
  const auto Divisor = Ctx.asConst(Right);
  if (!Width || Width > 64 || !Divisor || Divisor->isZero() ||
      Divisor->getActiveBits() > Width)
    return {};
  // zext(x) / N and zext(x) % N fit x's lane. A nonzero high
  // dividend, sign extension, or a denominator outside that lane cannot use
  // this identity. Keep the wide result so subsequent slices remain exact.
  const auto NarrowDivisor = Ctx.mkConst(Width, Divisor->getZExtValue());
  return Ctx.mkZExt(Opcode == NdOp::INT_DIV
                        ? Ctx.mkUDiv(Dividend, NarrowDivisor)
                        : Ctx.mkURem(Dividend, NarrowDivisor),
                    128);
}

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_LOWSCALARWIDEDIVISION_H
