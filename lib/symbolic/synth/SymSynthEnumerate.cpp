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

#include "neverd/symbolic/Limits.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace neverd::symbolic::synth {

namespace {

constexpr size_t HashWords = limits::symbolic::kSynthBehaviourHashWords;
constexpr size_t HashBlocks = limits::symbolic::kSynthBehaviourHashBlocks;
static_assert(HashWords % HashBlocks == 0 && HashBlocks > 1);
constexpr size_t HashBlockLength = HashWords / HashBlocks;

size_t hashBlockStart(size_t Words, size_t Block) {
  return (Words - HashBlockLength) * Block / (HashBlocks - 1);
}

// Store the sampled points first in a native-word column. All operators are
// pointwise, so packing once at entry preserves the complete behaviour and
// avoids gathering the same four blocks for every candidate in the search.
llvm::SmallVector<size_t, 32> packedPointOrder(size_t Points,
                                               size_t ValueWords) {
  llvm::SmallVector<size_t, 32> Order;
  if (ValueWords != 1 || Points <= HashWords)
    return Order;
  Order.reserve(Points);
  size_t Next = 0;
  for (size_t Block = 0; Block != HashBlocks; ++Block) {
    const size_t Start = hashBlockStart(Points, Block);
    for (size_t I = Start; I != Start + HashBlockLength; ++I)
      Order.push_back(I);
  }
  for (size_t Block = 0; Block != HashBlocks; ++Block) {
    const size_t Start = hashBlockStart(Points, Block);
    for (; Next != Start; ++Next)
      Order.push_back(Next);
    Next += HashBlockLength;
  }
  return Order;
}

template <typename Word>
uint64_t hashBehaviour(llvm::ArrayRef<Word> Words, bool Packed) {
  std::array<Word, HashWords> Sample;
  if (Packed)
    Words = Words.take_front(HashWords);
  else if (Words.size() > HashWords) {
    for (size_t I = 0; I != HashBlocks; ++I)
      std::copy_n(Words.data() + hashBlockStart(Words.size(), I),
                  HashBlockLength, Sample.data() + I * HashBlockLength);
    Words = Sample;
  }
  // Reserve the sentinel keys of the behaviour index.
  return llvm::xxh3_64bits(reinterpret_cast<const uint8_t *>(Words.data()),
                           Words.size() * sizeof(Word)) >>
         1;
}

/// The behaviour array this thread keeps between searches.  Growing a new one
/// for each search copies it repeatedly and faults in fresh pages.
struct RetainedColumns {
  std::variant<std::vector<uint32_t>, std::vector<uint64_t>> Words;
  size_t WordsPerRow = 0, Rows = 0;
};
RetainedColumns &retainedBehaviours() {
  static thread_local RetainedColumns Words;
  return Words;
}

template <typename Word>
void appendSignature(const Signature &Sig, std::vector<Word> &Words,
                     llvm::ArrayRef<size_t> Order) {
  if (!Order.empty()) {
    for (size_t I : Order)
      Words.push_back(static_cast<Word>(Sig[I].getZExtValue()));
    return;
  }
  for (const llvm::APInt &Value : Sig) {
    if constexpr (sizeof(Word) < sizeof(uint64_t))
      Words.push_back(static_cast<Word>(Value.getZExtValue()));
    else
      Words.insert(Words.end(), Value.getRawData(),
                   Value.getRawData() + Value.getNumWords());
  }
}

/// The terms built so far, one per distinct behaviour, grouped by cost.
///
/// Lookup goes through a hash of the behaviour to the terms carrying that
/// hash, with the candidates in a bucket compared outright.  Storing the hash
/// rather than the behaviour as the key keeps one copy of each signature —
/// they are the bulk of the memory here — and matches how the expression
/// context interns its own nodes.  Behaviours share one flat array, since
/// measuring a candidate reads its operands' at every point.
template <typename Word> class TermBank {
  using Behaviour = llvm::ArrayRef<Word>;

public:
  TermBank(SymContext &Ctx, size_t BehaviourWords, bool Packed)
      : Ctx(Ctx), BehaviourWords(BehaviourWords), Packed(Packed) {
    auto &Retained = retainedBehaviours();
    if (auto *Words = std::get_if<std::vector<Word>>(&Retained.Words)) {
      Flat.swap(*Words);
      if (Retained.WordsPerRow == BehaviourWords)
        PriorRows = Retained.Rows;
    }
  }

  ~TermBank() {
    constexpr size_t Limit = limits::symbolic::kSynthRetainedBehaviourWords *
                             sizeof(uint64_t) / sizeof(Word);
    if (Flat.capacity() <= Limit) {
      auto &Retained = retainedBehaviours();
      Retained.Words = std::move(Flat);
      Retained.WordsPerRow = BehaviourWords;
      Retained.Rows = size();
    }
  }

  // Reserve the index only after the leaf and fitting stages need enumeration.
  // Completed rows from the same column layout bound the hint, and each new
  // row still needs at least one unit of this search's remaining work.
  void prepareEnumeration(size_t Work) {
    const size_t Rows = std::min(PriorRows, Work);
    ByHash.reserve(Rows);
    Previous.reserve(Rows);
    Pending.reserve(Rows);
  }

  TermBank(const TermBank &) = delete;
  TermBank &operator=(const TermBank &) = delete;

  /// Record \p Node under \p Words at \p Cost, unless something already behaves
  /// that way.  True when it was recorded.
  bool insert(SymRef Node, Behaviour Words, size_t Cost) {
    if (!remember(Words, hashBehaviour(Words, Packed)))
      return false;
    llvm::copy(Words, nextBehaviour().begin());
    addMaterialized(Node, Cost);
    return true;
  }

  /// Record the recipe for a new behaviour without interning a node.
  void defer(GramOp Op, llvm::ArrayRef<size_t> Operands, size_t Cost) {
    Pending.push_back({Op, Operands.front(), Operands.back()});
    add(Cost);
  }

  // Only a target match needs an expression. Replay earlier representatives
  // in insertion order so canonical builders and proof callbacks see the
  // same nodes they would have seen during eager enumeration.
  SymRef materialize(GramOp Op, llvm::ArrayRef<size_t> Operands) {
    auto build = [&](GramOp Kind, size_t Left, size_t Right) {
      const SymRef Args[] = {Nodes[Left], Nodes[Right]};
      return buildGramOp(
          Ctx, Kind,
          llvm::ArrayRef(Args).take_front(isUnaryGramOp(Kind) ? 1 : 2));
    };
    for (const auto &R : Pending)
      Nodes.push_back(build(R.Op, R.Left, R.Right));
    Pending.clear();
    return build(Op, Operands.front(), Operands.back());
  }

  void addMaterialized(SymRef Node, size_t Cost) {
    Nodes.push_back(Node);
    add(Cost);
  }

  // Heads and links are one-based so zero terminates a collision chain.
  // Reserve the next index only after comparing the complete signatures.
  bool remember(Behaviour Words, uint64_t Hash) {
    auto &Head = ByHash[Hash];
    for (size_t I = Head; I; I = Previous[I - 1])
      if (behaviour(I - 1) == Words)
        return false;
    Previous.push_back(Head);
    Head = Previous.size();
    return true;
  }

  // An uncommitted signature is the next row in the bank. A duplicate
  // reuses that space. Growing it before operand lookup keeps their pointers
  // valid for the whole measurement, even across a reallocation.
  llvm::MutableArrayRef<Word> nextBehaviour() {
    const size_t Begin = (Nodes.size() + Pending.size()) * BehaviourWords;
    const size_t End = Begin + BehaviourWords;
    if (End > Flat.size())
      Flat.resize(End);
    return llvm::MutableArrayRef(Flat).slice(Begin, BehaviourWords);
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

  size_t size() const { return Nodes.size() + Pending.size(); }
  size_t numCosts() const { return Costs.size(); }
  size_t costAt(size_t I) const { return Costs[I]; }

private:
  void add(size_t Cost) {
    const size_t Index = Nodes.size() + Pending.size() - 1;
    std::vector<size_t> &Tier = ByCost[Cost];
    if (Tier.empty())
      Costs.push_back(Cost);
    Tier.push_back(Index);
  }

  struct Recipe {
    GramOp Op;
    size_t Left, Right;
  };
  SymContext &Ctx;
  std::vector<Recipe> Pending;
  size_t BehaviourWords;
  bool Packed;
  std::vector<SymRef> Nodes;
  size_t PriorRows = 0;
  std::vector<Word> Flat;
  /// Term indices by cost, in the order they were built, so that the search
  /// visits them the same way on every run.
  std::unordered_map<size_t, std::vector<size_t>> ByCost;
  std::vector<size_t> Costs;
  llvm::DenseMap<uint64_t, size_t> ByHash;
  std::vector<size_t> Previous;
};

template <typename Word>
SearchOutcome enumerateColumns(SymContext &Ctx, const SynthProblem &P,
                               const SynthOptions &Opts, const OpSemantics &Sem,
                               SearchEffort &Effort, Checker &Check,
                               const std::optional<SynthVerifyFn> &Verify,
                               uint64_t &ProofQueries) {
  using Behaviour = llvm::ArrayRef<Word>;
  SearchOutcome Result;

  if (Opts.MaxCost == 0)
    return Result;

  const size_t Points = P.Grid.size();
  const size_t ValueWords = llvm::APInt::getNumWords(P.Width);
  const auto Order = packedPointOrder(Points, ValueWords);
  const bool Packed = !Order.empty();
  TermBank<Word> Bank(Ctx, Points * ValueWords, Packed);

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
  std::vector<Word> Target, Measured;
  constexpr size_t Absent = std::numeric_limits<size_t>::max();
  size_t Zero = Absent, One = Absent, Ones = Absent, TargetIndex = Absent;
  appendSignature(P.Target, Target, Order);
  for (size_t I = 0; I < Terminals.size(); ++I) {
    Measured.clear();
    appendSignature(TerminalSigs[I], Measured, Order);
    const bool Matches = Behaviour(Measured) == Behaviour(Target);
    const size_t Index = Bank.size();
    if (Bank.insert(Terminals[I], Measured, 1)) {
      if (Matches)
        TargetIndex = Index;
      if (Ctx.isConst(Terminals[I])) {
        const auto K = Ctx.constValue(Terminals[I]);
        if (K.isZero())
          Zero = Index;
        if (K.isOne())
          One = Index;
        if (K.isAllOnes())
          Ones = Index;
      }
    }
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

  Bank.prepareEnumeration(Effort.remaining());

  // Literal terms alone, for the shifts: an amount that is itself computed
  // multiplies the branching factor of every shift by the whole pool, for a
  // shape that hardly occurs in what this engine is aimed at.  Read back out
  // of the bank rather than off the terminal list, because a terminal that
  // behaved like an earlier one was never recorded and the two orderings would
  // then disagree.
  llvm::SmallVector<size_t, 8> LiteralTerms;
  llvm::SmallVector<bool, 16> Overshift(Bank.size());
  for (size_t I : Bank.atCost(1))
    if (Ctx.isConst(Bank.node(I))) {
      LiteralTerms.push_back(I);
      Overshift[I] = Ctx.constValue(Bank.node(I)).uge(P.Width);
    }

  llvm::SmallVector<llvm::APInt, 2> Args;

  // Read a candidate's behaviour off its operands'.  Nearly every search is at
  // a machine width, where a column of words needs no per-point interpretation.
  auto measure = [&](GramOp Op, llvm::ArrayRef<size_t> Operands,
                     llvm::MutableArrayRef<Word> Out) {
    if constexpr (std::is_same_v<Word, uint64_t>) {
      if (ValueWords > 1) {
        Args.resize(Operands.size());
        for (size_t Pt = 0; Pt < Points; ++Pt) {
          for (size_t K = 0; K < Operands.size(); ++K)
            Args[K] = llvm::APInt(
                P.Width,
                Bank.behaviour(Operands[K]).slice(Pt * ValueWords, ValueWords));
          const llvm::APInt Value = Sem.apply(Op, Args);
          std::copy_n(Value.getRawData(), ValueWords,
                      Out.data() + Pt * ValueWords);
        }
        return;
      }
    }
    Sem.applyColumn(Op, Bank.behaviour(Operands[0]).data(),
                    Operands.size() > 1 ? Bank.behaviour(Operands[1]).data()
                                        : nullptr,
                    Out.data(), Points);
  };

  // Consider one candidate: read its behaviour off its operands', and build it
  // only if that behaviour is worth having.  Returns false to say the caller
  // should stop, either because the budget is gone or because an answer was
  // accepted.
  const uint64_t TargetHash = hashBehaviour(Behaviour(Target), Packed);
  auto consider = [&](GramOp Op, llvm::ArrayRef<size_t> Operands, size_t Cost) {
    if (!Effort.spend())
      return false;

    // These operations reproduce a column already in the bank. The ordinary
    // offer still runs if that column matched the target: another recipe can
    // refute a sample collision or satisfy the verifier.
    const size_t A = Operands.front(), B = Operands.back();
    size_t Known = Absent;
    switch (Op) {
    case GramOp::Add:
    case GramOp::Xor:
      if (A == Zero || B == Zero)
        Known = A == Zero ? B : A;
      else if (Op == GramOp::Xor && A == B)
        Known = Zero;
      break;
    case GramOp::Sub:
      Known = B == Zero ? A : (A == B ? Zero : Absent);
      break;
    case GramOp::Mul:
      if (A == Zero || B == Zero)
        Known = Zero;
      else if (A == One || B == One)
        Known = A == One ? B : A;
      break;
    case GramOp::And:
    case GramOp::Or: {
      const size_t Identity = Op == GramOp::And ? Ones : Zero;
      const size_t Absorber = Op == GramOp::And ? Zero : Ones;
      if (A == B || B == Identity || A == Absorber)
        Known = A;
      else if (A == Identity || B == Absorber)
        Known = B;
      break;
    }
    case GramOp::Shl:
    case GramOp::LShr:
    case GramOp::AShr:
      if (A == Zero || B == Zero || (Op == GramOp::AShr && A == Ones))
        Known = A;
      else if (Op != GramOp::AShr && B < Overshift.size() && Overshift[B])
        Known = Zero;
      break;
    case GramOp::Neg:
      if (A == Zero)
        Known = A;
      break;
    case GramOp::Not:
      Known = A == Zero ? Ones : (A == Ones ? Zero : Absent);
      break;
    }
    if (Known != Absent)
      return Known != TargetIndex ||
             offer(Bank.materialize(Op, Operands), Cost);

    const auto Column = Bank.nextBehaviour();
    measure(Op, Operands, Column);

    const uint64_t Hash = hashBehaviour(Behaviour(Column), Packed);
    const bool Matches =
        Hash == TargetHash && Behaviour(Column) == Behaviour(Target);
    const bool Fresh = Bank.remember(Column, Hash);
    if (!Matches) {
      if (Fresh)
        Bank.defer(Op, Operands, Cost);
      return true;
    }
    const SymRef Node = Bank.materialize(Op, Operands);
    if (Fresh) {
      TargetIndex = Bank.size();
      Bank.addMaterialized(Node, Cost);
    }
    return offer(Node, Cost);
  };

  // Operands come only from cheaper tiers. Counting these same groups lets
  // the last observable tier skip building signatures no later term can use.
  auto visitGroups = [&](size_t Cost, auto &&Visit) {
    for (unsigned I = 0; I < kNumGramOps; ++I) {
      const GramOp Op = GramOp(I);
      if (isUnaryGramOp(Op) &&
          !Visit(Op, Bank.atCost(Cost - 1), llvm::ArrayRef<size_t>{}, false))
        return false;
    }
    for (unsigned I = 0; I < kNumGramOps; ++I) {
      const GramOp Op = GramOp(I);
      if (isUnaryGramOp(Op))
        continue;
      const bool FixedAmount = isShiftGramOp(Op) && !Opts.AllowVariableShifts;
      const bool Symmetric = isCommutativeGramOp(Op);
      const size_t NumCosts = Bank.numCosts();
      for (size_t J = 0; J < NumCosts; ++J) {
        const size_t Left = Bank.costAt(J);
        if (Left >= Cost - 1)
          break;
        const size_t Right = Cost - 1 - Left;
        if ((FixedAmount && Right != 1) || (Symmetric && Left > Right))
          continue;
        const auto Rhs = FixedAmount ? llvm::ArrayRef<size_t>(LiteralTerms)
                                     : Bank.atCost(Right);
        if (!Visit(Op, Bank.atCost(Left), Rhs, Symmetric && Left == Right))
          return false;
      }
    }
    return true;
  };

  auto visitTier = [&](size_t Cost, auto &&Visit) {
    return visitGroups(Cost, [&](GramOp Op, llvm::ArrayRef<size_t> Lhs,
                                 llvm::ArrayRef<size_t> Rhs, bool Triangular) {
      if (isUnaryGramOp(Op)) {
        for (size_t A : Lhs)
          if (!Visit(Op, llvm::ArrayRef(A), Cost))
            return false;
      } else {
        for (size_t I = 0; I < Lhs.size(); ++I)
          for (size_t B : Rhs.drop_front(Triangular ? I : 0)) {
            const size_t Args[] = {Lhs[I], B};
            if (!Visit(Op, Args, Cost))
              return false;
          }
      }
      return true;
    });
  };

  auto isLastTier = [&](size_t Cost, size_t Left) {
    if (Cost == Opts.MaxCost)
      return true;
    return !visitGroups(Cost, [&](GramOp Op, llvm::ArrayRef<size_t> Lhs,
                                  llvm::ArrayRef<size_t> Rhs, bool Triangular) {
      size_t Rows = Lhs.size(), Cols = isUnaryGramOp(Op) ? 1 : Rhs.size();
      if (Triangular) {
        // N * (N+1) / 2 pairs, halving the even factor first.
        Cols = (Rows + 1) / (Rows % 2 ? 2 : 1);
        if (Rows % 2 == 0)
          Rows /= 2;
      }
      if (Rows && Cols > (Left - 1) / Rows)
        return false;
      Left -= Rows * Cols;
      return true;
    });
  };

  for (size_t Cost = 2;; ++Cost) {
    const size_t WorkBeforeTier = Effort.used();
    const size_t Left = Effort.remaining();
    if (!Left)
      return Result;
    if (ValueWords == 1 && isLastTier(Cost, Left)) {
      // A mismatch at one measured point rules out a match of the complete
      // grid. A surviving match replays this tier in the ordinary order,
      // preserving even the recipes and proof callbacks before that match.
      size_t Visited = 0;
      bool Match = false;
      llvm::SmallVector<Word, 16> Inputs, Results;
      const size_t Pt = Packed ? HashWords - 1 : Points - 1;
      visitGroups(Cost, [&](GramOp Op, llvm::ArrayRef<size_t> Lhs,
                            llvm::ArrayRef<size_t> Rhs, bool Triangular) {
        const bool Unary = isUnaryGramOp(Op);
        const auto Indices = Unary ? Lhs : Rhs;
        if (Lhs.empty() || Indices.empty())
          return true;
        // Read the probe point once per operand in this group. Evaluate a
        // whole row against the fixed left operand with the same word
        // semantics as a full signature. A work cutoff limits the row itself.
        Inputs.clear();
        for (size_t I : Indices.take_front(Left - Visited))
          Inputs.push_back(Bank.behaviour(I)[Pt]);
        Results.resize_for_overwrite(Inputs.size());
        auto probeRow = [&](size_t A, size_t Start) {
          const size_t Count = std::min(Inputs.size() - Start, Left - Visited);
          if (Unary)
            Sem.applyColumn<Word>(Op, Inputs.data(), nullptr, Results.data(),
                                  Count);
          else {
            const Word X = Bank.behaviour(A)[Pt];
            Sem.applyColumn<Word, true>(Op, &X, Inputs.data() + Start,
                                        Results.data(), Count);
          }
          Visited += Count;
          for (size_t I = 0; I != Count; ++I) {
            if (Results[I] != Target[Pt])
              continue;
            const size_t Other = Indices[Start + I];
            const size_t Pair[] = {Unary ? Other : A, Other};
            measure(Op, llvm::ArrayRef(Pair).take_front(Unary ? 1 : 2),
                    Measured);
            if (Behaviour(Measured) == Behaviour(Target)) {
              Match = true;
              return false;
            }
          }
          return Visited != Left;
        };
        if (Unary)
          return probeRow(0, 0);
        for (size_t J = 0; J != Lhs.size(); ++J)
          if (!probeRow(Lhs[J], Triangular ? J : 0))
            return false;
        return true;
      });
      if (!Match) {
        Effort.spend(std::max<size_t>(Visited, 1));
        return Result;
      }
    }
    if (!visitTier(Cost, consider))
      return Result;

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

} // namespace

SearchOutcome enumerateShortest(SymContext &Ctx, const SynthProblem &P,
                                const SynthOptions &Opts,
                                const OpSemantics &Sem, SearchEffort &Effort,
                                Checker &Check,
                                const std::optional<SynthVerifyFn> &Verify,
                                uint64_t &ProofQueries) {
  // Every measured value fits this lane. The grid, work, offer order and
  // independent verifier are shared with the arbitrary-width search.
  if (P.Width <= 32)
    return enumerateColumns<uint32_t>(Ctx, P, Opts, Sem, Effort, Check, Verify,
                                      ProofQueries);
  return enumerateColumns<uint64_t>(Ctx, P, Opts, Sem, Effort, Check, Verify,
                                    ProofQueries);
}

} // namespace neverd::symbolic::synth
