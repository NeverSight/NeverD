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

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
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

uint64_t SymContext::evalU64(SymRef R, llvm::ArrayRef<uint64_t> VarVals) const {
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
  // Zero marks a scheduled node; completed entries contain slot + 1. One
  // table therefore tracks visitation and resolves operand slots without
  // allocating a second set for the same reachable nodes.
  llvm::SmallDenseMap<uint32_t, uint32_t,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Slot;
  llvm::SmallVector<std::pair<SymRef, bool>, 64> Work{{Root, false}};
  llvm::SmallVector<SymRef, 64> PostOrder;
  size_t OperandCount = 0;

  while (!Work.empty()) {
    auto [Cur, Expanded] = Work.pop_back_val();
    if (Expanded) {
      auto Position = Slot.find(Cur.index());
      if (Position->second)
        continue;
      Position->second = static_cast<uint32_t>(PostOrder.size()) + 1;
      PostOrder.push_back(Cur);
      OperandCount += Ctx.numOperands(Cur);
      continue;
    }
    if (!Slot.try_emplace(Cur.index(), 0).second)
      continue;
    Work.emplace_back(Cur, true);
    for (SymRef C : Ctx.operands(Cur))
      Work.emplace_back(C, false);
  }

  // Lower to flat steps.  Operand slots are resolved here, once, so the
  // evaluation loops never touch the hash map or the DAG.
  Steps.reserve(PostOrder.size());
  ArgSlots.reserve(OperandCount);
  uint32_t NextSlot = 0;
  for (SymRef R : PostOrder) {
    const SymNode &N = Ctx.node(R);
    const auto OutputSlot = NextSlot++;
    if (N.Width > 64)
      FitsU64 = false;
    if (N.Op == SymOp::Const) {
      ConstantSlots.emplace_back(OutputSlot, R);
      continue;
    }
    if (N.Op == SymOp::Var)
      Vars.push_back(static_cast<uint32_t>(N.Aux));

    Step S;
    S.Op = N.Op;
    S.Width = N.Width;
    S.Aux = N.Aux;
    S.Node = R;
    S.OutputSlot = OutputSlot;
    S.FirstArg = static_cast<uint32_t>(ArgSlots.size());
    S.NumArgs = N.NumOperands;
    for (SymRef C : Ctx.operands(R))
      ArgSlots.push_back(Slot.find(C.index())->second - 1);
    Steps.push_back(S);
  }

  RootSlot = Slot.find(Root.index())->second - 1;

  // The lowering already visits every reachable variable. Retain the public
  // sorted, unique order without a second DAG walk and its temporary set.
  std::sort(Vars.begin(), Vars.end());
  Vars.erase(std::unique(Vars.begin(), Vars.end()), Vars.end());

  if (FitsU64) {
    ScratchU64.resize(PostOrder.size());
    for (const auto &[Slot, Node] : ConstantSlots)
      ScratchU64[Slot] = Ctx.node(Node).Aux;
  }
}

uint64_t SymEvalPlan::evalU64(llvm::ArrayRef<uint64_t> VarVals) {
  return evalU64With(
      [&](uint32_t Id) { return Id < VarVals.size() ? VarVals[Id] : 0; });
}

namespace {

// Keep arbitrary-precision construction and cleanup out of the common native
// evaluator's stack frame. General operators still have one semantics owner.
LLVM_ATTRIBUTE_NOINLINE
uint64_t evalU64ViaAP(const SymContext &Ctx, SymRef Node,
                      llvm::ArrayRef<uint32_t> Args, const uint64_t *Values) {
  llvm::SmallVector<llvm::APInt, 4> APArgs;
  for (size_t K = 0; K < Args.size(); ++K)
    APArgs.emplace_back(Ctx.width(Ctx.operand(Node, K)), Values[Args[K]]);
  return evalNodeAP(Ctx, Node, APArgs).getZExtValue();
}

} // namespace

uint64_t
SymEvalPlan::evalU64With(llvm::function_ref<uint64_t(uint32_t)> Lookup) {
  assert(FitsU64 && "evalU64 requires every node to fit in 64 bits");

  auto mask = [](uint64_t V, uint32_t W) -> uint64_t {
    // fitsU64 already establishes 1..64 bits. Shifting ones right also
    // handles the full word without a shift by 64 or a per-step branch.
    return V & (~uint64_t(0) >> (64 - W));
  };

  const uint32_t *Args = ArgSlots.data();
  uint64_t *S = ScratchU64.data();

  for (const Step &St : Steps) {
    const auto I = St.OutputSlot;
    const uint32_t *A = Args + St.FirstArg;
    uint32_t W = St.Width;
    uint64_t Res = 0;

    switch (St.Op) {
    case SymOp::Const:
      llvm_unreachable("constants are initialized before evaluation");
    case SymOp::Var:
      Res = Lookup(static_cast<uint32_t>(St.Aux));
      break;
    case SymOp::Add:
      // Canonical associative nodes have at least two operands. Evaluate
      // that common pair directly, then fold any remaining operands.
      Res = S[A[0]] + S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res += S[A[K]];
      break;
    case SymOp::Mul:
      Res = S[A[0]] * S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res *= S[A[K]];
      break;
    case SymOp::And:
      // Every earlier slot is normalized to its declared width. These
      // bitwise operators preserve that invariant without another mask.
      Res = S[A[0]] & S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res &= S[A[K]];
      S[I] = Res;
      continue;
    case SymOp::Or:
      Res = S[A[0]] | S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res |= S[A[K]];
      S[I] = Res;
      continue;
    case SymOp::Xor:
      Res = S[A[0]] ^ S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res ^= S[A[K]];
      S[I] = Res;
      continue;
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
      S[I] = Sh >= W ? 0 : (S[A[0]] >> Sh);
      continue;
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
      S[I] = Y == 0 ? X : (X % Y);
      continue;
    }
    case SymOp::Eq:
      S[I] = S[A[0]] == S[A[1]];
      continue;
    case SymOp::Ult:
      S[I] = S[A[0]] < S[A[1]];
      continue;
    case SymOp::Ule:
      S[I] = S[A[0]] <= S[A[1]];
      continue;
    case SymOp::Ite:
      S[I] = S[A[0]] ? S[A[1]] : S[A[2]];
      continue;
    case SymOp::ZExt:
      S[I] = S[A[0]];
      continue;
    case SymOp::Extract:
      Res = S[A[0]] >> St.Aux;
      break;
    default: {
      // The remaining operators need width-exact or signed semantics that are
      // stated once in evalNodeAP.  They are rare in the MBA inner loop, so
      // reconstructing operand APInts here costs little and avoids a second,
      // divergent copy of the semantics.
      Res = evalU64ViaAP(Ctx, St.Node, {A, St.NumArgs}, S);
      break;
    }
    }

    S[I] = mask(Res, W);
  }

  return ScratchU64[RootSlot];
}

llvm::APInt SymEvalPlan::eval(llvm::ArrayRef<llvm::APInt> VarVals) {
  return evalWith([&](uint32_t Id) -> const llvm::APInt * {
    return Id < VarVals.size() ? &VarVals[Id] : nullptr;
  });
}

namespace {

template <typename GetArgument>
llvm::APInt evalNodeAPImpl(const SymContext &Ctx, SymRef R, size_t NumArgs,
                           GetArgument &&Arg) {
  const SymNode &N = Ctx.node(R);
  uint32_t W = N.Width;

  switch (N.Op) {
  case SymOp::Const:
    return Ctx.constValue(R);
  case SymOp::Var:
    return llvm::APInt(W, 0);
  case SymOp::Add: {
    llvm::APInt Acc(W, 0);
    for (size_t I = 0; I < NumArgs; ++I)
      Acc += Arg(I);
    return Acc;
  }
  case SymOp::Mul: {
    llvm::APInt Acc(W, 1);
    for (size_t I = 0; I < NumArgs; ++I)
      Acc *= Arg(I);
    return Acc;
  }
  case SymOp::And: {
    llvm::APInt Acc = llvm::APInt::getAllOnes(W);
    for (size_t I = 0; I < NumArgs; ++I)
      Acc &= Arg(I);
    return Acc;
  }
  case SymOp::Or: {
    llvm::APInt Acc(W, 0);
    for (size_t I = 0; I < NumArgs; ++I)
      Acc |= Arg(I);
    return Acc;
  }
  case SymOp::Xor: {
    llvm::APInt Acc(W, 0);
    for (size_t I = 0; I < NumArgs; ++I)
      Acc ^= Arg(I);
    return Acc;
  }
  case SymOp::Not:
    return ~Arg(0);
  case SymOp::Shl:
    return Arg(1).uge(W) ? llvm::APInt(W, 0)
                         : Arg(0).shl(Arg(1).getZExtValue());
  case SymOp::LShr:
    return Arg(1).uge(W) ? llvm::APInt(W, 0)
                         : Arg(0).lshr(Arg(1).getZExtValue());
  case SymOp::AShr:
    if (Arg(1).uge(W))
      return Arg(0).isNegative() ? llvm::APInt::getAllOnes(W)
                                 : llvm::APInt(W, 0);
    return Arg(0).ashr(Arg(1).getZExtValue());
  case SymOp::UDiv:
    return Arg(1).isZero() ? llvm::APInt::getAllOnes(W) : Arg(0).udiv(Arg(1));
  case SymOp::SDiv:
    if (Arg(1).isZero())
      return Arg(0).isNonNegative() ? llvm::APInt::getAllOnes(W)
                                    : llvm::APInt(W, 1);
    if (Arg(0).isMinSignedValue() && Arg(1).isAllOnes())
      return Arg(0);
    return Arg(0).sdiv(Arg(1));
  case SymOp::URem:
    return Arg(1).isZero() ? Arg(0) : Arg(0).urem(Arg(1));
  case SymOp::SRem:
    if (Arg(1).isZero())
      return Arg(0);
    if (Arg(0).isMinSignedValue() && Arg(1).isAllOnes())
      return llvm::APInt(W, 0);
    return Arg(0).srem(Arg(1));
  case SymOp::Rol:
    return Arg(0).rotl(Arg(1).urem(W));
  case SymOp::Ror:
    return Arg(0).rotr(Arg(1).urem(W));
  case SymOp::Extract:
    return Arg(0).extractBits(W, static_cast<uint32_t>(N.Aux));
  case SymOp::Concat: {
    llvm::APInt Acc(W, 0);
    uint32_t Shift = W;
    for (size_t I = 0; I < NumArgs; ++I) {
      const auto &A = Arg(I);
      Shift -= A.getBitWidth();
      Acc |= A.zext(W).shl(Shift);
    }
    return Acc;
  }
  case SymOp::ZExt:
    return Arg(0).zext(W);
  case SymOp::SExt:
    return Arg(0).sext(W);
  case SymOp::Ite:
    return Arg(0).isZero() ? Arg(2) : Arg(1);
  case SymOp::Eq:
    return llvm::APInt(1, Arg(0) == Arg(1));
  case SymOp::Ult:
    return llvm::APInt(1, Arg(0).ult(Arg(1)));
  case SymOp::Ule:
    return llvm::APInt(1, Arg(0).ule(Arg(1)));
  case SymOp::Slt:
    return llvm::APInt(1, Arg(0).slt(Arg(1)));
  case SymOp::Sle:
    return llvm::APInt(1, Arg(0).sle(Arg(1)));
  }
  llvm_unreachable("unhandled SymOp in evalNodeAP");
}
} // namespace

llvm::APInt SymEvalPlan::evalWith(
    llvm::function_ref<const llvm::APInt *(uint32_t)> Lookup) {
  // Native-width measurements never use APInt scratch. Allocate it only
  // when this entry point is called, retaining it for subsequent evaluations.
  if (ScratchAP.empty()) {
    ScratchAP.resize(numSteps());
    // Complete immutable slots before invoking an external input lookup.
    // A lookup may throw; the same plan must remain usable on the next call.
    for (const auto &[Slot, Node] : ConstantSlots)
      ScratchAP[Slot] = Ctx.constValue(Node);
  }
  for (const Step &St : Steps) {
    const auto I = St.OutputSlot;
    const uint32_t *A = ArgSlots.data() + St.FirstArg;

    if (St.Op == SymOp::Var) {
      const llvm::APInt *V = Lookup(static_cast<uint32_t>(St.Aux));
      ScratchAP[I] =
          V ? (V->getBitWidth() == St.Width ? *V : V->zextOrTrunc(St.Width))
            : llvm::APInt(St.Width, 0);
      continue;
    }

    // Every operand precedes this output slot. Read it directly through
    // the shared operator evaluator instead of copying wide APInts into a
    // temporary argument vector for every step and validation assignment.
    ScratchAP[I] = evalNodeAPImpl(
        Ctx, St.Node, St.NumArgs,
        [&](size_t K) -> const llvm::APInt & { return ScratchAP[A[K]]; });
  }

  return ScratchAP[RootSlot];
}

llvm::APInt evalNodeAP(const SymContext &Ctx, SymRef R,
                       llvm::ArrayRef<llvm::APInt> Args) {
  return evalNodeAPImpl(
      Ctx, R, Args.size(),
      [&](size_t I) -> const llvm::APInt & { return Args[I]; });
}

} // namespace neverd::symbolic
