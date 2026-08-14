//===- SymSynthDetail.h - Shared synthesis internals ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the pieces of the search that more than one of the synthesis
/// translation units needs: the grammar, the reduction of an arbitrary
/// expression to a body over independent inputs, the sample grid and the
/// behaviour signatures read off it, the two searches, and the check every
/// candidate has to survive.
///
/// One rule runs through all of it.  A candidate's *behaviour* — its outputs
/// at the sample points — is computed before the candidate is built, from the
/// behaviours of its operands.  Building costs an interning, and the great
/// majority of candidates are discarded for behaving like something already
/// seen, so building first would spend the expensive half of the work on the
/// candidates that turn out not to matter.
///
/// This header is an implementation detail of the symbolic library and should
/// not be included outside lib/symbolic/synth/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYNTH_SYMSYNTHDETAIL_H
#define NEVERD_SYMBOLIC_SYNTH_SYMSYNTHDETAIL_H

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace neverd::symbolic::synth {

//===----------------------------------------------------------------------===//
// The grammar
//===----------------------------------------------------------------------===//

/// The operators a candidate may be built out of.
///
/// This is the arithmetic and bitwise core of the expression language plus the
/// shifts, which between them is what the residue of an obfuscation is written
/// in.  The structural operators — extraction, concatenation, the width
/// changes — and the predicates are deliberately outside it: they change the
/// width or produce a single bit, so admitting them would turn one uniform
/// search space into a union of differently typed ones, for subterms that are
/// better served by standing an input in front of them.
///
/// \c Sub and \c Neg have no node of their own; the builders write them as a
/// sum and a product by all-ones.  They are in the grammar anyway because they
/// are the shapes a reader sees, and because reaching `x - y` in one step
/// rather than three is the difference between finding it and not.
enum class GramOp : uint8_t {
  Add,
  Sub,
  Mul,
  And,
  Or,
  Xor,
  Shl,
  LShr,
  AShr,
  Not,
  Neg,
};

inline constexpr unsigned kNumGramOps = 11;

constexpr bool isUnaryGramOp(GramOp Op) {
  return Op == GramOp::Not || Op == GramOp::Neg;
}

constexpr bool isShiftGramOp(GramOp Op) {
  return Op == GramOp::Shl || Op == GramOp::LShr || Op == GramOp::AShr;
}

/// True for the operators whose operands may be swapped without changing the
/// result, so the search need only build one of each pair.
constexpr bool isCommutativeGramOp(GramOp Op) {
  return Op == GramOp::Add || Op == GramOp::Mul || Op == GramOp::And ||
         Op == GramOp::Or || Op == GramOp::Xor;
}

/// The grammar operator an existing node uses, when the grammar has one.
std::optional<GramOp> gramOpOf(SymOp Op);

/// Apply \p Op to \p Args through the canonicalizing builders.
SymRef buildGramOp(SymContext &Ctx, GramOp Op, llvm::ArrayRef<SymRef> Args);

/// The meaning of each grammar operator, on values rather than on nodes.
///
/// What an operator does is stated once, in \c evalNodeAP, which takes a node.
/// The search needs the same answer for operands it has not built a node for
/// yet, so this interns one node per operator over two placeholder inputs the
/// canonicalizing builders cannot fold, and hands those nodes back.  Restating
/// the semantics here instead would give the engine a second opinion about
/// what `ashr` means, and the two would drift.
class OpSemantics {
public:
  OpSemantics(SymContext &Ctx, uint32_t Width);

  /// The value \c buildGramOp would produce for operands of these values.
  llvm::APInt apply(GramOp Op, llvm::ArrayRef<llvm::APInt> Args) const;

private:
  const SymContext &Ctx;
  llvm::APInt Ones;
  /// One node per operator, invalid for the two the builders synthesize.
  SymRef Template[kNumGramOps];
};

//===----------------------------------------------------------------------===//
// Points and behaviours
//===----------------------------------------------------------------------===//

/// One value per input: the place an expression is asked what it produces.
using SamplePoint = llvm::SmallVector<llvm::APInt, 4>;

/// What a term produces at every point of a grid.
///
/// Two terms with the same signature are interchangeable everywhere the search
/// can tell, which is what lets the enumeration keep one of them and drop the
/// other.
using Signature = std::vector<llvm::APInt>;

bool signaturesEqual(const Signature &A, const Signature &B);

/// A hash over the raw words of every value, so that lookup does not depend on
/// how wide the values happen to be.
uint64_t hashSignature(const Signature &S);

/// Bits of \p A that match \p B, out of every bit of every value.
///
/// This is what the sampler climbs.  A candidate that gets most of the output
/// bits right at most points is close to one that gets them all right, and a
/// plain right-or-wrong count per point cannot see that: it reports a
/// candidate one bit away and a candidate entirely unrelated as equally bad,
/// which leaves the sampler nothing to follow.
size_t agreeingBits(const Signature &A, const Signature &B);

/// Total bits a signature holds, i.e. what \c agreeingBits returns for a pair
/// that matches everywhere.
size_t totalBits(const Signature &S);

/// Points to measure at, in a fixed order for a given input size and seed.
std::vector<SamplePoint> buildGrid(unsigned NumLeaves, uint32_t Width,
                                   size_t MaxPoints, uint64_t Seed);

/// Evaluate \p R over \p Grid, with \p LeafVars naming the variable each
/// column of a point belongs to.
///
/// This is the oracle, and it is a \c SymEvalPlan because that is what makes
/// asking the same expression thousands of questions affordable: the plan
/// flattens the graph once, so a shared subterm — of which an obfuscated
/// expression is mostly made — is visited once per point rather than once per
/// occurrence per point.
Signature evaluateOnGrid(const SymContext &Ctx, SymRef R,
                         llvm::ArrayRef<uint32_t> LeafVars,
                         llvm::ArrayRef<SamplePoint> Grid);

//===----------------------------------------------------------------------===//
// The problem
//===----------------------------------------------------------------------===//

/// An expression reduced to something the search can work on: a body over
/// independent inputs, the terminals it may be rebuilt from, and what it
/// produces at the points it will be measured at.
struct SynthProblem {
  /// The input rewritten over inputs the grammar can drive.
  SymRef Body;
  /// Those inputs, in the order a \c SamplePoint assigns to them.
  llvm::SmallVector<SymRef, 8> Leaves;
  /// Variable id of each leaf, in the same order.
  llvm::SmallVector<uint32_t, 8> LeafVars;
  /// Placeholder node index to the subterm standing behind it.  Empty when
  /// the whole input was already in the grammar.
  std::unordered_map<uint32_t, SymRef> Hidden;
  /// Literals the search may use as terminals.
  llvm::SmallVector<SymRef, 8> Constants;
  /// Where every candidate is measured.
  std::vector<SamplePoint> Grid;
  /// What \c Body produces there.
  Signature Target;
  uint32_t Width = 0;
};

enum class ProblemStatus : uint8_t {
  /// Ready to search.
  Ready,
  /// No inputs, or a body that is one input — nothing a search could shorten.
  Trivial,
  /// More inputs than \c SynthOptions::MaxLeaves allows.
  TooManyLeaves,
};

ProblemStatus buildProblem(SymContext &Ctx, SymRef Root,
                           const SynthOptions &Opts, SynthProblem &Out);

/// The terminals a candidate may start from: every leaf, then every literal.
llvm::SmallVector<SymRef, 16> terminalsOf(const SynthProblem &P);

/// What each terminal produces at each grid point.  No evaluation is needed:
/// a leaf's value at a point *is* the point, and a literal's is itself.
std::vector<Signature> terminalSignatures(const SymContext &Ctx,
                                          const SynthProblem &P);

//===----------------------------------------------------------------------===//
// Budget
//===----------------------------------------------------------------------===//

/// A bounded number of candidates.
///
/// Both searches grow faster than any description of expression shape can
/// bound, so what stops them is a count of candidates considered rather than a
/// limit on which candidates are allowed.  One counter spans both, which is
/// what makes the split between them a scheduling decision rather than a
/// second number to tune against the first.
class SearchEffort {
public:
  explicit SearchEffort(size_t Limit) : Left(Limit) {}

  /// Charge \p Units candidates.  False once nothing is left, and from then
  /// on for good.
  bool spend(size_t Units = 1) {
    if (Units > Left) {
      Spent += Left;
      Left = 0;
      return false;
    }
    Left -= Units;
    Spent += Units;
    return true;
  }

  bool empty() const { return Left == 0; }
  size_t remaining() const { return Left; }
  size_t used() const { return Spent; }

private:
  size_t Left;
  size_t Spent = 0;
};

//===----------------------------------------------------------------------===//
// Checking
//===----------------------------------------------------------------------===//

enum class Verdict : uint8_t {
  /// The candidate does not behave like the body.
  Refuted,
  /// A decision procedure could not settle the pair.
  ProofIncomplete,
  /// It survived a grid it was not selected by.
  AcceptedBySamples,
  /// It also survived a caller-supplied decision procedure.
  AcceptedByVerifier,
};

/// Everything an accepted candidate has to get past.
///
/// The grid here is deliberately *not* the one the search selected against.  A
/// candidate reproduces the search grid by definition — that is what being a
/// candidate means — so checking it there again would confirm only that the
/// selection worked.  Points it was never fitted to are the only ones that can
/// say anything.
struct Checker {
  std::vector<SamplePoint> Grid;
  llvm::SmallVector<uint32_t, 8> LeafVars;

  /// Whether \p Candidate may stand in for \p Body.
  ///
  /// Sampling runs first even when a procedure is supplied, and can veto on
  /// its own: a disagreement at a concrete point refutes the pair outright, so
  /// finding one is both cheaper than a decision procedure and beyond appeal.
  Verdict check(SymContext &Ctx, SymRef Body, SymRef Candidate,
                const std::optional<SynthVerifyFn> &Verify,
                uint64_t &ProofQueries) const;
};

/// The check \p P's answers have to survive, over points drawn away from the
/// ones \p P was measured at.
Checker makeChecker(const SynthProblem &P, const SynthOptions &Opts);

//===----------------------------------------------------------------------===//
// The searches
//===----------------------------------------------------------------------===//

/// What a search came back with.
struct SearchOutcome {
  /// The accepted candidate, over the problem's leaves.  Invalid when the
  /// search found nothing.
  SymRef Candidate;
  /// Its grammar cost, in the units of \c SynthOptions::MaxCost.
  size_t Cost = 0;
  SynthEvidence Evidence = SynthEvidence::None;
  /// Something reproduced every search point and was disproved.  Worth
  /// telling apart from an incomplete proof: retrying cannot rescue it.
  bool SawRefuted = false;
  /// Something reproduced every search point, but proof ran out of resources
  /// or did not model the pair.  A stronger procedure can change this answer.
  bool SawProofIncomplete = false;
  /// The search must stop because its proof procedure returned Unknown.
  /// Trying another candidate cannot make an incomplete procedure complete.
  bool StopForProof = false;
};

/// Build candidates in ascending cost until one behaves like the body and
/// survives \p Check.
///
/// Because the enumeration is exhaustive up to the cost it reaches, the first
/// candidate it accepts is the shortest one the grammar can express — up to
/// the one liberty the cost model takes, which is that it counts the tree a
/// candidate is written as before the builders canonicalize it.
SearchOutcome enumerateShortest(SymContext &Ctx, const SynthProblem &P,
                                const SynthOptions &Opts,
                                const OpSemantics &Sem, SearchEffort &Effort,
                                const Checker &Check,
                                const std::optional<SynthVerifyFn> &Verify,
                                uint64_t &ProofQueries);

/// Mutate a straight-line program towards the body's behaviour.
///
/// Reaches sizes the enumeration cannot, and gives up the guarantee that comes
/// with exhaustiveness: what it returns is the shortest thing it *found*.
SearchOutcome searchStochastically(SymContext &Ctx, const SynthProblem &P,
                                   const SynthOptions &Opts,
                                   const OpSemantics &Sem, SearchEffort &Effort,
                                   const Checker &Check,
                                   const std::optional<SynthVerifyFn> &Verify,
                                   uint64_t &ProofQueries);

//===----------------------------------------------------------------------===//
// Shared helpers
//===----------------------------------------------------------------------===//

/// Every node reachable from \p Root, in an order where each child precedes
/// its parent.  Interning appends a node only once its operands exist, so
/// ascending index order is such an order.
std::vector<uint32_t> reachableAscending(const SymContext &Ctx, SymRef Root);

/// A random word of \p Width bits, drawn from \p State by the same generator
/// everywhere so that one seed fixes the whole run.
llvm::APInt randomWord(uint64_t &State, uint32_t Width);

/// The next value of the generator every random choice in this engine is drawn
/// from.
///
/// Written out rather than taken from a standard generator because the
/// standard library fixes the sequence of the *engines* and not of the
/// distributions layered on them, so a candidate chosen through one would
/// depend on which library the build used.  A seed is only worth stating if it
/// determines the answer.
uint64_t nextRandom(uint64_t &State);

} // namespace neverd::symbolic::synth

#endif // NEVERD_SYMBOLIC_SYNTH_SYMSYNTHDETAIL_H
