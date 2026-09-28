//===- SymWideArithmetic.cpp - Proved split-word recovery -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymWideArithmetic.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace neverd::symbolic {

SymRef recoverSplitWordArithmetic(SymContext &Ctx, SymRef Expr,
                                  SynthVerifyFn Verify) {
  if (!Expr.isValid() || Ctx.op(Expr) != SymOp::Concat ||
      Ctx.numOperands(Expr) != 2)
    return Expr;
  const SymRef Upper = Ctx.operand(Expr, 0);
  const SymRef Lower = Ctx.operand(Expr, 1);
  const uint32_t Width = Ctx.width(Lower);
  if (Width < 8 || Width != Ctx.width(Upper) || Ctx.op(Lower) != SymOp::Add)
    return Expr;

  llvm::SmallVector<SymRef, 8> Inputs;
  llvm::SmallVector<SymRef, 32> Work{Expr};
  llvm::DenseSet<uint32_t> Seen;
  while (!Work.empty()) {
    const SymRef Current = Work.pop_back_val();
    if (!Seen.insert(Current.index()).second)
      continue;
    if (Ctx.isVar(Current)) {
      if (Ctx.width(Current) != Width)
        return Expr;
      Inputs.push_back(Current);
      if (Inputs.size() > 10)
        return Expr;
      continue;
    }
    const auto Operands = Ctx.operands(Current);
    Work.append(Operands.begin(), Operands.end());
  }
  if (Inputs.size() < 4 || Inputs.size() % 2 != 0)
    return Expr;
  std::sort(Inputs.begin(), Inputs.end(),
            [](SymRef A, SymRef B) { return A.index() < B.index(); });

  // A low word cannot depend on a high input of ordinary packed arithmetic.
  // Partition by actual dependence; pairing within one coefficient class is
  // immaterial to the sum, so input order does not affect the result.
  llvm::DenseSet<uint32_t> LowInputIds;
  Work = {Lower};
  Seen.clear();
  while (!Work.empty()) {
    const SymRef Current = Work.pop_back_val();
    if (!Seen.insert(Current.index()).second)
      continue;
    if (Ctx.isVar(Current)) {
      LowInputIds.insert(Current.index());
      continue;
    }
    const auto Operands = Ctx.operands(Current);
    Work.append(Operands.begin(), Operands.end());
  }
  llvm::SmallVector<SymRef, 4> LowInputs, HighInputs;
  for (SymRef Input : Inputs) {
    if (LowInputIds.contains(Input.index()))
      LowInputs.push_back(Input);
    else
      HighInputs.push_back(Input);
  }
  if (LowInputs.size() != HighInputs.size())
    return Expr;

  // A packed +/- input changes the result by +/-1 for its low word and by
  // +/-2^Width for its high word. This identifies each coefficient without
  // enumerating pairings or signs. The all-ones response rejects accidental
  // one-point matches; neither response is an equivalence proof.
  std::vector<llvm::APInt> Values;
  Values.reserve(Ctx.numVars());
  for (uint32_t Id = 0; Id < Ctx.numVars(); ++Id)
    Values.emplace_back(Ctx.varInfo(Id).Width, 0);
  const llvm::APInt OffsetValue = Ctx.eval(Expr, Values);
  const uint32_t Wide = Ctx.width(Expr);
  auto Coefficient = [&](SymRef Input, bool High) {
    const uint32_t Id = Ctx.varId(Input);
    llvm::APInt Unit(Wide, 1);
    if (High)
      Unit <<= Width;
    Values[Id] = llvm::APInt(Width, 1);
    const llvm::APInt Delta = Ctx.eval(Expr, Values) - OffsetValue;
    Values[Id] = llvm::APInt::getAllOnes(Width);
    const llvm::APInt AllDelta = Ctx.eval(Expr, Values) - OffsetValue;
    llvm::APInt AllUnit = Values[Id].zext(Wide);
    if (High)
      AllUnit <<= Width;
    Values[Id] = llvm::APInt(Width, 0);
    if (Delta == Unit && AllDelta == AllUnit)
      return 1;
    if (Delta == -Unit && AllDelta == -AllUnit)
      return -1;
    return 0;
  };
  llvm::SmallVector<SymRef, 4> PositiveLow, NegativeLow;
  llvm::SmallVector<SymRef, 4> PositiveHigh, NegativeHigh;
  for (SymRef Input : LowInputs) {
    const int Sign = Coefficient(Input, false);
    if (!Sign)
      return Expr;
    (Sign > 0 ? PositiveLow : NegativeLow).push_back(Input);
  }
  for (SymRef Input : HighInputs) {
    const int Sign = Coefficient(Input, true);
    if (!Sign)
      return Expr;
    (Sign > 0 ? PositiveHigh : NegativeHigh).push_back(Input);
  }
  if (PositiveLow.size() != PositiveHigh.size() ||
      NegativeLow.size() != NegativeHigh.size())
    return Expr;

  SymRef Arithmetic = Ctx.mkZero(Wide);
  for (unsigned I = 0; I < PositiveLow.size(); ++I)
    Arithmetic =
        Ctx.mkAdd(Arithmetic, Ctx.mkConcat(PositiveHigh[I], PositiveLow[I]));
  for (unsigned I = 0; I < NegativeLow.size(); ++I)
    Arithmetic =
        Ctx.mkSub(Arithmetic, Ctx.mkConcat(NegativeHigh[I], NegativeLow[I]));
  const SymRef Offset = Ctx.mkConst(OffsetValue);
  const SymRef Candidate =
      Ctx.isConstZero(Offset) ? Arithmetic : Ctx.mkAdd(Arithmetic, Offset);
  if (Ctx.readabilityCost(Candidate) >= Ctx.readabilityCost(Expr))
    return Expr;

  // Mixed assignments cheaply reject nonlinear coincidences at the basis
  // points. Only the caller's complete equivalence proof accepts a rewrite.
  uint64_t Seed = 0x9e3779b97f4a7c15ULL;
  for (unsigned Sample = 0; Sample < 8; ++Sample) {
    for (SymRef Input : Inputs) {
      Seed = Seed * 6364136223846793005ULL + 1;
      Values[Ctx.varId(Input)] = llvm::APInt(Width, Sample ? Seed : ~0ULL);
    }
    if (Ctx.eval(Candidate, Values) != Ctx.eval(Expr, Values))
      return Expr;
  }
  // For L' = L + c_low, the carry into the upper word is [L' < L].
  // Subtracting c and that carry from the two halves is an exact inverse of
  // adding the wide offset. Prove the offset-free relation instead; this
  // avoids making the solver rediscover a long constant-carry chain.
  SymRef ProofBefore = Expr;
  SymRef ProofAfter = Candidate;
  if (!OffsetValue.isZero()) {
    const SymRef LowOffset = Ctx.mkConst(OffsetValue.trunc(Width));
    const SymRef HighOffset = Ctx.mkConst(OffsetValue.lshr(Width).trunc(Width));
    const SymRef BaseLow = Ctx.mkSub(Lower, LowOffset);
    const SymRef Carry = Ctx.mkZExt(Ctx.mkUlt(Lower, BaseLow), Width);
    const SymRef BaseHigh = Ctx.mkSub(Ctx.mkSub(Upper, HighOffset), Carry);
    ProofBefore = Ctx.mkConcat(BaseHigh, BaseLow);
    ProofAfter = Arithmetic;
  }
  if (Verify(Ctx, ProofBefore, ProofAfter) == SynthVerification::Equivalent)
    return Candidate;
  return Expr;
}

} // namespace neverd::symbolic
