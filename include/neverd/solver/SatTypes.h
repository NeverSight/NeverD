//===- SatTypes.h - Variables, literals and outcomes ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The vocabulary every layer of the reasoning engine shares: a boolean
/// variable, a literal over one, and the three-valued answers to "what is this
/// worth" and "can this be satisfied".
///
/// A literal is a single 32-bit word — the variable number doubled, with the
/// low bit carrying the sign.  That packing is why the clause database, the
/// watch lists and the trail are all flat integer arrays: negating a literal
/// is an exclusive-or, and "which polarity is this" is an array index rather
/// than a branch.  Keeping every structure the search touches in its inner
/// loop contiguous matters more to the running time than any single heuristic
/// in the layers above it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_SATTYPES_H
#define NEVERD_SOLVER_SATTYPES_H

#include <cstdint>

namespace neverd::solver {

/// A boolean variable, numbered from zero within one solver.
using SatVar = uint32_t;

/// The largest variable a solver can address.  A literal packs the variable
/// number doubled and reserves the all-ones word for "no literal", so this
/// bound describes the packing rather than any expected workload.
inline constexpr SatVar kMaxSatVar = (SatVar(1) << 31) - 2;

/// Stands for "no variable" in tables that have to record absence.
inline constexpr SatVar kInvalidSatVar = ~SatVar(0);

/// A variable together with a polarity.
class SatLit {
public:
  constexpr SatLit() = default;

  /// The literal for \p V, complemented when \p Negated.
  static constexpr SatLit mk(SatVar V, bool Negated = false) {
    return SatLit((V << 1) | SatVar(Negated));
  }
  static constexpr SatLit positive(SatVar V) { return mk(V, false); }
  static constexpr SatLit negative(SatVar V) { return mk(V, true); }

  /// Rebuild a literal from \c index().  Watch lists and other
  /// polarity-indexed tables store literals this way.
  static constexpr SatLit fromIndex(uint32_t I) { return SatLit(I); }

  constexpr SatVar var() const { return Code >> 1; }
  constexpr bool isNegated() const { return (Code & 1) != 0; }
  /// The packed word, which is also this literal's slot in any table with one
  /// entry per polarity.
  constexpr uint32_t index() const { return Code; }
  constexpr bool isValid() const { return Code != kInvalidCode; }
  constexpr explicit operator bool() const { return isValid(); }

  constexpr SatLit operator~() const { return SatLit(Code ^ 1); }

  /// `Positive ? *this : ~*this`, written without a branch because encoders
  /// choose polarity from a boolean far more often than they negate.
  constexpr SatLit withPolarity(bool Positive) const {
    return SatLit(Code ^ uint32_t(!Positive));
  }

  friend constexpr bool operator==(SatLit A, SatLit B) {
    return A.Code == B.Code;
  }
  friend constexpr bool operator!=(SatLit A, SatLit B) {
    return A.Code != B.Code;
  }
  /// Order by packed word: variables ascending, positive before negative.
  /// Clause construction sorts by it, which puts duplicates and a
  /// complementary pair next to each other and makes both a linear scan.
  friend constexpr bool operator<(SatLit A, SatLit B) {
    return A.Code < B.Code;
  }

private:
  static constexpr uint32_t kInvalidCode = ~uint32_t(0);
  constexpr explicit SatLit(uint32_t C) : Code(C) {}

  uint32_t Code = kInvalidCode;
};

/// What a literal is worth under an assignment.
enum class SatValue : uint8_t { False = 0, True = 1, Unknown = 2 };

inline constexpr SatValue negate(SatValue V) {
  return V == SatValue::Unknown ? V : SatValue(uint8_t(V) ^ 1);
}

/// The answer to a satisfiability question.
///
/// \c Unknown means a budget or finite resource ran out.  \c Invalid means the
/// question itself was malformed, such as an assumption naming a variable the
/// solver does not have.  Keeping those outcomes separate lets a caller retry
/// only an incomplete search and report a broken query as an error.  Neither
/// is a proof or a witness.
enum class SatResult : uint8_t {
  Unsat = 0,
  Sat = 1,
  Unknown = 2,
  Invalid = 3,
};

inline const char *satResultName(SatResult R) {
  switch (R) {
  case SatResult::Unsat:
    return "unsat";
  case SatResult::Sat:
    return "sat";
  case SatResult::Unknown:
    return "unknown";
  case SatResult::Invalid:
    return "invalid";
  }
  return "?";
}

inline const char *satValueName(SatValue V) {
  switch (V) {
  case SatValue::False:
    return "false";
  case SatValue::True:
    return "true";
  case SatValue::Unknown:
    return "unknown";
  }
  return "?";
}

} // namespace neverd::solver

#endif // NEVERD_SOLVER_SATTYPES_H
