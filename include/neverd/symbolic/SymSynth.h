//===- SymSynth.h - Oracle-guided expression synthesis ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers a short expression by *searching* for one that behaves the same
/// way, rather than by deriving one from the structure of what is there.
///
/// The MBA solver next door works the other way round, and the difference is
/// the reason both exist.  It identifies an algebra the expression lives in,
/// measures the expression at the points that pin a member of that algebra
/// down, and writes the answer back out.  Every rewrite it makes is exact by
/// construction, and it makes none at all outside the algebra: a logical shift
/// right, a division, two spellings of the same subterm that it has to treat
/// as two unrelated inputs.  Obfuscated code is full of exactly those, sitting
/// between the parts the measurement can read.
///
/// What is left over is usually *small*.  A block the measurement declines is
/// rarely an intricate function of twelve inputs; far more often it is two or
/// three inputs and a handful of operators, wearing a hundred nodes of
/// disguise.  A short answer that cannot be derived can still be *found*, and
/// finding it needs nothing from the expression but the ability to run it.
///
/// So this treats the expression as a blackbox.  Pick a set of points, ask the
/// expression what it produces at each of them, and then look for the shortest
/// expression in a small grammar that produces the same outputs.  Two searches
/// share that specification:
///
///   - *Enumeration*, in ascending cost, keeping one representative of each
///     distinct behaviour.  Two candidates that agree at every sample point
///     will agree at every point any larger candidate built on top of them is
///     ever measured at, so keeping both only doubles the work below them.
///     Discarding one is what turns an exponential enumeration into a tractable
///     one, and it is the reason the search reaches sizes worth reaching.
///
///   - *A sampler*, for when enumeration cannot reach far enough.  It holds a
///     single candidate, mutates it, and keeps or discards the mutation by how
///     many output bits it gets right.  That gives up the guarantee of finding
///     the shortest answer, and buys the ability to look at candidates the
///     enumeration would need to build the whole space below to reach.
///
/// The catch is stated plainly: agreement at finitely many points is evidence,
/// not proof.  So a candidate is never returned on the strength of the points
/// it was selected by.  It is re-checked against a separate, larger set of
/// points it was not fitted to — fitting and checking on the same data proves
/// only that the fit happened — and the caller may supply a decision procedure
/// through \c SynthVerifyFn that settles the question outright.  A result
/// reports which of the two backed its candidate, so nothing downstream has
/// to guess
/// whether it is looking at a proof or at a very good guess.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMSYNTH_H
#define NEVERD_SYMBOLIC_SYMSYNTH_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <cstdint>

namespace neverd::symbolic {

/// The answer from a decision procedure asked whether two expressions are
/// equal at every assignment of their inputs.
enum class SynthVerification : uint8_t {
  /// No assignment makes the expressions differ.
  Equivalent = 1,
  /// A concrete assignment makes the expressions differ.
  Different = 2,
  /// The procedure did not settle the question within its capabilities or
  /// resource budget.
  Unknown = 3,
};
static_assert(static_cast<uint8_t>(SynthVerification::Equivalent) == 1 &&
              static_cast<uint8_t>(SynthVerification::Different) == 2 &&
              static_cast<uint8_t>(SynthVerification::Unknown) == 3);

const char *synthVerificationName(SynthVerification Verification);

/// A decision procedure that settles whether two expressions are equal at
/// every assignment of their inputs when it can.
///
/// The search proposes; this disposes.  \c llvm::function_ref keeps the call
/// allocation-free while allowing a solver and its options to be captured by
/// a local adapter without making the Symbolic component depend on Solver.
///
/// \c Different and \c Unknown both reject a production rewrite, but remain
/// distinct in \c SynthResult so a caller can report the former and retry the
/// latter with a larger budget.
using SynthVerifyFn =
    llvm::function_ref<SynthVerification(SymContext &Ctx, SymRef A, SymRef B)>;

struct SynthOptions {
  /// Largest candidate the enumeration will build, counted as the number of
  /// nodes in the tree it writes out.  `x + y` is three, `2 * (x >> 4)` is
  /// five.
  ///
  /// The number of candidates grows faster than exponentially in this, so it
  /// is a statement about the answers worth looking for rather than a
  /// performance dial: past a certain size a "simplified" expression is not
  /// one.  \c MaxWork is what actually stops a runaway search.
  size_t MaxCost = 6;

  /// Points a candidate must reproduce to be considered at all.
  ///
  /// Too few and unrelated expressions collide, which costs a verification
  /// each time.  Too many and every candidate costs more to measure.  The
  /// grid always contains the corners — every input all-zeros or all-ones —
  /// because that is where an expression built out of bitwise identities is
  /// most likely to agree by accident, and random points besides.
  size_t MaxSamples = 64;

  /// Points an accepted candidate is re-checked at, none of which it was
  /// selected by.  Only meaningful when no \c SynthVerifyFn is supplied, and
  /// then it is the entire strength of the answer.
  unsigned VerifySamples = 512;

  /// Run the sampler when the enumeration comes back empty.
  bool UseStochasticFallback = true;

  /// Candidates both searches may consider between them.
  ///
  /// This is a resource budget rather than a shape cutoff: nothing about which
  /// expressions can be expressed depends on it, only how much of the space is
  /// looked at before the attempt is abandoned.  Exhausting it is reported, so
  /// a caller can tell "there is nothing there" from "I could not afford to
  /// look".
  size_t MaxWork = size_t(1) << 20;

  /// Most inputs the expression may have before the search declines.
  ///
  /// Every extra input multiplies the branching factor of the enumeration and
  /// the number of points needed to tell candidates apart.  Beyond a handful
  /// the search is not going to finish, and saying so immediately is better
  /// than spending the whole budget discovering it.
  unsigned MaxLeaves = 8;

  /// Most literals the search may use as terminals.  They are chosen rather
  /// than guessed — see the note on coefficient reading in the implementation
  /// — so a small number goes a long way.
  unsigned MaxConstants = 8;

  /// Operations in the straight-line program the sampler mutates.  This bounds
  /// the size of what the sampler can express, the way \c MaxCost bounds the
  /// enumeration.
  unsigned StochasticSlots = 6;

  /// Independent attempts the sampler makes.  The first starts from a greedily
  /// constructed program and the rest from random ones, so raising this trades
  /// time for coverage of a search space with many local optima.
  unsigned StochasticRestarts = 6;

  /// Mutations the sampler tries per attempt.
  size_t StochasticIterations = 4096;

  /// Let either search build a shift whose amount is itself computed.
  ///
  /// Off by default: a data-dependent shift is rare in the residue this engine
  /// is aimed at, while admitting one multiplies the branching factor of every
  /// shift by the size of the whole candidate pool.  It also asks a great deal
  /// of the sampling, since two shifts by computed amounts agree wherever both
  /// amounts run off the end of the word — which is nearly everywhere.
  bool AllowVariableShifts = false;

  /// Return a rewrite even when it reads worse than what it replaces.  For
  /// measuring the search, not for using it.
  bool AllowGrowth = false;

  /// Seeds every random choice: the sample grid, the sampler's starting
  /// programs, and its mutations.  Two runs with the same seed on the same
  /// build produce the same answer, which is what makes a difference in output
  /// a real change rather than a reshuffle.
  uint64_t Seed = 0x243F6A8885A308D3ull;
};

/// What became of an attempt to synthesize, and why nothing did when nothing
/// did.
///
/// A caller deciding whether to spend more — a larger budget, a bigger
/// grammar, a real decision procedure — cannot tell any of these apart from a
/// bare "unchanged", and they call for entirely different responses.
enum class SynthOutcome : uint8_t {
  /// There was nothing to search over: no inputs, or an expression that is one
  /// opaque operation the grammar cannot see inside of.  Spending more would
  /// not help.
  NotApplicable,
  /// Searched, and nothing shorter than what is already there came out.
  AlreadyShortest,
  /// More inputs than the search is willing to enumerate over.
  TooManyInputs,
  /// The search stopped at \c SynthOptions::MaxWork with candidates left
  /// unexamined.  A larger budget can change this answer.
  BudgetExhausted,
  /// Something reproduced every sample point and then failed verification.
  /// An independent sample or the decision procedure found an assignment at
  /// which it computes a different value.
  Counterexample,
  /// Something reproduced every sample point, but the decision procedure
  /// could not settle equivalence within its capabilities or resource budget.
  ProofIncomplete,
  /// A shorter candidate survived the requested verification policy.  It is
  /// in \c SynthResult::Expr; \c SynthResult::Evidence says whether that policy
  /// was sampling or proof.
  Synthesized,
};

/// What stands behind an accepted synthesis candidate.
enum class SynthEvidence : uint8_t {
  /// Nothing was accepted.
  None,
  /// Agreement at every point of a grid the candidate was not selected by.
  /// Strong, and still not a proof: a caller that needs one supplies a
  /// \c SynthVerifyFn.
  Samples,
  /// A caller-supplied decision procedure accepted the pair, on top of the
  /// sampling.
  Verifier,
};

const char *synthOutcomeName(SynthOutcome Outcome);
const char *synthEvidenceName(SynthEvidence Evidence);

struct SynthResult {
  /// The synthesized expression, or the input unchanged.
  SymRef Expr;
  bool Changed = false;
  /// What the expression costs a reader, before and after.  Same measure the
  /// MBA solver ranks by, so the two engines' answers are comparable.
  size_t SizeBefore = 0;
  size_t SizeAfter = 0;
  /// Independent inputs the expression was searched over.  Subterms outside
  /// the grammar count as one input each.
  unsigned NumLeaves = 0;
  /// Grammar cost of the accepted candidate, in the units of
  /// \c SynthOptions::MaxCost.  Zero when nothing was accepted.
  size_t CandidateCost = 0;
  SynthOutcome Outcome = SynthOutcome::NotApplicable;
  SynthEvidence Evidence = SynthEvidence::None;
  /// The semantic status when the attempt concluded.  \c Different can come
  /// from either a concrete sample or the procedure.  \c Unknown also covers
  /// the absence of a procedure: sampling can discover a candidate but cannot
  /// prove a production rewrite safe.
  SynthVerification Verification = SynthVerification::Unknown;
  /// Candidates examined.  This is what the answer cost to find rather than
  /// how large the answer is.
  size_t Work = 0;
  /// Calls made to the proof procedure.  A mismatch on the independent sample
  /// grid is a concrete refutation, but is not a proof-procedure query.
  uint64_t ProofQueries = 0;
  /// True when the sampler rather than the enumeration produced the answer,
  /// which also means it is the shortest form *found* rather than the shortest
  /// form that exists within the grammar.
  bool FromSampler = false;
};

/// Discover a short candidate that behaves like \p E on independent samples.
///
/// This is deliberately a heuristic, sample-only analysis interface.  Its
/// result may be useful for presentation or as a proof candidate, but a
/// transforming pass must call \c synthesizeEquivalent instead.
SynthResult discoverSynthesisCandidate(SymContext &Ctx, SymRef E,
                                       const SynthOptions &Opts = {});

/// Search for a short expression and ask \p Verify to decide each candidate
/// that survives the independent sample grid.
SynthResult synthesize(SymContext &Ctx, SymRef E, const SynthOptions &Opts,
                       SynthVerifyFn Verify);

/// The expression \c synthesize settled on: a shorter equivalent when one was
/// found and proved equivalent, and \p Input itself otherwise.
///
/// This is the form to call from a rewriting pass, which has nothing to do
/// with the difference between "already shortest" and "could not afford to
/// look" and only wants the better of the two expressions.  No proof
/// procedure, a refutation, and an incomplete proof all preserve \p Input.
SymRef synthesizeEquivalent(SymContext &Ctx, SymRef Input,
                            const SynthOptions &Opts = {});

/// The proof-gated form of \c synthesizeEquivalent.  This overload is
/// intentionally distinct from the compatibility path above: verifier
/// absence is represented by the overload selected, never by a null
/// \c llvm::function_ref.
SymRef synthesizeEquivalent(SymContext &Ctx, SymRef Input,
                            const SynthOptions &Opts, SynthVerifyFn Verify);

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMSYNTH_H
