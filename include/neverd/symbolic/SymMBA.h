//===- SymMBA.h - Mixed boolean-arithmetic simplification -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers the short form of a mixed boolean-arithmetic expression.
///
/// MBA obfuscation replaces a small arithmetic expression with a sprawling one
/// that mixes `+ - *` and `& | ^ ~` so that neither an algebraic simplifier nor
/// a boolean one makes progress alone: every rewrite either side offers is
/// blocked by an operator belonging to the other.  `x + y` becomes
/// `(x ^ y) + 2 * (x & y)`, and that in turn becomes something with fifty
/// terms.  Being immune to peephole rewriting is the whole point of it, so a
/// rule-driven simplifier of any size will not touch it.
///
/// It comes apart because of a structural fact.  Call an expression *linear
/// MBA* when it is a sum of constant multiples of purely bitwise terms.  Any
/// such expression is completely determined by its value at the 2^t points
/// where every input is either all-zeros or all-ones — a t-input expression
/// has only 2^t degrees of freedom no matter how large it is written.  So the
/// obfuscation cannot survive being *measured*: evaluate at those points, read
/// off the coefficients, and write the result back out in whichever form comes
/// out shortest.
///
/// Anything that is not part of that algebra — a division, a shift by a
/// variable, a comparison — is replaced by a placeholder before measuring, and
/// put back afterwards.  That makes the input a linear MBA by construction
/// rather than by hope, so the rewrite is exact rather than a guess that
/// happens to check out.
///
/// A *product* of two bitwise terms is the one thing that gets more than a
/// placeholder.  Measurement cannot read one — at a corner every bitwise term
/// is all-zeros or all-ones, so `B * B` and `-B` agree at all 2^t of them and
/// disagree everywhere else — but it does not have to.  Writing each factor as
/// the sum of the parts of the word it selects and multiplying out gives the
/// coefficients symbolically, and that is what recognises
/// `(x & y) * (x | y) + (x & ~y) * (~x & y)` as `x * y` however it was
/// dressed up.  Beyond degree two the term becomes a placeholder like anything
/// else.
///
/// Every width is handled the same way, because constants are \c llvm::APInt.
/// A 256-bit EVM word is measured exactly like a 32-bit one.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMMBA_H
#define NEVERD_SYMBOLIC_SYMMBA_H

#include "neverd/symbolic/SymExpr.h"

namespace neverd::symbolic {

struct MBAOptions {
  /// Largest number of distinct inputs the solver will measure over.  The cost
  /// is 2^MaxAtoms evaluations, so this is the dial between reach and time.
  /// The identities obfuscators build from are overwhelmingly over two or
  /// three variables, so the default leaves a wide margin.
  unsigned MaxAtoms = 16;

  /// Give up on the conjunction-basis form once it needs more terms than this.
  /// It always exists but can have 2^t of them, and building a result the size
  /// guard will only reject is wasted work.
  unsigned MaxTerms = 64;

  /// Random assignments the rewrite is checked against before it is returned.
  /// The derivation is exact, so this is a guard against a mistake in it
  /// rather than part of the argument.
  unsigned VerifySamples = 64;

  /// Return a rewrite even when it reads worse than what it replaces.  For
  /// measuring the solver, not for using it.
  bool AllowGrowth = false;

  /// Roughly how many graph nodes \c simplifyMBADeep will visit while looking
  /// for regions to measure.  Walking a large expression region by region is
  /// quadratic in its size; past this budget the remaining nodes are still
  /// rebuilt around whatever was simplified underneath them, but no new
  /// measurement is started.
  size_t MaxWork = size_t(1) << 22;
};

struct MBAResult {
  /// The simplified expression, or the input unchanged.
  SymRef Expr;
  bool Changed = false;
  /// What the expression costs a reader, before and after.  This is close to a
  /// node count but charges nothing for the all-ones literal, which is a sign
  /// or a mask rather than a quantity — see the note in the implementation.
  size_t SizeBefore = 0;
  size_t SizeAfter = 0;
  /// How many distinct inputs the expression was measured over.  Zero when the
  /// solver declined to work on it at all.
  unsigned NumAtoms = 0;
};

/// Measure \p E as one region and rewrite it into the best form that admits.
///
/// Returns \p E unchanged when it has too many inputs to measure, or when
/// nothing came out better than what is already there.  Subterms the theory
/// cannot see inside of are left exactly as they are; use \c simplifyMBADeep
/// to reach those too.
MBAResult simplifyMBA(SymContext &Ctx, SymRef E, const MBAOptions &Opts = {});

/// Simplify \p E and everything inside it, innermost first.
///
/// One pass of \c simplifyMBA sees a single layer: it measures what it can and
/// treats the rest as inputs.  Obfuscation is applied in layers, though, each
/// one hiding the last, so undoing it takes a walk that reaches the innermost
/// expression first and then measures each layer over the shortened result of
/// the one below.  This is the entry point to use on real obfuscated code.
MBAResult simplifyMBADeep(SymContext &Ctx, SymRef E,
                          const MBAOptions &Opts = {});

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMMBA_H
