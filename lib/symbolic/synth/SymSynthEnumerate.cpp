//===- SymSynthEnumerate.cpp - Shortest-first candidate enumeration -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the exhaustive half of the search: build every expression the
/// grammar can write, cheapest first, and stop at the first one that behaves
/// like the body.
///
/// Written naively that is hopeless.  With t terminals and k operators the
/// number of trees of cost c grows like (k * t^2)^(c/2), and the interesting
/// costs start around five.  What makes it finish is that the search does not
/// care which expressions exist, only which *behaviours* do — and behaviours
/// are far scarcer than expressions.  `x + y`, `(x ^ y) + 2 * (x & y)` and
/// `(x | y) + (x & y)` are one behaviour written three ways, and anything
/// built on top of one of them could have been built on top of any other.  So
/// the enumeration keeps a single representative of each behaviour it has seen
/// and discards the rest, which collapses the base of that exponential to the
/// number of distinct functions reachable at each cost.  That is the whole
/// trick, and it is worth roughly the difference between a search that ends
/// and one that does not.
///
/// Two consequences are worth stating because they shape the code.
///
/// A behaviour is a finite reading, so two genuinely different functions can
/// share one and the enumeration will keep only the first.  For an ordinary
/// building block that costs nothing — either representative composes the same
/// way, up to the coincidence.  For the *answer* it would cost everything: a
/// coincidence arriving before the real match would take the slot and the real
/// match would never be offered.  So a candidate that behaves like the body is
/// always offered to the check, whether or not its behaviour is new.
///
/// And the builders canonicalize, so the node a candidate turns into may be
/// smaller than the tree that produced it.  Cost therefore orders the search
/// but does not decide anything: what an accepted answer is worth is settled
/// afterwards, against the expression it would replace.
///
//===----------------------------------------------------------------------===//

#include "SymSynthDetail.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd::symbolic::synth {

namespace {

/// The raw words of a behaviour, one after another.
using Behaviour = llvm::ArrayRef<uint64_t>;

uint64_t hashBehaviour(Behaviour Words) {
  uint64_t Hash = 0xCBF29CE484222325ull;
  for (const uint64_t Word : Words) {
    Hash ^= Word;
    Hash *= 0x100000001B3ull;
  }
  return Hash;
}

void appendSignature(const Signature &Sig, std::vector<uint64_t> &Words) {
  for (const llvm::APInt &Value : Sig)
    Words.insert(Words.end(), Value.getRawData(),
                 Value.getRawData() + Value.getNumWords());
}

/// The terms built so far, one per distinct behaviour, grouped by cost.
///
/// Lookup goes through a hash of the behaviour to the terms carrying that
/// hash, with the candidates in a bucket compared outright.  Storing the hash
/// rather than the behaviour as the key keeps one copy of each signature —
/// they are the bulk of the memory here — and matches how the expression
/// context interns its own nodes.  Behaviours share one flat array, since
/// measuring a candidate reads its operands' at every point.
class TermBank {
public:
  explicit TermBank(size_t BehaviourWords) : BehaviourWords(BehaviourWords) {}

  /// Record \p Node under \p Words at \p Cost, unless something already behaves
  /// that way.  True when it was recorded.
  bool insert(SymRef Node, Behaviour Words, size_t Cost) {
    const uint64_t Hash = hashBehaviour(Words);
    llvm::SmallVector<size_t, 2> &Bucket = ByHash[Hash];
    for (size_t I : Bucket)
      if (behaviour(I) == Words)
        return false;

    const size_t Index = Nodes.size();
    Nodes.push_back(Node);
    Flat.insert(Flat.end(), Words.begin(), Words.end());
    Bucket.push_back(Index);
    std::vector<size_t> &Tier = ByCost[Cost];
    if (Tier.empty())
      Costs.push_back(Cost);
    Tier.push_back(Index);
    return true;
  }

  bool contains(Behaviour Words) const {
    auto It = ByHash.find(hashBehaviour(Words));
    if (It == ByHash.end())
      return false;
    for (size_t I : It->second)
      if (behaviour(I) == Words)
        return true;
    return false;
  }

  SymRef node(size_t I) const { return Nodes[I]; }
  Behaviour behaviour(size_t I) const {
    return Behaviour(Flat.data() + I * BehaviourWords, BehaviourWords);
  }

  llvm::ArrayRef<size_t> atCost(size_t Cost) const {
    auto It = ByCost.find(Cost);
    return It == ByCost.end() ? llvm::ArrayRef<size_t>()
                              : llvm::ArrayRef<size_t>(It->second);
  }

  size_t numCosts() const { return Costs.size(); }
  size_t costAt(size_t I) const { return Costs[I]; }

private:
  size_t BehaviourWords;
  std::vector<SymRef> Nodes;
  std::vector<uint64_t> Flat;
  /// Term indices by cost, in the order they were built, so that the search
  /// visits them the same way on every run.
  std::unordered_map<size_t, std::vector<size_t>> ByCost;
  std::vector<size_t> Costs;
  std::unordered_map<uint64_t, llvm::SmallVector<size_t, 2>> ByHash;
};

} // namespace

SearchOutcome enumerateShortest(SymContext &Ctx, const SynthProblem &P,
                                const SynthOptions &Opts,
                                const OpSemantics &Sem, SearchEffort &Effort,
                                Checker &Check,
                                const std::optional<SynthVerifyFn> &Verify,
                                uint64_t &ProofQueries) {
  SearchOutcome Result;

  if (Opts.MaxCost == 0)
    return Result;

  const size_t Points = P.Grid.size();
  const size_t ValueWords = llvm::APInt::getNumWords(P.Width);
  TermBank Bank(Points * ValueWords);

  // Charge a grid's worth of work per offer, including candidates whose
  // refutation is cached.  This preserves the work limit and search order
  // when many expressions canonicalize to the same rejected candidate.
  const size_t CheckCost = std::max<size_t>(Check.Grid.size(), 1);

  // Offer a term to the check and, when it survives, end the search.  Returns
  // false to say the caller should stop.
  auto offer = [&](SymRef Node, size_t Cost) {
    if (!Effort.spend(CheckCost))
      return false;
    switch (Check.check(Node, Verify, ProofQueries)) {
    case Verdict::Refuted:
      Result.SawRefuted = true;
      return true;
    case Verdict::ProofIncomplete:
      Result.SawProofIncomplete = true;
      Result.StopForProof = true;
      return false;
    case Verdict::AcceptedBySamples:
      Result.Evidence = SynthEvidence::Samples;
      break;
    case Verdict::AcceptedByVerifier:
      Result.Evidence = SynthEvidence::Verifier;
      break;
    }
    Result.Candidate = Node;
    Result.Cost = Cost;
    return false;
  };

  // Cost one: the leaves and the literals.  A body that behaves like one of
  // them is the shortest answer there is, so they are checked like anything
  // else rather than assumed uninteresting.
  const llvm::SmallVector<SymRef, 16> Terminals = terminalsOf(P);
  const std::vector<Signature> TerminalSigs = terminalSignatures(Ctx, P);
  std::vector<uint64_t> Target, Measured;
  appendSignature(P.Target, Target);
  for (size_t I = 0; I < Terminals.size(); ++I) {
    Measured.clear();
    appendSignature(TerminalSigs[I], Measured);
    const bool Matches = Behaviour(Measured) == Behaviour(Target);
    Bank.insert(Terminals[I], Measured, 1);
    if (Matches && !offer(Terminals[I], 1))
      return Result;
  }
  if (Opts.MaxCost == 1)
    return Result;

  // Whole coefficient vectors are cheap to read and expensive to enumerate.
  // Keep terminal proofs first, and share their refutation cache with this
  // optional proposal so a sampling collision is never proved twice.
  if (Opts.UseAffineFitting) {
    SearchOutcome Affine =
        fitAffine(Ctx, P, Opts, Effort, Check, Verify, ProofQueries);
    Affine.SawRefuted |= Result.SawRefuted;
    if (Affine.Candidate.isValid() || Affine.StopForProof)
      return Affine;
    Result.SawRefuted = Affine.SawRefuted;
    SearchOutcome Typed =
        fitTypedAffine(Ctx, P, Opts, Effort, Verify, ProofQueries);
    Typed.SawRefuted |= Result.SawRefuted;
    if (Typed.Candidate.isValid() || Typed.StopForProof)
      return Typed;
    Result.SawRefuted = Typed.SawRefuted;
    Result.SawBudgetExhausted = Typed.SawBudgetExhausted;
  }

  if (Opts.UseQuadraticFitting) {
    SearchOutcome Quadratic =
        fitQuadratic(Ctx, P, Opts, Effort, Check, Verify, ProofQueries);
    Quadratic.SawRefuted |= Result.SawRefuted;
    Quadratic.SawBudgetExhausted |= Result.SawBudgetExhausted;
    if (Quadratic.Candidate.isValid() || Quadratic.StopForProof)
      return Quadratic;
    Result.SawRefuted = Quadratic.SawRefuted;
  }

  // Literal terms alone, for the shifts: an amount that is itself computed
  // multiplies the branching factor of every shift by the whole pool, for a
  // shape that hardly occurs in what this engine is aimed at.  Read back out
  // of the bank rather than off the terminal list, because a terminal that
  // behaved like an earlier one was never recorded and the two orderings would
  // then disagree.
  llvm::SmallVector<size_t, 8> LiteralTerms;
  for (size_t I : Bank.atCost(1))
    if (Ctx.isConst(Bank.node(I)))
      LiteralTerms.push_back(I);

  Measured.resize(Points * ValueWords);
  llvm::SmallVector<llvm::APInt, 2> Args;
  llvm::SmallVector<SymRef, 2> ArgNodes;

  // Read a candidate's behaviour off its operands'.  Nearly every search is at
  // a machine width, where a column of words needs no per-point interpretation.
  auto measure = [&](GramOp Op, llvm::ArrayRef<size_t> Operands) {
    if (ValueWords == 1) {
      Sem.applyColumn(Op, Bank.behaviour(Operands[0]).data(),
                      Operands.size() > 1 ? Bank.behaviour(Operands[1]).data()
                                          : nullptr,
                      Measured.data(), Points);
      return;
    }
    Args.resize(Operands.size());
    for (size_t Pt = 0; Pt < Points; ++Pt) {
      for (size_t K = 0; K < Operands.size(); ++K)
        Args[K] = llvm::APInt(
            P.Width,
            Bank.behaviour(Operands[K]).slice(Pt * ValueWords, ValueWords));
      const llvm::APInt Value = Sem.apply(Op, Args);
      std::copy_n(Value.getRawData(), ValueWords,
                  Measured.data() + Pt * ValueWords);
    }
  };

  // Consider one candidate: read its behaviour off its operands', and build it
  // only if that behaviour is worth having.  Returns false to say the caller
  // should stop, either because the budget is gone or because an answer was
  // accepted.
  auto consider = [&](GramOp Op, llvm::ArrayRef<size_t> Operands, size_t Cost) {
    if (!Effort.spend())
      return false;

    measure(Op, Operands);

    const bool Matches = Behaviour(Measured) == Behaviour(Target);
    const bool Fresh = !Bank.contains(Measured);
    if (!Matches && !Fresh)
      return true;

    ArgNodes.clear();
    for (size_t I : Operands)
      ArgNodes.push_back(Bank.node(I));
    SymRef Node = buildGramOp(Ctx, Op, ArgNodes);

    if (Fresh)
      Bank.insert(Node, Measured, Cost);
    return !Matches || offer(Node, Cost);
  };

  for (size_t Cost = 2;; ++Cost) {
    const size_t WorkBeforeTier = Effort.used();
    for (unsigned OpIndex = 0; OpIndex < kNumGramOps; ++OpIndex) {
      const GramOp Op = GramOp(OpIndex);
      if (!isUnaryGramOp(Op))
        continue;
      for (size_t I : Bank.atCost(Cost - 1)) {
        size_t Operands[] = {I};
        if (!consider(Op, Operands, Cost))
          return Result;
      }
    }

    for (unsigned OpIndex = 0; OpIndex < kNumGramOps; ++OpIndex) {
      const GramOp Op = GramOp(OpIndex);
      if (isUnaryGramOp(Op))
        continue;
      const bool FixedAmount = isShiftGramOp(Op) && !Opts.AllowVariableShifts;
      const bool Symmetric = isCommutativeGramOp(Op);

      // A binary node costs one plus its operands, so every split of the
      // remaining cost between them is one way to reach this size.
      const size_t NumOperandCosts = Bank.numCosts();
      for (size_t CostIndex = 0; CostIndex < NumOperandCosts; ++CostIndex) {
        const size_t Left = Bank.costAt(CostIndex);
        if (Left >= Cost - 1)
          break;
        const size_t Right = Cost - 1 - Left;
        if (FixedAmount && Right != 1)
          continue;
        // Swapping the operands of a commutative operator interns the same
        // node, so only one order of each pair is built.
        if (Symmetric && Left > Right)
          continue;

        llvm::ArrayRef<size_t> Lhs = Bank.atCost(Left);
        llvm::ArrayRef<size_t> Rhs = FixedAmount
                                         ? llvm::ArrayRef<size_t>(LiteralTerms)
                                         : Bank.atCost(Right);
        for (size_t A = 0; A < Lhs.size(); ++A)
          for (size_t B = 0; B < Rhs.size(); ++B) {
            if (Symmetric && Left == Right && Rhs[B] < Lhs[A])
              continue;
            size_t Operands[] = {Lhs[A], Rhs[B]};
            if (!consider(Op, Operands, Cost))
              return Result;
          }
      }
    }

    // A tier with no candidates must still be bounded by the caller's work
    // budget.  Otherwise a very large MaxCost can spend unbounded time walking
    // empty cost values after the finite behaviour space is saturated.
    if (Effort.used() == WorkBeforeTier && !Effort.spend())
      return Result;
    if (Cost == Opts.MaxCost)
      break;
  }
  return Result;
}

} // namespace neverd::symbolic::synth
