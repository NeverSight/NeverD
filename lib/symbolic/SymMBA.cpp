//===- SymMBA.cpp - Mixed boolean-arithmetic simplification ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the measure-and-rewrite loop described in SymMBA.h.
///
/// The theory it rests on, in one paragraph.  Split each bitwise term of a
/// linear MBA into minterms — the bitwise functions that pick out the bit
/// positions where the inputs match one particular pattern.  Minterms have
/// disjoint bit supports and together cover every position, so as integers
/// they sum to the all-ones word.  Substituting the split back in and
/// collecting gives, for any linear MBA over t inputs,
///
///     e  =  sum over the 2^t patterns m of  w_m * M_m
///
/// with the weights `w_m` the only thing that distinguishes one such
/// expression from another.  Setting every input to all-zeros or all-ones puts
/// every bit position into the same pattern k, which leaves `M_k` at all-ones
/// and every other minterm at zero, so the expression evaluates to `-w_k`.
/// One evaluation per pattern therefore reads off every weight, and the size
/// the expression was written at has nothing to do with it.
///
/// Three ways of writing the weights back out are tried, because none of them
/// is shortest for every function:
///
///   - *Constant*, when the weights agree.
///   - *Grouped*, `sum over distinct weights v of v * B_v` where `B_v` selects
///     the patterns carrying that weight.  This is what turns
///     `(x | y) - (x & y)` back into `x ^ y`.
///   - *Conjunction basis*, the weights inverted over the subset lattice.
///     This is what turns `(x ^ y) + 2 * (x & y)` back into `x + y`, which the
///     grouped form cannot: it would return the input.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymMBA.h"

#include "neverd/symbolic/SymBitwise.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd::symbolic {

namespace {

//===----------------------------------------------------------------------===//
// Deciding what the measurement can see
//===----------------------------------------------------------------------===//

/// What part an operator plays in the linear theory.
///
/// This classification is what makes the measurement exact rather than
/// hopeful.  A node is only seen through when the algebra can account for it;
/// everything else becomes an input, and what is left is then a linear MBA
/// over those inputs by construction.
enum class Role : uint8_t {
  /// An input: a variable, or a subterm the theory cannot see inside of and
  /// will stand a placeholder in front of.
  Atom,
  /// A literal.  Admissible as a term of a sum or as a coefficient, but not
  /// inside a bitwise operator, where it would tell bit positions apart and
  /// break the uniformity the whole measurement depends on.
  Literal,
  /// A bitwise function of inputs.
  Bitwise,
  /// A product of two bitwise functions.  Recognised only when the caller asks
  /// for it, because the linear measurement cannot read one: at a corner every
  /// bitwise term is all-zeros or all-ones, so `B * B` and `-B` take the same
  /// value at every one of them while differing everywhere else.
  Product,
  /// A sum of constant multiples of the above.
  Linear,
};

/// True for what may appear inside a bitwise operator and still leave the
/// result a bitwise function of the inputs.
bool isBitwiseOrAtom(Role R) { return R == Role::Bitwise || R == Role::Atom; }

/// Whether measuring at \p R can do more than rebuilding its children.
///
/// Operators outside this set are opaque boundaries in both linear and
/// polynomial readings.  Their children have already been visited by the deep
/// walk, so running a region analysis at the boundary can only rediscover that
/// it is opaque.  Skipping it is what keeps a long chain of divisions, casts or
/// extracts linear in depth without hiding an MBA nested underneath.
bool canMeasureAtRoot(const SymContext &Ctx, SymRef R) {
  switch (Ctx.op(R)) {
  case SymOp::Add:
  case SymOp::Mul:
  case SymOp::And:
  case SymOp::Or:
  case SymOp::Xor:
  case SymOp::Not:
    return true;
  default:
    return false;
  }
}

/// Every node reachable from \p Root, in an order where each child precedes
/// its parent.  Interning appends a node only once its operands exist, so
/// ascending index order is such an order.
std::vector<uint32_t> reachableInOrder(const SymContext &Ctx, SymRef Root) {
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

void classify(const SymContext &Ctx, llvm::ArrayRef<uint32_t> Order,
              const llvm::DenseSet<uint32_t> &ForcedAtoms, bool AllowProducts,
              llvm::DenseMap<uint32_t, Role> &Roles) {
  for (uint32_t Index : Order) {
    SymRef R(Index);
    if (ForcedAtoms.contains(Index)) {
      Roles[Index] = Role::Atom;
      continue;
    }

    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    auto roleOf = [&](SymRef C) { return Roles.lookup(C.index()); };

    Role Result;
    switch (Ctx.op(R)) {
    case SymOp::Const:
      Result = Role::Literal;
      break;

    case SymOp::And:
    case SymOp::Or:
    case SymOp::Xor:
      // A surviving literal operand here is a mask: the builders already
      // folded away the all-zeros and all-ones cases, so whatever is left
      // tells bit positions apart, and a term that does that is not something
      // the measurement can read.
      Result = llvm::all_of(
                   Ops, [&](SymRef C) { return isBitwiseOrAtom(roleOf(C)); })
                   ? Role::Bitwise
                   : Role::Atom;
      break;

    case SymOp::Not:
      // Complement is the one bitwise operator that is also affine: `~z` is
      // `-z - 1`.  So it never has to become an input.  Over a bitwise operand
      // it stays bitwise; over an arithmetic one the identity carries it back
      // into the sum, which is what recovers `~(x - 1)` as `-x`.
      Result = isBitwiseOrAtom(roleOf(Ops[0])) ? Role::Bitwise : Role::Linear;
      break;

    case SymOp::Add:
      // Every role is an acceptable summand: a bitwise term, a literal, a
      // nested sum, or a bare input with coefficient one.
      Result = Role::Linear;
      break;

    case SymOp::Mul: {
      // mkMul folds every literal factor into one and puts it first, so the
      // unknown factors are whatever follows it.
      llvm::ArrayRef<SymRef> Unknown =
          Ctx.isConst(Ops[0]) ? Ops.drop_front() : Ops;
      if (Unknown.size() == 1) {
        Result = Role::Linear;
      } else if (AllowProducts && llvm::all_of(Unknown, [&](SymRef C) {
                   return isBitwiseOrAtom(roleOf(C));
                 })) {
        Result = Role::Product;
      } else {
        // A factor that is itself a sum is not a bitwise function.  Its product
        // remains opaque here; product arity itself is controlled later by a
        // resource budget rather than by a semantic cutoff.
        Result = Role::Atom;
      }
      break;
    }

    default:
      Result = Role::Atom;
      break;
    }
    Roles[Index] = Result;
  }
}

/// Find the arithmetic subterms that have to become inputs, and keep looking
/// until no more appear.
///
/// A sum inside a bitwise operator is not a bitwise function of the inputs, so
/// something has to give.  Giving up on the whole bitwise node would be sound
/// and nearly useless: obfuscation builds exactly this shape, wrapping an
/// arithmetic term in `^` and `&` so that the two occurrences look unrelated.
/// Demoting the *sum* instead makes both occurrences the same input, and
/// `(P ^ y) + 2 * (P & y)` measures as `P + y` — with `P` recovered whole.
///
/// A demotion can turn a node that was out of reach into a bitwise one, which
/// can expose another sum underneath it, so this repeats.  It terminates
/// because the set of demoted nodes only ever grows.
void demoteArithmeticUnderBitwise(const SymContext &Ctx,
                                  llvm::ArrayRef<uint32_t> Order,
                                  bool AllowProducts,
                                  llvm::DenseSet<uint32_t> &ForcedAtoms,
                                  llvm::DenseMap<uint32_t, Role> &Roles) {
  for (;;) {
    Roles.clear();
    classify(Ctx, Order, ForcedAtoms, AllowProducts, Roles);

    bool Added = false;
    for (uint32_t Index : Order) {
      SymRef R(Index);
      SymOp Op = Ctx.op(R);
      if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
        continue;
      for (SymRef C : Ctx.operands(R)) {
        Role CRole = Roles.lookup(C.index());
        if (CRole != Role::Linear && CRole != Role::Product)
          continue;
        // A complement that is only arithmetic because of what it wraps: push
        // the demotion through it, so the complement itself stays bitwise and
        // only the sum underneath becomes an input.
        SymRef Target = Ctx.op(C) == SymOp::Not ? Ctx.operand(C, 0) : C;
        Added |= ForcedAtoms.insert(Target.index()).second;
      }
    }
    if (!Added)
      return;
  }
}

//===----------------------------------------------------------------------===//
// Standing placeholders in for what cannot be seen through
//===----------------------------------------------------------------------===//

/// A stable supply of placeholder inputs, numbered per width.
///
/// Minting a brand new variable for every hidden subterm would grow the
/// context's variable table in proportion to how much code was analysed, and
/// that table is what sizes the assignment array of every later evaluation.  A
/// numbered pool keeps it proportional to the widest single expression
/// instead.  The placeholders never escape: they are substituted away before a
/// result is returned.
class Placeholders {
public:
  Placeholders(SymContext &Ctx, const llvm::DenseSet<uint32_t> &Reserved)
      : Ctx(Ctx), Reserved(Reserved) {}

  /// A fresh input of \p Width bits.  The width is the subterm's own, not the
  /// region's: a placeholder stands in an operand slot, and an operand keeps
  /// the width its operator was built with, so minting at any other width would
  /// hand a rebuilt node operands that disagree.
  SymRef take(uint32_t Width) {
    for (;;) {
      std::string Name =
          ("mba$" + llvm::Twine(Width) + "." + llvm::Twine(Next++)).str();
      SymRef V = Ctx.mkVar(Name, Width);
      // Refuse a name the expression under study already uses, which would
      // quietly identify two different things.
      if (!Reserved.contains(V.index()))
        return V;
    }
  }

private:
  SymContext &Ctx;
  const llvm::DenseSet<uint32_t> &Reserved;
  unsigned Next = 0;
};

/// The expression rewritten over inputs the solver can drive, plus the map
/// that puts the hidden subterms back.
struct Abstraction {
  SymRef Body;
  /// Placeholder variable node index to the subterm it stands for.
  std::unordered_map<uint32_t, SymRef> Hidden;
};

/// Rewrite \p Root over inputs the solver can drive.
///
/// With \p AllowProducts a product of two bitwise terms survives as itself
/// rather than becoming one opaque input, which is what lets the degree-two
/// expansion further down see the factors it needs.
std::optional<Abstraction> abstractToMBA(SymContext &Ctx, SymRef Root,
                                         bool AllowProducts) {
  std::vector<uint32_t> Order = reachableInOrder(Ctx, Root);
  llvm::DenseSet<uint32_t> ForcedAtoms;
  llvm::DenseMap<uint32_t, Role> Roles;
  demoteArithmeticUnderBitwise(Ctx, Order, AllowProducts, ForcedAtoms, Roles);

  // Nothing to measure when the whole expression is one opaque thing.
  if (Roles.lookup(Root.index()) == Role::Atom)
    return std::nullopt;

  llvm::DenseSet<uint32_t> Reserved;
  for (uint32_t Index : Order)
    if (Ctx.isVar(SymRef(Index)))
      Reserved.insert(Index);

  Placeholders Pool(Ctx, Reserved);
  Abstraction Out;
  llvm::DenseMap<uint32_t, SymRef> Rewritten;

  for (uint32_t Index : Order) {
    SymRef R(Index);
    if (Roles.lookup(Index) == Role::Atom) {
      if (Ctx.isVar(R)) {
        Rewritten[Index] = R;
        continue;
      }
      // One placeholder per distinct subterm, so a term the obfuscator
      // repeated stays recognisably the same term.
      SymRef V = Pool.take(Ctx.width(R));
      Rewritten[Index] = V;
      Out.Hidden.emplace(V.index(), R);
      continue;
    }

    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ops.empty()) {
      Rewritten[Index] = R;
      continue;
    }
    llvm::SmallVector<SymRef, 8> NewOps;
    NewOps.reserve(Ops.size());
    for (SymRef C : Ops)
      NewOps.push_back(Rewritten.lookup(C.index()));

    // Spend the complement identity where it buys linearity and nowhere else:
    // over a bitwise operand `~z` is already something the measurement reads,
    // and rewriting it there would only make the answer longer.
    if (Ctx.op(R) == SymOp::Not && Roles.lookup(Index) == Role::Linear) {
      Rewritten[Index] =
          Ctx.mkSub(Ctx.mkNeg(NewOps[0]), Ctx.mkOne(Ctx.width(R)));
      continue;
    }
    Rewritten[Index] = Ctx.rebuild(R, NewOps);
  }

  Out.Body = Rewritten.lookup(Root.index());
  return Out;
}

//===----------------------------------------------------------------------===//
// Measurement
//===----------------------------------------------------------------------===//

std::optional<size_t> cornerCount(size_t NumAtoms) {
  if (NumAtoms >= std::numeric_limits<size_t>::digits)
    return std::nullopt;
  return size_t(1) << NumAtoms;
}

/// Evaluate \p Body at every assignment of its inputs to all-zeros or
/// all-ones, and return the weights of its minterm decomposition.
std::vector<llvm::APInt> measure(const SymContext &Ctx, SymRef Body,
                                 llvm::ArrayRef<uint32_t> Atoms) {
  const uint32_t Width = Ctx.width(Body);
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  assert(Corners && "corner table does not fit the host address space");

  SymEvalPlan Plan(Ctx, Body);
  std::vector<llvm::APInt> Weights(*Corners);

  if (Plan.fitsU64()) {
    const uint64_t Ones =
        Width == 64 ? ~uint64_t(0) : (uint64_t(1) << Width) - 1;
    std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
    for (size_t K = 0; K < *Corners; ++K) {
      for (unsigned J = 0; J < NumAtoms; ++J)
        Assignment[Atoms[J]] = (K >> J) & 1 ? Ones : 0;
      // The corner value is the negated weight, so negating is what turns a
      // reading into a weight.
      Weights[K] = -llvm::APInt(Width, Plan.evalU64(Assignment),
                                /*isSigned=*/false, /*implicitTrunc=*/true);
    }
    return Weights;
  }

  const llvm::APInt Zero(Width, 0);
  const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), Zero);
  for (size_t K = 0; K < *Corners; ++K) {
    for (unsigned J = 0; J < NumAtoms; ++J)
      Assignment[Atoms[J]] = (K >> J) & 1 ? Ones : Zero;
    Weights[K] = -Plan.eval(Assignment);
  }
  return Weights;
}

/// Invert a subset-sum over the lattice of input patterns, in place.
///
/// On entry `C[K]` holds the weight of pattern K; on exit it holds the
/// coefficient of the conjunction K, meaning the value that makes the sum of
/// `C[S]` over every subset S of K reproduce the weight that was there.
/// Subtracting one dimension at a time is what makes this cost t * 2^t rather
/// than the 3^t a direct enumeration of submasks would.
void invertOverSubsets(std::vector<llvm::APInt> &C, unsigned NumAtoms) {
  for (unsigned J = 0; J < NumAtoms; ++J) {
    const size_t Bit = size_t(1) << J;
    for (size_t K = 0; K < C.size(); ++K)
      if (K & Bit)
        C[K] -= C[K ^ Bit];
  }
}

//===----------------------------------------------------------------------===//
// Writing the weights back out
//===----------------------------------------------------------------------===//

/// Order weights by value.  They all share a width, so an unsigned compare is
/// total.
struct APIntLess {
  bool operator()(const llvm::APInt &A, const llvm::APInt &B) const {
    return A.ult(B);
  }
};

/// `sum over distinct weights v of v * B_v`, with `B_v` selecting the patterns
/// carrying that weight.  Exact because the minterms of one weight are
/// disjoint, so their union is a bitwise function like any other.
std::optional<SymRef> groupedForm(SymContext &Ctx,
                                  llvm::ArrayRef<llvm::APInt> Weights,
                                  llvm::ArrayRef<SymRef> Atoms,
                                  size_t TermBudget) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  if (NumAtoms > kMaxTruthTableVars)
    return std::nullopt;

  // Ordered rather than hashed, so the terms come out the same way on every
  // run and a difference in output is a real change instead of a reshuffle.
  std::map<llvm::APInt, TruthTable, APIntLess> Groups;
  for (size_t K = 0; K < Weights.size(); ++K) {
    if (Weights[K].isZero())
      continue;
    Groups[Weights[K]] |= TruthTable(1) << K;
  }
  if (Groups.empty())
    return Ctx.mkZero(Ctx.width(Atoms[0]));
  if (Groups.size() > TermBudget)
    return std::nullopt;

  llvm::SmallVector<SymRef, 8> Terms;
  for (const auto &[Value, Table] : Groups)
    Terms.push_back(
        Ctx.mkMul(Ctx.mkConst(Value), synthesizeBitwise(Ctx, Table, Atoms)));
  return Ctx.mkAdd(Terms);
}

/// `sum over subsets S of c_S * AND(inputs in S)`, the conjunction basis.
///
/// The empty conjunction is the all-ones word, so its coefficient contributes
/// the constant term with the sign turned around.
std::optional<SymRef> conjunctionForm(SymContext &Ctx,
                                      std::vector<llvm::APInt> Coefficients,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      size_t TermBudget) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const uint32_t Width = Ctx.width(Atoms[0]);
  invertOverSubsets(Coefficients, NumAtoms);

  size_t NumTerms = 0;
  for (const llvm::APInt &C : Coefficients)
    NumTerms += !C.isZero();
  if (NumTerms > TermBudget)
    return std::nullopt;

  llvm::SmallVector<SymRef, 16> Terms;
  for (size_t K = 0; K < Coefficients.size(); ++K) {
    const llvm::APInt &C = Coefficients[K];
    if (C.isZero())
      continue;
    if (K == 0) {
      Terms.push_back(Ctx.mkConst(-C));
      continue;
    }
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned J = 0; J < NumAtoms; ++J)
      if (K & (size_t(1) << J))
        Factors.push_back(Atoms[J]);
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(C), Ctx.mkAnd(Factors)));
  }
  if (Terms.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkAdd(Terms);
}

//===----------------------------------------------------------------------===//
// Ranking and checking
//===----------------------------------------------------------------------===//

/// What an expression costs whoever reads it.
///
/// Two things make this different from the size of the graph.
///
/// The first is sharing.  A subterm reachable by three paths is one node in
/// the graph and three appearances in anything that writes the expression out,
/// because writing it out is a walk of the tree the graph denotes.  Ranking by
/// graph size therefore calls an expression that says the same thing three
/// times cheaper than one that says two things once each, which is backwards.
/// So this counts the tree.
///
/// The second is the all-ones literal, which costs nothing.  It is never a
/// quantity: it is the sign of a negation — which is how negation is stored —
/// or the mask of a complement.  Charging for it would make `x - y` dearer
/// than `~(~x + y)` and rank the bitwise form above the arithmetic one this
/// exists to recover.
///
/// SymContext caches this per node.  Candidate generation appends nodes to the
/// context and asks for their costs repeatedly; extending one prefix cache
/// keeps all of those queries linear in the number of nodes built, while the
/// number returned still describes the tree a reader sees.
size_t readingCost(const SymContext &Ctx, SymRef R) {
  return Ctx.readabilityCost(R);
}

llvm::APInt randomWord(std::mt19937_64 &Rng, uint32_t Width) {
  llvm::SmallVector<uint64_t, 4> Words((Width + 63) / 64);
  for (uint64_t &Word : Words)
    Word = Rng();
  return llvm::APInt(Width, Words);
}

/// Compare two expressions at random points.
///
/// The derivation above is exact, so a disagreement means a mistake in it
/// rather than an expression the theory did not cover.  This is the net that
/// catches such a mistake before a wrong answer reaches a user.
bool agreeOnSamples(const SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Samples) {
  SymEvalPlan PlanA(Ctx, A);
  SymEvalPlan PlanB(Ctx, B);

  std::vector<llvm::APInt> Assignment;
  Assignment.reserve(Ctx.numVars());
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Assignment.emplace_back(Ctx.varInfo(uint32_t(I)).Width, 0);

  std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
  for (unsigned S = 0; S < Samples; ++S) {
    for (size_t I = 0; I < Assignment.size(); ++I) {
      uint32_t W = Ctx.varInfo(uint32_t(I)).Width;
      // The first few rounds pin every input to a corner of the space, where a
      // mistake in the corner arithmetic itself would show; the rest are
      // ordinary values, where a mistake in the algebra would.
      switch (S) {
      case 0:
        Assignment[I] = llvm::APInt(W, 0);
        break;
      case 1:
        Assignment[I] = llvm::APInt::getAllOnes(W);
        break;
      case 2:
        Assignment[I] = llvm::APInt(W, 1);
        break;
      default:
        Assignment[I] = randomWord(Rng, W);
        break;
      }
    }
    if (PlanA.eval(Assignment) != PlanB.eval(Assignment))
      return false;
  }
  return true;
}

/// The most terms a written-out form may have and still be worth building.
///
/// Every term of a sum contributes at least one node to the tree it is read
/// from, so a form with more terms than \p E has nodes cannot come out shorter
/// than \p E, and building it only for the size guard to reject it is wasted
/// work.  Deriving the bound from the expression at hand is what lets a large
/// input accept a large answer: a hundred-term form is a bad trade against ten
/// nodes and an excellent one against five thousand, and a fixed cap cannot
/// tell those apart.  Growth mode is measuring the solver rather than using it,
/// so nothing is withheld from it.
size_t termBudget(const SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  return Opts.AllowGrowth ? std::numeric_limits<size_t>::max()
                          : readingCost(Ctx, E);
}

/// Every way of writing a set of minterm weights that the solver knows.
void linearCandidates(SymContext &Ctx, std::vector<llvm::APInt> Weights,
                      llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                      llvm::SmallVectorImpl<SymRef> &Out) {
  if (llvm::all_of(Weights,
                   [&](const llvm::APInt &W) { return W == Weights[0]; }))
    Out.push_back(Ctx.mkConst(-Weights[0]));
  if (std::optional<SymRef> Grouped =
          groupedForm(Ctx, Weights, Atoms, TermBudget))
    Out.push_back(*Grouped);
  // Handing the weights over rather than copying: at the widths and arity this
  // reaches, a copy would be tens of thousands of allocations.
  if (std::optional<SymRef> Conjunctions =
          conjunctionForm(Ctx, std::move(Weights), Atoms, TermBudget))
    Out.push_back(*Conjunctions);
}

SymRef cheapestOf(const SymContext &Ctx, llvm::ArrayRef<SymRef> Candidates) {
  SymRef Best;
  size_t BestCost = 0;
  for (SymRef Candidate : Candidates) {
    size_t Cost = readingCost(Ctx, Candidate);
    if (!Best.isValid() || Cost < BestCost) {
      Best = Candidate;
      BestCost = Cost;
    }
  }
  return Best;
}

/// A rewrite, and how many inputs the reading that produced it was over.  The
/// count travels with the candidate because the two readings disagree about
/// it: the linear one counts a product of unknowns as one opaque input, the
/// polynomial one looks inside and counts its factors.  Reporting the larger of
/// the two would describe a reading that lost.
struct Candidate {
  SymRef Expr;
  unsigned NumAtoms = 0;
  /// True only after an exact verifier, separate from candidate synthesis, has
  /// established that \c Expr equals the region it may replace.
  bool Proven = false;
};

//===----------------------------------------------------------------------===//
// Products
//===----------------------------------------------------------------------===//
//
// A product of bitwise terms cannot be measured the way a linear MBA can.  At a
// corner every bitwise term is all-zeros or all-ones, so `B * B` and `-B` agree
// at all 2^t of them and disagree everywhere else: the corner values no longer
// pin the expression down.
//
// They do not have to.  Writing each factor as the sum of the minterms it
// selects and multiplying out gives, for any polynomial in bitwise terms of t
// inputs,
//
//     e  =  sum_m w_m M_m  +  sum over monomials M_{m1} * ... * M_{md} of c *
//     ..
//
// and every coefficient can be computed *symbolically* — each factor's truth
// table is one corner sweep of that factor alone, and the rest is bookkeeping.
// No system has to be solved and nothing is guessed.
//
// What that buys is the identity underneath polynomial MBA obfuscation.  From
// the disjoint splits `x = (x & y) + (x & ~y)` and `y = (x & y) + (~x & y)`,
// multiplying out gives
//
//     x * y  =  (x & y) * (x | y)  +  (x & ~y) * (~x & y)
//
// and the right-hand side expands to exactly the coefficients of the left.  So
// searching the small products for one whose expansion matches is enough to
// recognise it, however it was written.  The same argument runs at any degree,
// which is what reaches `x * y * z` and the cubes above it: only the arity of
// the monomials changes, not the reasoning.

/// One term of a polynomial: a coefficient and the factors it multiplies.  No
/// factors is a constant, one is a linear term, more is where measurement stops
/// working and the expansion takes over.
struct PolyTerm {
  llvm::APInt Coeff;
  llvm::SmallVector<SymRef, 4> Factors;
};

/// Accumulate the terms of `Scale * Node` into \p Out, or fail when some term
/// is of a higher degree than the expansion covers.
bool collectTerms(const SymContext &Ctx, SymRef Node, const llvm::APInt &Scale,
                  llvm::SmallVectorImpl<PolyTerm> &Out) {
  struct WorkItem {
    SymRef Node;
    llvm::APInt Scale;
  };
  llvm::SmallVector<WorkItem, 32> Work{{Node, Scale}};

  while (!Work.empty()) {
    WorkItem Item = Work.pop_back_val();
    SymRef Current = Item.Node;

    if (Ctx.op(Current) == SymOp::Add) {
      llvm::ArrayRef<SymRef> Summands = Ctx.operands(Current);
      // Preserve the recursive walk's stable left-to-right term order.
      for (auto It = Summands.rbegin(); It != Summands.rend(); ++It)
        Work.push_back({*It, Item.Scale});
      continue;
    }

    if (Ctx.isConst(Current)) {
      Out.push_back(PolyTerm{Item.Scale * Ctx.constValue(Current), {}});
      continue;
    }

    if (Ctx.op(Current) == SymOp::Mul) {
      llvm::ArrayRef<SymRef> Ops = Ctx.operands(Current);
      llvm::APInt Coeff = Item.Scale;
      unsigned First = 0;
      if (Ctx.isConst(Ops[0])) {
        Coeff *= Ctx.constValue(Ops[0]);
        First = 1;
      }
      llvm::ArrayRef<SymRef> Factors = Ops.drop_front(First);
      // A scaled sum has to be distributed rather than treated as one factor:
      // `3 * (a + b)` is two terms, and reading it as a single factor would
      // hand the expansion something that is not a bitwise function at all.
      if (Factors.size() == 1) {
        Work.push_back({Factors[0], std::move(Coeff)});
        continue;
      }
      Out.push_back(
          PolyTerm{std::move(Coeff), {Factors.begin(), Factors.end()}});
      continue;
    }

    Out.push_back(PolyTerm{std::move(Item.Scale), {Current}});
  }
  return true;
}

/// Split \p Body into terms the expansion covers, or nothing when some term is
/// of a higher degree than it does.
std::optional<llvm::SmallVector<PolyTerm, 8>>
splitIntoTerms(const SymContext &Ctx, SymRef Body) {
  llvm::SmallVector<PolyTerm, 8> Terms;
  if (!collectTerms(Ctx, Body, llvm::APInt(Ctx.width(Body), 1), Terms))
    return std::nullopt;
  return Terms;
}

/// The truth table of \p F, if \p F really is a bitwise function of the inputs.
///
/// The check is the point.  A factor that takes some third value at a corner
/// is not a bitwise function, and reading a table off it anyway would produce
/// a confidently wrong expansion.  Classification rules most of those out
/// already; this rules out the rest without having to trust it.
std::optional<TruthTable> bitwiseTable(const SymContext &Ctx, SymRef F,
                                       llvm::ArrayRef<uint32_t> AtomIds) {
  const uint32_t Width = Ctx.width(F);
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  const size_t Corners = size_t(1) << NumAtoms;
  SymEvalPlan Plan(Ctx, F);
  TruthTable Table = 0;

  if (Plan.fitsU64()) {
    const uint64_t Ones =
        Width == 64 ? ~uint64_t(0) : (uint64_t(1) << Width) - 1;
    std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
    for (size_t K = 0; K < Corners; ++K) {
      for (unsigned J = 0; J < NumAtoms; ++J)
        Assignment[AtomIds[J]] = (K >> J) & 1 ? Ones : 0;
      uint64_t Value = Plan.evalU64(Assignment);
      if (Value == Ones)
        Table |= TruthTable(1) << K;
      else if (Value != 0)
        return std::nullopt;
    }
    return Table;
  }

  const llvm::APInt Zero(Width, 0);
  const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), Zero);
  for (size_t K = 0; K < Corners; ++K) {
    for (unsigned J = 0; J < NumAtoms; ++J)
      Assignment[AtomIds[J]] = (K >> J) & 1 ? Ones : Zero;
    llvm::APInt Value = Plan.eval(Assignment);
    if (Value == Ones)
      Table |= TruthTable(1) << K;
    else if (!Value.isZero())
      return std::nullopt;
  }
  return Table;
}

/// A bounded amount of semantic-simplification work.
///
/// The same counter covers graph traversal, corner measurements, proof
/// recomputation, product expansion, and factor recovery.  The latter two are
/// exponential in the expression, which is an algorithmic fact rather than a
/// reason to reject a particular degree.  Exhaustive mode passes the largest
/// size_t and therefore removes the budget without changing the representation
/// or the set of degrees it can express.
class WorkBudget {
public:
  explicit WorkBudget(size_t Limit)
      : Remaining(Limit),
        Unlimited(Limit == std::numeric_limits<size_t>::max()) {}

  bool consume(size_t Units = 1) {
    if (Unlimited) {
      Used = Units > std::numeric_limits<size_t>::max() - Used
                 ? std::numeric_limits<size_t>::max()
                 : Used + Units;
      return true;
    }
    if (Units > Remaining) {
      Used += Remaining;
      Remaining = 0;
      Exhausted = true;
      return false;
    }
    Remaining -= Units;
    Used += Units;
    return true;
  }

  bool exhausted() const { return Exhausted; }
  size_t used() const { return Used; }

private:
  size_t Remaining;
  size_t Used = 0;
  bool Unlimited;
  bool Exhausted = false;
};

/// A monomial of arbitrary degree: the minterms whose product it is, sorted.
///
/// The old packed integer key made degree a property of storage width.  A
/// vector key costs a small allocation only for higher-degree terms and lets
/// the resource budget, rather than an eight-byte container, decide how far a
/// search goes.
using Monomial = std::vector<uint16_t>;

Monomial makeMonomial(llvm::ArrayRef<uint16_t> Sorted) {
  return Monomial(Sorted.begin(), Sorted.end());
}

unsigned monomialDegree(const Monomial &Key) {
  return static_cast<unsigned>(Key.size());
}

/// The minterms a monomial names, as a set.
TruthTable monomialSupport(const Monomial &Key) {
  TruthTable Support = 0;
  for (uint16_t Index : Key)
    Support |= TruthTable(1) << Index;
  return Support;
}

/// An expression's coefficients over the minterm basis: the degree-one part
/// indexed by minterm, and every higher monomial keyed by its packing.
struct PolyForm {
  std::vector<llvm::APInt> Linear;
  std::map<Monomial, llvm::APInt> Higher;
  /// True when some term was a product, even if the products then cancelled.
  /// That is the difference between "linear all along", which the measurement
  /// already handled, and "quadratic and above but it came to nothing", which
  /// is a real answer the measurement could not have reached.
  bool SawProduct = false;
  /// Longest monomial still carrying a coefficient.
  unsigned Degree = 0;
};

/// The minterms \p Table selects, as indices.
llvm::SmallVector<uint16_t, 8> selectedMinterms(TruthTable Table,
                                                size_t Corners) {
  llvm::SmallVector<uint16_t, 8> Out;
  for (size_t M = 0; M < Corners; ++M)
    if (truthTableAt(Table, M))
      Out.push_back(static_cast<uint16_t>(M));
  return Out;
}

/// Walk every way of taking one minterm from each factor, handing each
/// resulting monomial to \p Visit.  Stops early when \p Visit returns false.
///
/// This is the whole of multiplying the factors out: a product of sums is the
/// sum over one choice per factor, and the minterms chosen name the monomial
/// however they were ordered.
template <typename FnT>
void forEachMonomial(llvm::ArrayRef<llvm::SmallVector<uint16_t, 8>> Selected,
                     WorkBudget &Budget, FnT Visit) {
  if (Selected.empty())
    return;
  llvm::SmallVector<size_t, 4> Pick(Selected.size(), 0);
  for (;;) {
    if (!Budget.consume())
      return;

    Monomial Key;
    Key.reserve(Selected.size());
    for (size_t J = 0; J < Selected.size(); ++J)
      Key.push_back(Selected[J][Pick[J]]);
    llvm::sort(Key);
    if (!Visit(Key))
      return;

    size_t J = Selected.size();
    while (J > 0) {
      if (++Pick[J - 1] < Selected[J - 1].size())
        break;
      Pick[J - 1] = 0;
      --J;
    }
    if (J == 0)
      return;
  }
}

std::optional<PolyForm> expandOverMinterms(const SymContext &Ctx,
                                           llvm::ArrayRef<PolyTerm> Terms,
                                           llvm::ArrayRef<uint32_t> AtomIds,
                                           uint32_t Width, WorkBudget &Budget) {
  const size_t Corners = size_t(1) << AtomIds.size();
  PolyForm Form;
  Form.Linear.assign(Corners, llvm::APInt(Width, 0));

  llvm::DenseMap<uint32_t, TruthTable> Cache;
  auto tableOf = [&](SymRef F) -> std::optional<TruthTable> {
    auto It = Cache.find(F.index());
    if (It != Cache.end())
      return It->second;
    std::optional<TruthTable> Table = bitwiseTable(Ctx, F, AtomIds);
    if (Table)
      Cache.try_emplace(F.index(), *Table);
    return Table;
  };

  llvm::SmallVector<llvm::SmallVector<uint16_t, 8>, 4> Selected;
  for (const PolyTerm &Term : Terms) {
    if (Term.Factors.empty()) {
      // The minterms sum to the all-ones word, so the constant c is the linear
      // form with every weight at -c.
      for (llvm::APInt &Weight : Form.Linear)
        Weight -= Term.Coeff;
      continue;
    }

    Selected.clear();
    for (SymRef Factor : Term.Factors) {
      std::optional<TruthTable> Table = tableOf(Factor);
      if (!Table)
        return std::nullopt;
      Selected.push_back(selectedMinterms(*Table, Corners));
    }

    if (Term.Factors.size() == 1) {
      for (uint16_t M : Selected[0])
        Form.Linear[M] += Term.Coeff;
      continue;
    }

    Form.SawProduct = true;
    // A factor selecting no minterm is the zero function, and so is the term.
    if (llvm::any_of(Selected,
                     [](llvm::ArrayRef<uint16_t> S) { return S.empty(); }))
      continue;

    forEachMonomial(Selected, Budget, [&](const Monomial &Key) {
      auto It = Form.Higher.try_emplace(Key, llvm::APInt(Width, 0)).first;
      It->second += Term.Coeff;
      return true;
    });
    if (Budget.exhausted())
      return std::nullopt;
  }

  // Cancellation can leave a monomial at zero, and a zero is not part of the
  // shape the search has to explain.
  for (auto It = Form.Higher.begin(); It != Form.Higher.end();)
    It = It->second.isZero() ? Form.Higher.erase(It) : std::next(It);
  for (const auto &[Key, Coeff] : Form.Higher)
    Form.Degree = std::max(Form.Degree, monomialDegree(Key));
  return Form;
}

/// Prove \p Before and \p After equal as linear MBA expressions.
///
/// This is deliberately separate from candidate synthesis.  The synthesizer
/// measures \p Before and invents a compact spelling from the weights; the
/// verifier instead abstracts their difference and accepts only when every
/// coefficient of that difference is zero.  It never asks which spelling was
/// chosen and therefore cannot bless a candidate merely because the code that
/// produced it says so.
bool proveLinearIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                         unsigned MaxAtoms, WorkBudget &Budget) {
  SymRef Delta = Ctx.mkSub(Before, After);
  if (Ctx.isConstZero(Delta))
    return true;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, Delta, /*AllowProducts=*/false);
  if (!Abstract)
    return false;

  llvm::SmallVector<uint32_t, 16> AtomIds;
  Ctx.collectVars(Abstract->Body, AtomIds);
  if (AtomIds.empty())
    return Ctx.isConstZero(Abstract->Body);
  if (AtomIds.size() > MaxAtoms)
    return false;
  std::optional<size_t> Corners = cornerCount(AtomIds.size());
  if (!Corners || !Budget.consume(*Corners))
    return false;

  std::vector<llvm::APInt> Coefficients = measure(Ctx, Abstract->Body, AtomIds);
  return llvm::all_of(Coefficients,
                      [](const llvm::APInt &C) { return C.isZero(); });
}

/// Prove \p Before and \p After equal as sparse polynomials over bitwise
/// minterms.
///
/// Product matching is intentionally absent here.  Both sides are subtracted,
/// expanded independently of whatever factor search produced \p After, and the
/// rewrite is accepted only when no linear or higher-degree coefficient
/// remains.
bool provePolynomialIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                             unsigned MaxAtoms, WorkBudget &Budget) {
  SymRef Delta = Ctx.mkSub(Before, After);
  if (Ctx.isConstZero(Delta))
    return true;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, Delta, /*AllowProducts=*/true);
  if (!Abstract)
    return false;

  llvm::SmallVector<uint32_t, 16> AtomIds;
  Ctx.collectVars(Abstract->Body, AtomIds);
  if (AtomIds.size() > MaxAtoms)
    return false;

  std::optional<llvm::SmallVector<PolyTerm, 8>> Terms =
      splitIntoTerms(Ctx, Abstract->Body);
  if (!Terms)
    return false;
  std::optional<PolyForm> Form = expandOverMinterms(
      Ctx, *Terms, AtomIds, Ctx.width(Abstract->Body), Budget);
  if (!Form)
    return false;

  return llvm::all_of(Form->Linear,
                      [](const llvm::APInt &C) { return C.isZero(); }) &&
         Form->Higher.empty();
}

/// Multiply \p Factors out and count how often each monomial is reached, or
/// report that they cannot be the product being looked for.
///
/// Failing at the first monomial the target does not name is what keeps the
/// search far cheaper than its bound: almost every candidate is wrong, and
/// almost every wrong one is wrong on its first monomial.
bool expandProduct(llvm::ArrayRef<TruthTable> Factors, size_t Corners,
                   const std::map<Monomial, llvm::APInt> &Higher,
                   std::map<Monomial, unsigned> &Counts, WorkBudget &Budget) {
  Counts.clear();
  llvm::SmallVector<llvm::SmallVector<uint16_t, 8>, 4> Selected;
  for (TruthTable Table : Factors) {
    Selected.push_back(selectedMinterms(Table, Corners));
    if (Selected.back().empty())
      return false;
  }

  bool Explains = true;
  forEachMonomial(Selected, Budget, [&](const Monomial &Key) {
    if (!Higher.count(Key)) {
      Explains = false;
      return false;
    }
    ++Counts[Key];
    return true;
  });
  // Reaching only monomials the target names is not enough; it has to reach all
  // of them, or the target holds something this product does not account for.
  return !Budget.exhausted() && Explains && Counts.size() == Higher.size();
}

/// Visit every product of \p Degree bitwise factors whose expansion is exactly
/// \p Higher, until \p Budget is spent.
///
/// The search is over factors rather than over all functions of the inputs,
/// because the target says a great deal about what the factors must be.  A
/// factor can only select a minterm the target names, and between them the
/// factors must name all of them; and a minterm repeated to the full degree is
/// a monomial exactly when *every* factor selects it, so those are known before
/// the search starts.  What is left to guess is a subset of the remainder,
/// which is what brings a degree-three product over three inputs -- the shape
/// polynomial obfuscation reaches for -- down from an unaffordable enumeration
/// to a few tens of thousands of candidates.
///
/// More than one product can match, and which reads best is not the order they
/// are found in, so the caller ranks every match reached within the budget.
/// Combination generation is iterative: product degree changes memory use, not
/// call-stack depth.
template <typename FnT>
void forEachProductMatch(const std::map<Monomial, llvm::APInt> &Higher,
                         unsigned Degree, unsigned NumAtoms, uint32_t Width,
                         WorkBudget &Budget, FnT Visit) {
  const size_t Corners = size_t(1) << NumAtoms;
  if (Degree < 2)
    return;

  TruthTable Active = 0;
  for (const auto &[Key, Coeff] : Higher)
    Active |= monomialSupport(Key);

  TruthTable Shared = 0;
  for (size_t M = 0; M < Corners; ++M) {
    Monomial Repeat(Degree, static_cast<uint16_t>(M));
    if (Higher.count(Repeat))
      Shared |= TruthTable(1) << M;
  }

  llvm::SmallVector<TruthTable, 64> Candidates;
  const TruthTable Free = Active & ~Shared;
  for (TruthTable Sub = Free;; Sub = (Sub - 1) & Free) {
    if (TruthTable Table = Shared | Sub) {
      if (!Budget.consume())
        return;
      Candidates.push_back(Table);
    }
    if (Sub == 0)
      break;
  }
  if (Candidates.empty())
    return;

  llvm::SmallVector<size_t, 4> Choice(Degree, 0);
  llvm::SmallVector<TruthTable, 4> Factors(Degree);
  std::map<Monomial, unsigned> Counts;
  // Nondecreasing tuples, because a product does not care what order its
  // factors are written in.
  for (;;) {
    if (!Budget.consume())
      return;
    TruthTable Union = 0;
    for (size_t I = 0; I < Degree; ++I) {
      Factors[I] = Candidates[Choice[I]];
      Union |= Factors[I];
    }

    if (Union == Active &&
        expandProduct(Factors, Corners, Higher, Counts, Budget)) {
      // Pin the coefficient on a monomial the product reaches exactly once.
      // Where every monomial is reached more than once no single coefficient
      // can be read off, and the shape is refused rather than guessed at.
      const llvm::APInt *Coeff = nullptr;
      for (const auto &[Key, Count] : Counts)
        if (Count == 1) {
          Coeff = &Higher.find(Key)->second;
          break;
        }
      bool Matches = Coeff && !Coeff->isZero();
      if (Matches) {
        for (const auto &[Key, Count] : Counts) {
          llvm::APInt Predicted =
              *Coeff * llvm::APInt(Width, Count, /*isSigned=*/false,
                                   /*implicitTrunc=*/true);
          if (Predicted != Higher.find(Key)->second) {
            Matches = false;
            break;
          }
        }
      }
      if (Matches && !Visit(*Coeff, Factors))
        return;
    }

    if (Budget.exhausted())
      return;

    size_t Pos = Degree;
    while (Pos > 0 && Choice[Pos - 1] + 1 == Candidates.size())
      --Pos;
    if (Pos == 0)
      return;
    const size_t Next = Choice[Pos - 1] + 1;
    for (size_t I = Pos - 1; I < Degree; ++I) {
      Choice[I] = Next;
    }
  }
}

/// Rewrite \p Body as one product plus a linear remainder, when it is of that
/// shape.
std::optional<SymRef> solvePolynomial(SymContext &Ctx, SymRef Body,
                                      llvm::ArrayRef<uint32_t> AtomIds,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      const MBAOptions &Opts,
                                      WorkBudget &Budget) {
  // Up to three inputs, bitwise synthesis is globally minimal.  Above that its
  // prime-implicant cover is still exact; candidate ranking and the final size
  // gate decide whether the resulting product is worth returning.
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  if (NumAtoms > kMaxTruthTableVars)
    return std::nullopt;

  std::optional<llvm::SmallVector<PolyTerm, 8>> Terms =
      splitIntoTerms(Ctx, Body);
  if (!Terms)
    return std::nullopt;

  const uint32_t Width = Ctx.width(Body);
  std::optional<PolyForm> Form =
      expandOverMinterms(Ctx, *Terms, AtomIds, Width, Budget);
  // Without a product there is nothing here the linear measurement did not
  // already see.
  if (!Form || !Form->SawProduct)
    return std::nullopt;

  llvm::SmallVector<SymRef, 4> Linear;
  linearCandidates(Ctx, std::move(Form->Linear), Atoms,
                   termBudget(Ctx, Body, Opts), Linear);
  SymRef Remainder = cheapestOf(Ctx, Linear);
  if (!Remainder.isValid())
    return std::nullopt;

  // The products may have cancelled each other out, in which case what looked
  // like a polynomial is linear after all and the remainder is the whole
  // answer.  That is worth reaching: it is the shape an obfuscator leaves
  // behind when it multiplies a term out and adds the pieces back.
  if (Form->Higher.empty())
    return Remainder;

  // One product contributes monomials of exactly its own degree, so a target
  // mixing degrees is a sum of products, which this does not model.
  for (const auto &[Key, Coeff] : Form->Higher)
    if (monomialDegree(Key) != Form->Degree)
      return std::nullopt;

  SymRef Best;
  size_t BestCost = 0;
  forEachProductMatch(
      Form->Higher, Form->Degree, NumAtoms, Width, Budget,
      [&](const llvm::APInt &Coeff, llvm::ArrayRef<TruthTable> Match) {
        llvm::SmallVector<SymRef, 8> Factors;
        Factors.reserve(Match.size() + 1);
        Factors.push_back(Ctx.mkConst(Coeff));
        for (TruthTable Table : Match)
          Factors.push_back(synthesizeBitwise(Ctx, Table, Atoms));
        SymRef Candidate = Ctx.mkAdd(Ctx.mkMul(Factors), Remainder);
        const size_t Cost = readingCost(Ctx, Candidate);
        if (!Best.isValid() || Cost < BestCost) {
          Best = Candidate;
          BestCost = Cost;
        }
        return true;
      });
  if (!Best.isValid())
    return std::nullopt;
  return Best;
}

//===----------------------------------------------------------------------===//
// One region
//===----------------------------------------------------------------------===//

/// One reading of an expression: what it looks like over inputs the solver can
/// drive, and what those inputs are.
struct Region {
  Abstraction Abstract;
  llvm::SmallVector<uint32_t, 16> AtomIds;
  llvm::SmallVector<SymRef, 16> Atoms;
};

/// What an attempt on a region found, beyond the expression it returns.
struct SolveReport {
  unsigned NumAtoms = 0;
  MBAOutcome Outcome = MBAOutcome::NotApplicable;
  MBAEvidence Evidence = MBAEvidence::None;
  /// Set when a reading was refused for naming more inputs than the budget
  /// allows.  That is the one refusal a larger budget would undo, so it is
  /// worth telling apart from having nothing to measure at all.
  bool TooWide = false;
  /// Set when any traversal, measurement, proof, or product search stopped at
  /// MaxWork.  Unlike an unsupported shape, a larger budget can change this
  /// answer.
  bool BudgetExhausted = false;
};

std::optional<Region> readRegion(SymContext &Ctx, SymRef E, unsigned MaxAtoms,
                                 bool AllowProducts, SolveReport &Rep) {
  std::optional<Abstraction> Abstract = abstractToMBA(Ctx, E, AllowProducts);
  if (!Abstract)
    return std::nullopt;

  Region Out;
  Out.Abstract = std::move(*Abstract);
  Ctx.collectVars(Out.Abstract.Body, Out.AtomIds);
  const bool TooWide =
      Out.AtomIds.size() > MaxAtoms || !cornerCount(Out.AtomIds.size());
  if (TooWide)
    Rep.TooWide = true;
  if (Out.AtomIds.empty() || TooWide)
    return std::nullopt;
  for (uint32_t Id : Out.AtomIds) {
    const SymVarInfo &Info = Ctx.varInfo(Id);
    Out.Atoms.push_back(Ctx.mkVar(Info.Name, Info.Width));
  }
  return Out;
}

/// Best form of \p E as a single region.
///
/// Two readings of the same expression are tried.  The linear one treats a
/// product of unknowns as opaque and measures everything around it; the
/// polynomial one keeps the product and expands it.  Neither subsumes the
/// other — the linear reading handles inputs the expansion cannot see inside
/// of, and the expansion handles products the measurement cannot read — so
/// both run and the shorter answer wins.
SymRef solveRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                   WorkBudget &Budget, SolveReport &Rep) {
  llvm::SmallVector<Candidate, 6> Candidates;
  bool Measured = false;

  if (std::optional<Region> Linear =
          readRegion(Ctx, E, Opts.MaxAtoms, /*AllowProducts=*/false, Rep)) {
    Measured = true;
    auto NumAtoms = static_cast<unsigned>(Linear->AtomIds.size());
    std::optional<size_t> Corners = cornerCount(Linear->AtomIds.size());
    if (!Corners || !Budget.consume(*Corners)) {
      Rep.BudgetExhausted = true;
    } else {
      llvm::SmallVector<SymRef, 4> Forms;
      linearCandidates(Ctx,
                       measure(Ctx, Linear->Abstract.Body, Linear->AtomIds),
                       Linear->Atoms, termBudget(Ctx, E, Opts), Forms);
      for (SymRef Form : Forms) {
        SymRef Rewritten = Linear->Abstract.Hidden.empty()
                               ? Form
                               : Ctx.substitute(Form, Linear->Abstract.Hidden);
        if (proveLinearIdentity(Ctx, E, Rewritten, Opts.MaxAtoms, Budget))
          Candidates.push_back({Rewritten, NumAtoms, true});
      }
    }
  }

  if (std::optional<Region> Poly =
          readRegion(Ctx, E, Opts.MaxAtoms, /*AllowProducts=*/true, Rep)) {
    Measured = true;
    if (std::optional<SymRef> Form =
            solvePolynomial(Ctx, Poly->Abstract.Body, Poly->AtomIds,
                            Poly->Atoms, Opts, Budget)) {
      SymRef Rewritten = Poly->Abstract.Hidden.empty()
                             ? *Form
                             : Ctx.substitute(*Form, Poly->Abstract.Hidden);
      if (provePolynomialIdentity(Ctx, E, Rewritten, Opts.MaxAtoms, Budget))
        Candidates.push_back(
            {Rewritten, static_cast<unsigned>(Poly->AtomIds.size()), true});
    }
  }
  Rep.BudgetExhausted |= Budget.exhausted();

  // Reaching a measurement at all is the difference between "there is nothing
  // here of the kind I read" and "I read it and it is already as short as it
  // gets".  Being refused for width is a third answer again, and the only one a
  // larger budget would change.
  if (Rep.Outcome == MBAOutcome::NotApplicable) {
    if (Rep.BudgetExhausted)
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    else if (Measured)
      Rep.Outcome = MBAOutcome::AlreadyShortest;
    else if (Rep.TooWide)
      Rep.Outcome = MBAOutcome::TooManyInputs;
  }

  const Candidate *Best = nullptr;
  size_t BestCost = 0;
  for (const Candidate &C : Candidates) {
    if (!C.Proven)
      continue;
    size_t Cost = readingCost(Ctx, C.Expr);
    if (!Best || Cost < BestCost) {
      Best = &C;
      BestCost = Cost;
    }
  }
  if (!Best || Best->Expr == E)
    return E;

  assert(Best->Proven && "an unproved MBA candidate reached selection");
  bool Verified = agreeOnSamples(Ctx, E, Best->Expr, Opts.VerifySamples);
  assert(Verified &&
         "an MBA rewrite disagreed with the expression it replaces");
  if (!Verified)
    return E;

  if (!Opts.AllowGrowth && BestCost > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Best->NumAtoms;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Best->Expr;
}

//===----------------------------------------------------------------------===//
// Regions too wide to measure whole
//===----------------------------------------------------------------------===//

/// Solve a region with more inputs than one measurement can afford by cutting
/// it into groups of summands that share no input.
///
/// The 2^t cost of a measurement is real arithmetic, not a tunable: it is how
/// many corners the weights are read off.  But it bounds a single measurement,
/// not an expression.  Two summands of a linear MBA that share no input are
/// independent, so a sum of them is several separate linear MBAs written next
/// to each other, and each can be measured over its own few inputs.  The price
/// is then set by the largest group instead of by the total, which puts an
/// expression over any number of inputs back in reach so long as they were
/// tangled a few at a time -- and tangling them all at once is something the
/// obfuscator cannot afford either, for the same 2^t reason.
///
/// Exactness comes from the independence: no input crosses a group boundary, so
/// summing the solved groups reproduces the original term for term.  The sample
/// check at the end guards against a slip in that reasoning rather than being
/// the reason to believe it.
SymRef solveWide(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                 WorkBudget &Budget, SolveReport &Rep) {
  // An expression cannot abstract to more inputs than it has nodes, so this
  // rejects everything narrow without paying for an abstraction first.
  if (Ctx.dagSize(E) <= Opts.MaxAtoms)
    return E;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, E, /*AllowProducts=*/false);
  if (!Abstract)
    return E;

  llvm::SmallVector<uint32_t, 32> AllAtoms;
  Ctx.collectVars(Abstract->Body, AllAtoms);
  // Measurable in one piece, which the ordinary solver has already tried;
  // splitting it could only reach the same answer by a longer road.
  if (AllAtoms.size() <= Opts.MaxAtoms)
    return E;

  // The cut is between summands, so there has to be a sum to cut.
  if (Ctx.op(Abstract->Body) != SymOp::Add)
    return E;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(Abstract->Body);

  // Two inputs share a group when one term mentions both; closing the relation
  // up means a chain of terms puts their inputs in one group too.
  llvm::DenseMap<uint32_t, uint32_t> Parent;
  for (uint32_t Id : AllAtoms)
    Parent[Id] = Id;
  auto find = [&Parent](uint32_t X) {
    while (Parent[X] != X) {
      Parent[X] = Parent[Parent[X]];
      X = Parent[X];
    }
    return X;
  };

  llvm::SmallVector<llvm::SmallVector<uint32_t, 4>, 16> PerTerm(Terms.size());
  for (size_t I = 0; I < Terms.size(); ++I) {
    Ctx.collectVars(Terms[I], PerTerm[I]);
    for (size_t J = 1; J < PerTerm[I].size(); ++J) {
      uint32_t A = find(PerTerm[I][0]);
      uint32_t B = find(PerTerm[I][J]);
      if (A != B)
        Parent[A] = B;
    }
  }

  // Keyed by group representative so the rebuilt sum comes out the same way on
  // every run; a term naming no input is a constant and joins no group.
  std::map<uint32_t, llvm::SmallVector<SymRef, 8>> Groups;
  llvm::SmallVector<SymRef, 4> Constants;
  for (size_t I = 0; I < Terms.size(); ++I) {
    if (PerTerm[I].empty()) {
      Constants.push_back(Terms[I]);
      continue;
    }
    Groups[find(PerTerm[I][0])].push_back(Terms[I]);
  }
  // A single group means the inputs really are all tangled together, so the
  // width belongs to the expression rather than to how it was written.
  if (Groups.size() < 2)
    return E;

  llvm::SmallVector<SymRef, 16> Parts;
  unsigned Widest = 0;
  bool AnySolved = false;
  for (const auto &Group : Groups) {
    llvm::ArrayRef<SymRef> GroupTerms = Group.second;
    SymRef Part =
        GroupTerms.size() == 1 ? GroupTerms[0] : Ctx.mkAdd(GroupTerms);
    SolveReport PartRep;
    SymRef Solved = solveRegion(Ctx, Part, Opts, Budget, PartRep);
    Rep.BudgetExhausted |= PartRep.BudgetExhausted;
    if (Solved != Part) {
      AnySolved = true;
      Widest = std::max(Widest, PartRep.NumAtoms);
    }
    Parts.push_back(Solved);
  }
  if (!AnySolved)
    return E;

  Parts.append(Constants.begin(), Constants.end());
  SymRef Rebuilt = Parts.size() == 1 ? Parts[0] : Ctx.mkAdd(Parts);
  if (!Abstract->Hidden.empty())
    Rebuilt = Ctx.substitute(Rebuilt, Abstract->Hidden);
  if (Rebuilt == E)
    return E;

  bool Verified = agreeOnSamples(Ctx, E, Rebuilt, Opts.VerifySamples);
  assert(Verified && "a split MBA rewrite disagreed with what it replaces");
  if (!Verified)
    return E;

  if (!Opts.AllowGrowth && readingCost(Ctx, Rebuilt) > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

/// Measure \p E as one region, then in independent parts when it is too wide.
/// This is the mask-free half of the region solver; \c solveMasked reduces to
/// it once it has split a masked expression into mask-free columns, so keeping
/// it separate is what stops the mask split from recursing into itself.
SymRef solveOneRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                      WorkBudget &Budget, SolveReport &Rep) {
  SymRef Solved = solveRegion(Ctx, E, Opts, Budget, Rep);
  return Solved == E ? solveWide(Ctx, E, Opts, Budget, Rep) : Solved;
}

//===----------------------------------------------------------------------===//
// Regions guarded by constant masks
//===----------------------------------------------------------------------===//

/// The distinct constants that appear as an operand of an `&`, `|` or `^` at
/// \p Width.
///
/// These are exactly the constants a corner measurement cannot see past.  A
/// bitwise term is uniform at a corner — all-zeros or all-ones — which is what
/// the whole measurement depends on; a constant operand breaks that uniformity
/// because it tells one bit position from another.  The builders have already
/// folded away the all-zeros and all-ones cases, so anything left here is a
/// genuine mask.  A constant used as a summand or a coefficient is not
/// collected: it does not defeat the measurement and does not partition the
/// word.
///
/// Only masks at \p Width are collected, because they are the ones that
/// partition this region's bits: a mask inside a narrower or wider subterm
/// belongs to that subterm's own region, which the layered walk visits on its
/// own.  Mixing widths would also be a plain type error -- the column bitmasks
/// are built at \p Width and an `APInt` operation across two widths asserts.
void collectBitwiseMasks(const SymContext &Ctx, SymRef E, uint32_t Width,
                         llvm::SmallVectorImpl<llvm::APInt> &Out) {
  for (uint32_t Index : reachableInOrder(Ctx, E)) {
    SymRef R(Index);
    SymOp Op = Ctx.op(R);
    if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
      continue;
    if (Ctx.width(R) != Width)
      continue;
    for (SymRef C : Ctx.operands(R))
      if (Ctx.isConst(C)) {
        llvm::APInt V = Ctx.constValue(C);
        if (llvm::none_of(Out, [&](const llvm::APInt &S) { return S == V; }))
          Out.push_back(V);
      }
  }
}

/// Partition the bit positions of a \p Width word into the columns on which
/// every mask is constant, each returned as the set of positions it holds.
///
/// Two positions belong together when no mask tells them apart.  Splitting the
/// full word by one mask at a time reaches that partition directly, and drops
/// the empty pieces so the column count is the number of distinct signatures
/// rather than the 2^k an enumeration of signatures would suggest.
llvm::SmallVector<llvm::APInt, 8>
maskColumns(uint32_t Width, llvm::ArrayRef<llvm::APInt> Masks) {
  llvm::SmallVector<llvm::APInt, 8> Cols;
  Cols.push_back(llvm::APInt::getAllOnes(Width));
  for (const llvm::APInt &M : Masks) {
    llvm::SmallVector<llvm::APInt, 8> Next;
    for (const llvm::APInt &Col : Cols) {
      llvm::APInt In = Col & M;
      llvm::APInt Out = Col & ~M;
      if (!In.isZero())
        Next.push_back(std::move(In));
      if (!Out.isZero())
        Next.push_back(std::move(Out));
    }
    Cols = std::move(Next);
  }
  return Cols;
}

/// Rewrite \p E as it behaves on the positions in \p ColMask.
///
/// Every collected mask is constant across the column, so inside a bitwise
/// operator it is all-ones (drop it) or all-zeros (kill the term) — which the
/// builders fold — and the result no longer distinguishes bit positions.  What
/// comes out is a mask-free expression that equals \p E on the column's bits.
SymRef restrictToColumn(SymContext &Ctx, SymRef E, const llvm::APInt &ColMask,
                        llvm::ArrayRef<llvm::APInt> Masks) {
  llvm::DenseMap<uint32_t, SymRef> Done;
  for (uint32_t Index : reachableInOrder(Ctx, E)) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ops.empty()) {
      Done[Index] = R;
      continue;
    }
    SymOp Op = Ctx.op(R);
    const bool Bitwise =
        Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor;
    llvm::SmallVector<SymRef, 8> NewOps;
    NewOps.reserve(Ops.size());
    // Masks were collected at the column's width, so a bitwise node at another
    // width holds none of them; leaving it alone also keeps the width-matched
    // APInt comparisons below from asserting.
    const bool SameWidth = Ctx.width(R) == ColMask.getBitWidth();
    for (SymRef C : Ops) {
      SymRef NC = Done.lookup(C.index());
      if (Bitwise && SameWidth && Ctx.isConst(C) &&
          llvm::any_of(Masks, [&](const llvm::APInt &M) {
            return M == Ctx.constValue(C);
          })) {
        // Uniform on the column by construction, so this is exact rather than a
        // choice: the mask either covers every position the column holds or
        // none of them.
        bool AllSet = (Ctx.constValue(C) & ColMask) == ColMask;
        NC = AllSet ? Ctx.mkOnes(Ctx.width(R)) : Ctx.mkZero(Ctx.width(R));
      }
      NewOps.push_back(NC);
    }
    Done[Index] = Ctx.rebuild(R, NewOps);
  }
  return Done.lookup(E.index());
}

/// Prove that changing bits outside one mask column cannot affect bits inside
/// it before the root is reached.
///
/// Replacing a constant mask by zero or all-ones is exact at the positions of
/// one column.  The replacement may differ elsewhere, though, and arithmetic
/// above that mask could carry the difference back into the column.  A path
/// made only of whole-word bitwise operators cannot: each output bit depends
/// only on the input bits at the same position.  Requiring every path from a
/// collected mask to the root to have that form makes column reassembly a
/// derivation rather than a sample-backed guess.
bool maskColumnsAreIndependent(const SymContext &Ctx, SymRef E, uint32_t Width,
                               llvm::ArrayRef<llvm::APInt> Masks) {
  std::vector<uint32_t> Order = reachableInOrder(Ctx, E);
  llvm::DenseMap<uint32_t, bool> SafeToRoot;
  SafeToRoot[E.index()] = true;

  for (auto It = Order.rbegin(); It != Order.rend(); ++It) {
    SymRef R(*It);
    const bool Safe = SafeToRoot.lookup(R.index());
    const SymOp Op = Ctx.op(R);

    bool HoldsMask = false;
    if (Ctx.width(R) == Width && isBitwise(Op))
      for (SymRef C : Ctx.operands(R))
        if (Ctx.isConst(C) && llvm::any_of(Masks, [&](const llvm::APInt &M) {
              return M == Ctx.constValue(C);
            })) {
          HoldsMask = true;
          break;
        }
    if (HoldsMask && !Safe)
      return false;

    const bool ChildSafe = Safe && Ctx.width(R) == Width && isBitwise(Op);
    for (SymRef C : Ctx.operands(R)) {
      auto [Pos, Inserted] = SafeToRoot.try_emplace(C.index(), ChildSafe);
      if (!Inserted)
        Pos->second &= ChildSafe;
    }
  }
  return true;
}

/// Solve a region a constant mask has made unmeasurable, by measuring each
/// mask-uniform column of the word on its own.
///
/// A mask defeats the corner measurement, but only across a column boundary:
/// on the positions where every mask is constant the expression is an ordinary
/// linear MBA, and there it can be measured.  Solving each column and keeping
/// the bits it owns reassembles the whole — which is what recovers, say,
/// `((x ^ y) + 2 * (x & y)) & 0xff` as `(x + y) & 0xff`.
///
/// The split is exact only when no arithmetic carry crosses a column boundary.
/// A low mask over a sum discards the carry it would have produced, so the
/// common case holds, but two summands masked to the same nibble do not: the
/// carry between them lands in a position the split has already decided.  The
/// path from every mask to the root is therefore proved to contain only
/// position-wise bitwise operators before any column is measured.
SymRef solveMasked(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                   WorkBudget &Budget, SolveReport &Rep) {
  const uint32_t Width = Ctx.width(E);
  llvm::SmallVector<llvm::APInt, 8> Masks;
  collectBitwiseMasks(Ctx, E, Width, Masks);
  if (Masks.empty())
    return E;
  if (!maskColumnsAreIndependent(Ctx, E, Width, Masks))
    return E;

  llvm::SmallVector<llvm::APInt, 8> Cols = maskColumns(Width, Masks);
  if (Cols.size() < 2)
    return E;

  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Cols.size());
  unsigned Widest = 0;
  bool AnySolved = false;
  for (const llvm::APInt &Col : Cols) {
    SymRef Ecol = restrictToColumn(Ctx, E, Col, Masks);
    SolveReport ColRep;
    SymRef Scol = solveOneRegion(Ctx, Ecol, Opts, Budget, ColRep);
    Rep.BudgetExhausted |= ColRep.BudgetExhausted;
    if (Scol != Ecol) {
      AnySolved = true;
      Widest = std::max(Widest, ColRep.NumAtoms);
    }
    // Clip each column to the bits it owns.  The columns are disjoint, so the
    // clipped parts share no bit and combining them with `|` is exact.
    Parts.push_back(Ctx.mkAnd(Scol, Ctx.mkConst(Col)));
  }
  if (!AnySolved)
    return E;

  SymRef Rebuilt = Ctx.mkOr(Parts);
  if (Rebuilt == E)
    return E;

  // Independence above made the split exact.  Sampling remains a defect net
  // for the implementation of the derivation, never the reason to accept it.
  bool Verified = agreeOnSamples(Ctx, E, Rebuilt, Opts.VerifySamples);
  assert(Verified && "a proved mask split disagreed with what it replaces");
  if (!Verified)
    return E;
  if (!Opts.AllowGrowth && readingCost(Ctx, Rebuilt) > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

/// Measure \p E as one region, splitting a wide one into independent parts and
/// a masked one into mask-uniform columns.
SymRef solveRegionOrSplit(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          WorkBudget &Budget, SolveReport &Rep) {
  SymRef Solved = solveOneRegion(Ctx, E, Opts, Budget, Rep);
  return Solved == E ? solveMasked(Ctx, E, Opts, Budget, Rep) : Solved;
}

} // namespace

const char *mbaOutcomeName(MBAOutcome Outcome) {
  switch (Outcome) {
  case MBAOutcome::NotApplicable:
    return "not-applicable";
  case MBAOutcome::AlreadyShortest:
    return "already-shortest";
  case MBAOutcome::TooManyInputs:
    return "too-many-inputs";
  case MBAOutcome::BudgetExhausted:
    return "budget-exhausted";
  case MBAOutcome::Rewritten:
    return "rewritten";
  }
  llvm_unreachable("unhandled MBA outcome");
}

const char *mbaEvidenceName(MBAEvidence Evidence) {
  switch (Evidence) {
  case MBAEvidence::None:
    return "none";
  case MBAEvidence::Derivation:
    return "derivation";
  case MBAEvidence::Samples:
    return "samples";
  }
  llvm_unreachable("unhandled MBA evidence");
}

MBAResult simplifyMBA(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;

  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;
  WorkBudget Budget(Opts.MaxWork);
  if (!Budget.consume(Ctx.dagSize(E))) {
    Result.Work = Budget.used();
    Result.Outcome = MBAOutcome::BudgetExhausted;
    return Result;
  }

  SolveReport Rep;
  Result.Expr = solveRegionOrSplit(Ctx, E, Opts, Budget, Rep);
  Result.Work = Budget.used();
  Result.NumAtoms = Rep.NumAtoms;
  Result.Outcome = Rep.Outcome;
  Result.Evidence = Rep.Evidence;
  if (Result.Expr == E)
    return Result;

  Result.SizeAfter = readingCost(Ctx, Result.Expr);
  Result.Changed = true;
  return Result;
}

MBAResult simplifyMBADeep(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;
  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;

  // Walking children before parents means that by the time a node is reached,
  // everything below it has already been shortened, so each layer is measured
  // over the result of the one beneath rather than over the obfuscation that
  // was wrapped around it.  It also keeps the walk iterative: obfuscated
  // expressions nest deeply enough that recursion is a real hazard.
  const std::vector<uint32_t> Order = reachableInOrder(Ctx, E);
  llvm::DenseMap<uint32_t, SymRef> Solved;
  WorkBudget Budget(Opts.MaxWork);
  bool Skipped = false;
  // The weakest evidence any layer rested on is what the whole answer rests on,
  // because the layers above were measured over what it produced.
  bool AnySampled = false;

  for (uint32_t Index : Order) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);

    SymRef Rebuilt = R;
    if (!Ops.empty()) {
      llvm::SmallVector<SymRef, 8> NewOps;
      NewOps.reserve(Ops.size());
      bool Changed = false;
      for (SymRef C : Ops) {
        SymRef S = Solved.lookup(C.index());
        Changed |= S != C;
        NewOps.push_back(S);
      }
      if (Changed)
        Rebuilt = Ctx.rebuild(R, NewOps);
    }

    if (!canMeasureAtRoot(Ctx, Rebuilt)) {
      Solved[Index] = Rebuilt;
      continue;
    }

    // Do not compute a subtree size after the budget has gone: the argument to
    // consume() would otherwise repeat the very traversal the exhausted budget
    // is meant to stop paying for at every remaining node.
    if (!Budget.exhausted() && Budget.consume(Ctx.dagSize(Rebuilt))) {
      SolveReport Rep;
      SymRef Measured = solveRegionOrSplit(Ctx, Rebuilt, Opts, Budget, Rep);
      if (Measured != Rebuilt) {
        Rebuilt = Measured;
        Result.NumAtoms = std::max(Result.NumAtoms, Rep.NumAtoms);
        AnySampled |= Rep.Evidence == MBAEvidence::Samples;
      } else if (Rep.Outcome == MBAOutcome::BudgetExhausted) {
        Skipped = true;
      } else if (Rep.Outcome == MBAOutcome::TooManyInputs &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::TooManyInputs;
      } else if (Rep.Outcome == MBAOutcome::AlreadyShortest &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::AlreadyShortest;
      }
    } else {
      Skipped = true;
    }
    Solved[Index] = Rebuilt;
  }

  Result.Work = Budget.used();
  SymRef Out = Solved.lookup(E.index());
  if (Out == E) {
    // Running out of budget with regions left unvisited is the one refusal that
    // says nothing about the expression, so it outranks the others.
    if (Skipped)
      Result.Outcome = MBAOutcome::BudgetExhausted;
    return Result;
  }

  Result.Expr = Out;
  Result.SizeAfter = readingCost(Ctx, Out);
  Result.Changed = true;
  Result.Outcome = MBAOutcome::Rewritten;
  Result.Evidence = AnySampled ? MBAEvidence::Samples : MBAEvidence::Derivation;
  return Result;
}

} // namespace neverd::symbolic
