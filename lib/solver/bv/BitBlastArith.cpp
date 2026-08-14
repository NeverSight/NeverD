//===- BitBlastArith.cpp - Adders, multipliers, dividers, compares --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the arithmetic circuits over literal vectors.
///
/// These are the textbook constructions — ripple carry, shift-and-add, long
/// division, a carry chain for comparison — and they are the textbook ones on
/// purpose.  Cleverer circuits exist, but the encoder in front of them folds
/// constants and shares equal gates, and that removes most of what the clever
/// ones were saving.  A carry-save tree, for instance, wins on a product of
/// two unknown values and loses on a product by a literal, which is the case
/// that actually turns up in recovered code.
///
/// The one place a choice was made against the obvious version is comparison.
/// Walking down from the most significant bit is the natural way to write it
/// and costs two gates per bit; running the carry chain of the subtraction
/// that the comparison stands for costs one, because the sums are never built.
///
//===----------------------------------------------------------------------===//

#include "BitBlastDetail.h"

#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cassert>
#include <cstddef>
#include <utility>

namespace neverd::solver::detail {

//===----------------------------------------------------------------------===//
// Selection
//===----------------------------------------------------------------------===//

void selectBits(CnfEncoder &E, SatLit Cond, LitSpan Then, LitSpan Else,
                LitVec &Out) {
  assert(Then.size() == Else.size() && "selection between unequal widths");

  Out.clear();
  Out.reserve(Then.size());
  for (size_t I = 0, N = Then.size(); I < N; ++I)
    Out.push_back(E.mkIte(Cond, Then[I], Else[I]));
}

//===----------------------------------------------------------------------===//
// Addition
//===----------------------------------------------------------------------===//

void addBits(CnfEncoder &E, LitSpan A, LitSpan B, SatLit CarryIn, LitVec &Out,
             SatLit *CarryOut) {
  assert(A.size() == B.size() && "addition of unequal widths");

  Out.clear();
  Out.reserve(A.size());

  SatLit Carry = CarryIn;
  for (size_t I = 0, N = A.size(); I < N; ++I) {
    // The carry leaving the top bit is only built when somebody asked for it;
    // for an addition modulo the width it is dead weight.
    if (I + 1 == N && CarryOut == nullptr) {
      const SatLit Ins[] = {A[I], B[I], Carry};
      Out.push_back(E.mkXor(Ins));
      return;
    }
    SatLit Sum;
    SatLit Next;
    E.mkFullAdder(A[I], B[I], Carry, Sum, Next);
    Out.push_back(Sum);
    Carry = Next;
  }

  if (CarryOut != nullptr)
    *CarryOut = Carry;
}

void negateBits(CnfEncoder &E, LitSpan A, LitVec &Out) {
  Out.clear();
  Out.reserve(A.size());

  // Complement and increment.  Written as its own loop rather than as an
  // addition of one because the carry of an increment is a conjunction rather
  // than a majority, which halves the gates.
  SatLit Carry = E.trueLit();
  for (size_t I = 0, N = A.size(); I < N; ++I) {
    SatLit Bit = ~A[I];
    Out.push_back(E.mkXor(Bit, Carry));
    if (I + 1 < N)
      Carry = E.mkAnd(Bit, Carry);
  }
}

//===----------------------------------------------------------------------===//
// Multiplication
//===----------------------------------------------------------------------===//

namespace {

/// Every bit of \p Bits, when all of them are already decided.
bool constantPattern(const CnfEncoder &E, LitSpan Bits,
                     llvm::SmallVectorImpl<bool> &Out) {
  Out.clear();
  Out.reserve(Bits.size());
  for (SatLit L : Bits) {
    if (!E.isConstant(L))
      return false;
    Out.push_back(E.isTrueLit(L));
  }
  return true;
}

/// One shifted copy of the multiplicand in a product by a literal.
struct ProductTerm {
  size_t Shift;
  bool Subtract;
};

/// Rewrite a constant multiplier as the shifted copies to add and to subtract.
///
/// A run of set bits from \c First to \c Last is worth `2^(Last+1) - 2^First`,
/// so a run of any length costs two terms rather than one per bit.  That pays
/// from three bits up, and it is what keeps the commonest literal of all cheap:
/// minus one is a run across the whole width, and recodes to a single
/// negation instead of one adder row per bit.
void recodeMultiplier(llvm::ArrayRef<bool> Multiplier,
                      llvm::SmallVectorImpl<ProductTerm> &Out) {
  Out.clear();
  const size_t N = Multiplier.size();

  for (size_t First = 0; First < N;) {
    if (!Multiplier[First]) {
      ++First;
      continue;
    }

    size_t Last = First;
    while (Last + 1 < N && Multiplier[Last + 1])
      ++Last;

    if (Last - First + 1 >= 3) {
      // The term above a run that reaches the top bit is shifted out of the
      // width entirely, and a term shifted past the width is zero.
      if (Last + 1 < N)
        Out.push_back({Last + 1, /*Subtract=*/false});
      Out.push_back({First, /*Subtract=*/true});
    } else {
      for (size_t Bit = First; Bit <= Last; ++Bit)
        Out.push_back({Bit, /*Subtract=*/false});
    }
    First = Last + 1;
  }
}

/// \p A times the literal whose bits are \p Multiplier.
void multiplyByPattern(CnfEncoder &E, LitSpan A,
                       llvm::ArrayRef<bool> Multiplier, LitVec &Out) {
  const size_t N = A.size();
  Out.assign(N, E.falseLit());

  llvm::SmallVector<ProductTerm, 8> Terms;
  recodeMultiplier(Multiplier, Terms);

  LitVec Addend;
  LitVec Sum;
  for (const ProductTerm &Term : Terms) {
    // A subtracted copy is added as its complement with a carry in, and the
    // complement covers the zeros shifted in below it as well.  The first term
    // meets a running total that is still zero, so its adder cells fold away
    // and a lone term costs nothing beyond the rewiring.
    const SatLit Fill = Term.Subtract ? E.trueLit() : E.falseLit();
    Addend.assign(N, Fill);
    for (size_t I = Term.Shift; I < N; ++I) {
      SatLit Bit = A[I - Term.Shift];
      Addend[I] = Term.Subtract ? ~Bit : Bit;
    }

    addBits(E, Out, Addend, Fill, Sum);
    Out.swap(Sum);
  }
}

} // namespace

void multiplyBits(CnfEncoder &E, LitSpan A, LitSpan B, LitVec &Out) {
  assert(A.size() == B.size() && "multiplication of unequal widths");

  // Either operand may be the literal: a product's operands are sorted by node
  // identity rather than by which of them happens to be known.
  llvm::SmallVector<bool, 64> Pattern;
  if (constantPattern(E, B, Pattern))
    return multiplyByPattern(E, A, Pattern, Out);
  if (constantPattern(E, A, Pattern))
    return multiplyByPattern(E, B, Pattern, Out);

  const size_t N = A.size();
  Out.assign(N, E.falseLit());

  for (size_t Row = 0; Row < N; ++Row) {
    SatLit Multiplier = B[Row];
    // A row whose multiplier bit is known false contributes nothing.  A
    // multiplier that is known in every bit was recoded above, so what reaches
    // here is one that is only partly decided — a masked or extended value,
    // typically — and those decided bits are still worth skipping.
    if (E.isFalseLit(Multiplier))
      continue;

    // Bits below the row's offset are unaffected, and bits above the width are
    // discarded, so only this window is built.
    SatLit Carry = E.falseLit();
    for (size_t I = Row; I < N; ++I) {
      SatLit Partial = E.mkAnd(A[I - Row], Multiplier);
      if (I + 1 == N) {
        const SatLit Ins[] = {Out[I], Partial, Carry};
        Out[I] = E.mkXor(Ins);
        break;
      }
      SatLit Sum;
      SatLit Next;
      E.mkFullAdder(Out[I], Partial, Carry, Sum, Next);
      Out[I] = Sum;
      Carry = Next;
    }
  }
}

//===----------------------------------------------------------------------===//
// Division
//===----------------------------------------------------------------------===//

void divideBits(CnfEncoder &E, LitSpan A, LitSpan B, LitVec *Quotient,
                LitVec *Remainder) {
  assert(A.size() == B.size() && "division of unequal widths");

  const size_t N = A.size();

  // The running remainder carries one bit more than the operands so that
  // shifting a dividend bit into it can never lose the top of it.  It stays
  // below the divisor after every step, so that extra bit is only ever used
  // between the shift and the subtraction.
  LitVec Rest(N + 1, E.falseLit());
  LitVec Quot(N, E.falseLit());
  LitVec Shifted(N + 1, E.falseLit());
  LitVec Difference(N + 1, E.falseLit());

  for (size_t Step = 0; Step < N; ++Step) {
    const size_t Bit = N - 1 - Step;

    for (size_t I = N; I > 0; --I)
      Shifted[I] = Rest[I - 1];
    Shifted[0] = A[Bit];

    // Subtract by adding the complement and one.  The carry leaving the top
    // says the subtraction did not borrow, which is exactly "the divisor
    // fits", so the comparison costs nothing beyond the subtraction itself.
    SatLit Carry = E.trueLit();
    for (size_t I = 0; I <= N; ++I) {
      SatLit Operand = I < N ? ~B[I] : E.trueLit();
      SatLit Sum;
      SatLit Next;
      E.mkFullAdder(Shifted[I], Operand, Carry, Sum, Next);
      Difference[I] = Sum;
      Carry = Next;
    }

    Quot[Bit] = Carry;
    for (size_t I = 0; I <= N; ++I)
      Rest[I] = E.mkIte(Carry, Difference[I], Shifted[I]);
  }

  if (Quotient != nullptr)
    *Quotient = std::move(Quot);
  if (Remainder != nullptr)
    Remainder->assign(Rest.begin(), Rest.begin() + N);
}

void divideSignedBits(CnfEncoder &E, LitSpan A, LitSpan B, LitVec *Quotient,
                      LitVec *Remainder) {
  assert(A.size() == B.size() && "division of unequal widths");
  assert(!A.empty() && "division of a zero-width value");

  const SatLit SignA = A.back();
  const SatLit SignB = B.back();

  LitVec Negated;
  LitVec MagnitudeA;
  LitVec MagnitudeB;

  negateBits(E, A, Negated);
  selectBits(E, SignA, Negated, A, MagnitudeA);
  negateBits(E, B, Negated);
  selectBits(E, SignB, Negated, B, MagnitudeB);

  LitVec UnsignedQuotient;
  LitVec UnsignedRemainder;
  divideBits(E, MagnitudeA, MagnitudeB,
             Quotient != nullptr ? &UnsignedQuotient : nullptr,
             Remainder != nullptr ? &UnsignedRemainder : nullptr);

  if (Quotient != nullptr) {
    negateBits(E, UnsignedQuotient, Negated);
    selectBits(E, E.mkXor(SignA, SignB), Negated, UnsignedQuotient, *Quotient);
  }

  if (Remainder != nullptr) {
    // A truncating remainder takes the sign of the dividend, which is also
    // what makes the zero-divisor case come out as the dividend unchanged.
    negateBits(E, UnsignedRemainder, Negated);
    selectBits(E, SignA, Negated, UnsignedRemainder, *Remainder);
  }
}

//===----------------------------------------------------------------------===//
// Comparison
//===----------------------------------------------------------------------===//

SatLit equalBits(CnfEncoder &E, LitSpan A, LitSpan B) {
  assert(A.size() == B.size() && "comparison of unequal widths");

  LitVec Same;
  Same.reserve(A.size());
  for (size_t I = 0, N = A.size(); I < N; ++I)
    Same.push_back(E.mkEquiv(A[I], B[I]));
  return E.mkAnd(Same);
}

SatLit unsignedAtLeast(CnfEncoder &E, LitSpan A, LitSpan B) {
  assert(A.size() == B.size() && "comparison of unequal widths");

  // The carry chain of `A + ~B + 1`.  Its sums are the difference, which
  // nobody wants here, so only the carries are built.
  SatLit Carry = E.trueLit();
  for (size_t I = 0, N = A.size(); I < N; ++I)
    Carry = E.mkMajority(A[I], ~B[I], Carry);
  return Carry;
}

SatLit lessThanBits(CnfEncoder &E, LitSpan A, LitSpan B, bool OrEqual,
                    bool Signed) {
  assert(A.size() == B.size() && "comparison of unequal widths");
  assert(!A.empty() && "comparison of a zero-width value");

  LitVec FlippedA;
  LitVec FlippedB;
  if (Signed) {
    FlippedA.assign(A.begin(), A.end());
    FlippedB.assign(B.begin(), B.end());
    FlippedA.back() = ~FlippedA.back();
    FlippedB.back() = ~FlippedB.back();
    A = FlippedA;
    B = FlippedB;
  }

  // `A <= B` is `B >= A`, and `A < B` is the failure of `A >= B`, so one
  // circuit with its operands the right way round answers both.
  return OrEqual ? unsignedAtLeast(E, B, A) : ~unsignedAtLeast(E, A, B);
}

} // namespace neverd::solver::detail
