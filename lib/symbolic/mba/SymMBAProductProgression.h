//===- SymMBAProductProgression.h - Modular windows ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_MBA_SYMMBAPRODUCTPROGRESSION_H
#define NEVERD_SYMBOLIC_MBA_SYMMBAPRODUCTPROGRESSION_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>

namespace neverd::symbolic::detail {

struct MaskedOffset {
  SymRef Base;
  unsigned Shift, Offset;
};

/// (X + Offset) XOR (Y * Z), or (X + Offset) XOR Y when Z.Base is empty.
/// Every present multiplicand is a masked, offset input.
struct ProductFactor {
  SymRef Source, X;
  unsigned Offset;
  MaskedOffset Y, Z;
};

struct ProductWindow {
  unsigned Period, Length, Copies, MaxExceptions;
};

std::optional<ProductWindow> chooseProductWindow(unsigned Bits, size_t Count,
                                                 bool SingleMask);

/// The factor at i uses X+XOffset+i*XStep, Y+i and Z+ZOffset+i*ZStep.
/// Missing indices complete the window. Every other source factor is retained.
struct ProductProgression {
  ProductWindow Window;
  ProductFactor Basis;
  unsigned Start, XOffset, ZOffset, XStep, ZStep;
  llvm::SmallVector<unsigned, 8> Missing;
  llvm::SmallVector<SymRef, 8> Retained;

  unsigned basePrecision(unsigned Bits) const;
};

bool hasMixedProgressionHint(const SymContext &Ctx, SymRef Root,
                             llvm::function_ref<bool(size_t)> Charge);

std::optional<ProductProgression>
matchProductProgression(llvm::MutableArrayRef<ProductFactor> Factors,
                        ProductWindow Window, bool HasRetained,
                        llvm::function_ref<bool(size_t, size_t)> Charge);

} // namespace neverd::symbolic::detail

#endif // NEVERD_SYMBOLIC_MBA_SYMMBAPRODUCTPROGRESSION_H
