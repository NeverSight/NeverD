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
      } else if (AllowProducts && Unknown.size() == 2 &&
                 llvm::all_of(Unknown, [&](SymRef C) {
                   return isBitwiseOrAtom(roleOf(C));
                 })) {
        Result = Role::Product;
      } else {
        // Degree three or higher, or a factor that is itself a sum.  Neither
        // is something the expansion below covers.
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
  Placeholders(SymContext &Ctx, uint32_t Width,
               const llvm::DenseSet<uint32_t> &Reserved)
      : Ctx(Ctx), Width(Width), Reserved(Reserved) {}

  SymRef take() {
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
  uint32_t Width;
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

  Placeholders Pool(Ctx, Ctx.width(Root), Reserved);
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
      SymRef V = Pool.take();
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

/// Evaluate \p Body at every assignment of its inputs to all-zeros or
/// all-ones, and return the weights of its minterm decomposition.
std::vector<llvm::APInt> measure(const SymContext &Ctx, SymRef Body,
                                 llvm::ArrayRef<uint32_t> Atoms) {
  const uint32_t Width = Ctx.width(Body);
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const size_t Corners = size_t(1) << NumAtoms;

  SymEvalPlan Plan(Ctx, Body);
  std::vector<llvm::APInt> Weights(Corners);

  if (Plan.fitsU64()) {
    const uint64_t Ones =
        Width == 64 ? ~uint64_t(0) : (uint64_t(1) << Width) - 1;
    std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
    for (size_t K = 0; K < Corners; ++K) {
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
  for (size_t K = 0; K < Corners; ++K) {
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
                                  const MBAOptions &Opts) {
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
  if (Groups.size() > Opts.MaxTerms)
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
                                      const MBAOptions &Opts) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const uint32_t Width = Ctx.width(Atoms[0]);
  invertOverSubsets(Coefficients, NumAtoms);

  unsigned NumTerms = 0;
  for (const llvm::APInt &C : Coefficients)
    NumTerms += !C.isZero();
  if (NumTerms > Opts.MaxTerms)
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
/// Counting is memoised per node, so the walk stays linear in the graph even
/// though the number it yields is the size of the tree.  That number saturates,
/// because a deeply shared graph denotes an exponentially large tree and all
/// that is ever done with the number is compare it against another candidate's.
size_t readingCost(const SymContext &Ctx, SymRef R) {
  constexpr size_t kCeiling = size_t(1) << 40;

  std::unordered_map<uint32_t, size_t> Cost;
  // Ascending index order reaches every operand before the node using it.
  for (uint32_t Index : reachableInOrder(Ctx, R)) {
    SymRef N(Index);
    size_t Total = Ctx.isConst(N) && Ctx.isConstOnes(N) ? 0 : 1;
    for (SymRef Operand : Ctx.operands(N)) {
      Total += Cost[Operand.index()];
      if (Total >= kCeiling) {
        Total = kCeiling;
        break;
      }
    }
    Cost[Index] = Total;
  }
  return Cost[R.index()];
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

/// Every way of writing a set of minterm weights that the solver knows.
void linearCandidates(SymContext &Ctx, std::vector<llvm::APInt> Weights,
                      llvm::ArrayRef<SymRef> Atoms, const MBAOptions &Opts,
                      llvm::SmallVectorImpl<SymRef> &Out) {
  if (llvm::all_of(Weights,
                   [&](const llvm::APInt &W) { return W == Weights[0]; }))
    Out.push_back(Ctx.mkConst(-Weights[0]));
  if (std::optional<SymRef> Grouped = groupedForm(Ctx, Weights, Atoms, Opts))
    Out.push_back(*Grouped);
  // Handing the weights over rather than copying: at the widths and arity this
  // reaches, a copy would be tens of thousands of allocations.
  if (std::optional<SymRef> Conjunctions =
          conjunctionForm(Ctx, std::move(Weights), Atoms, Opts))
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
/// degree-two one looks inside and counts its factors.  Reporting the larger
/// of the two would describe a reading that lost.
struct Candidate {
  SymRef Expr;
  unsigned NumAtoms = 0;
};

//===----------------------------------------------------------------------===//
// Degree two
//===----------------------------------------------------------------------===//
//
// A product of two bitwise terms cannot be measured the way a linear MBA can.
// At a corner every bitwise term is all-zeros or all-ones, so `B * B` and `-B`
// agree at all 2^t of them and disagree everywhere else: the corner values no
// longer pin the expression down.
//
// They do not have to.  Writing each factor as the sum of the minterms it
// selects and multiplying out gives, for any degree-two polynomial in bitwise
// terms of t inputs,
//
//     e  =  sum_m w_m M_m  +  sum_{m <= n} d_mn M_m M_n
//
// and both coefficient sets can be computed *symbolically* — each factor's
// truth table is one corner sweep of that factor alone, and the rest is
// bookkeeping.  No system has to be solved and nothing is guessed.
//
// What that buys is the identity underneath polynomial MBA obfuscation.  From
// the disjoint splits `x = (x & y) + (x & ~y)` and `y = (x & y) + (~x & y)`,
// multiplying out gives
//
//     x * y  =  (x & y) * (x | y)  +  (x & ~y) * (~x & y)
//
// and the right-hand side expands to exactly the coefficients of the left.  So
// searching the small products for one whose expansion matches is enough to
// recognise it, however it was written.

/// One term of a polynomial: a coefficient and the factors it multiplies.  No
/// factors is a constant, one is a linear term, two is where measurement stops
/// working.
struct PolyTerm {
  llvm::APInt Coeff;
  llvm::SmallVector<SymRef, 2> Factors;
};

/// Accumulate the terms of `Scale * Node` into \p Out, or fail when some term
/// is of a higher degree than the expansion covers.
bool collectTerms(const SymContext &Ctx, SymRef Node, const llvm::APInt &Scale,
                  unsigned Depth, llvm::SmallVectorImpl<PolyTerm> &Out) {
  // Sums and products are both flattened, so a chain of them alternates and
  // stays shallow.  The bound is only there so that no input can turn this
  // into an unbounded descent.
  if (Depth == 0)
    return false;

  if (Ctx.op(Node) == SymOp::Add) {
    for (SymRef Summand : Ctx.operands(Node))
      if (!collectTerms(Ctx, Summand, Scale, Depth - 1, Out))
        return false;
    return true;
  }

  if (Ctx.isConst(Node)) {
    Out.push_back(PolyTerm{Scale * Ctx.constValue(Node), {}});
    return true;
  }

  if (Ctx.op(Node) == SymOp::Mul) {
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(Node);
    llvm::APInt Coeff = Scale;
    unsigned First = 0;
    if (Ctx.isConst(Ops[0])) {
      Coeff *= Ctx.constValue(Ops[0]);
      First = 1;
    }
    llvm::ArrayRef<SymRef> Factors = Ops.drop_front(First);
    if (Factors.size() > 2)
      return false;
    // A scaled sum has to be distributed rather than treated as one factor:
    // `3 * (a + b)` is two terms, and reading it as a single factor would hand
    // the expansion something that is not a bitwise function at all.
    if (Factors.size() == 1)
      return collectTerms(Ctx, Factors[0], Coeff, Depth - 1, Out);
    Out.push_back(PolyTerm{Coeff, {Factors[0], Factors[1]}});
    return true;
  }

  Out.push_back(PolyTerm{Scale, {Node}});
  return true;
}

/// Split \p Body into terms of degree at most two, or nothing when some term
/// is of a higher degree than the expansion covers.
std::optional<llvm::SmallVector<PolyTerm, 8>>
splitIntoTerms(const SymContext &Ctx, SymRef Body) {
  llvm::SmallVector<PolyTerm, 8> Terms;
  if (!collectTerms(Ctx, Body, llvm::APInt(Ctx.width(Body), 1), /*Depth=*/32,
                    Terms))
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

/// An expression's coefficients over the minterm basis.  \c Quadratic is a
/// square array indexed by `m * Corners + n`, of which only `m <= n` is used.
struct MintermForm {
  std::vector<llvm::APInt> Linear;
  std::vector<llvm::APInt> Quadratic;
  bool AnyQuadratic = false;
};

std::optional<MintermForm> expandOverMinterms(const SymContext &Ctx,
                                              llvm::ArrayRef<PolyTerm> Terms,
                                              llvm::ArrayRef<uint32_t> AtomIds,
                                              uint32_t Width) {
  const size_t Corners = size_t(1) << AtomIds.size();
  MintermForm Form;
  Form.Linear.assign(Corners, llvm::APInt(Width, 0));
  Form.Quadratic.assign(Corners * Corners, llvm::APInt(Width, 0));

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

  for (const PolyTerm &Term : Terms) {
    if (Term.Factors.empty()) {
      // The minterms sum to the all-ones word, so the constant c is the linear
      // form with every weight at -c.
      for (llvm::APInt &Weight : Form.Linear)
        Weight -= Term.Coeff;
      continue;
    }

    std::optional<TruthTable> First = tableOf(Term.Factors[0]);
    if (!First)
      return std::nullopt;
    if (Term.Factors.size() == 1) {
      for (size_t M = 0; M < Corners; ++M)
        if (truthTableAt(*First, M))
          Form.Linear[M] += Term.Coeff;
      continue;
    }

    std::optional<TruthTable> Second = tableOf(Term.Factors[1]);
    if (!Second)
      return std::nullopt;
    Form.AnyQuadratic = true;
    for (size_t M = 0; M < Corners; ++M) {
      if (!truthTableAt(*First, M))
        continue;
      for (size_t N = 0; N < Corners; ++N)
        if (truthTableAt(*Second, N))
          Form.Quadratic[std::min(M, N) * Corners + std::max(M, N)] +=
              Term.Coeff;
    }
  }
  return Form;
}

/// How often `M_m * M_n` appears when the product of the two functions is
/// multiplied out.  Never more than twice, because the basis element is
/// symmetric and each factor either selects a minterm or does not.
unsigned pairCount(TruthTable S1, TruthTable S2, size_t M, size_t N) {
  if (M == N)
    return truthTableAt(S1, M) && truthTableAt(S2, M) ? 1 : 0;
  return (truthTableAt(S1, M) && truthTableAt(S2, N) ? 1u : 0u) +
         (truthTableAt(S1, N) && truthTableAt(S2, M) ? 1u : 0u);
}

struct SingleProduct {
  llvm::APInt Coeff;
  TruthTable First = 0;
  TruthTable Second = 0;
};

/// Search the small products for every one whose expansion is exactly
/// \p Quadratic.
///
/// The pair is unordered, so only half the square is walked.  Verification
/// stops at the first basis element the coefficient fails to explain, which
/// for almost every pair is immediately, so the walk costs far less than its
/// bound suggests.
///
/// More than one pair can match, and which of them reads best is not the order
/// they are found in, so they are all collected and ranked by the caller.  The
/// cap only bounds a case that should not arise.
llvm::SmallVector<SingleProduct, 4>
matchProducts(llvm::ArrayRef<llvm::APInt> Quadratic, unsigned NumAtoms,
              uint32_t Width) {
  constexpr unsigned kMaxMatches = 8;
  const size_t Corners = size_t(1) << NumAtoms;
  const TruthTable Mask = truthTableMask(NumAtoms);
  llvm::SmallVector<SingleProduct, 4> Matches;

  for (TruthTable S1 = 1; S1 <= Mask && Matches.size() < kMaxMatches; ++S1) {
    for (TruthTable S2 = S1; S2 <= Mask && Matches.size() < kMaxMatches; ++S2) {
      // Pin the coefficient on a basis element the product reaches exactly
      // once.  One always exists unless the product is identically zero.
      std::optional<llvm::APInt> Coeff;
      for (size_t M = 0; M < Corners && !Coeff; ++M)
        for (size_t N = M; N < Corners; ++N)
          if (pairCount(S1, S2, M, N) == 1) {
            Coeff = Quadratic[M * Corners + N];
            break;
          }
      if (!Coeff || Coeff->isZero())
        continue;

      bool Explains = true;
      for (size_t M = 0; M < Corners && Explains; ++M)
        for (size_t N = M; N < Corners; ++N) {
          llvm::APInt Predicted =
              *Coeff * llvm::APInt(Width, pairCount(S1, S2, M, N),
                                   /*isSigned=*/false, /*implicitTrunc=*/true);
          if (Predicted != Quadratic[M * Corners + N]) {
            Explains = false;
            break;
          }
        }
      if (Explains)
        Matches.push_back(SingleProduct{*Coeff, S1, S2});
    }
  }
  return Matches;
}

/// Rewrite \p Body as one product plus a linear remainder, when it is of that
/// shape.
std::optional<SymRef> solveDegreeTwo(SymContext &Ctx, SymRef Body,
                                     llvm::ArrayRef<uint32_t> AtomIds,
                                     llvm::ArrayRef<SymRef> Atoms,
                                     const MBAOptions &Opts) {
  // The search walks every function of the inputs, so it is only affordable
  // while there are few of them.  Products of more than three unknowns are not
  // something obfuscation produces anyway.
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  if (NumAtoms > kMaxOptimalTruthTableVars)
    return std::nullopt;

  std::optional<llvm::SmallVector<PolyTerm, 8>> Terms =
      splitIntoTerms(Ctx, Body);
  if (!Terms)
    return std::nullopt;

  const uint32_t Width = Ctx.width(Body);
  std::optional<MintermForm> Form =
      expandOverMinterms(Ctx, *Terms, AtomIds, Width);
  // Without a product there is nothing here the linear measurement did not
  // already see.
  if (!Form || !Form->AnyQuadratic)
    return std::nullopt;

  llvm::SmallVector<SymRef, 4> Linear;
  linearCandidates(Ctx, std::move(Form->Linear), Atoms, Opts, Linear);
  SymRef Remainder = cheapestOf(Ctx, Linear);
  if (!Remainder.isValid())
    return std::nullopt;

  // The products may have cancelled each other out, in which case what looked
  // quadratic is linear after all and the remainder is the whole answer.  That
  // is worth reaching: it is the shape an obfuscator leaves behind when it
  // multiplies a term out and adds the pieces back.
  if (llvm::all_of(Form->Quadratic,
                   [](const llvm::APInt &C) { return C.isZero(); }))
    return Remainder;

  llvm::SmallVector<SymRef, 4> Candidates;
  for (const SingleProduct &Product :
       matchProducts(Form->Quadratic, NumAtoms, Width))
    Candidates.push_back(Ctx.mkAdd(
        Ctx.mkMul(Ctx.mkConst(Product.Coeff),
                  Ctx.mkMul(synthesizeBitwise(Ctx, Product.First, Atoms),
                            synthesizeBitwise(Ctx, Product.Second, Atoms))),
        Remainder));

  SymRef Best = cheapestOf(Ctx, Candidates);
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

std::optional<Region> readRegion(SymContext &Ctx, SymRef E, unsigned MaxAtoms,
                                 bool AllowProducts) {
  std::optional<Abstraction> Abstract = abstractToMBA(Ctx, E, AllowProducts);
  if (!Abstract)
    return std::nullopt;

  Region Out;
  Out.Abstract = std::move(*Abstract);
  Ctx.collectVars(Out.Abstract.Body, Out.AtomIds);
  if (Out.AtomIds.empty() || Out.AtomIds.size() > MaxAtoms)
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
/// degree-two one keeps the product and expands it.  Neither subsumes the
/// other — the linear reading handles inputs the expansion cannot see inside
/// of, and the expansion handles products the measurement cannot read — so
/// both run and the shorter answer wins.
SymRef solveRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                   unsigned *NumAtomsOut) {
  llvm::SmallVector<Candidate, 6> Candidates;

  if (std::optional<Region> Linear =
          readRegion(Ctx, E, Opts.MaxAtoms, /*AllowProducts=*/false)) {
    auto NumAtoms = static_cast<unsigned>(Linear->AtomIds.size());
    llvm::SmallVector<SymRef, 4> Forms;
    linearCandidates(Ctx, measure(Ctx, Linear->Abstract.Body, Linear->AtomIds),
                     Linear->Atoms, Opts, Forms);
    for (SymRef Form : Forms)
      Candidates.push_back(
          {Linear->Abstract.Hidden.empty()
               ? Form
               : Ctx.substitute(Form, Linear->Abstract.Hidden),
           NumAtoms});
  }

  if (std::optional<Region> Poly =
          readRegion(Ctx, E, Opts.MaxAtoms, /*AllowProducts=*/true)) {
    if (std::optional<SymRef> Form = solveDegreeTwo(
            Ctx, Poly->Abstract.Body, Poly->AtomIds, Poly->Atoms, Opts))
      Candidates.push_back({Poly->Abstract.Hidden.empty()
                                ? *Form
                                : Ctx.substitute(*Form, Poly->Abstract.Hidden),
                            static_cast<unsigned>(Poly->AtomIds.size())});
  }

  const Candidate *Best = nullptr;
  size_t BestCost = 0;
  for (const Candidate &C : Candidates) {
    size_t Cost = readingCost(Ctx, C.Expr);
    if (!Best || Cost < BestCost) {
      Best = &C;
      BestCost = Cost;
    }
  }
  if (!Best || Best->Expr == E)
    return E;

  bool Verified = agreeOnSamples(Ctx, E, Best->Expr, Opts.VerifySamples);
  assert(Verified && "an MBA rewrite disagreed with the expression it replaces");
  if (!Verified)
    return E;

  if (!Opts.AllowGrowth && BestCost > readingCost(Ctx, E))
    return E;

  if (NumAtomsOut)
    *NumAtomsOut = Best->NumAtoms;
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
                 unsigned *NumAtomsOut) {
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
    unsigned Atoms = 0;
    SymRef Solved = solveRegion(Ctx, Part, Opts, &Atoms);
    if (Solved != Part) {
      AnySolved = true;
      Widest = std::max(Widest, Atoms);
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

  if (NumAtomsOut)
    *NumAtomsOut = Widest;
  return Rebuilt;
}

/// Measure \p E, and when it is too wide for one measurement, try again in
/// independent parts.
SymRef solveRegionOrSplit(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          unsigned *NumAtomsOut) {
  SymRef Solved = solveRegion(Ctx, E, Opts, NumAtomsOut);
  return Solved == E ? solveWide(Ctx, E, Opts, NumAtomsOut) : Solved;
}

} // namespace

MBAResult simplifyMBA(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;

  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;
  Result.Expr = solveRegionOrSplit(Ctx, E, Opts, &Result.NumAtoms);
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
  size_t Work = 0;

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

    if (Work < Opts.MaxWork) {
      Work += Ctx.dagSize(Rebuilt);
      unsigned Atoms = 0;
      SymRef Measured = solveRegionOrSplit(Ctx, Rebuilt, Opts, &Atoms);
      if (Measured != Rebuilt) {
        Rebuilt = Measured;
        Result.NumAtoms = std::max(Result.NumAtoms, Atoms);
      }
    }
    Solved[Index] = Rebuilt;
  }

  SymRef Out = Solved.lookup(E.index());
  if (Out == E)
    return Result;

  Result.Expr = Out;
  Result.SizeAfter = readingCost(Ctx, Out);
  Result.Changed = true;
  return Result;
}

} // namespace neverd::symbolic
