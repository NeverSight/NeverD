//===- SymBitwise.h - Truth tables and bitwise synthesis --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Boolean functions of the inputs of an MBA region, and how to turn one back
/// into the shortest expression that computes it.
///
/// This is the half of MBA solving that decides whether the answer reads well.
/// The solver's algebra tells it *which* boolean function a term stands for;
/// what it hands the user depends entirely on how that function is written
/// back out.  A conjunction-basis expansion is always available and always
/// correct, and for exclusive-or it produces `x + y - 2 * (x & y)`.  Searching
/// the space of small expressions instead produces `x ^ y`.
///
/// Three constructions are offered, and which one wins is a question of cost
/// rather than of arity.  Below a tabulation ceiling the shortest expression
/// that exists is found outright.  Above it a function is written either as a
/// sum of products, which is short when it is close to a union of cubes, or as
/// an exclusive-or of products, which is short when it is close to a parity.
/// Obfuscation produces both, and each construction is a disaster on the other
/// one's shapes — a seven-input parity is one exclusive-or of seven terms and
/// sixty-four products of seven literals — so both are costed and the cheaper
/// is kept.  Both are exact, so the choice between them is only ever about how
/// the answer reads.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMBITWISE_H
#define NEVERD_SYMBOLIC_SYMBITWISE_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MathExtras.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace neverd::symbolic {

/// The largest arity a truth table is ever built at.
///
/// A table of t inputs is 2^t bits, so this is a statement about addressable
/// memory rather than about the algebra: past it the table itself stops
/// fitting, never mind anything computed from it.  It is deliberately far
/// above where any caller's own budget stops, so that it is a backstop against
/// an overflowed shift count rather than the limit anybody actually meets.
constexpr unsigned kMaxTruthTableAtoms = 30;

/// The 2^\p NumVars input patterns a function of that arity has, or nothing
/// when a table of that arity is not built.
///
/// Every `1 << t` in the solver goes through here.  A shift by a count the
/// type cannot hold is undefined behaviour, and "as many inputs as you like"
/// is a promise about effort rather than a licence to compute a corner count
/// that does not exist.
constexpr std::optional<size_t> patternCount(unsigned NumVars) {
  if (NumVars > kMaxTruthTableAtoms)
    return std::nullopt;
  return size_t(1) << NumVars;
}

/// The truth table of a boolean function: bit \p K is the function's value on
/// the input pattern whose set bits say which inputs are true.
///
/// This used to be one machine word, which put the arity ceiling at six.  That
/// ceiling described the container rather than the problem: a seven-input
/// identity is no harder to recover than a six-input one, and abandoning it
/// because a register ran out meant the solver's reach was set by a storage
/// decision.  A bitvector-backed table removes that.  Six inputs and below
/// still sit in the inline storage of an \c llvm::APInt and cost exactly what
/// the word did; above it the table grows on the heap, and how far to go
/// becomes a resource question the caller answers.
class TruthTable {
public:
  /// A table of no inputs, everywhere false.  Containers need a default; no
  /// construction in the solver relies on this value.
  TruthTable() : Bits(1, 0) {}

  static TruthTable zero(unsigned NumVars) {
    return TruthTable(llvm::APInt::getZero(entryCount(NumVars)));
  }
  static TruthTable ones(unsigned NumVars) {
    return TruthTable(llvm::APInt::getAllOnes(entryCount(NumVars)));
  }

  unsigned numVars() const { return llvm::Log2_32(Bits.getBitWidth()); }
  /// The 2^t input patterns this table has an entry for.
  size_t entries() const { return Bits.getBitWidth(); }

  bool at(size_t Pattern) const { return Bits[static_cast<unsigned>(Pattern)]; }
  void set(size_t Pattern) { Bits.setBit(static_cast<unsigned>(Pattern)); }
  void setValue(size_t Pattern, bool Value) {
    Bits.setBitVal(static_cast<unsigned>(Pattern), Value);
  }

  bool isZero() const { return Bits.isZero(); }
  bool isOnes() const { return Bits.isAllOnes(); }
  /// How many patterns the function is true on.
  size_t count() const { return Bits.popcount(); }

  /// The entries read as one integer.  Only defined where the table fits a
  /// word, which is the same place the exhaustive search tabulates.
  uint64_t packed() const { return Bits.getZExtValue(); }

  TruthTable &operator|=(const TruthTable &O) {
    Bits |= O.Bits;
    return *this;
  }
  TruthTable &operator&=(const TruthTable &O) {
    Bits &= O.Bits;
    return *this;
  }
  TruthTable &operator^=(const TruthTable &O) {
    Bits ^= O.Bits;
    return *this;
  }

  friend TruthTable operator|(TruthTable A, const TruthTable &B) {
    return A |= B;
  }
  friend TruthTable operator&(TruthTable A, const TruthTable &B) {
    return A &= B;
  }
  friend TruthTable operator~(const TruthTable &A) {
    return TruthTable(~A.Bits);
  }
  friend bool operator==(const TruthTable &A, const TruthTable &B) {
    return A.Bits == B.Bits;
  }
  friend bool operator!=(const TruthTable &A, const TruthTable &B) {
    return !(A == B);
  }

  /// The next subset of \p Universe strictly below this one, in the descending
  /// walk that visits every subset of \p Universe exactly once and ends at the
  /// empty one.  Subtracting one and re-masking is what makes that walk cost
  /// one step per subset instead of one per pattern.
  TruthTable nextSubsetBelow(const TruthTable &Universe) const {
    llvm::APInt Next(Bits);
    --Next;
    Next &= Universe.Bits;
    return TruthTable(std::move(Next));
  }

private:
  explicit TruthTable(llvm::APInt B) : Bits(std::move(B)) {}

  static unsigned entryCount(unsigned NumVars) {
    assert(NumVars <= kMaxTruthTableAtoms && "truth table arity out of range");
    return 1u << NumVars;
  }

  llvm::APInt Bits;
};

/// The table of the function that is true exactly when input \p Index is.
TruthTable atomTruthTable(unsigned Index, unsigned NumVars);

/// The inputs \p Table actually depends on, as a bitmask over `0..t-1`.
/// Obfuscated expressions routinely mention a variable whose value cannot
/// reach the result, and dropping it before synthesis is what keeps the answer
/// from mentioning it either.
uint32_t truthTableSupport(const TruthTable &Table);

/// How hard \c synthesizeBitwise may try before it reports that it cannot
/// write a function down affordably.
///
/// None of these is a statement about which functions are expressible.  Every
/// function of every arity has an exclusive-or normal form and a sum-of-
/// products form, and both are exact; these decide only how much time and
/// memory finding and building one may cost, so that a caller asking for more
/// reach gets more reach rather than an allocation it cannot survive.
struct BitwiseSynthesisLimits {
  /// Inputs at or below which the shortest expression that exists is returned,
  /// found by tabulating every function of that arity and relaxing every pair.
  ///
  /// There are 2^(2^t) functions of t inputs and the relaxation is quadratic
  /// in that count, so the cost is not exponential but doubly so: three inputs
  /// is 256 functions and finishes in microseconds, four is 65536 and takes
  /// minutes, and five is a table nobody builds.  The dial exists because a
  /// caller that wants the fourth may have the minutes, not because the
  /// default should creep upwards.  Values above four are clamped, since a
  /// table of 2^32 recipes is an allocation failure rather than a longer wait.
  unsigned MaxOptimalAtoms = 3;

  /// Operations synthesis may spend.  The optimal path charges the first
  /// all-pairs relaxation before requesting its shared table; the two general
  /// constructions charge their table sweeps and merge rounds.  This is what
  /// stops either path from starting a search that would not finish.
  size_t MaxWork = size_t(1) << 22;

  /// Reading cost a synthesized expression may reach.
  ///
  /// A caller replaces a term with a synthesized one only when the result is
  /// shorter, so anything past what is being replaced is already refused.
  /// Costing the form before building it is what keeps the refusal from
  /// costing the memory of a sixty-thousand-node expression first.
  size_t MaxCost = std::numeric_limits<size_t>::max();
};

/// Build a purely bitwise expression over \p Atoms realising \p Table.
///
/// With \c BitwiseSynthesisLimits::MaxOptimalAtoms inputs or fewer — after the
/// ones the function ignores have been dropped — the result is the shortest
/// expression there is, found by searching every function of that arity by
/// increasing size.  Above that, both a cover by prime implicants and an
/// exclusive-or normal form are costed and the cheaper is built: each is
/// correct, and each is the short one for a different family of functions.
///
/// Returns nothing when no form fits the limits.  That is a resource answer
/// rather than a claim about the function, and the caller's response is to
/// drop the candidate rather than to report a failure.
///
/// \p Atoms must be non-empty and share a width, which becomes the width of
/// the result, and must number exactly \c Table.numVars().
std::optional<SymRef>
synthesizeBitwise(SymContext &Ctx, const TruthTable &Table,
                  llvm::ArrayRef<SymRef> Atoms,
                  const BitwiseSynthesisLimits &Limits = {});

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMBITWISE_H
