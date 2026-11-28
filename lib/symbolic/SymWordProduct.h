//===- SymWordProduct.h - Wide bitvector multiplication ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact modular multiplication for short signed scales at wide bit widths.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SYMBOLIC_SYMWORDPRODUCT_H
#define NEVERD_LIB_SYMBOLIC_SYMWORDPRODUCT_H

#include "neverd/symbolic/Limits.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstdint>

namespace neverd::symbolic::detail {

// Multiply a word by a signed literal with a short significand. Its trailing
// zeros discard the same number of high input bits. Result may alias either
// operand. Leave it untouched when a full-width multiply is preferable.
inline bool multiplyByShortScale(const llvm::APInt &Scale,
                                 const llvm::APInt &Value,
                                 llvm::APInt &Result) {
  const unsigned W = Scale.getBitWidth(), Shift = Scale.countr_zero();
  if (Shift == W) {
    Result = Scale;
    return true;
  }
  const unsigned Live = W - Shift, Bits = Scale.getSignificantBits() - Shift;
  const bool Negative = Scale.isNegative();
  if (Bits <= 64) {
    const unsigned Take = std::min(64u, Live);
    uint64_t Unit = Scale.extractBitsAsZExtValue(Take, Shift);
    if (Negative)
      Unit = -uint64_t(llvm::SignExtend64(Unit, Take));
    Result = Value;
    if (Unit != 1)
      Result *= Unit;
    if (Negative)
      Result.negate();
    Result <<= Shift;
    return true;
  }
  const unsigned Words = llvm::APInt::getNumWords(Live);
  const unsigned MaxUnitWords =
      Scale.getNumWords() /
      ::neverd::limits::symbolic::kSymExprMinProductLimbReduction;
  if (Scale.getNumWords() <
          ::neverd::limits::symbolic::kSymExprMinMultiLimbProductWords ||
      llvm::APInt::getNumWords(Bits) > MaxUnitWords)
    return false;
  auto Unit = Scale.extractBits(Bits, Shift);
  if (Negative)
    Unit.negate();
  llvm::SmallVector<uint64_t, 8> Product(Words, 0);
  for (unsigned I = 0; I != Unit.getNumWords(); ++I)
    if (const uint64_t Limb = Unit.getRawData()[I])
      llvm::APInt::tcMultiplyPart(Product.data() + I, Value.getRawData(), Limb,
                                  0, Words - I, Words - I, true);
  auto Low = llvm::APInt(Live, Product);
  if (Negative)
    Low.negate();
  Result = Low.zextOrTrunc(W).shl(Shift);
  return true;
}

// Either word can supply a short multiplier. Try the extra scans only when
// a two-limb significand can meet the required reduction.
inline llvm::APInt multiplyWords(const llvm::APInt &A, const llvm::APInt &B) {
  if (A.getNumWords() >=
      2 * ::neverd::limits::symbolic::kSymExprMinProductLimbReduction) {
    llvm::APInt Result;
    if (multiplyByShortScale(A, B, Result) ||
        multiplyByShortScale(B, A, Result))
      return Result;
  }
  return A * B;
}

} // namespace neverd::symbolic::detail

#endif // NEVERD_LIB_SYMBOLIC_SYMWORDPRODUCT_H
