//===- SymMBAMaskedSum.cpp - Weighted literal-mask sums -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize and independently certify weighted bitwise views of opaque words.

#include "Limits.h"
#include "SymMBADetail.h"

#include <map>
#include <vector>

namespace neverd::symbolic::detail {
namespace {

class MaskResources {
  WorkBudget &Work;
  size_t Bytes;

public:
  MaskResources(WorkBudget &Work, size_t Bytes) : Work(Work), Bytes(Bytes) {}

  size_t remainingBytes() const { return Bytes; }

  bool charge(size_t Units, size_t Storage = 0) {
    if (Storage > Bytes || !Work.canConsume(Units))
      return false;
    Work.consume(Units);
    Bytes -= Storage;
    return true;
  }

  bool array(size_t Count, size_t ElementBytes) {
    return (!ElementBytes || Count <= Bytes / ElementBytes) &&
           charge(Count, Count * ElementBytes);
  }

  bool product(size_t A, size_t B) {
    return (!A || B <= std::numeric_limits<size_t>::max() / A) && charge(A * B);
  }
};

struct MaskTerm {
  SymRef Original;
  llvm::APInt Coefficient;
  llvm::APInt Mask;
};

struct CoefficientLess {
  bool operator()(const llvm::APInt &A, const llvm::APInt &B) const {
    return A.ult(B);
  }
};

bool isLiteralView(const SymContext &Ctx, SymRef R) {
  if (Ctx.op(R) == SymOp::Not)
    return true;
  return (Ctx.op(R) == SymOp::And || Ctx.op(R) == SymOp::Or ||
          Ctx.op(R) == SymOp::Xor) &&
         Ctx.numOperands(R) >= 2 && Ctx.isConst(Ctx.operand(R, 0));
}

struct BitwiseView {
  SymRef Carrier;
  llvm::APInt Zero, One;
};

// Compose unary literal maps from the outside in. The two words describe
// each output bit when the corresponding carrier bit is zero or one.
// Arithmetic inside the remaining carrier is opaque.
std::optional<BitwiseView> collectView(SymContext &Ctx, SymRef R,
                                       MaskResources &Resources) {
  const unsigned Width = Ctx.width(R);
  const size_t Words = (size_t(Width) + 63) / 64;
  BitwiseView View{R, llvm::APInt(Width, 0), llvm::APInt::getAllOnes(Width)};
  for (unsigned Depth = 0; isLiteralView(Ctx, View.Carrier); ++Depth) {
    if (Depth == ::neverd::symbolic::detail::limits::kMaxMaskedSumViewDepth ||
        !Resources.charge(1 + Words, 64 + 2 * Words * 8))
      return std::nullopt;
    const auto Op = Ctx.op(View.Carrier);
    if (Op == SymOp::Not) {
      std::swap(View.Zero, View.One);
      View.Carrier = Ctx.operand(View.Carrier, 0);
      continue;
    }
    const auto Ops = Ctx.operands(View.Carrier);
    if (!Resources.array(Ops.size(), 2 * sizeof(SymRef)))
      return std::nullopt;
    auto Literal = Ctx.constValue(Ops[0]);
    llvm::SmallVector<SymRef, 8> Inputs(Ops.begin() + 1, Ops.end());
    if (Op == SymOp::And) {
      View.One = (View.One & Literal) | (View.Zero & ~Literal);
      View.Carrier = Ctx.mkAnd(Inputs);
    } else if (Op == SymOp::Or) {
      View.Zero = (View.One & Literal) | (View.Zero & ~Literal);
      View.Carrier = Ctx.mkOr(Inputs);
    } else {
      auto Zero = (View.Zero & ~Literal) | (View.One & Literal);
      View.One = (View.One & ~Literal) | (View.Zero & Literal);
      View.Zero = std::move(Zero);
      View.Carrier = Ctx.mkXor(Inputs);
    }
  }
  return View;
}

// This checker does not use collectView or the candidate's coefficient table.
// Establish the pointwise grammar recursively, then evaluate its two Boolean
// assignments using full-width bitwise operations. Checking arbitrary
// arithmetic at zero/ones would not establish that grammar.
std::optional<std::pair<llvm::APInt, llvm::APInt>>
checkBitwiseView(const SymContext &Ctx, SymRef R, SymRef Carrier,
                 MaskResources &Resources, unsigned Depth = 0) {
  const unsigned Width = Ctx.width(Carrier);
  const size_t Words = (size_t(Width) + 63) / 64;
  if (Depth == ::neverd::symbolic::detail::limits::kMaxMaskedSumViewDepth ||
      Ctx.width(R) != Width || !Resources.charge(1 + Words, 64 + 2 * Words * 8))
    return std::nullopt;
  if (R == Carrier)
    return std::pair(llvm::APInt(Width, 0), llvm::APInt::getAllOnes(Width));
  if (Ctx.isConst(R))
    return std::pair(Ctx.constValue(R), Ctx.constValue(R));
  const auto Op = Ctx.op(R);
  if (Op == SymOp::Not) {
    auto Child =
        checkBitwiseView(Ctx, Ctx.operand(R, 0), Carrier, Resources, Depth + 1);
    if (!Child)
      return std::nullopt;
    return std::pair(~Child->first, ~Child->second);
  }
  if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
    return std::nullopt;
  auto Ops = Ctx.operands(R);
  if (!Resources.charge(Ops.size()))
    return std::nullopt;
  // Canonical n-ary construction flattens literal XOR/AND/OR around a carrier.
  if (Ctx.op(Carrier) == Op && Ctx.isConst(Ops[0]) &&
      Ops.drop_front() == Ctx.operands(Carrier)) {
    const auto &K = Ctx.constValue(Ops[0]);
    if (Op == SymOp::And)
      return std::pair(llvm::APInt(Width, 0), K);
    if (Op == SymOp::Or)
      return std::pair(K, llvm::APInt::getAllOnes(Width));
    return std::pair(K, ~K);
  }
  auto Zero =
      Op == SymOp::And ? llvm::APInt::getAllOnes(Width) : llvm::APInt(Width, 0);
  auto One = Zero;
  for (SymRef Child : Ops) {
    auto Value = checkBitwiseView(Ctx, Child, Carrier, Resources, Depth + 1);
    if (!Value)
      return std::nullopt;
    if (Op == SymOp::And) {
      Zero &= Value->first;
      One &= Value->second;
    } else if (Op == SymOp::Or) {
      Zero |= Value->first;
      One |= Value->second;
    } else {
      Zero ^= Value->first;
      One ^= Value->second;
    }
  }
  return std::pair(std::move(Zero), std::move(One));
}

// Read a proposed equality independently of the coefficient-group builder.
// Sums, constant scaling and pointwise bitwise views of this exact carrier are
// affine in its individual bits. Other words must cancel by exact identity.
bool proveMaskIdentity(const SymContext &Ctx, SymRef Before, SymRef After,
                       SymRef Carrier, MaskResources &Resources) {
  const unsigned Width = Ctx.width(Carrier);
  const size_t Words = Width / 64 + (Width % 64 != 0);
  const size_t EntryBytes = 128 + 3 * Words * sizeof(uint64_t);
  struct Item {
    SymRef Expr;
    llvm::APInt Scale;
  };
  if (!Resources.array(2, EntryBytes))
    return false;
  std::vector<Item> Work{{Before, llvm::APInt(Width, 1)},
                         {After, llvm::APInt::getAllOnes(Width)}};
  std::vector<MaskTerm> Terms;
  std::map<uint32_t, llvm::APInt> Residuals;
  llvm::APInt Constant(Width, 0);
  auto append = [&](const llvm::APInt &Scale, const llvm::APInt &Mask) {
    if (!Resources.charge(Words, EntryBytes))
      return false;
    Terms.push_back({{}, Scale, Mask});
    return true;
  };
  auto residual = [&](SymRef R, const llvm::APInt &Scale) {
    if (!Resources.charge(Words, EntryBytes))
      return false;
    auto [It, Added] = Residuals.try_emplace(R.index(), Width, 0);
    It->second += Scale;
    return true;
  };
  auto bitwise = [&](SymRef R, const llvm::APInt &Scale) {
    auto Value = checkBitwiseView(Ctx, R, Carrier, Resources);
    if (!Value)
      return residual(R, Scale);
    if (!Resources.product(Words, Words))
      return false;
    const auto &[Zero, One] = *Value;
    Constant += Scale * Zero;
    auto Positive = One & ~Zero, Negative = Zero & ~One;
    return (Positive.isZero() || append(Scale, Positive)) &&
           (Negative.isZero() || append(-Scale, Negative));
  };
  while (!Work.empty()) {
    Item Current = std::move(Work.back());
    Work.pop_back();
    SymRef R = Current.Expr;
    if (!Resources.charge(1 + Words) || Ctx.width(R) != Width)
      return false;
    if (R == Carrier) {
      if (!append(Current.Scale, llvm::APInt::getAllOnes(Width)))
        return false;
      continue;
    }
    if (Ctx.isConst(R)) {
      if (!Resources.product(Words, Words))
        return false;
      Constant += Current.Scale * Ctx.constValue(R);
      continue;
    }
    auto Ops = Ctx.operands(R);
    if (Ctx.op(R) == SymOp::Add) {
      if (!Resources.array(Ops.size(), EntryBytes))
        return false;
      for (SymRef Operand : Ops)
        Work.push_back({Operand, Current.Scale});
    } else if (Ctx.op(R) == SymOp::Mul && Ctx.isConst(Ops[0])) {
      if (!Resources.product(Words, Words))
        return false;
      const auto Scale = Current.Scale * Ctx.constValue(Ops[0]);
      if (Ops.size() == 2) {
        if (!Resources.charge(0, EntryBytes))
          return false;
        Work.push_back({Ops[1], Scale});
      } else {
        // The canonical builder flattens K * (x * y) into K * x * y.
        // Match all factors exactly; unrelated products remain unsupported.
        if (!Resources.charge(Ops.size()))
          return false;
        if (Ctx.op(Carrier) == SymOp::Mul &&
            Ops.drop_front() == Ctx.operands(Carrier)) {
          if (!append(Scale, llvm::APInt::getAllOnes(Width)))
            return false;
        } else if (!residual(R, Current.Scale)) {
          return false;
        }
      }
    } else if (Ctx.op(R) == SymOp::And && Ops.size() >= 2 &&
               Ctx.isConst(Ops[0])) {
      auto Inputs = Ops.drop_front();
      if (!Resources.charge(Inputs.size()))
        return false;
      const bool SameCarrier =
          (Inputs.size() == 1 && Inputs[0] == Carrier) ||
          (Ctx.op(Carrier) == SymOp::And && Inputs == Ctx.operands(Carrier));
      if (!(SameCarrier ? append(Current.Scale, Ctx.constValue(Ops[0]))
                        : bitwise(R, Current.Scale)))
        return false;
    } else if (Ctx.op(R) == SymOp::Not || Ctx.op(R) == SymOp::Or ||
               Ctx.op(R) == SymOp::Xor) {
      if (!bitwise(R, Current.Scale))
        return false;
    } else if (!residual(R, Current.Scale)) {
      return false;
    }
  }
  if (!Constant.isZero())
    return false;
  for (const auto &[Index, Coefficient] : Residuals)
    if (!Resources.charge(Words) || !Coefficient.isZero())
      return false;
  // A word is the sum of its one-bit basis vectors. The certified grammar
  // above distributes over that sum, even when arithmetic carries occur.
  // Check the actual shifted contribution, including its modular overflow,
  // instead of trusting the synthesizer's unshifted coefficient buckets.
  for (unsigned Bit = 0; Bit != Width; ++Bit) {
    llvm::APInt Difference(Width, 0);
    for (const auto &Term : Terms) {
      if (!Resources.charge(1 + Words))
        return false;
      if (Term.Mask[Bit])
        Difference += Term.Coefficient.shl(Bit);
    }
    if (!Difference.isZero())
      return false;
  }
  return true;
}

} // namespace

bool proveMaskedSumIdentity(const SymContext &Ctx, SymRef Before, SymRef After,
                            SymRef Carrier, WorkBudget &Budget,
                            size_t MaxBytes) {
  if (!Before.isValid() || !After.isValid() || !Carrier.isValid())
    return false;
  MaskResources Resources(Budget, MaxBytes);
  return proveMaskIdentity(Ctx, Before, After, Carrier, Resources);
}

SymRef solveWeightedMasks(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          WorkBudget &Budget, SolveReport &Rep) {
  if (Ctx.op(E) != SymOp::Add ||
      Opts.MaxTableBytes <
          ::neverd::symbolic::detail::limits::
              kSymMBAMaskedSumSolveWeightedMasksOptsMaxTableBytesLimit)
    return E;
  const auto Summands = Ctx.operands(E);
  if (!Budget.canConsume(Summands.size()))
    return E;
  // A repeated underlying nonliteral operand suggests a shared carrier.
  // This allocation-free hint can have collisions; only the exact grouping
  // and proof below authorize a rewrite. Ordinary arithmetic pays no table
  // construction or coefficient work for an inapplicable reading.
  uint64_t Seen = 0;
  bool Plausible = false;
  for (SymRef Term : Summands) {
    if (Ctx.op(Term) == SymOp::Mul && Ctx.numOperands(Term) == 2 &&
        Ctx.isConst(Ctx.operand(Term, 0))) {
      Term = Ctx.operand(Term, 1);
      // Reassociation can put a common sign or scale around an additive group.
      if (Ctx.op(Term) == SymOp::Add) {
        Plausible = true;
        break;
      }
    }
    if (!isLiteralView(Ctx, Term))
      continue;
    for (unsigned Depth = 0;
         isLiteralView(Ctx, Term) &&
         Depth != ::neverd::symbolic::detail::limits::kMaxMaskedSumViewDepth;
         ++Depth) {
      if (!Budget.consume())
        return E;
      Term = Ctx.operand(Term, Ctx.op(Term) == SymOp::Not ? 0 : 1);
    }
    const uint64_t Bit = uint64_t(1) << (Term.index() % 64);
    if (Seen & Bit) {
      Plausible = true;
      break;
    }
    Seen |= Bit;
  }
  if (!Plausible)
    return E;
  const unsigned Width = Ctx.width(E);
  const size_t Words = Width / 64 + (Width % 64 != 0);
  const size_t EntryBytes = 192 + 4 * Words * sizeof(uint64_t);
  MaskResources Resources(Budget, Opts.MaxTableBytes);
  if (!Resources.array(Summands.size(), EntryBytes))
    return E;
  // Reconstructing a flattened carrier can grow the context's operand pool.
  // Keep no borrowed operand range across any DAG-building call.
  llvm::SmallVector<SymRef, 8> OwnedSummands(Summands.begin(), Summands.end());
  struct Item {
    SymRef Expr;
    llvm::APInt Scale;
  };
  std::vector<Item> Pending;
  for (SymRef Term : OwnedSummands)
    Pending.push_back({Term, llvm::APInt(Width, 1)});
  bool Expanded = false;
  struct ViewTerm {
    SymRef Original;
    llvm::APInt Coefficient, Zero, One;
  };
  std::map<uint32_t, std::vector<ViewTerm>> Groups;
  llvm::SmallVector<SymRef, 8> Kept;
  while (!Pending.empty()) {
    auto [Body, Scale] = std::move(Pending.back());
    Pending.pop_back();
    if (!Resources.charge(1 + Words))
      return E;
    if (Ctx.op(Body) == SymOp::Add) {
      auto Children = Ctx.operands(Body);
      if (!Resources.array(Children.size(), EntryBytes))
        return E;
      for (SymRef Child : Children)
        Pending.push_back({Child, Scale});
      Expanded = true;
      continue;
    }
    if (Ctx.op(Body) == SymOp::Mul && Ctx.numOperands(Body) == 2 &&
        Ctx.isConst(Ctx.operand(Body, 0))) {
      if (!Resources.product(Words, Words) || !Resources.charge(0, EntryBytes))
        return E;
      Pending.push_back(
          {Ctx.operand(Body, 1), Scale * Ctx.constValue(Ctx.operand(Body, 0))});
      continue;
    }
    if (!isLiteralView(Ctx, Body)) {
      Kept.push_back(Scale.isOne() ? Body
                                   : Ctx.mkMul(Ctx.mkConst(Scale), Body));
      continue;
    }
    auto View = collectView(Ctx, Body, Resources);
    if (!View)
      return E;
    SymRef Original =
        Scale.isOne() ? Body : Ctx.mkMul(Ctx.mkConst(Scale), Body);
    Groups[View->Carrier.index()].push_back(
        {Original, Scale, View->Zero, View->One});
  }
  // Several words may share enclosing scales and subtractions. Preserve the
  // distributed input before changing any group: a separate arithmetic proof
  // will check it against E, including every scale and opaque residual.
  // Per-word bitwise proofs then compose by congruence over this exact sum.
  const bool Composed = Expanded && Groups.size() > 1;
  SymRef Distributed;
  if (Composed) {
    if (!Resources.array(Kept.size(), 64))
      return E;
    llvm::SmallVector<SymRef, 8> Originals(Kept);
    for (const auto &[Index, Terms] : Groups) {
      if (!Resources.array(Terms.size(), 64))
        return E;
      for (const auto &Term : Terms)
        Originals.push_back(Term.Original);
    }
    Distributed = Ctx.mkAdd(Originals);
  }

  SymRef GroupLiteral;
  if (Groups.size() == 1 &&
      llvm::any_of(Kept, [&](SymRef R) { return Ctx.isConst(R); })) {
    if (!Resources.array(Kept.size(), sizeof(SymRef)) ||
        !Resources.charge(1 + Words, 128 + 2 * Words * sizeof(uint64_t)))
      return E;
    llvm::SmallVector<SymRef, 4> Literals;
    for (SymRef R : Kept)
      if (Ctx.isConst(R))
        Literals.push_back(R);
    // Early word normalization may fold forced bits into a separate literal.
    // Let a single carrier choose its XOR orientation with that same offset.
    // Keep the original terms until their complete equality is proved below.
    GroupLiteral = Ctx.mkAdd(Literals);
  }

  bool Changed = false;
  for (const auto &[Index, Terms] : Groups) {
    auto retain = [&] {
      for (const auto &Term : Terms)
        Kept.push_back(Term.Original);
    };
    if (Terms.size() < 2) {
      retain();
      continue;
    }
    // Cover the coefficient vector, worst-case bucket tree, masks and emitted
    // DAG nodes before allocating. Charges remain cumulative across groups.
    if (!Resources.array(Width, 256 + 8 * Words * sizeof(uint64_t)))
      return E;
    std::vector<llvm::APInt> Coefficients(Width, llvm::APInt(Width, 0));
    llvm::APInt Constant =
        GroupLiteral ? Ctx.constValue(GroupLiteral) : llvm::APInt(Width, 0);
    for (const auto &Term : Terms) {
      if (!Resources.product(Words, Words))
        return E;
      Constant += Term.Coefficient * Term.Zero;
      for (unsigned Bit = 0; Bit != Width; ++Bit) {
        if (!Resources.charge(1 + Words))
          return E;
        if (Term.One[Bit] && !Term.Zero[Bit])
          Coefficients[Bit] += Term.Coefficient;
        else if (Term.Zero[Bit] && !Term.One[Bit])
          Coefficients[Bit] -= Term.Coefficient;
      }
    }
    std::map<llvm::APInt, llvm::APInt, CoefficientLess> Buckets;
    for (unsigned Bit = 0; Bit != Width; ++Bit) {
      if (!Resources.charge(1 + Words))
        return E;
      const auto &K = Coefficients[Bit];
      if (K.shl(Bit).isZero())
        continue;
      auto [It, Added] = Buckets.try_emplace(K, Width, 0);
      It->second.setBit(Bit);
    }
    SymRef Carrier(Index);
    llvm::SmallVector<SymRef, 8> Parts, Originals;
    SymRef SingleScale, SingleWord;
    while (!Buckets.empty()) {
      auto K = Buckets.begin()->first;
      auto Mask = Buckets.begin()->second;
      Buckets.erase(Buckets.begin());
      llvm::APInt Flip(Width, 0);
      // Opposite coefficients are one masked XOR plus a constant correction.
      // Erasing the first bucket also handles the self-opposite top-bit scale.
      if (auto Opposite = Buckets.find(-K); Opposite != Buckets.end()) {
        Flip = Opposite->second;
        Buckets.erase(Opposite);
        if (!Resources.product(Words, 2 * Words))
          return E;
        auto Forward = Constant - K * Flip;
        auto Reverse = Constant + K * Mask;
        // Both XOR orientations are exact. Prefer the one that removes the
        // constant, including negative weights and narrow modular weights.
        if (Reverse.isZero() && !Forward.isZero()) {
          K = -K;
          std::swap(Mask, Flip);
          Constant = std::move(Reverse);
        } else {
          Constant = std::move(Forward);
        }
        Mask |= Flip;
      }
      // An even coefficient cannot observe its highest input bits. Restoring
      // those bits can eliminate a mask altogether without modular division.
      auto ExpandedMask =
          Mask | llvm::APInt::getHighBitsSet(Width, K.countr_zero());
      SingleScale = Ctx.mkConst(K);
      SingleWord = Ctx.mkAnd(Ctx.mkXor(Carrier, Ctx.mkConst(Flip)),
                             Ctx.mkConst(ExpandedMask));
      Parts.push_back(Ctx.mkMul(SingleScale, SingleWord));
    }
    if (Parts.size() == 1 && !Constant.isZero()) {
      if (!Resources.charge(1 + Words, 2 * Words * sizeof(uint64_t)))
        return E;
      if (Constant == Ctx.constValue(SingleScale)) {
        if (!Resources.charge(3, 384 + 2 * Words * sizeof(uint64_t)))
          return E;
        // K + K*word == -K*~word. Emit the compact group before adding
        // unrelated residuals, whose flat sum would hide this completion.
        Parts.front() =
            Ctx.mkMul(Ctx.mkConst(-Constant), Ctx.mkNot(SingleWord));
        Constant.clearAllBits();
      }
    }
    if (!Constant.isZero())
      Parts.push_back(Ctx.mkConst(Constant));
    for (const auto &Term : Terms)
      Originals.push_back(Term.Original);
    if (GroupLiteral)
      Originals.push_back(GroupLiteral);
    SymRef Before = Ctx.mkAdd(Originals);
    SymRef Candidate = Parts.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(Parts);
    if (readingScore(Ctx, Candidate) >= readingScore(Ctx, Before) ||
        ((!Expanded || Composed) &&
         !proveMaskIdentity(Ctx, Before, Candidate, Carrier, Resources))) {
      retain();
      continue;
    }
    if (GroupLiteral) {
      if (!Resources.charge(Kept.size()))
        return E;
      llvm::erase_if(Kept, [&](SymRef R) { return Ctx.isConst(R); });
    }
    Kept.push_back(Candidate);
    Changed = true;
  }
  if (!Changed)
    return E;
  SymRef Answer = Ctx.mkAdd(Kept);
  if (readingScore(Ctx, Answer) >= readingScore(Ctx, E) ||
      (Expanded && !Composed &&
       !proveMaskIdentity(Ctx, E, Answer, SymRef(Groups.begin()->first),
                          Resources)))
    return E;
  // No fresh allowance: distribution must fit what all group construction and
  // proofs left. Failure cannot publish even a partially proved composition.
  if (Composed && !proveArithmeticIdentity(Ctx, E, Distributed, Budget,
                                           Resources.remainingBytes()))
    return E;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Answer;
}

} // namespace neverd::symbolic::detail
