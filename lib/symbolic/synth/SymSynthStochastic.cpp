//===- SymSynthStochastic.cpp - Sampling the space of candidates ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the search that takes over when enumerating every candidate up
/// to the size of the answer is no longer affordable.
///
/// The enumeration's guarantee — the first answer it finds is the shortest one
/// there is — is bought by building the entire space below that size.  Past
/// six or seven operations that space stops fitting in any budget, and the
/// guarantee stops being worth its price.  What replaces it is a walk: hold
/// one candidate, change a little of it, and keep the change or throw it away.
///
/// The walk needs somewhere to walk *to*, which is where a right-or-wrong
/// score fails.  Under it every candidate that is not the answer scores the
/// same, the landscape is flat, and mutation is a lottery.  Counting agreeing
/// *bits* instead makes it a slope: a candidate producing three quarters of
/// the output bits correctly is genuinely nearer the answer than one producing
/// half, and the mutations that raise the count point the way.  That is the
/// whole reason a walk can reach in thousands of steps what the enumeration
/// would need billions of candidates to build up to.
///
/// A slope climbed only upwards stops at the first hilltop, and this landscape
/// has many, so a worse candidate is sometimes kept anyway — less often the
/// worse it is.  The acceptance rule is written in integers rather than as an
/// exponential of a floating-point ratio, because a seed is only worth stating
/// if it decides the answer, and a library's floating-point details are not
/// something a seed controls.
///
/// Candidates are held as a straight-line program rather than as a tree: a
/// list of slots, each applying one operator to earlier slots or to terminals,
/// with one slot named as the result.  A tree makes every mutation a question
/// of where to cut and how to graft; a program makes it a question of which
/// small integer to change.  Slots the result does not depend on cost nothing,
/// because they are never built, and they earn their keep as material a later
/// mutation can reach for.
///
//===----------------------------------------------------------------------===//

#include "SymSynthDetail.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::symbolic::synth {

namespace {

/// A candidate as a list of operations over a flat value space: an index below
/// the terminal count names a terminal, and anything above it names the result
/// of that many slots in.  A slot may only read values that come before it, so
/// the program is acyclic by construction and no mutation can make it
/// otherwise.
struct Program {
  struct Slot {
    GramOp Op = GramOp::Add;
    size_t A = 0;
    size_t B = 0;
  };
  llvm::SmallVector<Slot, 8> Slots;
  size_t Result = 0;
};

/// What the options let a sampled program contain.
///
/// The enumeration keeps a shift's amount to a literal by construction: it
/// never builds any other kind.  The sampler arrives at its candidates by
/// changing one field of one slot at a time, so nothing about the way it moves
/// enforces anything, and the same options would otherwise describe two
/// different search spaces depending on which half of the engine answered.
///
/// A move that leaves the shape is repaired rather than rejected.  Rejecting
/// would make the walk's step distribution depend on which moves happen to
/// land inside, which is a bias with no argument behind it.
class ProgramShape {
public:
  ProgramShape(const SynthProblem &P, const SynthOptions &Opts)
      : FirstLiteral(P.Leaves.size()),
        Base(P.Leaves.size() + P.Constants.size()),
        FixedAmounts(!Opts.AllowVariableShifts) {
    for (unsigned I = 0; I < kNumGramOps; ++I) {
      const GramOp Op = GramOp(I);
      // A shift needs somewhere for its amount to come from.  With a computed
      // amount ruled out and no literal left to use instead there is nowhere,
      // so the operator drops out of the alphabet rather than being admitted
      // in a shape it is not allowed to have.
      if (FixedAmounts && isShiftGramOp(Op) && FirstLiteral == Base)
        continue;
      Ops.push_back(Op);
    }
  }

  size_t base() const { return Base; }
  llvm::ArrayRef<GramOp> ops() const { return Ops; }
  GramOp pickOp(uint64_t &State) const {
    return Ops[nextRandom(State) % Ops.size()];
  }

  /// Half-open range of values \p Op's second operand may take in a slot that
  /// can see \p Reach values below it.
  std::pair<size_t, size_t> secondOperand(GramOp Op, size_t Reach) const {
    if (isUnaryGramOp(Op))
      return {0, 1};
    if (FixedAmounts && isShiftGramOp(Op))
      return {FirstLiteral, Base};
    return {0, Reach};
  }

  /// Put \p S back inside the shape after a move that may have left it.
  void repair(Program::Slot &S) const {
    if (!FixedAmounts || !isShiftGramOp(S.Op) || FirstLiteral == Base)
      return;
    if (S.B < FirstLiteral || S.B >= Base)
      S.B = FirstLiteral + S.B % (Base - FirstLiteral);
  }

private:
  llvm::SmallVector<GramOp, kNumGramOps> Ops;
  /// Terminals at or above this index are literals; below it they are inputs.
  size_t FirstLiteral;
  size_t Base;
  bool FixedAmounts;
};

/// Evaluates programs over one fixed grid.
///
/// The terminals' behaviours never change, so they are read once; a slot's is
/// then its operator applied point by point to the behaviours below it,
/// exactly as in the enumeration.  Scratch space is held across calls because
/// the walk asks this question tens of thousands of times, and only the slots
/// the result actually reaches are computed.
class ProgramEvaluator {
public:
  ProgramEvaluator(const OpSemantics &Sem, std::vector<Signature> Terminals,
                   uint32_t Width, size_t Points, size_t NumSlots)
      : Sem(Sem), Terminals(std::move(Terminals)),
        Scratch(NumSlots, Signature(Points, llvm::APInt(Width, 0))) {}

  size_t numTerminals() const { return Terminals.size(); }

  /// Behaviour of \p Prog's named result.
  const Signature &run(const Program &Prog) {
    const size_t Base = numTerminals();
    const size_t Depth = Prog.Result < Base ? 0 : Prog.Result - Base + 1;
    for (size_t K = 0; K < Depth; ++K) {
      const Program::Slot &S = Prog.Slots[K];
      const unsigned Arity = isUnaryGramOp(S.Op) ? 1 : 2;
      const Signature &A = valueAt(S.A);
      const Signature &B = valueAt(S.B);
      Signature &Out = Scratch[K];
      for (size_t Pt = 0; Pt < Out.size(); ++Pt) {
        llvm::APInt Args[] = {A[Pt], B[Pt]};
        Out[Pt] = Sem.apply(S.Op, llvm::ArrayRef<llvm::APInt>(Args, Arity));
      }
    }
    return valueAt(Prog.Result);
  }

private:
  const Signature &valueAt(size_t Index) const {
    return Index < Terminals.size() ? Terminals[Index]
                                    : Scratch[Index - Terminals.size()];
  }

  const OpSemantics &Sem;
  std::vector<Signature> Terminals;
  std::vector<Signature> Scratch;
};

/// The slots \p Prog's result depends on.
llvm::DenseSet<size_t> liveSlots(const Program &Prog, size_t Base) {
  llvm::DenseSet<size_t> Live{Prog.Result};
  // Backwards, because a slot can only be read by a later one: by the time a
  // slot is reached, every reader of it has already been visited.
  for (size_t K = Prog.Slots.size(); K-- > 0;) {
    if (!Live.contains(Base + K))
      continue;
    Live.insert(Prog.Slots[K].A);
    if (!isUnaryGramOp(Prog.Slots[K].Op))
      Live.insert(Prog.Slots[K].B);
  }
  return Live;
}

/// Turn the live part of \p Prog into an expression.
SymRef buildProgram(SymContext &Ctx, const Program &Prog,
                    llvm::ArrayRef<SymRef> Terminals) {
  const size_t Base = Terminals.size();
  const llvm::DenseSet<size_t> Live = liveSlots(Prog, Base);

  llvm::SmallVector<SymRef, 40> Value(Base + Prog.Slots.size());
  for (size_t I = 0; I < Base; ++I)
    Value[I] = Terminals[I];
  for (size_t K = 0; K < Prog.Slots.size(); ++K) {
    if (!Live.contains(Base + K))
      continue;
    const Program::Slot &S = Prog.Slots[K];
    if (isUnaryGramOp(S.Op)) {
      SymRef Arg[] = {Value[S.A]};
      Value[Base + K] = buildGramOp(Ctx, S.Op, Arg);
      continue;
    }
    SymRef Args[] = {Value[S.A], Value[S.B]};
    Value[Base + K] = buildGramOp(Ctx, S.Op, Args);
  }
  return Value[Prog.Result];
}

/// What \p Prog costs in the units the enumeration counts in: the size of the
/// tree its result spells out, with a shared slot charged once per appearance
/// because that is how often a reader meets it.
constexpr size_t saturatingAdd(size_t A, size_t B) {
  constexpr size_t Max = std::numeric_limits<size_t>::max();
  return B > Max - A ? Max : A + B;
}

static_assert(saturatingAdd(std::numeric_limits<size_t>::max(), 1) ==
              std::numeric_limits<size_t>::max());
static_assert(saturatingAdd(4, 7) == 11);

size_t programCost(const Program &Prog, size_t Base) {
  llvm::SmallVector<size_t, 40> Cost(Base + Prog.Slots.size(), 1);
  for (size_t K = 0; K < Prog.Slots.size(); ++K) {
    const Program::Slot &S = Prog.Slots[K];
    size_t SlotCost = saturatingAdd(1, Cost[S.A]);
    if (!isUnaryGramOp(S.Op))
      SlotCost = saturatingAdd(SlotCost, Cost[S.B]);
    Cost[Base + K] = SlotCost;
  }
  return Cost[Prog.Result];
}

/// Fill \p Prog slot by slot, each time taking the operation that gets the
/// most output bits right.
///
/// A walk that starts from noise spends most of its steps climbing out of it,
/// and on the small problems this engine sees the climb is often the whole
/// job.  Taking the best single operation, then the best pair, and so on costs
/// one sweep of the operator table per slot and hands the walk something
/// already close.  It also settles the smallest cases outright: when one
/// operation over two terminals reproduces the body, this finds it before any
/// random choice is made, so the answer to an easy question does not depend on
/// the seed.
std::optional<size_t> initialiseGreedily(Program &Prog, ProgramEvaluator &Eval,
                                         const ProgramShape &Shape,
                                         const Signature &Target,
                                         SearchEffort &Effort) {
  const size_t Base = Eval.numTerminals();
  size_t BestOverall = 0;
  size_t BestResult = Base;
  bool Evaluated = false;

  for (size_t K = 0; K < Prog.Slots.size() && !Effort.empty(); ++K) {
    const size_t Reach = Base + K;
    Prog.Result = Reach;

    Program::Slot Best = Prog.Slots[K];
    size_t BestScore = 0;
    bool Any = false;

    for (GramOp Op : Shape.ops()) {
      if (Effort.empty())
        break;
      const auto [FirstB, LastB] = Shape.secondOperand(Op, Reach);
      for (size_t A = 0; A < Reach && !Effort.empty(); ++A)
        for (size_t B = FirstB; B < LastB; ++B) {
          if (!Effort.spend())
            break;
          Prog.Slots[K] = Program::Slot{Op, A, B};
          const size_t Score = agreeingBits(Eval.run(Prog), Target);
          Evaluated = true;
          if (Any && Score <= BestScore)
            continue;
          Any = true;
          BestScore = Score;
          Best = Prog.Slots[K];
        }
    }

    Prog.Slots[K] = Best;
    if (K == 0 || BestScore > BestOverall) {
      BestOverall = BestScore;
      BestResult = Reach;
    }
  }
  Prog.Result = BestResult;
  if (!Evaluated)
    return std::nullopt;
  return BestOverall;
}

void initialiseRandomly(Program &Prog, const ProgramShape &Shape,
                        uint64_t &State) {
  const size_t Base = Shape.base();
  for (size_t K = 0; K < Prog.Slots.size(); ++K) {
    const size_t Reach = Base + K;
    Prog.Slots[K].Op = Shape.pickOp(State);
    Prog.Slots[K].A = size_t(nextRandom(State) % Reach);
    Prog.Slots[K].B = size_t(nextRandom(State) % Reach);
    Shape.repair(Prog.Slots[K]);
  }
  Prog.Result = size_t(nextRandom(State) % (Base + Prog.Slots.size()));
}

/// Change one small thing.
///
/// The moves are the kinds of mistake a candidate can be making: the wrong
/// operator, either of the wrong operands, or the right value computed in a
/// slot nothing reads.  Keeping them separate lets the walk correct one at a
/// time, which is what a bit-counting score can guide it to do; a move that
/// changed several at once would mostly be a fresh random program.
void mutate(Program &Prog, const ProgramShape &Shape, uint64_t &State) {
  const size_t Base = Shape.base();
  const size_t NumSlots = Prog.Slots.size();
  // Renaming the result is one move among several rather than one in three,
  // because it is the only one that discards everything learned about the slot
  // the walk was working on.
  const unsigned Move = unsigned(nextRandom(State) % 8);
  if (Move == 0) {
    Prog.Result = size_t(nextRandom(State) % (Base + NumSlots));
    return;
  }

  const size_t K = size_t(nextRandom(State) % NumSlots);
  const size_t Reach = Base + K;
  switch (Move % 3) {
  case 0:
    Prog.Slots[K].Op = Shape.pickOp(State);
    break;
  case 1:
    Prog.Slots[K].A = size_t(nextRandom(State) % Reach);
    break;
  default:
    Prog.Slots[K].B = size_t(nextRandom(State) % Reach);
    break;
  }
  Shape.repair(Prog.Slots[K]);
}

/// Whether a step that would give up \p Loss bits should be taken anyway.
///
/// The chance halves for every \p Tolerance bits given up, which is the usual
/// shape of an annealed acceptance and is exact in integers: a loss inside the
/// tolerance is always taken, and one far outside it never is.
bool acceptsRegression(size_t Loss, size_t Tolerance, uint64_t &State) {
  const size_t Halvings = Loss / std::max<size_t>(Tolerance, 1);
  if (Halvings >= 64)
    return false;
  return (nextRandom(State) & ((uint64_t(1) << Halvings) - 1)) == 0;
}

} // namespace

SearchOutcome searchStochastically(SymContext &Ctx, const SynthProblem &P,
                                   const SynthOptions &Opts,
                                   const OpSemantics &Sem, SearchEffort &Effort,
                                   const Checker &Check,
                                   const std::optional<SynthVerifyFn> &Verify,
                                   uint64_t &ProofQueries) {
  SearchOutcome Result;

  if (Opts.StochasticSlots == 0 || Opts.StochasticRestarts == 0 ||
      Effort.empty())
    return Result;

  const llvm::SmallVector<SymRef, 16> Terminals = terminalsOf(P);
  const size_t Base = Terminals.size();
  const ProgramShape Shape(P, Opts);
  const size_t NumSlots =
      std::min<size_t>(Opts.StochasticSlots, Effort.remaining());

  ProgramEvaluator Eval(Sem, terminalSignatures(Ctx, P), P.Width, P.Grid.size(),
                        NumSlots);
  const size_t Perfect = totalBits(P.Target);
  // One word's worth of bits is given up freely and progressively less beyond
  // that, so the walk can cross a valley the width of a single sample without
  // being free to wander the whole space.
  const size_t Tolerance = std::max<size_t>(P.Width, 1);

  // A check evaluates two expressions at every point of its own grid, so it is
  // worth hundreds of ordinary steps.  Charging it that much is what stops a
  // walk that keeps rediscovering one refuted coincidence from spending the
  // whole budget on the same answer.
  const size_t CheckCost = std::max<size_t>(Check.Grid.size(), 1);

  uint64_t State = Opts.Seed;

  for (unsigned Restart = 0; Restart < Opts.StochasticRestarts; ++Restart) {
    if (Effort.empty())
      break;

    Program Prog;
    Prog.Slots.resize(NumSlots);
    std::optional<size_t> InitialScore;
    if (Restart == 0) {
      InitialScore = initialiseGreedily(Prog, Eval, Shape, P.Target, Effort);
    } else {
      if (!Effort.spend())
        break;
      initialiseRandomly(Prog, Shape, State);
      InitialScore = agreeingBits(Eval.run(Prog), P.Target);
    }
    if (!InitialScore)
      break;

    size_t Score = *InitialScore;

    for (size_t Step = 0; Step <= Opts.StochasticIterations; ++Step) {
      if (Score == Perfect) {
        if (!Effort.spend(CheckCost))
          return Result;
        SymRef Node = buildProgram(Ctx, Prog, Terminals);
        const Verdict Said =
            Check.check(Ctx, P.Body, Node, Verify, ProofQueries);
        if (Said == Verdict::AcceptedBySamples ||
            Said == Verdict::AcceptedByVerifier) {
          Result.Candidate = Node;
          Result.Cost = programCost(Prog, Base);
          Result.Evidence = Said == Verdict::AcceptedByVerifier
                                ? SynthEvidence::Verifier
                                : SynthEvidence::Samples;
          return Result;
        }
        Result.SawRefuted |= Said == Verdict::Refuted;
        Result.SawProofIncomplete |= Said == Verdict::ProofIncomplete;
        if (Said == Verdict::ProofIncomplete) {
          Result.StopForProof = true;
          return Result;
        }
        if (Step == Opts.StochasticIterations || !Effort.spend())
          break;
        // A coincidence, or a pair the caller's procedure would not confirm.
        // The move is forced rather than offered: keeping this program because
        // it scores well is exactly how the walk would come straight back to
        // the answer that was just turned down.
        mutate(Prog, Shape, State);
        Score = agreeingBits(Eval.run(Prog), P.Target);
        continue;
      }

      if (Step == Opts.StochasticIterations || !Effort.spend())
        break;

      const Program Previous = Prog;
      mutate(Prog, Shape, State);
      const size_t Next = agreeingBits(Eval.run(Prog), P.Target);
      if (Next >= Score || acceptsRegression(Score - Next, Tolerance, State)) {
        Score = Next;
        continue;
      }
      Prog = Previous;
    }
  }
  return Result;
}

} // namespace neverd::symbolic::synth
