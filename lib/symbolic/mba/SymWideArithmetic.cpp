//===- SymWideArithmetic.cpp - Proved split-word recovery -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymWideArithmetic.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
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

  llvm::SmallVector<SymRef, 4> Inputs;
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
      if (Inputs.size() > 4)
        return Expr;
      continue;
    }
    const auto Operands = Ctx.operands(Current);
    Work.append(Operands.begin(), Operands.end());
  }
  if (Inputs.size() != 4)
    return Expr;
  std::sort(Inputs.begin(), Inputs.end(),
            [](SymRef A, SymRef B) { return A.index() < B.index(); });

  // A handful of deterministic assignments reject wrong pairings cheaply.
  // They never authorize a rewrite; only Verify can do that.
  std::vector<llvm::APInt> Values;
  Values.reserve(Ctx.numVars());
  for (uint32_t Id = 0; Id < Ctx.numVars(); ++Id)
    Values.emplace_back(Ctx.varInfo(Id).Width, 0);
  llvm::SmallVector<llvm::APInt, 8> Expected;
  const uint64_t Seeds[8][4] = {
      {0, 0, 0, 0},
      {1, 2, 4, 8},
      {~0ULL, 1, 0, 0},
      {0, ~0ULL, 1, 0},
      {~0ULL, ~0ULL, ~0ULL, ~0ULL},
      {0x13579bdfULL, 0x2468ace0ULL, 0x31415926ULL, 0x27182818ULL},
      {0x80000000ULL, 0x7fffffffULL, 0xffffffffULL, 1},
      {0xaaaaaaaaULL, 0x55555555ULL, 0x33333333ULL, 0xccccccccULL}};
  for (unsigned Sample = 0; Sample < 8; ++Sample) {
    for (unsigned I = 0; I < 4; ++I)
      Values[Ctx.varId(Inputs[I])] = llvm::APInt(Width, Seeds[Sample][I]);
    Expected.push_back(Ctx.eval(Expr, Values));
  }

  std::array<unsigned, 4> Order{0, 1, 2, 3};
  llvm::DenseSet<uint32_t> Tried;
  do {
    const SymRef A = Ctx.mkConcat(Inputs[Order[0]], Inputs[Order[1]]);
    const SymRef B = Ctx.mkConcat(Inputs[Order[2]], Inputs[Order[3]]);
    for (bool Subtract : {false, true}) {
      // The two operands of addition are exchangeable; subtraction is not.
      if (!Subtract && Order[0] > Order[2])
        continue;
      const SymRef Candidate = Subtract ? Ctx.mkSub(A, B) : Ctx.mkAdd(A, B);
      if (!Tried.insert(Candidate.index()).second ||
          Ctx.readabilityCost(Candidate) >= Ctx.readabilityCost(Expr))
        continue;
      bool Matches = true;
      for (unsigned Sample = 0; Sample < 8 && Matches; ++Sample) {
        for (unsigned I = 0; I < 4; ++I)
          Values[Ctx.varId(Inputs[I])] = llvm::APInt(Width, Seeds[Sample][I]);
        Matches = Ctx.eval(Candidate, Values) == Expected[Sample];
      }
      if (Matches &&
          Verify(Ctx, Expr, Candidate) == SynthVerification::Equivalent)
        return Candidate;
    }
  } while (std::next_permutation(Order.begin(), Order.end()));
  return Expr;
}

} // namespace neverd::symbolic
