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
/// A *product* of bitwise terms is the one thing that gets more than a
/// placeholder.  Measurement cannot read one — at a corner every bitwise term
/// is all-zeros or all-ones, so `B * B` and `-B` agree at all 2^t of them and
/// disagree everywhere else — but it does not have to.  Writing each factor as
/// the sum of the parts of the word it selects and multiplying out gives the
/// coefficients symbolically, and that is what recognises
/// `(x & y) * (x | y) + (x & ~y) * (~x & y)` as `x * y` however it was dressed
/// up.  The same argument runs at any arity, so `x * y * z` and the degrees
/// above it come back the same way; only the size of the search for a matching
/// product sets where it stops being worth doing.
///
/// A *constant mask* is the other thing measurement cannot read, and for the
/// opposite reason: it is the one operand that tells one bit position from
/// another, which is exactly the uniformity a corner reading depends on.  It
/// does not have to be read as a whole.  Cutting the word into the groups of
/// positions that every mask treats alike leaves, on each group, an ordinary
/// linear MBA — so each is measured on its own and the answer is reassembled
/// from the bits each group owns.  That is what gets inside
/// `((x ^ y) + 2 * (x & y)) & 0xff`.  The cut is only valid while no arithmetic
/// carry crosses between groups, which is checked rather than assumed.
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
  /// Largest number of mutually dependent inputs measured in one region.  The
  /// cost is 2^MaxAtoms evaluations, so independent dependency components and
  /// mask-uniform columns are split before this bound is considered.
  unsigned MaxAtoms = 16;

  /// Random assignments the rewrite is checked against before it is returned.
  /// The derivation is exact, so this is a guard against a mistake in it
  /// rather than part of the argument.
  unsigned VerifySamples = 64;

  /// Return a rewrite even when it reads worse than what it replaces.  For
  /// measuring the solver, not for using it.
  bool AllowGrowth = false;

  /// Work units available to region measurement and combinatorial product
  /// expansion/factor recovery.  This is a resource budget, not a nesting or
  /// expression-shape cutoff: the iterative deep walk still visits every node,
  /// and opaque boundaries do not spend it.  Once exhausted, remaining regions
  /// stay intact and the result reports \c MBAOutcome::BudgetExhausted.
  /// Setting this to the largest size_t removes the resource budget.
  size_t MaxWork = size_t(1) << 22;
};

/// What became of an attempt to simplify, and why nothing did when nothing did.
///
/// "Unchanged" is three different answers wearing one face.  A caller deciding
/// whether to spend more — a wider measurement, a longer walk — and a user
/// reading a report both need to tell "there is nothing here" from "there is
/// something here I could not afford", and neither can from a bare flag.
enum class MBAOutcome : uint8_t {
  /// Nothing in the expression belongs to the algebra the solver works in, so
  /// there was nothing to measure.  Spending more would not help.
  NotApplicable,
  /// Measured, and no form shorter than what is already there exists.
  AlreadyShortest,
  /// More inputs than one measurement can afford, and no split into
  /// independent parts or mask-uniform columns was available.  A larger
  /// \c MBAOptions::MaxAtoms would reach it, at 2^t the cost.
  TooManyInputs,
  /// The layered walk or polynomial search stopped at its work budget.  A
  /// larger \c MBAOptions::MaxWork would reach the remaining work.
  BudgetExhausted,
  /// A shorter form was found and is in \c MBAResult::Expr.
  Rewritten,
};

/// What stands behind a rewrite that was made.
enum class MBAEvidence : uint8_t {
  /// Nothing was rewritten.
  None,
  /// The derivation is exact by construction and a separate deterministic
  /// coefficient verifier accepted it.  A sample check may also run, but only
  /// as a defect net rather than as the reason to trust the result.
  Derivation,
  /// Reserved for callers or optional backends that explicitly choose a
  /// heuristic result.  The built-in production solvers do not return sampled
  /// rewrites: random checks can expose a defect but cannot prove equivalence.
  /// Kept in the public enum so the versioned C ABI remains stable.
  Samples,
};

const char *mbaOutcomeName(MBAOutcome Outcome);
const char *mbaEvidenceName(MBAEvidence Evidence);

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
  MBAOutcome Outcome = MBAOutcome::NotApplicable;
  MBAEvidence Evidence = MBAEvidence::None;
  /// Work units consumed by graph traversal, corner measurements, coefficient
  /// verification, and polynomial expansion/search.  This is what the answer
  /// cost to find rather than how large the answer is.
  size_t Work = 0;
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
