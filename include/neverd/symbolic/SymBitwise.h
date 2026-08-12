//===- SymBitwise.h - Truth tables and bitwise synthesis --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Boolean functions of a few inputs, and how to turn one back into the
/// shortest expression that computes it.
///
/// This is the half of MBA solving that decides whether the answer reads well.
/// The solver's algebra tells it *which* boolean function a term stands for;
/// what it hands the user depends entirely on how that function is written
/// back out.  A conjunction-basis expansion is always available and always
/// correct, and for exclusive-or it produces `x + y - 2 * (x & y)`.  Searching
/// the space of small expressions instead produces `x ^ y`.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMBITWISE_H
#define NEVERD_SYMBOLIC_SYMBITWISE_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>

namespace neverd::symbolic {

/// The truth table of a boolean function: bit \p K is the function's value on
/// the input pattern whose set bits say which inputs are true.
///
/// One machine word holds the whole table, which is what keeps the search
/// below cheap.  Six inputs is where a word runs out, and it is also past the
/// point where a table has any hope of collapsing into a *short* expression,
/// so the limit costs nothing that matters.
using TruthTable = uint64_t;

constexpr unsigned kMaxTruthTableVars = 6;

/// The number of inputs at or below which synthesis returns the shortest
/// expression that exists rather than merely a correct one.
constexpr unsigned kMaxOptimalTruthTableVars = 3;

/// All ones across the 2^\p NumVars entries a table of that arity uses.
constexpr TruthTable truthTableMask(unsigned NumVars) {
  return NumVars >= kMaxTruthTableVars ? ~TruthTable(0)
                                       : (TruthTable(1) << (1u << NumVars)) - 1;
}

/// The value of \p Table on input pattern \p Pattern.
constexpr bool truthTableAt(TruthTable Table, unsigned Pattern) {
  return (Table >> Pattern) & 1;
}

/// The table of the function that is true exactly when input \p Index is.
TruthTable atomTruthTable(unsigned Index, unsigned NumVars);

/// The inputs \p Table actually depends on, as a bitmask over `0..NumVars-1`.
/// Obfuscated expressions routinely mention a variable whose value cannot
/// reach the result, and dropping it before synthesis is what keeps the answer
/// from mentioning it either.
unsigned truthTableSupport(TruthTable Table, unsigned NumVars);

/// Build a purely bitwise expression over \p Atoms realising \p Table.
///
/// With \c kMaxOptimalTruthTableVars inputs or fewer — after the ones the
/// function ignores have been dropped — the result is the shortest expression
/// there is, found by searching every function of that arity by increasing
/// size.  Above that it is a cover by prime implicants: correct, and usually
/// good, but not guaranteed minimal.
///
/// \p Atoms must be non-empty and share a width, which becomes the width of
/// the result.
SymRef synthesizeBitwise(SymContext &Ctx, TruthTable Table,
                         llvm::ArrayRef<SymRef> Atoms);

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMBITWISE_H
