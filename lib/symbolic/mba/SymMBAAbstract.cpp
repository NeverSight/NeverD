//===- SymMBAAbstract.cpp - Deciding what an MBA measurement can see ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the step that turns an arbitrary expression into a linear MBA
/// over inputs the solver can drive.
///
/// Every node is classified by the part it plays in the linear theory.  A node
/// is only seen through when the algebra can account for it; everything else
/// becomes an input, and what is left is then a linear MBA over those inputs by
/// construction.  That is what makes the measurement in SymMBAMeasure.cpp exact
/// rather than hopeful.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd::symbolic::detail {

namespace {

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

/// A surviving literal in a bitwise operator is a mask.  It is not uniform
/// enough to measure as a fixed coefficient, but it can safely become an
/// opaque input for that use: proving an identity for every mask value is
/// stronger than proving it only for the literal at hand.
bool isBitwiseOperand(Role R) {
  return isBitwiseOrAtom(R) || R == Role::Literal;
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
      // A surviving literal operand here is a mask.  The abstraction rewrites
      // that occurrence to an opaque input, while ordinary bitwise inputs can
      // pass through unchanged.
      Result = llvm::all_of(
                   Ops, [&](SymRef C) { return isBitwiseOperand(roleOf(C)); })
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

} // namespace

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
  llvm::DenseMap<uint32_t, SymRef> MaskInputs;

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
    const SymOp Op = Ctx.op(R);
    const bool IsBitwise =
        Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor;
    for (SymRef C : Ops) {
      if (!IsBitwise || Roles.lookup(C.index()) != Role::Literal) {
        NewOps.push_back(Rewritten.lookup(C.index()));
        continue;
      }

      auto It = MaskInputs.find(C.index());
      if (It == MaskInputs.end()) {
        SymRef V = Pool.take(Ctx.width(C));
        It = MaskInputs.insert({C.index(), V}).first;
        Out.Hidden.emplace(V.index(), C);
      }
      NewOps.push_back(It->second);
    }

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

} // namespace neverd::symbolic::detail
