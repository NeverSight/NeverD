//===- SymExprMask.cpp - Local word-mask reasoning -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymExprMask.h"

#include <map>
#include <optional>
#include <vector>

namespace neverd::symbolic {
namespace {

bool hasConstantMask(const SymContext &Ctx, SymRef R) {
  return Ctx.op(R) == SymOp::And && Ctx.isConst(Ctx.operand(R, 0));
}

bool mayRestoreField(const SymContext &C, SymRef R) {
  if (C.op(R) == SymOp::Mul && C.numOperands(R) == 2 &&
      C.isConst(C.operand(R, 0)))
    R = C.operand(R, 1);
  return C.op(R) == SymOp::ZExt;
}

struct RestoredField {
  SymRef Source;
  llvm::APInt Mask;
};

// A narrowed field shifted back to its original position is a masked view of
// the complete word. Inspect only these immediate wrappers, without expanding
// the word or asking a solver. Signed extension and moved/dynamic fields keep
// their original expression.
std::optional<RestoredField> restoredField(const SymContext &C, SymRef R) {
  const auto Width = C.width(R);
  unsigned Position = 0;
  if (C.op(R) == SymOp::Mul && C.numOperands(R) == 2 &&
      C.isConst(C.operand(R, 0))) {
    const auto Factor = C.constValue(C.operand(R, 0));
    if (!Factor.isPowerOf2())
      return {};
    Position = Factor.logBase2();
    R = C.operand(R, 1);
  }
  if (C.op(R) != SymOp::ZExt)
    return {};
  R = C.operand(R, 0);
  auto Mask = llvm::APInt::getLowBitsSet(Width, C.width(R));
  if (hasConstantMask(C, R) && C.numOperands(R) == 2) {
    Mask &= C.constValue(C.operand(R, 0)).zext(Width);
    R = C.operand(R, 1);
  }
  if (C.op(R) != SymOp::Extract)
    return {};
  uint64_t Low = C.node(R).Aux;
  R = C.operand(R, 0);
  if (C.op(R) == SymOp::LShr && C.isConst(C.operand(R, 1))) {
    const auto Count = C.constValue(C.operand(R, 1));
    if (Count.uge(C.width(R)))
      return {};
    Low += Count.getZExtValue();
    R = C.operand(R, 0);
  }
  if (C.width(R) != Width || Low != Position)
    return {};
  return RestoredField{R, Mask.shl(Position)};
}

struct MaskGroup {
  llvm::APInt Mask;
  llvm::SmallVector<size_t, 4> Positions;
  bool Overlaps = false;
};

} // namespace

SymRef SymContext::internMaskedSource(llvm::ArrayRef<SymRef> Source,
                                      const llvm::APInt &Mask) {
  assert(!Source.empty());
  if (Mask.isZero())
    return mkZero(Mask.getBitWidth());
  if (Mask.isAllOnes() && Source.size() == 1)
    return Source[0];
  llvm::SmallVector<SymRef, 8> Factors;
  if (!Mask.isAllOnes())
    Factors.push_back(mkConst(Mask));
  Factors.append(Source.begin(), Source.end());
  return intern(SymOp::And, Mask.getBitWidth(), Factors, 0);
}

bool SymContext::mergeMaskedOperands(llvm::SmallVectorImpl<SymRef> &Terms,
                                     bool RequireDisjoint) {
  unsigned MaskedCount = 0;
  uint64_t CommonBits = ~uint64_t(0);
  bool HasFields = false;
  for (SymRef R : Terms) {
    if (hasConstantMask(*this, R)) {
      if (RequireDisjoint) {
        const SymNode &Mask = node(operand(R, 0));
        CommonBits &=
            Mask.Width <= 64 ? Mask.Aux : WideConsts[Mask.Aux].getRawData()[0];
      }
    } else if (mayRestoreField(*this, R)) {
      HasFields = true;
    } else {
      continue;
    }
    ++MaskedCount;
    if (MaskedCount >= 2 && (!RequireDisjoint || HasFields || CommonBits == 0))
      break;
  }
  // A common low-word bit prevents every pair from being disjoint. Ignoring
  // higher words keeps this sufficient check allocation-free; its failure
  // still requires grouping rather than assuming masks are disjoint.
  if (MaskedCount < 2 || (RequireDisjoint && !HasFields && CommonBits != 0))
    return false;

  // AND is variadic, so its non-constant operand list identifies the source.
  // Copy these immediate edges before any builder can grow the operand pool.
  std::map<std::vector<SymRef>, MaskGroup> Groups;
  for (size_t I = 0; I < Terms.size(); ++I) {
    SymRef R = Terms[I];
    std::vector<SymRef> Source;
    llvm::APInt Mask(width(R), 0);
    if (hasConstantMask(*this, R)) {
      const auto Ops = operands(R);
      Source.assign(Ops.begin() + 1, Ops.end());
      Mask = constValue(Ops[0]);
    } else if (auto Field = restoredField(*this, R)) {
      Mask = std::move(Field->Mask);
      if (op(Field->Source) == SymOp::And) {
        for (auto Factor : operands(Field->Source))
          if (isConst(Factor))
            Mask &= constValue(Factor);
          else
            Source.push_back(Factor);
      } else {
        Source.push_back(Field->Source);
      }
    } else {
      continue;
    }
    auto [It, Inserted] =
        Groups.try_emplace(std::move(Source), MaskGroup{Mask, {}, false});
    MaskGroup &Group = It->second;
    if (!Inserted) {
      Group.Overlaps |= !(Group.Mask & Mask).isZero();
      Group.Mask |= Mask;
    }
    Group.Positions.push_back(I);
  }

  bool Changed = false;
  for (const auto &[Source, Group] : Groups) {
    if (Group.Positions.size() < 2 || (RequireDisjoint && Group.Overlaps))
      continue;
    // Calling mkAnd here could recursively re-enter demanded-add rewriting
    // through the source. Reuse its already canonical factors instead.
    Terms[Group.Positions.front()] = internMaskedSource(Source, Group.Mask);
    for (size_t I = 1; I < Group.Positions.size(); ++I)
      Terms[Group.Positions[I]] = SymRef();
    Changed = true;
  }
  if (Changed)
    llvm::erase_if(Terms, [](SymRef R) { return !R.isValid(); });
  return Changed;
}

SymRef SymContext::simplifyLowMaskedAdd(SymRef Sum, const llvm::APInt &Mask) {
  if (op(Sum) != SymOp::Add || !Mask.isMask() || Mask.isAllOnes())
    return Sum;

  auto CoveredBase = [&](SymRef Term) {
    if (op(Term) == SymOp::Mul && numOperands(Term) == 2 &&
        isConst(operand(Term, 0)))
      Term = operand(Term, 1);
    if (!hasConstantMask(*this, Term) ||
        (constValue(operand(Term, 0)) & Mask) != Mask)
      return SymRef();
    return Term;
  };
  auto Ops = operands(Sum);
  if (llvm::none_of(Ops, [&](SymRef R) { return CoveredBase(R).isValid(); }))
    return Sum;
  llvm::SmallVector<SymRef, 8> Terms(Ops.begin(), Ops.end());

  // Collect repeated sources before mkAdd can flatten them. A shared sum
  // exposed by several masks must not expand once per masked occurrence.
  // Canonical sums may already encode repeated masks with a coefficient.
  std::map<SymRef, llvm::APInt> Counts;
  for (SymRef Term : Terms) {
    llvm::APInt Coeff(Mask.getBitWidth(), 1);
    if (SymRef Base = CoveredBase(Term); Base.isValid()) {
      if (Base != Term)
        Coeff = constValue(operand(Term, 0));
      auto Factors = operands(Base);
      llvm::SmallVector<SymRef, 8> Source(Factors.begin() + 1, Factors.end());
      Term = internMaskedSource(Source,
                                llvm::APInt::getAllOnes(Mask.getBitWidth()));
    }
    auto [It, Inserted] = Counts.try_emplace(Term, Coeff);
    if (!Inserted)
      It->second += Coeff;
  }
  Terms.clear();
  for (const auto &[Source, Count] : Counts) {
    if (Count.isZero())
      continue;
    Terms.push_back(Count.isOne() ? Source : mkMul(mkConst(Count), Source));
  }
  // Carries into the low prefix depend only on that prefix of each addend.
  // The caller retains Mask; the new sum's high bits need not match Sum.
  return Terms.empty() ? mkZero(Mask.getBitWidth()) : mkAdd(Terms);
}

std::optional<unsigned> detail::matchingShiftPower(const SymContext &Ctx,
                                                   SymRef Count,
                                                   const llvm::APInt &Factor) {
  if (!Factor.isPowerOf2() || !Ctx.isConst(Count))
    return std::nullopt;
  unsigned K = Factor.logBase2();
  if (Ctx.constValue(Count).getLimitedValue() != K)
    return std::nullopt;
  return K;
}

} // namespace neverd::symbolic
