//===- SymFunnel.h - Bit windows in joined shifts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SYMBOLIC_SYMFUNNEL_H
#define NEVERD_LIB_SYMBOLIC_SYMFUNNEL_H

#include "neverd/symbolic/SymExpr.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::symbolic::detail {

struct WordWindow {
  SymRef Word;
  uint64_t Offset;
};

// Check the operator shape before budgeted callers inspect the literal limbs.
struct FunnelShifts {
  SymRef High, Right;
  SymOp Join;

  static std::optional<FunnelShifts> match(const SymContext &Ctx, SymRef V) {
    const auto Op = Ctx.op(V);
    if ((Op != SymOp::Or && Op != SymOp::Xor && Op != SymOp::Add) ||
        Ctx.numOperands(V) != 2)
      return std::nullopt;
    auto H = Ctx.operand(V, 0), R = Ctx.operand(V, 1);
    if (Ctx.op(H) == SymOp::LShr || Ctx.op(H) == SymOp::AShr)
      std::swap(H, R);
    if ((Ctx.op(R) != SymOp::LShr && Ctx.op(R) != SymOp::AShr) ||
        (Ctx.op(H) != SymOp::Mul && Ctx.op(H) != SymOp::And) ||
        !Ctx.isConst(Ctx.operand(H, 0)) || !Ctx.isConst(Ctx.operand(R, 1)))
      return std::nullopt;
    return FunnelShifts{H, R, Op};
  }

  bool sharesSource(const SymContext &Ctx) const {
    return Ctx.op(High) == SymOp::Mul && Ctx.op(Right) == SymOp::LShr &&
           Ctx.numOperands(High) == 2 &&
           Ctx.operand(High, 1) == Ctx.operand(Right, 0);
  }

  // Read a window owned by one shift. A complete rotation of one word
  // can also supply windows that cross the seam.
  std::optional<WordWindow> project(const SymContext &Ctx, uint64_t At,
                                    unsigned Size) const {
    const unsigned Width = Ctx.width(High);
    const auto Digits = Ctx.constWords(Ctx.operand(High, 0));
    const unsigned Cut = llvm::APInt::tcLSB(Digits.data(), Digits.size());
    // Right-shift builders normalize a constant count to [1, Width).
    const unsigned Shift = Ctx.constWords(Ctx.operand(Right, 1)).front();
    const unsigned LowEnd = Width - Shift;
    if (At + Size <= Cut) {
      if (At + Size <= LowEnd)
        return WordWindow{Ctx.operand(Right, 0), At + Shift};
      return WordWindow{Right, At};
    }
    if (Ctx.op(Right) == SymOp::AShr)
      return std::nullopt;
    const bool ShiftedWord =
        Ctx.op(High) == SymOp::Mul && Ctx.numOperands(High) == 2 &&
        llvm::APInt::tcMSB(Digits.data(), Digits.size()) == Cut;
    if (LowEnd == Cut && ShiftedWord && sharesSource(Ctx))
      return WordWindow{Ctx.operand(Right, 0), (At + Shift) % Width};
    // A sum can carry out of the overlap into an otherwise isolated high
    // window. OR and XOR do not move information across bit positions.
    if (At < std::max(Cut, LowEnd) || At + Size > Width ||
        (Join == SymOp::Add && LowEnd > Cut))
      return std::nullopt;
    // With multiple multiplicands the scaled product still owns the
    // complete high slice. Do not drop any of its variable factors.
    if (!ShiftedWord)
      return WordWindow{High, At};
    return WordWindow{Ctx.operand(High, 1), At - Cut};
  }
};

} // namespace neverd::symbolic::detail

#endif
