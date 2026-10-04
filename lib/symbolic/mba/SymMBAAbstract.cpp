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
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"

#include <map>
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

/// An exact affine relation for a hidden input.  Its bases stop at bitwise or
/// opaque expressions; no truth-table sampling is used to discover a relation.
struct AffineInput {
  std::map<uint32_t, llvm::APInt> Terms;
  llvm::APInt Offset;
};

/// Limits only the optional recovery of affine relations. Keeping allocation
/// charges after storage is released also bounds temporary coefficient maps.
class AffineResources {
  WorkBudget Fallback{MBAOptions{}.MaxWork};
  WorkBudget &Work;
  size_t Bytes;
  bool Complete = true;

public:
  AffineResources(WorkBudget *Budget, size_t MaxBytes)
      : Work(Budget ? *Budget : Fallback), Bytes(MaxBytes) {}

  bool charge(size_t Units, size_t Storage = 0) {
    if (!Complete || Storage > Bytes || !Work.consume(Units))
      return Complete = false;
    Bytes -= Storage;
    return true;
  }

  bool complete() const { return Complete; }

  bool array(size_t Count, size_t ElementBytes) {
    if (ElementBytes &&
        Count > std::numeric_limits<size_t>::max() / ElementBytes)
      return Complete = false;
    return charge(Count, Count * ElementBytes);
  }

  bool product(const llvm::APInt &Value) {
    const size_t Words = Value.getNumWords();
    if (Words > std::numeric_limits<size_t>::max() / Words)
      return Complete = false;
    return charge(Words * Words, Words * sizeof(uint64_t));
  }

  bool order(size_t Count) {
    size_t Levels = 0;
    for (size_t N = Count ? Count - 1 : 0; N; N >>= 1)
      ++Levels;
    if (Levels && Count > std::numeric_limits<size_t>::max() / Levels)
      return Complete = false;
    return charge(Count * Levels);
  }

  template <typename MapT>
  bool add(MapT &Map, uint32_t Index, const llvm::APInt &Coefficient) {
    const size_t Words = Coefficient.getNumWords();
    if (!charge(1 + Words))
      return false;
    if (Coefficient.isZero())
      return true;
    auto It = Map.find(Index);
    if (It != Map.end()) {
      It->second += Coefficient;
      return true;
    }
    if (!charge(1, 128) || !array(Words, sizeof(uint64_t)))
      return false;
    Map.emplace(Index, Coefficient);
    return true;
  }
};

using AffineCache = std::map<uint32_t, AffineInput>;

bool expandsAffine(const SymContext &Ctx, SymRef R) {
  if (Ctx.op(R) == SymOp::Add || Ctx.op(R) == SymOp::Not)
    return true;
  llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
  return Ctx.op(R) == SymOp::Mul && Ops.size() == 2 && Ctx.isConst(Ops[0]);
}

std::optional<AffineInput>
affineInput(const SymContext &Ctx, SymRef Root,
            const llvm::DenseSet<uint32_t> &ForcedAtoms,
            const llvm::DenseMap<uint32_t, Role> &Roles,
            const AffineCache &Cache, AffineResources &Resources) {
  const unsigned Width = Ctx.width(Root);
  if (!Resources.array((size_t(Width) + 63) / 64, sizeof(uint64_t)))
    return std::nullopt;
  AffineInput Out{{}, llvm::APInt(Width, 0)};
  // Parents have larger indices than children.  Combining all incoming
  // coefficients before visiting a child avoids expanding shared DAG paths.
  std::map<uint32_t, llvm::APInt, std::greater<uint32_t>> Pending;
  if (!Resources.add(Pending, Root.index(), llvm::APInt(Width, 1)))
    return std::nullopt;
  while (!Pending.empty()) {
    if (!Resources.charge(1))
      return std::nullopt;
    auto It = Pending.begin();
    SymRef R(It->first);
    llvm::APInt Coefficient = std::move(It->second);
    Pending.erase(It);
    if (Coefficient.isZero())
      continue;
    if (Ctx.isConst(R)) {
      if (!Resources.product(Coefficient))
        return std::nullopt;
      Out.Offset += Coefficient * Ctx.constValue(R);
      continue;
    }
    if (R != Root && (ForcedAtoms.contains(R.index()) ||
                      Roles.lookup(R.index()) != Role::Linear)) {
      if (!Resources.add(Out.Terms, R.index(), Coefficient))
        return std::nullopt;
      continue;
    }
    auto Cached = Cache.find(R.index());
    if (Cached != Cache.end()) {
      if (!Resources.product(Coefficient))
        return std::nullopt;
      Out.Offset += Coefficient * Cached->second.Offset;
      for (const auto &[Base, Scale] : Cached->second.Terms) {
        if (!Resources.product(Coefficient) ||
            !Resources.add(Out.Terms, Base, Coefficient * Scale))
          return std::nullopt;
      }
      continue;
    }

    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ctx.op(R) == SymOp::Add) {
      for (SymRef C : Ops)
        if (!Resources.add(Pending, C.index(), Coefficient))
          return std::nullopt;
    } else if (Ctx.op(R) == SymOp::Mul && Ops.size() == 2 &&
               Ctx.isConst(Ops[0])) {
      if (!Resources.product(Coefficient) ||
          !Resources.add(Pending, Ops[1].index(),
                         Coefficient * Ctx.constValue(Ops[0])))
        return std::nullopt;
    } else if (Ctx.op(R) == SymOp::Not) {
      Out.Offset -= Coefficient;
      if (!Resources.add(Pending, Ops[0].index(), -Coefficient))
        return std::nullopt;
    } else {
      if (!Resources.add(Out.Terms, R.index(), Coefficient))
        return std::nullopt;
    }
  }
  for (auto It = Out.Terms.begin(); It != Out.Terms.end();) {
    if (!Resources.charge(1))
      return std::nullopt;
    if (It->second.isZero()) {
      It = Out.Terms.erase(It);
    } else {
      ++It;
    }
  }
  return Out;
}

/// Cache shared linear tails, not every prefix of a linear chain. Memoizing
/// every prefix would itself use quadratic space when each adds a new base.
bool cacheSharedAffineInputs(const SymContext &Ctx,
                             llvm::ArrayRef<uint32_t> Roots,
                             const llvm::DenseSet<uint32_t> &ForcedAtoms,
                             const llvm::DenseMap<uint32_t, Role> &Roles,
                             AffineCache &Cache, AffineResources &Resources) {
  llvm::SmallVector<uint32_t, 32> Pending, Shared;
  llvm::DenseSet<uint32_t> Seen;
  llvm::DenseMap<uint32_t, uint8_t> Uses;
  if (!Resources.array(Roots.size(), 2 * sizeof(uint32_t)))
    return false;
  Pending.append(Roots.begin(), Roots.end());
  while (!Pending.empty()) {
    const uint32_t Index = Pending.pop_back_val();
    if (!Resources.charge(1))
      return false;
    if (Seen.contains(Index))
      continue;
    if (!Resources.charge(1, 64))
      return false;
    Seen.insert(Index);
    SymRef R(Index);
    if (!expandsAffine(Ctx, R))
      continue;
    for (SymRef C : Ctx.operands(R)) {
      if (!Resources.charge(1))
        return false;
      if (ForcedAtoms.contains(C.index()) ||
          Roles.lookup(C.index()) != Role::Linear)
        continue;
      auto It = Uses.find(C.index());
      if (It == Uses.end()) {
        if (!Resources.charge(1, 64))
          return false;
        It = Uses.insert({C.index(), 0}).first;
      }
      if (It->second < 2 && ++It->second == 2) {
        if (!Resources.array(1, 2 * sizeof(uint32_t)))
          return false;
        Shared.push_back(C.index());
      }
      if (!Resources.array(1, 2 * sizeof(uint32_t)))
        return false;
      Pending.push_back(C.index());
    }
  }
  if (!Resources.order(Shared.size()))
    return false;
  llvm::sort(Shared);
  for (uint32_t Index : Shared) {
    auto Input =
        affineInput(Ctx, SymRef(Index), ForcedAtoms, Roles, Cache, Resources);
    if (!Input || !Resources.charge(1, 128))
      return false;
    Cache.emplace(Index, std::move(*Input));
  }
  return true;
}

std::optional<llvm::APInt> inverseOdd(const llvm::APInt &Coefficient,
                                      AffineResources &Resources) {
  // Newton iteration doubles the correct low bits each time.  All arithmetic
  // remains modulo the original word width, including widths above 64 bits.
  if (!Resources.array(Coefficient.getNumWords(), sizeof(uint64_t)))
    return std::nullopt;
  llvm::APInt Inverse(Coefficient.getBitWidth(), 1);
  for (uint64_t Bits = 1; Bits < Coefficient.getBitWidth(); Bits *= 2) {
    if (!Resources.product(Coefficient) || !Resources.product(Coefficient))
      return std::nullopt;
    Inverse *= 2 - Coefficient * Inverse;
  }
  return Inverse;
}

struct WeightedInput {
  SymRef Hidden;
  llvm::APInt Coefficient;
  unsigned Shift;
  std::optional<llvm::APInt> Inverse;
};

using WeightedInputs = llvm::DenseMap<uint32_t, WeightedInput>;

bool recordWeightedInput(
    const SymContext &Ctx, uint32_t Index,
    const llvm::DenseMap<uint32_t, SymRef> &OriginalRewritten,
    WeightedInputs &Inputs, AffineResources &Resources) {
  SymRef R(Index);
  llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
  if (Ctx.op(R) != SymOp::Mul || Ops.size() != 2 || !Ctx.isConst(Ops[0]))
    return true;
  const llvm::APInt &Coefficient = Ctx.constValue(Ops[0]);
  // Odd coefficients already have the stronger whole-base inverse recovery.
  if (Coefficient[0])
    return true;
  if (!Resources.charge(Coefficient.getNumWords()))
    return false;
  if (Coefficient.isZero())
    return true;
  const unsigned Shift = Coefficient.countr_zero();
  SymRef Base = OriginalRewritten.lookup(Ops[1].index());
  auto It = Inputs.find(Base.index());
  // The smallest power of two dividing a coefficient admits every weighted
  // use admitted by the others. Keep one stable relation per complete base,
  // rather than comparing every hidden input with every arithmetic term.
  if (It != Inputs.end() && It->second.Shift <= Shift)
    return true;
  if (!Resources.charge(1, 128) ||
      !Resources.array(Coefficient.getNumWords(), sizeof(uint64_t)))
    return false;
  WeightedInput Input{OriginalRewritten.lookup(Index), Coefficient, Shift,
                      std::nullopt};
  if (It == Inputs.end())
    Inputs.try_emplace(Base.index(), std::move(Input));
  else
    It->second = std::move(Input);
  return true;
}

/// Recover d*T from P=k*T only when the constant congruence k*q=d is soluble.
/// This never recovers T itself from an even multiple. Arithmetic stays at
/// the original width; only the constant inverse uses the reduced modulus.
std::optional<llvm::APInt> weightedQuotient(WeightedInput &Input,
                                            const llvm::APInt &Coefficient,
                                            AffineResources &Resources,
                                            bool &Complete) {
  auto refuse = [&]() -> std::optional<llvm::APInt> {
    Complete = false;
    return std::nullopt;
  };
  if (!Resources.array(Coefficient.getNumWords(), sizeof(uint64_t)))
    return refuse();
  if (Coefficient.countr_zero() < Input.Shift)
    return std::nullopt;
  const unsigned Width = Coefficient.getBitWidth();
  llvm::APInt Quotient(Width, 1);
  if (Coefficient != Input.Coefficient) {
    if (!Resources.array(Coefficient.getNumWords(), sizeof(uint64_t)))
      return refuse();
    if (Coefficient == -Input.Coefficient) {
      Quotient = llvm::APInt::getAllOnes(Width);
    } else {
      const unsigned ReducedWidth = Width - Input.Shift;
      if (!Resources.array(Coefficient.getNumWords(), 2 * sizeof(uint64_t)))
        return refuse();
      if (!Input.Inverse) {
        auto Inverse = inverseOdd(
            Input.Coefficient.lshr(Input.Shift).trunc(ReducedWidth), Resources);
        if (!Inverse)
          return refuse();
        Input.Inverse = std::move(*Inverse);
      }
      if (!Resources.product(*Input.Inverse))
        return refuse();
      llvm::APInt Reduced =
          Coefficient.lshr(Input.Shift).trunc(ReducedWidth) * *Input.Inverse;
      // Both lifts solve the same congruence. Prefer one itself to its
      // negative alias at reduced width one; otherwise keep the signed lift.
      Quotient = Reduced.isOne() ? llvm::APInt(Width, 1) : Reduced.sext(Width);
    }
  }
  if (!Resources.product(Coefficient))
    return refuse();
  if (Input.Coefficient * Quotient != Coefficient)
    return std::nullopt;
  return Quotient;
}

bool affineOrder(const SymContext &Ctx, SymRef Root,
                 std::vector<uint32_t> &Order, AffineResources &Resources) {
  llvm::SmallVector<SymRef, 32> Pending;
  llvm::DenseSet<uint32_t> Seen;
  if (!Resources.array(1, 2 * sizeof(SymRef)))
    return false;
  Pending.push_back(Root);
  while (!Pending.empty()) {
    SymRef R = Pending.pop_back_val();
    if (!Resources.charge(1))
      return false;
    if (Seen.contains(R.index()))
      continue;
    if (!Resources.charge(1, 64) || !Resources.array(1, 2 * sizeof(uint32_t)))
      return false;
    Seen.insert(R.index());
    Order.push_back(R.index());
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (!Resources.array(Ops.size(), 2 * sizeof(SymRef)))
      return false;
    Pending.append(Ops.begin(), Ops.end());
  }
  if (!Resources.order(Order.size()))
    return false;
  llvm::sort(Order);
  return true;
}

bool chargeAffineNode(const SymContext &Ctx, SymOp Op,
                      llvm::ArrayRef<SymRef> Ops, AffineResources &Resources) {
  size_t Count = 0;
  for (SymRef C : Ops) {
    const size_t N = (Op == SymOp::Add || Op == SymOp::Mul) && Ctx.op(C) == Op
                         ? Ctx.numOperands(C)
                         : 1;
    if (N > std::numeric_limits<size_t>::max() - Count)
      return false;
    Count += N;
  }
  return Resources.charge(1, 128) && Resources.array(Count, 32);
}

std::optional<SymRef> weightedTerm(SymContext &Ctx, SymRef R,
                                   WeightedInputs &Inputs,
                                   AffineResources &Resources, bool &Complete) {
  if (Ctx.op(R) != SymOp::Mul || Ctx.numOperands(R) != 2 ||
      !Ctx.isConst(Ctx.operand(R, 0)))
    return std::nullopt;
  auto It = Inputs.find(Ctx.operand(R, 1).index());
  if (It == Inputs.end())
    return std::nullopt;
  auto Quotient = weightedQuotient(
      It->second, Ctx.constValue(Ctx.operand(R, 0)), Resources, Complete);
  if (!Quotient)
    return std::nullopt;
  if (!Resources.charge(1, 128) ||
      !Resources.array(Quotient->getNumWords(), sizeof(uint64_t))) {
    Complete = false;
    return std::nullopt;
  }
  SymRef Factors[] = {Ctx.mkConst(*Quotient), It->second.Hidden};
  if (!chargeAffineNode(Ctx, SymOp::Mul, Factors, Resources)) {
    Complete = false;
    return std::nullopt;
  }
  return Ctx.mkMul(Factors);
}

SymRef restoreWeightedUses(SymContext &Ctx, SymRef Body,
                           llvm::ArrayRef<uint32_t> Order,
                           WeightedInputs &Inputs, AffineResources &Resources) {
  llvm::DenseMap<uint32_t, SymRef> Changed;
  for (uint32_t Index : Order) {
    if (!Resources.charge(1))
      return Body;
    SymRef R(Index);
    // The completed abstraction has already hidden arithmetic bitwise
    // operands and expanded arithmetic complements. Only sums and constant
    // products can carry a recovered term to their arithmetic consumers.
    const bool Product = Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
                         Ctx.isConst(Ctx.operand(R, 0));
    if (Ctx.op(R) != SymOp::Add && !Product)
      continue;
    bool Complete = true;
    auto Replacement = Product
                           ? weightedTerm(Ctx, R, Inputs, Resources, Complete)
                           : std::nullopt;
    if (!Complete)
      return Body;
    if (!Replacement) {
      bool HasChanged = false;
      for (SymRef C : Ctx.operands(R)) {
        if (!Resources.charge(1))
          return Body;
        HasChanged |= Changed.contains(C.index());
      }
      if (!HasChanged)
        continue;
      if (!Resources.array(Ctx.numOperands(R), 2 * sizeof(SymRef)))
        return Body;
      llvm::SmallVector<SymRef, 8> Ops;
      for (SymRef C : Ctx.operands(R)) {
        auto It = Changed.find(C.index());
        Ops.push_back(It == Changed.end() ? C : It->second);
      }
      if (!chargeAffineNode(Ctx, Ctx.op(R), Ops, Resources))
        return Body;
      Replacement = Ctx.rebuild(R, Ops);
    }
    if (*Replacement != R) {
      if (!Resources.charge(1, 128))
        return Body;
      Changed[Index] = *Replacement;
    }
  }
  auto It = Changed.find(Body.index());
  return It == Changed.end() ? Body : It->second;
}

using AffineInputs = llvm::SmallVector<std::pair<uint32_t, AffineInput>, 4>;

std::optional<size_t> affineHash(const AffineInput &Input, bool Complement,
                                 AffineResources &Resources) {
  if (!Resources.array(Input.Offset.getNumWords(), sizeof(uint64_t)))
    return std::nullopt;
  llvm::hash_code Hash = llvm::hash_combine(
      Input.Offset.getBitWidth(),
      llvm::hash_value(Complement ? ~Input.Offset : Input.Offset));
  for (const auto &[Base, Coefficient] : Input.Terms) {
    if (!Resources.array(Coefficient.getNumWords(), sizeof(uint64_t)))
      return std::nullopt;
    Hash = llvm::hash_combine(
        Hash, Base, llvm::hash_value(Complement ? -Coefficient : Coefficient));
  }
  return static_cast<size_t>(Hash);
}

bool areComplements(const AffineInput &A, const AffineInput &B,
                    AffineResources &Resources) {
  if (A.Offset.getBitWidth() != B.Offset.getBitWidth() ||
      A.Terms.size() != B.Terms.size())
    return false;
  if (!Resources.array(A.Offset.getNumWords(), sizeof(uint64_t)) ||
      A.Offset != ~B.Offset)
    return false;
  auto Other = B.Terms.begin();
  for (const auto &[Base, Coefficient] : A.Terms) {
    if (!Resources.array(Coefficient.getNumWords(), sizeof(uint64_t)) ||
        Base != Other->first || Coefficient != -Other->second)
      return false;
    ++Other;
  }
  return true;
}

/// Index complete affine inputs, including even multiples. Only the exact
/// identity F+G=-1 permits identifying their bitwise uses as complements; no
/// inverse of either input is needed or inferred.
class ComplementInputs {
  std::unordered_multimap<size_t, size_t> ByHash;
  llvm::DenseMap<uint32_t, SymRef> Sources;

public:
  bool record(const AffineInputs &Inputs, uint32_t Index,
              const AffineInput &Input,
              const llvm::DenseSet<uint32_t> &ForcedAtoms,
              const llvm::DenseMap<uint32_t, SymRef> &OriginalRewritten,
              AffineResources &Resources) {
    auto Opposite = affineHash(Input, true, Resources);
    auto Hash = affineHash(Input, false, Resources);
    if (!Opposite || !Hash)
      return false;
    SymRef Source;
    auto [First, Last] = ByHash.equal_range(*Opposite);
    for (auto It = First; It != Last; ++It) {
      if (!Resources.charge(1))
        return false;
      const auto &[Earlier, Candidate] = Inputs[It->second];
      if (areComplements(Input, Candidate, Resources)) {
        Source = OriginalRewritten.lookup(Earlier);
        break;
      }
      if (!Resources.complete())
        return false;
    }
    // A previously forced complete source is an affine boundary. Recognise
    // its direct complement without expanding that source or chasing aliases.
    if (!Source && Input.Terms.size() == 1 && Input.Offset.isAllOnes() &&
        Input.Terms.begin()->second.isAllOnes() &&
        ForcedAtoms.contains(Input.Terms.begin()->first))
      Source = OriginalRewritten.lookup(Input.Terms.begin()->first);
    if (Source) {
      if (!Resources.charge(1, 128))
        return false;
      Sources[OriginalRewritten.lookup(Index).index()] = Source;
    }
    if (!Resources.charge(1, 128))
      return false;
    ByHash.emplace(*Hash, Inputs.size());
    return true;
  }

  SymRef apply(SymContext &Ctx, SymRef Body, AffineResources &Resources) {
    if (Sources.empty() || !Resources.complete())
      return Body;
    std::vector<uint32_t> Order;
    if (!affineOrder(Ctx, Body, Order, Resources))
      return Body;
    llvm::DenseMap<uint32_t, SymRef> Changed;
    for (uint32_t Index : Order) {
      if (!Resources.charge(1))
        return Body;
      SymRef R(Index);
      auto Source = Sources.find(Index);
      SymRef Replacement = R;
      if (Source != Sources.end()) {
        if (!Resources.charge(1, 128))
          return Body;
        Replacement = Ctx.mkNot(Source->second);
      } else {
        bool HasChanged = false;
        for (SymRef C : Ctx.operands(R)) {
          if (!Resources.charge(1))
            return Body;
          HasChanged |= Changed.contains(C.index());
        }
        if (!HasChanged)
          continue;
        if (!Resources.array(Ctx.numOperands(R), 2 * sizeof(SymRef)))
          return Body;
        llvm::SmallVector<SymRef, 8> Ops;
        for (SymRef C : Ctx.operands(R)) {
          auto It = Changed.find(C.index());
          Ops.push_back(It == Changed.end() ? C : It->second);
        }
        if (!chargeAffineNode(Ctx, Ctx.op(R), Ops, Resources))
          return Body;
        Replacement = Ctx.rebuild(R, Ops);
      }
      if (Replacement != R) {
        if (!Resources.charge(1, 128))
          return Body;
        Changed[Index] = Replacement;
      }
    }
    auto It = Changed.find(Body.index());
    return It == Changed.end() ? Body : It->second;
  }
};

// Complementary affine offsets have opposite parity. Inspect only immediate
// terms; a nested linear source keeps the full relation search enabled. A
// forced source also stays eligible for the direct -Source-1 relation.
bool mayHaveComplements(const SymContext &Ctx, llvm::ArrayRef<uint32_t> Roots,
                        const llvm::DenseSet<uint32_t> &ForcedAtoms,
                        const llvm::DenseMap<uint32_t, Role> &Roles,
                        AffineResources &Resources) {
  if (Roots.size() < 2)
    return false;
  auto termParity = [&](SymRef R, bool IsRoot) -> std::optional<bool> {
    if (!Resources.charge(1))
      return std::nullopt;
    if (Ctx.isConst(R))
      return Ctx.constValue(R)[0];
    if (!IsRoot) {
      if (ForcedAtoms.contains(R.index()))
        return std::nullopt;
      if (Roles.lookup(R.index()) != Role::Linear)
        return false;
    }
    if (Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
        Ctx.isConst(Ctx.operand(R, 0))) {
      if (!Ctx.constValue(Ctx.operand(R, 0))[0])
        return false;
      SymRef Source = Ctx.operand(R, 1);
      if (ForcedAtoms.contains(Source.index()) ||
          Roles.lookup(Source.index()) == Role::Linear)
        return std::nullopt;
      return false;
    }
    if (Ctx.op(R) == SymOp::Add || Ctx.op(R) == SymOp::Not)
      return std::nullopt;
    return false;
  };
  unsigned Parities = 0;
  for (uint32_t Index : Roots) {
    SymRef R(Index);
    bool Parity = false;
    if (Ctx.op(R) == SymOp::Add) {
      if (!Resources.charge(1))
        return true;
      for (SymRef Term : Ctx.operands(R)) {
        auto Part = termParity(Term, false);
        if (!Part)
          return true;
        Parity ^= *Part;
      }
    } else {
      auto Part = termParity(R, true);
      if (!Part)
        return true;
      Parity = *Part;
    }
    Parities |= 1u << Parity;
    if (Parities == 3)
      return true;
  }
  return false;
}

/// Recover arithmetic uses of bases from exact hidden affine relations.
/// Subtracting a candidate may fold -(-x) to x, or flatten a+(a+b) to 2*a+b.
/// Reabstracting those forms must not forget the relation to the hidden input.
/// For P = c*T + Rest with odd c, T = inverse(c)*(P-Rest) modulo the word
/// width. This substitution is valid for every original assignment, while the
/// later coefficient proof still quantifies over all abstract inputs
/// independently.
SymRef restoreAffineRelations(
    SymContext &Ctx, SymRef Body, llvm::ArrayRef<uint32_t> OriginalOrder,
    const llvm::DenseSet<uint32_t> &ForcedAtoms,
    const llvm::DenseMap<uint32_t, Role> &OriginalRoles,
    const llvm::DenseMap<uint32_t, SymRef> &OriginalRewritten,
    bool AllowProducts, WorkBudget *Budget, size_t MaxBytes) {
  if (ForcedAtoms.empty())
    return Body;
  AffineResources Resources(Budget, MaxBytes);
  llvm::SmallVector<uint32_t, 4> Roots;
  for (uint32_t Index : OriginalOrder) {
    if (!Resources.charge(1))
      return Body;
    if (ForcedAtoms.contains(Index)) {
      if (!Resources.array(1, 2 * sizeof(uint32_t)))
        return Body;
      Roots.push_back(Index);
    }
  }
  const bool MayComplement =
      mayHaveComplements(Ctx, Roots, ForcedAtoms, OriginalRoles, Resources);
  if (!Resources.complete())
    return Body;
  AffineCache Cache;
  if (!cacheSharedAffineInputs(Ctx, Roots, ForcedAtoms, OriginalRoles, Cache,
                               Resources))
    return Body;
  AffineInputs Inputs;
  ComplementInputs Complements;
  bool HasOddInputs = false;
  WeightedInputs Weighted;
  for (uint32_t Index : Roots) {
    if (!recordWeightedInput(Ctx, Index, OriginalRewritten, Weighted,
                             Resources))
      return Body;
    auto Input = affineInput(Ctx, SymRef(Index), ForcedAtoms, OriginalRoles,
                             Cache, Resources);
    if (!Input)
      return Body;
    if (Input->Terms.contains(Index))
      continue;
    const bool HasOdd = llvm::any_of(
        Input->Terms, [](const auto &Term) { return Term.second[0]; });
    if (MayComplement && !Complements.record(Inputs, Index, *Input, ForcedAtoms,
                                             OriginalRewritten, Resources))
      return Body;
    if (!HasOdd && !MayComplement)
      continue;
    HasOddInputs |= HasOdd;
    if (!Resources.array(1, 2 * sizeof(std::pair<uint32_t, AffineInput>)))
      return Body;
    Inputs.emplace_back(Index, std::move(*Input));
  }
  auto finish = [&](SymRef Result) {
    Result = Complements.apply(Ctx, Result, Resources);
    return Resources.complete() ? Result : Body;
  };
  if (!HasOddInputs && Weighted.empty())
    return finish(Body);

  std::vector<uint32_t> Order;
  if (!affineOrder(Ctx, Body, Order, Resources))
    return Body;
  if (!HasOddInputs)
    return finish(restoreWeightedUses(Ctx, Body, Order, Weighted, Resources));
  if (!Resources.array(Order.size(), 320))
    return Body;
  llvm::DenseMap<uint32_t, Role> Roles;
  classify(Ctx, Order, {}, AllowProducts, Roles);
  llvm::DenseSet<uint32_t> VisibleBitwise;
  for (uint32_t Index : Order) {
    if (!Resources.charge(1))
      return Body;
    if (Roles.lookup(Index) == Role::Bitwise) {
      VisibleBitwise.insert(Index);
      for (SymRef C : Ctx.operands(SymRef(Index))) {
        if (!Resources.charge(1))
          return Body;
        VisibleBitwise.insert(C.index());
      }
    }
  }

  llvm::DenseMap<uint32_t, SymRef> Aliases;
  for (const auto &[Index, Input] : Inputs) {
    auto Pivot = Input.Terms.end();
    for (auto It = Input.Terms.begin(); It != Input.Terms.end(); ++It) {
      if (!Resources.charge(1))
        return Body;
      SymRef Base = OriginalRewritten.lookup(It->first);
      if (!It->second[0] || Aliases.contains(Base.index()))
        continue;
      if (Pivot == Input.Terms.end())
        Pivot = It;
      // A visible bitwise base must keep its own independent input.  Prefer
      // eliminating one that appears only in arithmetic positions instead.
      if (!VisibleBitwise.contains(Base.index())) {
        Pivot = It;
        break;
      }
    }
    if (Pivot == Input.Terms.end())
      continue;

    if (!Resources.charge(1, 128) ||
        !Resources.array(Input.Offset.getNumWords(), sizeof(uint64_t)) ||
        !Resources.array(Input.Terms.size(), 2 * sizeof(SymRef)))
      return Body;
    llvm::SmallVector<SymRef, 8> Rest{Ctx.mkConst(Input.Offset)};
    for (const auto &[Base, Coefficient] : Input.Terms) {
      if (!Resources.charge(1))
        return Body;
      if (Base == Pivot->first)
        continue;
      if (!Resources.charge(1, 128) ||
          !Resources.array(Coefficient.getNumWords(), sizeof(uint64_t)))
        return Body;
      SymRef Factors[] = {Ctx.mkConst(Coefficient),
                          OriginalRewritten.lookup(Base)};
      if (!chargeAffineNode(Ctx, SymOp::Mul, Factors, Resources))
        return Body;
      Rest.push_back(Ctx.mkMul(Factors));
    }
    auto Inverse = inverseOdd(Pivot->second, Resources);
    if (!Inverse || !chargeAffineNode(Ctx, SymOp::Add, Rest, Resources))
      return Body;
    SymRef RestSum = Ctx.mkAdd(Rest);
    if (!Resources.charge(1, 128) ||
        !Resources.array(Inverse->getNumWords(), sizeof(uint64_t)))
      return Body;
    SymRef Negative[] = {Ctx.mkOnes(Ctx.width(RestSum)), RestSum};
    if (!chargeAffineNode(Ctx, SymOp::Mul, Negative, Resources))
      return Body;
    SymRef Difference[] = {OriginalRewritten.lookup(Index),
                           Ctx.mkMul(Negative)};
    if (!chargeAffineNode(Ctx, SymOp::Add, Difference, Resources))
      return Body;
    SymRef Factors[] = {Ctx.mkConst(*Inverse), Ctx.mkAdd(Difference)};
    if (!chargeAffineNode(Ctx, SymOp::Mul, Factors, Resources) ||
        !Resources.charge(1, 64))
      return Body;
    SymRef Alias = Ctx.mkMul(Factors);
    Aliases[OriginalRewritten.lookup(Pivot->first).index()] = Alias;
  }
  if (Aliases.empty() && Weighted.empty())
    return finish(Body);

  // Templates use only the completed original abstraction, never one another.
  // The fixed traversal excludes newly built templates, so shared pivots and
  // dependent relations cannot recursively expand aliases or create cycles.
  llvm::DenseMap<uint32_t, SymRef> Rewritten;
  llvm::DenseMap<uint32_t, SymRef> Arithmetic;
  for (uint32_t Index : Order) {
    if (!Resources.charge(1))
      return Body;
    SymRef R(Index);
    const bool IsArithmetic = Roles.lookup(Index) == Role::Linear;
    if (!Resources.array(Ctx.numOperands(R), 2 * sizeof(SymRef)))
      return Body;
    llvm::SmallVector<SymRef, 8> Ops;
    for (SymRef C : Ctx.operands(R))
      Ops.push_back(IsArithmetic ? Arithmetic.lookup(C.index())
                                 : Rewritten.lookup(C.index()));
    if (!Ops.empty() && !chargeAffineNode(Ctx, Ctx.op(R), Ops, Resources))
      return Body;
    SymRef Rebuilt = Ops.empty() ? R : Ctx.rebuild(R, Ops);
    Rewritten[Index] = Rebuilt;
    auto It = Aliases.find(Index);
    Arithmetic[Index] = It == Aliases.end() ? Rebuilt : It->second;
    if (IsArithmetic) {
      bool Complete = true;
      auto Replacement = weightedTerm(Ctx, R, Weighted, Resources, Complete);
      if (!Complete)
        return Body;
      if (Replacement)
        Arithmetic[Index] = *Replacement;
    }
  }
  return finish(Rewritten.lookup(Body.index()));
}

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
                                         bool AllowProducts, WorkBudget *Budget,
                                         size_t MaxBytes) {
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
  llvm::DenseMap<uint32_t, SymRef> HiddenInputs;
  auto Hide = [&](SymRef R) {
    auto It = HiddenInputs.find(R.index());
    if (It != HiddenInputs.end())
      return It->second;
    SymRef V = Pool.take(Ctx.width(R));
    HiddenInputs[R.index()] = V;
    Out.Hidden.emplace(V.index(), R);
    return V;
  };

  // Zero extension commutes with AND/OR/XOR, but not with narrow arithmetic.
  // Measure the widened bitwise view over shared opaque widened leaves. This
  // preserves relations after the DAG builders contract zero extensions,
  // without moving a narrow carry or complement into the wider word.
  AffineResources WidenResources(Budget, MaxBytes);
  llvm::DenseMap<uint64_t, SymRef> Widened;
  auto WidenBitwise = [&](auto &&Self, SymRef R, unsigned Width,
                          unsigned Depth) -> std::optional<SymRef> {
    if (Depth >= 32 || !WidenResources.charge(1))
      return std::nullopt;
    const uint64_t Key = (uint64_t{Width} << 32) | R.index();
    if (auto It = Widened.find(Key); It != Widened.end())
      return It->second;
    if (!WidenResources.charge(1, 128))
      return std::nullopt;
    const auto Op = Ctx.op(R);
    SymRef Value;
    if (!isBitwise(Op)) {
      if (!WidenResources.array((size_t{Width} + 63) / 64, sizeof(uint64_t)))
        return std::nullopt;
      Value = Hide(Ctx.mkZExt(R, Width));
    } else if (Op == SymOp::Not) {
      auto Inner = Self(Self, Ctx.operand(R, 0), Width, Depth + 1);
      if (!Inner ||
          !WidenResources.array((size_t{Width} + 63) / 64, sizeof(uint64_t)))
        return std::nullopt;
      // A narrow complement flips only its original bits after widening.
      auto Mask = Ctx.mkConst(llvm::APInt::getLowBitsSet(Width, Ctx.width(R)));
      SymRef Operands[] = {*Inner, Hide(Mask)};
      if (!chargeAffineNode(Ctx, SymOp::Xor, Operands, WidenResources))
        return std::nullopt;
      Value = Ctx.mkXor(Operands);
    } else {
      if (!WidenResources.array(Ctx.numOperands(R), 2 * sizeof(SymRef)))
        return std::nullopt;
      auto Source = Ctx.operands(R);
      llvm::SmallVector<SymRef, 8> Operands(Source.begin(), Source.end());
      for (SymRef &Operand : Operands) {
        auto Wide = Self(Self, Operand, Width, Depth + 1);
        if (!Wide)
          return std::nullopt;
        Operand = *Wide;
      }
      if (!chargeAffineNode(Ctx, Op, Operands, WidenResources))
        return std::nullopt;
      if (Op == SymOp::And)
        Value = Ctx.mkAnd(Operands);
      else if (Op == SymOp::Or)
        Value = Ctx.mkOr(Operands);
      else
        Value = Ctx.mkXor(Operands);
    }
    Widened[Key] = Value;
    return Value;
  };

  for (uint32_t Index : Order) {
    SymRef R(Index);
    if (Roles.lookup(Index) == Role::Atom) {
      if (Ctx.isVar(R)) {
        Rewritten[Index] = R;
        continue;
      }
      if (Ctx.op(R) == SymOp::ZExt) {
        SymRef Inner = Ctx.operand(R, 0);
        const auto Op = Ctx.op(Inner);
        if (isBitwise(Op))
          if (auto Wide = WidenBitwise(WidenBitwise, Inner, Ctx.width(R), 0)) {
            Rewritten[Index] = *Wide;
            continue;
          }
      }
      // One placeholder per distinct subterm, so a term the obfuscator
      // repeated stays recognisably the same term.
      Rewritten[Index] = Hide(R);
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

      NewOps.push_back(Hide(C));
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

  Out.IndependentBody = Rewritten.lookup(Root.index());
  Out.Body =
      restoreAffineRelations(Ctx, Out.IndependentBody, Order, ForcedAtoms,
                             Roles, Rewritten, AllowProducts, Budget, MaxBytes);
  return Out;
}

} // namespace neverd::symbolic::detail
