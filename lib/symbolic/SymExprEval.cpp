//===- SymExprEval.cpp - Evaluating symbolic expressions -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements one-shot expression evaluation, the reusable \c SymEvalPlan,
/// and the width-exact operator semantics shared with MBA verification.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <cassert>
#include <cstddef>
#include <unordered_map>
#include <utility>

namespace neverd::symbolic {

bool SymContext::fitsU64(SymRef R) const {
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 32> Work{R};
  while (!Work.empty()) {
    SymRef Cur = Work.pop_back_val();
    if (!Seen.insert(Cur.index()).second)
      continue;
    if (width(Cur) > 64)
      return false;
    for (SymRef C : operands(Cur))
      Work.push_back(C);
  }
  return true;
}

llvm::APInt SymContext::eval(SymRef R,
                             llvm::ArrayRef<llvm::APInt> VarVals) const {
  SymEvalPlan Plan(*this, R);
  return Plan.eval(VarVals);
}

uint64_t SymContext::evalU64(SymRef R,
                             llvm::ArrayRef<uint64_t> VarVals) const {
  SymEvalPlan Plan(*this, R);
  return Plan.evalU64(VarVals);
}

//===----------------------------------------------------------------------===//
// SymEvalPlan
//===----------------------------------------------------------------------===//

SymEvalPlan::SymEvalPlan(const SymContext &Ctx, SymRef Root)
    : Ctx(Ctx), Root(Root) {
  // Iterative post-order so a deeply nested expression cannot overflow the
  // stack; obfuscators emit exactly that shape on purpose.
  std::unordered_map<uint32_t, uint32_t> Slot;
  llvm::DenseSet<uint32_t> Visited;
  llvm::SmallVector<std::pair<SymRef, bool>, 64> Work{{Root, false}};
  llvm::SmallVector<SymRef, 64> PostOrder;

  while (!Work.empty()) {
    auto [Cur, Expanded] = Work.pop_back_val();
    if (Expanded) {
      if (Slot.count(Cur.index()))
        continue;
      Slot.emplace(Cur.index(), static_cast<uint32_t>(PostOrder.size()));
      PostOrder.push_back(Cur);
      continue;
    }
    if (!Visited.insert(Cur.index()).second)
      continue;
    Work.emplace_back(Cur, true);
    for (SymRef C : Ctx.operands(Cur))
      Work.emplace_back(C, false);
  }

  // Lower to flat steps.  Operand slots are resolved here, once, so the
  // evaluation loops never touch the hash map or the DAG.
  Steps.reserve(PostOrder.size());
  for (SymRef R : PostOrder) {
    const SymNode &N = Ctx.node(R);
    if (N.Width > 64)
      FitsU64 = false;

    Step S;
    S.Op = N.Op;
    S.Width = N.Width;
    S.Aux = N.Aux;
    S.Node = R;
    S.FirstArg = static_cast<uint32_t>(ArgSlots.size());
    S.NumArgs = N.NumOperands;
    for (SymRef C : Ctx.operands(R))
      ArgSlots.push_back(Slot.at(C.index()));
    Steps.push_back(S);
  }

  RootSlot = Slot.at(Root.index());

  llvm::SmallVector<uint32_t, 8> V;
  Ctx.collectVars(Root, V);
  Vars.assign(V.begin(), V.end());

  ScratchU64.resize(Steps.size());
  ScratchAP.resize(Steps.size());
}

uint64_t SymEvalPlan::evalU64(llvm::ArrayRef<uint64_t> VarVals) {
  assert(FitsU64 && "evalU64 requires every node to fit in 64 bits");

  auto mask = [](uint64_t V, uint32_t W) -> uint64_t {
    return W >= 64 ? V : (V & ((uint64_t(1) << W) - 1));
  };

  const uint32_t *Args = ArgSlots.data();
  uint64_t *S = ScratchU64.data();

  for (size_t I = 0; I < Steps.size(); ++I) {
    const Step &St = Steps[I];
    const uint32_t *A = Args + St.FirstArg;
    uint32_t W = St.Width;
    uint64_t Res = 0;

    switch (St.Op) {
    case SymOp::Const:
      Res = St.Aux;
      break;
    case SymOp::Var:
      Res = St.Aux < VarVals.size() ? VarVals[St.Aux] : 0;
      break;
    case SymOp::Add:
      for (uint32_t K = 0; K < St.NumArgs; ++K)
        Res += S[A[K]];
      break;
    case SymOp::Mul:
      Res = 1;
      for (uint32_t K = 0; K < St.NumArgs; ++K)
        Res *= S[A[K]];
      break;
    case SymOp::And:
      Res = ~uint64_t(0);
      for (uint32_t K = 0; K < St.NumArgs; ++K)
        Res &= S[A[K]];
      break;
    case SymOp::Or:
      for (uint32_t K = 0; K < St.NumArgs; ++K)
        Res |= S[A[K]];
      break;
    case SymOp::Xor:
      for (uint32_t K = 0; K < St.NumArgs; ++K)
        Res ^= S[A[K]];
      break;
    case SymOp::Not:
      Res = ~S[A[0]];
      break;
    case SymOp::Shl: {
      uint64_t Sh = S[A[1]];
      Res = Sh >= W ? 0 : (S[A[0]] << Sh);
      break;
    }
    case SymOp::LShr: {
      uint64_t Sh = S[A[1]];
      Res = Sh >= W ? 0 : (S[A[0]] >> Sh);
      break;
    }
    case SymOp::AShr: {
      uint64_t V = S[A[0]], Sh = S[A[1]];
      bool Neg = (V >> (W - 1)) & 1;
      if (Sh == 0)
        Res = V;
      else if (Sh >= W)
        Res = Neg ? ~uint64_t(0) : 0;
      else
        Res = Neg ? ((V >> Sh) | ~((uint64_t(1) << (W - Sh)) - 1)) : (V >> Sh);
      break;
    }
    case SymOp::UDiv: {
      uint64_t X = S[A[0]], Y = S[A[1]];
      Res = Y == 0 ? ~uint64_t(0) : (X / Y);
      break;
    }
    case SymOp::URem: {
      uint64_t X = S[A[0]], Y = S[A[1]];
      Res = Y == 0 ? X : (X % Y);
      break;
    }
    case SymOp::Eq:
      Res = S[A[0]] == S[A[1]];
      break;
    case SymOp::Ult:
      Res = S[A[0]] < S[A[1]];
      break;
    case SymOp::Ule:
      Res = S[A[0]] <= S[A[1]];
      break;
    case SymOp::Ite:
      Res = S[A[0]] ? S[A[1]] : S[A[2]];
      break;
    case SymOp::ZExt:
      Res = S[A[0]];
      break;
    case SymOp::Extract:
      Res = S[A[0]] >> St.Aux;
      break;
    default: {
      // The remaining operators need width-exact or signed semantics that are
      // stated once in evalNodeAP.  They are rare in the MBA inner loop, so
      // reconstructing operand APInts here costs little and avoids a second,
      // divergent copy of the semantics.
      llvm::SmallVector<llvm::APInt, 4> APArgs;
      for (uint32_t K = 0; K < St.NumArgs; ++K)
        APArgs.emplace_back(Ctx.width(Ctx.operand(St.Node, K)), S[A[K]]);
      Res = evalNodeAP(Ctx, St.Node, APArgs).getZExtValue();
      break;
    }
    }

    S[I] = mask(Res, W);
  }

  return Steps.empty() ? 0 : ScratchU64[RootSlot];
}

llvm::APInt SymEvalPlan::eval(llvm::ArrayRef<llvm::APInt> VarVals) {
  for (size_t I = 0; I < Steps.size(); ++I) {
    const Step &St = Steps[I];
    const uint32_t *A = ArgSlots.data() + St.FirstArg;

    if (St.Op == SymOp::Const) {
      ScratchAP[I] = Ctx.constValue(St.Node);
      continue;
    }
    if (St.Op == SymOp::Var) {
      llvm::APInt V =
          St.Aux < VarVals.size() ? VarVals[St.Aux] : llvm::APInt(St.Width, 0);
      ScratchAP[I] = V.getBitWidth() == St.Width ? V : V.zextOrTrunc(St.Width);
      continue;
    }

    llvm::SmallVector<llvm::APInt, 4> APArgs;
    APArgs.reserve(St.NumArgs);
    for (uint32_t K = 0; K < St.NumArgs; ++K)
      APArgs.push_back(ScratchAP[A[K]]);
    ScratchAP[I] = evalNodeAP(Ctx, St.Node, APArgs);
  }

  return Steps.empty() ? llvm::APInt(Ctx.width(Root), 0) : ScratchAP[RootSlot];
}

llvm::APInt evalNodeAP(const SymContext &Ctx, SymRef R,
                       llvm::ArrayRef<llvm::APInt> Args) {
  const SymNode &N = Ctx.node(R);
  uint32_t W = N.Width;

  switch (N.Op) {
  case SymOp::Const:
    return Ctx.constValue(R);
  case SymOp::Var:
    return llvm::APInt(W, 0);
  case SymOp::Add: {
    llvm::APInt Acc(W, 0);
    for (const auto &A : Args)
      Acc += A;
    return Acc;
  }
  case SymOp::Mul: {
    llvm::APInt Acc(W, 1);
    for (const auto &A : Args)
      Acc *= A;
    return Acc;
  }
  case SymOp::And: {
    llvm::APInt Acc = llvm::APInt::getAllOnes(W);
    for (const auto &A : Args)
      Acc &= A;
    return Acc;
  }
  case SymOp::Or: {
    llvm::APInt Acc(W, 0);
    for (const auto &A : Args)
      Acc |= A;
    return Acc;
  }
  case SymOp::Xor: {
    llvm::APInt Acc(W, 0);
    for (const auto &A : Args)
      Acc ^= A;
    return Acc;
  }
  case SymOp::Not:
    return ~Args[0];
  case SymOp::Shl:
    return Args[1].uge(W) ? llvm::APInt(W, 0)
                          : Args[0].shl(Args[1].getZExtValue());
  case SymOp::LShr:
    return Args[1].uge(W) ? llvm::APInt(W, 0)
                          : Args[0].lshr(Args[1].getZExtValue());
  case SymOp::AShr:
    if (Args[1].uge(W))
      return Args[0].isNegative() ? llvm::APInt::getAllOnes(W)
                                  : llvm::APInt(W, 0);
    return Args[0].ashr(Args[1].getZExtValue());
  case SymOp::UDiv:
    return Args[1].isZero() ? llvm::APInt::getAllOnes(W)
                            : Args[0].udiv(Args[1]);
  case SymOp::SDiv:
    if (Args[1].isZero())
      return Args[0].isNonNegative() ? llvm::APInt::getAllOnes(W)
                                     : llvm::APInt(W, 1);
    if (Args[0].isMinSignedValue() && Args[1].isAllOnes())
      return Args[0];
    return Args[0].sdiv(Args[1]);
  case SymOp::URem:
    return Args[1].isZero() ? Args[0] : Args[0].urem(Args[1]);
  case SymOp::SRem:
    if (Args[1].isZero())
      return Args[0];
    if (Args[0].isMinSignedValue() && Args[1].isAllOnes())
      return llvm::APInt(W, 0);
    return Args[0].srem(Args[1]);
  case SymOp::Rol:
    return Args[0].rotl(Args[1].urem(llvm::APInt(W, W)).getZExtValue());
  case SymOp::Ror:
    return Args[0].rotr(Args[1].urem(llvm::APInt(W, W)).getZExtValue());
  case SymOp::Extract:
    return Args[0].extractBits(W, static_cast<uint32_t>(N.Aux));
  case SymOp::Concat: {
    llvm::APInt Acc(W, 0);
    uint32_t Shift = W;
    for (const auto &A : Args) {
      Shift -= A.getBitWidth();
      Acc |= A.zext(W).shl(Shift);
    }
    return Acc;
  }
  case SymOp::ZExt:
    return Args[0].zext(W);
  case SymOp::SExt:
    return Args[0].sext(W);
  case SymOp::Ite:
    return Args[0].isZero() ? Args[2] : Args[1];
  case SymOp::Eq:
    return llvm::APInt(1, Args[0] == Args[1]);
  case SymOp::Ult:
    return llvm::APInt(1, Args[0].ult(Args[1]));
  case SymOp::Ule:
    return llvm::APInt(1, Args[0].ule(Args[1]));
  case SymOp::Slt:
    return llvm::APInt(1, Args[0].slt(Args[1]));
  case SymOp::Sle:
    return llvm::APInt(1, Args[0].sle(Args[1]));
  }
  llvm_unreachable("unhandled SymOp in evalNodeAP");
}

} // namespace neverd::symbolic
