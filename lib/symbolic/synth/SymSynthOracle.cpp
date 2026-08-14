//===- SymSynthOracle.cpp - Running an expression as a blackbox -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements everything the search knows about the expression it is trying to
/// reproduce, which is only ever what that expression *does*: the reduction to
/// a body over independent inputs, the points it is asked about, the readings
/// taken there, and the check an answer has to survive.
///
/// Two of these deserve a word.
///
/// The reduction stands an input in front of every subterm the grammar cannot
/// express, and that is what makes the whole approach safe rather than merely
/// hopeful.  Inputs vary independently, so a candidate that matches the body
/// over them matches it for *every* value those subterms could take, not
/// merely for the values they happen to take together in this expression.  The
/// exchange runs one way only: the reduction can lose an answer — two inputs
/// that are secretly the same subterm will never be recognised as such — and
/// cannot invent one.
///
/// The check runs on the reduced pair rather than on the concrete one, for the
/// same reason.  Proving the bodies equal over independent inputs implies the
/// concrete expressions are equal once the subterms are substituted back;
/// proving the concrete pair equal implies nothing about the bodies.  Checking
/// the stronger statement is also what makes a supplied decision procedure
/// useful, since it is handed a small expression over free variables rather
/// than the original with all of its opaque machinery still attached.
///
//===----------------------------------------------------------------------===//

#include "SymSynthDetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <utility>

namespace neverd::symbolic::synth {

namespace {

/// Prefix of the inputs stood in front of subterms outside the grammar.  The
/// punctuation cannot appear in a parsed identifier, so a placeholder can
/// never be confused with a variable the expression already had.
constexpr llvm::StringLiteral kPlaceholderPrefix("synth$");

/// Shifts the verification grid's random points away from the search grid's.
/// Any constant does; that this one is arbitrary is the point.
constexpr uint64_t kCheckSeedSalt = 0xB5026F5AA96619E9ull;

/// Name of one of the two placeholder operands an operator template is built
/// over.  Named rather than freshly minted so that repeated calls on one
/// context intern the same templates instead of growing its variable table
/// once per call.
std::string templateName(uint32_t Width, unsigned Slot) {
  return ("synth$op." + llvm::Twine(Width) + "." + llvm::Twine(Slot)).str();
}

} // namespace

//===----------------------------------------------------------------------===//
// Randomness
//===----------------------------------------------------------------------===//

uint64_t nextRandom(uint64_t &State) {
  // A multiply-xorshift step: cheap, well mixed in its high bits, and short
  // enough to read that "the same seed gives the same answer" is checkable by
  // eye rather than by trusting a library.
  State += 0x9E3779B97F4A7C15ull;
  uint64_t Z = State;
  Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
  Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
  return Z ^ (Z >> 31);
}

llvm::APInt randomWord(uint64_t &State, uint32_t Width) {
  llvm::SmallVector<uint64_t, 4> Words((Width + 63) / 64);
  for (uint64_t &Word : Words)
    Word = nextRandom(State);
  return llvm::APInt(Width, Words);
}

//===----------------------------------------------------------------------===//
// The grammar's operators
//===----------------------------------------------------------------------===//

std::optional<GramOp> gramOpOf(SymOp Op) {
  switch (Op) {
  case SymOp::Add:
    return GramOp::Add;
  case SymOp::Mul:
    return GramOp::Mul;
  case SymOp::And:
    return GramOp::And;
  case SymOp::Or:
    return GramOp::Or;
  case SymOp::Xor:
    return GramOp::Xor;
  case SymOp::Shl:
    return GramOp::Shl;
  case SymOp::LShr:
    return GramOp::LShr;
  case SymOp::AShr:
    return GramOp::AShr;
  case SymOp::Not:
    return GramOp::Not;
  default:
    return std::nullopt;
  }
}

SymRef buildGramOp(SymContext &Ctx, GramOp Op, llvm::ArrayRef<SymRef> Args) {
  switch (Op) {
  case GramOp::Add:
    return Ctx.mkAdd(Args[0], Args[1]);
  case GramOp::Sub:
    return Ctx.mkSub(Args[0], Args[1]);
  case GramOp::Mul:
    return Ctx.mkMul(Args[0], Args[1]);
  case GramOp::And:
    return Ctx.mkAnd(Args[0], Args[1]);
  case GramOp::Or:
    return Ctx.mkOr(Args[0], Args[1]);
  case GramOp::Xor:
    return Ctx.mkXor(Args[0], Args[1]);
  case GramOp::Shl:
    return Ctx.mkShl(Args[0], Args[1]);
  case GramOp::LShr:
    return Ctx.mkLShr(Args[0], Args[1]);
  case GramOp::AShr:
    return Ctx.mkAShr(Args[0], Args[1]);
  case GramOp::Not:
    return Ctx.mkNot(Args[0]);
  case GramOp::Neg:
    return Ctx.mkNeg(Args[0]);
  }
  llvm_unreachable("unhandled grammar operator");
}

OpSemantics::OpSemantics(SymContext &Ctx, uint32_t Width)
    : Ctx(Ctx), Ones(llvm::APInt::getAllOnes(Width)) {
  // Two distinct non-constant operands, which is exactly what the builders'
  // folding rules have nothing to say about, so each template comes back as
  // the plain node for its operator.
  SymRef A = Ctx.mkVar(templateName(Width, 0), Width);
  SymRef B = Ctx.mkVar(templateName(Width, 1), Width);

  for (unsigned I = 0; I < kNumGramOps; ++I) {
    GramOp Op = GramOp(I);
    // Subtraction and negation are spelled by the builders as a sum and a
    // product, so they borrow those templates in apply() rather than owning
    // one that no node would ever have.
    if (Op == GramOp::Sub || Op == GramOp::Neg)
      continue;
    SymRef Args[] = {A, B};
    Template[I] = buildGramOp(
        Ctx, Op, llvm::ArrayRef<SymRef>(Args, isUnaryGramOp(Op) ? 1 : 2));
    assert(gramOpOf(Ctx.op(Template[I])) == Op &&
           "a template folded into something other than its own operator");
  }
}

llvm::APInt OpSemantics::apply(GramOp Op,
                               llvm::ArrayRef<llvm::APInt> Args) const {
  switch (Op) {
  case GramOp::Neg: {
    llvm::APInt Product[] = {Ones, Args[0]};
    return evalNodeAP(Ctx, Template[unsigned(GramOp::Mul)], Product);
  }
  case GramOp::Sub: {
    llvm::APInt Sum[] = {Args[0], apply(GramOp::Neg, Args.drop_front())};
    return evalNodeAP(Ctx, Template[unsigned(GramOp::Add)], Sum);
  }
  default:
    return evalNodeAP(Ctx, Template[unsigned(Op)], Args);
  }
}

//===----------------------------------------------------------------------===//
// Behaviours
//===----------------------------------------------------------------------===//

bool signaturesEqual(const Signature &A, const Signature &B) {
  return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin());
}

uint64_t hashSignature(const Signature &S) {
  uint64_t Hash = 0xCBF29CE484222325ull;
  for (const llvm::APInt &V : S)
    for (unsigned I = 0, N = V.getNumWords(); I < N; ++I) {
      Hash ^= V.getRawData()[I];
      Hash *= 0x100000001B3ull;
    }
  return Hash;
}

size_t agreeingBits(const Signature &A, const Signature &B) {
  assert(A.size() == B.size() && "signatures span different grids");
  size_t Agree = 0;
  for (size_t I = 0; I < A.size(); ++I)
    Agree += A[I].getBitWidth() - (A[I] ^ B[I]).popcount();
  return Agree;
}

size_t totalBits(const Signature &S) {
  size_t Total = 0;
  for (const llvm::APInt &V : S)
    Total += V.getBitWidth();
  return Total;
}

//===----------------------------------------------------------------------===//
// The grid
//===----------------------------------------------------------------------===//

namespace {

/// A word whose *magnitude* is drawn, rather than its value.
///
/// A uniform draw over a machine word is always an enormous number — a 32-bit
/// one lands below a hundred about once in forty million — and an enormous
/// number is the one thing a shift amount can never usefully be, because every
/// amount at or above the width produces the same answer.  Choosing how many
/// bits the value may occupy before choosing the value spreads the draw across
/// magnitudes instead, so the small values that tell one shift from another
/// turn up as often as the large ones that cannot.
llvm::APInt randomMagnitude(uint64_t &State, uint32_t Width) {
  const auto Bits = uint32_t(1 + nextRandom(State) % Width);
  const llvm::APInt V = randomWord(State, Width);
  return Bits >= Width ? V : (V & llvm::APInt::getLowBitsSet(Width, Bits));
}

} // namespace

std::vector<SamplePoint> buildGrid(unsigned NumLeaves, uint32_t Width,
                                   size_t MaxPoints, uint64_t Seed) {
  // Each class of point below answers a question no other class can, so none
  // of them is allowed to crowd out the rest: each takes a share of the grid
  // and the random draws take what is left.  The floor keeps every share
  // non-empty when a caller asks for a very small grid.
  const size_t Points = std::max<size_t>(MaxPoints, 4 * NumLeaves + 16);

  const llvm::APInt Zero(Width, 0);
  const llvm::APInt One(Width, 1);
  const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);

  std::vector<SamplePoint> Grid;
  Grid.reserve(Points);

  auto push = [&](SamplePoint Point, size_t Cap) {
    if (Grid.size() < Cap)
      Grid.push_back(std::move(Point));
  };

  // All inputs at zero, then one input at one and the rest at zero.  These are
  // the points a coefficient is read at, so a candidate that uses a read
  // coefficient is measured where the reading came from.  They are also what
  // tells the inputs apart from each other and from a literal: without them a
  // grid could give two inputs the same column, and a candidate that used the
  // wrong one would look right.  So they are never crowded out.
  Grid.emplace_back(NumLeaves, Zero);
  for (unsigned I = 0; I < NumLeaves; ++I) {
    SamplePoint Point(NumLeaves, Zero);
    Point[I] = One;
    Grid.push_back(std::move(Point));
  }

  // Every input all-zeros or all-ones.  At such a point each bit position of
  // the word sees the same inputs as every other, which is what makes a corner
  // the one place a sprawling bitwise expression collapses to the small
  // arithmetic one it was built from.
  const size_t CornerCap = Grid.size() + Points / 4;
  const size_t Corners = NumLeaves < 20 ? (size_t(1) << NumLeaves) : ~size_t(0);
  for (size_t K = 0; K < Corners && Grid.size() < CornerCap; ++K) {
    SamplePoint Point;
    for (unsigned L = 0; L < NumLeaves; ++L)
      Point.push_back((K >> L) & 1 ? Ones : Zero);
    push(std::move(Point), CornerCap);
  }

  // Small counting numbers, walked one at a time rather than sampled.
  //
  // Every point above holds inputs at zero, one, or all-ones, and every point
  // below holds a number too large to shift by.  Between them sits the range
  // an amount is actually written in, and it is the range where distinct
  // shifts stop being distinguishable if it is missed: `x << y` and `x >> y`
  // are both zero at almost every assignment, and agree with each other there.
  // A grid without this class accepts either for the other.  Walking it beats
  // sampling it because the range is small enough to exhaust — a candidate
  // that is wrong at one amount and right at the rest is then found rather
  // than hoped about.  The background of ones matters as much as the value
  // does: at a background of zeros a product vanishes and takes the varied
  // input with it.
  const size_t RampCap = Grid.size() + Points / 4;
  // One past the width is where a shift stops meaning anything, and a word
  // narrower than that is exhausted by the corners long before this runs.
  const uint64_t RampEnd = uint64_t(Width) + 1;
  for (uint64_t V = 1;
       Grid.size() < RampCap && V <= RampEnd && llvm::isUIntN(Width, V); ++V) {
    const llvm::APInt Value(Width, V);
    push(SamplePoint(NumLeaves, Value), RampCap);
    // At one input, and at a value of one, the point below is the point above.
    if (NumLeaves == 1 || V == 1)
      continue;
    for (unsigned I = 0; I < NumLeaves; ++I) {
      SamplePoint Point(NumLeaves, One);
      Point[I] = Value;
      push(std::move(Point), RampCap);
    }
  }

  // Two inputs at one, where the coefficient of a product is read.
  const size_t PairCap = Grid.size() + Points / 8;
  for (unsigned I = 0; I < NumLeaves; ++I)
    for (unsigned J = I + 1; J < NumLeaves; ++J) {
      SamplePoint Point(NumLeaves, Zero);
      Point[I] = One;
      Point[J] = One;
      push(std::move(Point), PairCap);
    }

  // The rest at random, half of it over values and half over magnitudes.  The
  // first half is what separates two expressions that differ in their high
  // bits; the second is what separates two that differ only in what they do
  // with a small number.
  uint64_t State = Seed;
  while (Grid.size() < Points) {
    SamplePoint Point;
    for (unsigned L = 0; L < NumLeaves; ++L)
      Point.push_back(nextRandom(State) & 1 ? randomWord(State, Width)
                                            : randomMagnitude(State, Width));
    Grid.push_back(std::move(Point));
  }
  return Grid;
}

Signature evaluateOnGrid(const SymContext &Ctx, SymRef R,
                         llvm::ArrayRef<uint32_t> LeafVars,
                         llvm::ArrayRef<SamplePoint> Grid) {
  SymEvalPlan Plan(Ctx, R);

  std::vector<llvm::APInt> Assignment;
  Assignment.reserve(Ctx.numVars());
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Assignment.emplace_back(Ctx.varInfo(uint32_t(I)).Width, 0);

  Signature Out;
  Out.reserve(Grid.size());
  for (const SamplePoint &Point : Grid) {
    for (size_t L = 0; L < LeafVars.size(); ++L)
      Assignment[LeafVars[L]] = Point[L];
    Out.push_back(Plan.eval(Assignment));
  }
  return Out;
}

//===----------------------------------------------------------------------===//
// Reducing an expression to a body over independent inputs
//===----------------------------------------------------------------------===//

std::vector<uint32_t> reachableAscending(const SymContext &Ctx, SymRef Root) {
  std::vector<uint32_t> Order;
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 64> Work{Root};
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    if (!Seen.insert(R.index()).second)
      continue;
    Order.push_back(R.index());
    for (SymRef C : Ctx.operands(R))
      Work.push_back(C);
  }
  llvm::sort(Order);
  return Order;
}

namespace {

/// True when \p R is something the grammar can say, so that the reduction can
/// leave it in place instead of hiding it behind an input.
///
/// Only the node itself is examined, not what is underneath it: an operand
/// that had to be hidden has already been replaced by an input by the time
/// this node is reached, and an input is something the grammar can say.  A
/// rule that also demanded transparent operands would be upward-closed — one
/// division anywhere would make the whole expression opaque — and there would
/// be nothing left to search over.
bool isTransparent(const SymContext &Ctx, SymRef R, uint32_t Width) {
  if (Ctx.width(R) != Width)
    return false;
  if (Ctx.isVar(R) || Ctx.isConst(R))
    return true;
  if (!gramOpOf(Ctx.op(R)))
    return false;
  // Every grammar operator takes operands at its own width, so this holds for
  // anything the builders produced.  Checking rather than asserting keeps a
  // node arriving from somewhere else from being rebuilt against an operand
  // the reduction never mapped.
  return llvm::all_of(Ctx.operands(R),
                      [&](SymRef C) { return Ctx.width(C) == Width; });
}

/// The literals the search may build with.
///
/// Guessing constants is hopeless — the space is 2^Width wide — so they are
/// read off the expression instead, from two places.
///
/// The first is the expression's own text: an obfuscation reuses the constants
/// of what it obscures, and a shift amount is the same amount whichever way
/// the expression is written.
///
/// The second is the oracle itself.  Ask it at all-zeros and the answer is
/// whatever constant it adds.  Ask it with one input at one and subtract that,
/// and the answer is that input's coefficient.  Ask with two inputs at one and
/// subtract both readings back off, and what is left is the coefficient of
/// their product.  So the numbers a short answer would need are not guessed at
/// all; they are measured, in a handful of evaluations, before the search
/// starts.
void harvestConstants(SymContext &Ctx, const SynthProblem &P,
                      const SynthOptions &Opts,
                      llvm::SmallVectorImpl<SymRef> &Out) {
  const auto NumLeaves = static_cast<unsigned>(P.Leaves.size());
  const uint32_t Width = P.Width;

  llvm::SmallVector<SamplePoint, 16> Probes;
  const llvm::APInt Zero(Width, 0);
  const llvm::APInt One(Width, 1);

  Probes.emplace_back(NumLeaves, Zero);
  for (unsigned I = 0; I < NumLeaves; ++I) {
    SamplePoint Point(NumLeaves, Zero);
    Point[I] = One;
    Probes.push_back(std::move(Point));
  }
  for (unsigned I = 0; I < NumLeaves; ++I)
    for (unsigned J = I + 1; J < NumLeaves; ++J) {
      SamplePoint Point(NumLeaves, Zero);
      Point[I] = One;
      Point[J] = One;
      Probes.push_back(std::move(Point));
    }

  Signature Read = evaluateOnGrid(Ctx, P.Body, P.LeafVars, Probes);

  llvm::SmallVector<llvm::APInt, 16> Wanted;
  auto want = [&](const llvm::APInt &V) {
    if (llvm::none_of(Wanted, [&](const llvm::APInt &S) { return S == V; }))
      Wanted.push_back(V);
  };

  // Zero, one and all-ones earn their place without being measured: they are
  // the identities and absorbing elements of every operator in the grammar, so
  // they are what an answer that ignores an input is written with.
  want(Zero);
  want(One);
  want(llvm::APInt::getAllOnes(Width));

  const llvm::APInt &AtZero = Read[0];
  want(AtZero);
  for (unsigned I = 0; I < NumLeaves; ++I)
    want(Read[1 + I] - AtZero);
  size_t Pair = 1 + NumLeaves;
  for (unsigned I = 0; I < NumLeaves; ++I)
    for (unsigned J = I + 1; J < NumLeaves; ++J, ++Pair)
      want(Read[Pair] - Read[1 + I] - Read[1 + J] + AtZero);

  for (uint32_t Index : reachableAscending(Ctx, P.Body)) {
    SymRef R(Index);
    if (Ctx.isConst(R) && Ctx.width(R) == Width)
      want(Ctx.constValue(R));
  }

  const size_t Keep = std::min<size_t>(Wanted.size(), Opts.MaxConstants);
  for (size_t I = 0; I < Keep; ++I)
    Out.push_back(Ctx.mkConst(Wanted[I]));
}

} // namespace

ProblemStatus buildProblem(SymContext &Ctx, SymRef Root,
                           const SynthOptions &Opts, SynthProblem &Out) {
  const uint32_t Width = Ctx.width(Root);
  Out.Width = Width;

  const std::vector<uint32_t> Order = reachableAscending(Ctx, Root);

  // Children precede parents, so an operand's replacement is always settled
  // before the node that uses it is reached.  A node of some other width is
  // left unmapped: nothing but a width-changing operator can consume it, that
  // operator is outside the grammar, and so the whole of it disappears behind
  // one input anyway.
  llvm::DenseMap<uint32_t, SymRef> Mapped;
  llvm::SmallVector<SymRef, 8> NewOps;
  for (uint32_t Index : Order) {
    SymRef R(Index);
    if (Ctx.width(R) != Width)
      continue;

    if (!isTransparent(Ctx, R, Width)) {
      SymRef Slot = Ctx.mkFreshVar(Width, kPlaceholderPrefix);
      Mapped[Index] = Slot;
      Out.Hidden.emplace(Slot.index(), R);
      continue;
    }

    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ops.empty()) {
      Mapped[Index] = R;
      continue;
    }
    NewOps.clear();
    for (SymRef C : Ops)
      NewOps.push_back(Mapped.lookup(C.index()));
    Mapped[Index] = Ctx.rebuild(R, NewOps);
  }

  Out.Body = Mapped.lookup(Root.index());
  assert(Out.Body.isValid() && "the root maps to nothing");

  // Inputs in variable-id order, so that the columns of a sample point mean
  // the same thing however the expression was walked.
  llvm::SmallVector<uint32_t, 8> Ids;
  Ctx.collectVars(Out.Body, Ids);
  llvm::DenseMap<uint32_t, SymRef> ByVar;
  for (uint32_t Index : reachableAscending(Ctx, Out.Body)) {
    SymRef R(Index);
    if (Ctx.isVar(R))
      ByVar[Ctx.varId(R)] = R;
  }
  for (uint32_t Id : Ids) {
    Out.LeafVars.push_back(Id);
    Out.Leaves.push_back(ByVar.lookup(Id));
  }

  // A body that is one input is the expression the reduction gave up on
  // entirely, and no search over that input can shorten it.
  if (Out.Leaves.empty() || Ctx.isVar(Out.Body))
    return ProblemStatus::Trivial;
  if (Out.Leaves.size() > Opts.MaxLeaves)
    return ProblemStatus::TooManyLeaves;

  harvestConstants(Ctx, Out, Opts, Out.Constants);
  Out.Grid = buildGrid(static_cast<unsigned>(Out.Leaves.size()), Width,
                       Opts.MaxSamples, Opts.Seed);
  Out.Target = evaluateOnGrid(Ctx, Out.Body, Out.LeafVars, Out.Grid);
  return ProblemStatus::Ready;
}

llvm::SmallVector<SymRef, 16> terminalsOf(const SynthProblem &P) {
  llvm::SmallVector<SymRef, 16> Terminals(P.Leaves.begin(), P.Leaves.end());
  Terminals.append(P.Constants.begin(), P.Constants.end());
  return Terminals;
}

std::vector<Signature> terminalSignatures(const SymContext &Ctx,
                                          const SynthProblem &P) {
  std::vector<Signature> Sigs;
  Sigs.reserve(P.Leaves.size() + P.Constants.size());

  for (size_t L = 0; L < P.Leaves.size(); ++L) {
    Signature Sig;
    Sig.reserve(P.Grid.size());
    for (const SamplePoint &Point : P.Grid)
      Sig.push_back(Point[L]);
    Sigs.push_back(std::move(Sig));
  }
  for (SymRef C : P.Constants)
    Sigs.emplace_back(P.Grid.size(), Ctx.constValue(C));
  return Sigs;
}

//===----------------------------------------------------------------------===//
// Checking
//===----------------------------------------------------------------------===//

Verdict Checker::check(SymContext &Ctx, SymRef Body, SymRef Candidate,
                       const std::optional<SynthVerifyFn> &Verify,
                       uint64_t &ProofQueries) const {
  Signature Want = evaluateOnGrid(Ctx, Body, LeafVars, Grid);
  Signature Got = evaluateOnGrid(Ctx, Candidate, LeafVars, Grid);
  if (!signaturesEqual(Want, Got))
    return Verdict::Refuted;
  if (!Verify)
    return Verdict::AcceptedBySamples;
  ++ProofQueries;
  switch ((*Verify)(Ctx, Body, Candidate)) {
  case SynthVerification::Equivalent:
    return Verdict::AcceptedByVerifier;
  case SynthVerification::Different:
    return Verdict::Refuted;
  case SynthVerification::Unknown:
    return Verdict::ProofIncomplete;
  }
  llvm_unreachable("unhandled synthesis verification");
}

Checker makeChecker(const SynthProblem &P, const SynthOptions &Opts) {
  Checker C;
  C.LeafVars = P.LeafVars;
  C.Grid = buildGrid(static_cast<unsigned>(P.Leaves.size()), P.Width,
                     Opts.VerifySamples, Opts.Seed ^ kCheckSeedSalt);
  return C;
}

} // namespace neverd::symbolic::synth
