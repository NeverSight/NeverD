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
#include <bit>
#include <cassert>
#include <cstddef>
#include <limits>
#include <utility>

namespace neverd::symbolic {

namespace {

bool preservesLowBits(SymOp Op) {
  return Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
         Op == SymOp::Or || Op == SymOp::Xor || Op == SymOp::Not;
}

// A sum of two scaled words retains their common low zero bits. Its
// complete value is still needed as one factor of the outer product.
unsigned sumScaleZeros(const SymContext &Ctx, SymRef S) {
  if (Ctx.op(S) != SymOp::Add || Ctx.numOperands(S) != 2)
    return 0;
  unsigned Zeros = Ctx.width(S);
  for (SymRef T : Ctx.operands(S)) {
    if (Ctx.op(T) == SymOp::Mul)
      T = Ctx.operand(T, 0);
    if (!Ctx.isConst(T))
      return 0;
    Zeros = std::min(Zeros, Ctx.constValue(T).countr_zero());
  }
  return Zeros;
}

// All prefix consumers share this demand propagation. An operation outside
// the modular prefix algebra retains every bit of its original operands.
template <bool Wide = false, typename Step, typename Visitor>
llvm::SmallVector<unsigned, 64>
visitRequiredPrefixes(const SymContext &Ctx, SymRef Root, uint32_t RootSlot,
                      size_t NumSlots, llvm::MutableArrayRef<Step> Steps,
                      llvm::ArrayRef<uint32_t> Args, Visitor Visit) {
  llvm::SmallVector<unsigned, 64> Needed(NumSlots, 1);
  Needed[RootSlot] = Ctx.width(Root);
  for (auto &St : llvm::reverse(Steps)) {
    const auto Demand = Needed[St.OutputSlot];
    const auto Operands = Ctx.operands(St.Node);
    unsigned Zeros = 0;
    if (St.Op == SymOp::Mul && Ctx.isConst(Operands.front()))
      Zeros = Ctx.constValue(Operands.front()).countr_zero();
    unsigned Coefficient = 0;
    if constexpr (Wide) {
      if (St.Op == SymOp::Mul && Demand > 64 && Zeros < Demand - 64) {
        for (unsigned I = 0; I < Operands.size(); ++I) {
          unsigned Candidate = sumScaleZeros(Ctx, Operands[I]);
          if (Candidate >= Demand - 64) {
            Zeros = Candidate;
            Coefficient = I;
            break;
          }
        }
      }
    }
    for (size_t I = 0; I < Operands.size(); ++I) {
      unsigned Bits = Ctx.width(Operands[I]);
      if (preservesLowBits(St.Op)) {
        Bits = Demand;
        if (Zeros && I != Coefficient)
          Bits -= std::min(Bits - 1, Zeros);
      }
      auto &Required = Needed[Args[St.FirstArg + I]];
      Required = std::max(Required, Bits);
    }
    Visit(St, Demand, Zeros, Coefficient);
  }
  return Needed;
}

// A scaled product can discard constant offsets in its factors. Account for
// every use before replacing a neutral residue by its operand slot. Retain
// all variable steps and scratch slots: callbacks and the public work count
// still describe the original reachable DAG.
template <typename Step>
LLVM_ATTRIBUTE_NOINLINE size_t prepareNativeResidues(
    const SymContext &Ctx, SymRef Root, uint32_t &RootSlot, size_t NumSlots,
    llvm::MutableArrayRef<Step> Steps, llvm::MutableArrayRef<uint32_t> Args) {
  const auto Needed =
      visitRequiredPrefixes(Ctx, Root, RootSlot, NumSlots, Steps, Args,
                            [](const Step &, unsigned, unsigned, unsigned) {});
  llvm::SmallVector<uint32_t, 64> Aliases;
  Aliases.reserve(NumSlots);
  for (size_t I = 0; I < NumSlots; ++I)
    Aliases.push_back(I);
  size_t Out = 0;
  for (const auto &St : Steps) {
    for (size_t I = 0; I < St.NumArgs; ++I)
      Args[St.FirstArg + I] = Aliases[Args[St.FirstArg + I]];
    if (St.NumArgs == 2 &&
        (St.Op == SymOp::Add || St.Op == SymOp::Mul || St.Op == SymOp::And ||
         St.Op == SymOp::Or || St.Op == SymOp::Xor) &&
        Ctx.isConst(Ctx.operand(St.Node, 0))) {
      const auto Mask = ~uint64_t(0) >> (64 - Needed[St.OutputSlot]);
      const auto Low = Ctx.node(Ctx.operand(St.Node, 0)).Aux & Mask;
      const bool Neutral = St.Op == SymOp::Mul   ? Low == 1
                           : St.Op == SymOp::And ? Low == Mask
                                                 : Low == 0;
      if (Neutral) {
        Aliases[St.OutputSlot] = Args[St.FirstArg + 1];
        continue;
      }
    }
    Steps[Out++] = St;
  }
  RootSlot = Aliases[RootSlot];
  return Out;
}

// Run only for wide plans, after lowering has resolved every operand slot.
// Keeping this pass out of the constructor also keeps its scratch storage and
// register saves out of native plans. Step is SymEvalPlan's private step type.
template <typename Step>
LLVM_ATTRIBUTE_NOINLINE void
prepareWidePrefixes(const SymContext &Ctx, SymRef Root, uint32_t RootSlot,
                    size_t NumSlots, llvm::MutableArrayRef<Step> Steps,
                    llvm::MutableArrayRef<uint32_t> ArgSlots) {
  visitRequiredPrefixes<true>(
      Ctx, Root, RootSlot, NumSlots, Steps, ArgSlots,
      [&](Step &St, unsigned Demand, unsigned Zeros, unsigned Coefficient) {
        if (preservesLowBits(St.Op) && Demand <= 64)
          St.Width = Demand;
        if (Zeros && Demand > 64) {
          const unsigned FactorBits = Demand - std::min(Demand - 1, Zeros);
          if (FactorBits <= 64) {
            St.Aux = ~uint64_t(0) >> (64 - FactorBits);
            std::swap(ArgSlots[St.FirstArg],
                      ArgSlots[St.FirstArg + Coefficient]);
          }
        }
      });
}

} // namespace

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
  llvm::SmallVector<std::pair<SymRef, uint32_t>, 64> PostOrder;
  size_t OperandCount = 0;
  uint32_t NextSlot = 0;

  while (!Work.empty()) {
    auto [Cur, Expanded] = Work.pop_back_val();
    if (Expanded) {
      const auto OutputSlot = NextSlot++;
      Slot.find(Cur.index())->second = OutputSlot + 1;
      PostOrder.emplace_back(Cur, OutputSlot);
      continue;
    }
    auto [Position, New] = Slot.try_emplace(Cur.index(), 0);
    if (!New)
      continue;
    const auto &N = Ctx.node(Cur);
    if (N.Width > 64)
      FitsU64 = false;
    // Leaves need neither operand resolution nor a second worklist visit.
    // Lower them now and keep inputs first in their original lookup order.
    if (N.Op == SymOp::Const || N.Op == SymOp::Var) {
      const auto OutputSlot = NextSlot++;
      Position->second = OutputSlot + 1;
      if (N.Op == SymOp::Const) {
        ConstantSlots.emplace_back(OutputSlot, Cur);
      } else {
        Vars.push_back(static_cast<uint32_t>(N.Aux));
        Steps.push_back({N.Op, N.Width, N.Aux, 0, 0, Cur, OutputSlot});
      }
      continue;
    }
    OperandCount += N.NumOperands;
    Work.emplace_back(Cur, true);
    for (SymRef C : Ctx.operands(Cur))
      Work.emplace_back(C, false);
  }

  // Lower to flat steps.  Operand slots are resolved here, once, so the
  // evaluation loops never touch the hash map or the DAG.
  Steps.reserve(NextSlot);
  ArgSlots.reserve(OperandCount);
  for (auto [R, OutputSlot] : PostOrder) {
    const SymNode &N = Ctx.node(R);
    Step S;
    S.Op = N.Op;
    S.Width = N.Width;
    S.Aux = N.Aux;
    // A native product has no node payload. Prepare its exact modulus once
    // for the long fold, without increasing the per-step storage.
    if (N.Op == SymOp::Mul && N.NumOperands > 2 && N.Width <= 64)
      S.Aux = ~uint64_t(0) >> (64 - N.Width);
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
    ScratchU64.resize(NextSlot);
    for (const auto &[Slot, Node] : ConstantSlots)
      ScratchU64[Slot] = Ctx.node(Node).Aux;
  } else {
    NativeState = NativePreparation::Done;
    // A scaled wide product observes only a prefix of each remaining factor.
    // Other operators retain complete operands: a high bit can affect even
    // the lowest result bit of a comparison, division or right shift.
    prepareWidePrefixes(Ctx, Root, RootSlot, NextSlot,
                        llvm::MutableArrayRef<Step>(Steps), ArgSlots);
  }
}

LLVM_ATTRIBUTE_NOINLINE void SymEvalPlan::prepareNativeReuse() {
  // One-shot callers need no extra DAG scan. Prepare residue aliases only
  // once reuse has been observed, before either evaluator reads its inputs.
  if (NativeState == NativePreparation::FirstEvaluation) {
    NativeState = NativePreparation::AwaitReuse;
    return;
  }
  NativeState = NativePreparation::Done;
  constexpr unsigned NativeBits = std::numeric_limits<uint64_t>::digits;
  unsigned AvailableZeros = 0, NeededZeros = NativeBits + 1;
  for (const auto &St : llvm::ArrayRef<Step>(Steps).drop_front(Vars.size())) {
    if (St.NumArgs < 2 || !Ctx.isConst(Ctx.operand(St.Node, 0)))
      continue;
    const auto Literal = Ctx.node(Ctx.operand(St.Node, 0)).Aux;
    if (St.Op == SymOp::Mul)
      AvailableZeros = std::min(
          NativeBits, AvailableZeros + unsigned(std::countr_zero(Literal)));
    if (St.NumArgs == 2 && preservesLowBits(St.Op)) {
      const auto Difference = St.Op == SymOp::Mul   ? Literal - 1
                              : St.Op == SymOp::And ? ~Literal
                                                    : Literal;
      const auto NeutralBits =
          std::min(St.Width, unsigned(std::countr_zero(Difference)));
      if (NeutralBits)
        NeededZeros = std::min(NeededZeros, St.Width - NeutralBits);
    }
    // Even the sum of every scale's trailing zeros is only an upper bound
    // on any one path. If it cannot hide a literal, no alias is possible.
    // A possible alias still needs the complete, shared-use demand analysis.
    if (AvailableZeros >= NeededZeros) {
      Steps.resize(prepareNativeResidues(Ctx, Root, RootSlot, ScratchU64.size(),
                                         llvm::MutableArrayRef<Step>(Steps),
                                         ArgSlots));
      return;
    }
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
  if (NativeState != NativePreparation::Done)
    prepareNativeReuse();

  auto mask = [](uint64_t V, uint32_t W) -> uint64_t {
    // fitsU64 already establishes 1..64 bits. Shifting ones right also
    // handles the full word without a shift by 64 or a per-step branch.
    return V & (~uint64_t(0) >> (64 - W));
  };

  const uint32_t *Args = ArgSlots.data();
  uint64_t *S = ScratchU64.data();

  for (const Step &St : llvm::ArrayRef<Step>(Steps).take_front(Vars.size()))
    S[St.OutputSlot] = mask(Lookup(static_cast<uint32_t>(St.Aux)), St.Width);

  for (const Step &St : llvm::ArrayRef<Step>(Steps).drop_front(Vars.size())) {
    const auto I = St.OutputSlot;
    const uint32_t *A = Args + St.FirstArg;
    uint32_t W = St.Width;
    uint64_t Res = 0;

    switch (St.Op) {
    case SymOp::Const:
    case SymOp::Var:
      llvm_unreachable("inputs and constants are initialized before operators");
    case SymOp::Add:
      // Canonical associative nodes have at least two operands. Evaluate
      // that common pair directly, then fold any remaining operands.
      Res = S[A[0]] + S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res += S[A[K]];
      break;
    case SymOp::Mul:
      Res = S[A[0]] * S[A[1]];
      if (St.NumArgs > 2) {
        // A zero first pair remains zero modulo this node's width. Check
        // once before the tight fold; per-factor checks slow nonzero words.
        // All operand steps, including input lookups, have already run.
        if (!(Res & St.Aux))
          break;
        uint32_t K = 2;
        if (St.NumArgs - K >= 4) {
          // Independent chains avoid serial multiplication latency on long
          // products. Unsigned wraparound preserves every low result bit.
          uint64_t B = 1, C = 1, D = 1;
          do {
            Res *= S[A[K]];
            B *= S[A[K + 1]];
            C *= S[A[K + 2]];
            D *= S[A[K + 3]];
            K += 4;
          } while (St.NumArgs - K >= 4);
          Res *= (B * C) * D;
        }
        for (; K < St.NumArgs; ++K)
          Res *= S[A[K]];
      }
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
void evalNodeAPImpl(const SymContext &Ctx, SymRef R, size_t NumArgs,
                    llvm::APInt &Result, GetArgument &&Arg) {
  const SymNode &N = Ctx.node(R);
  uint32_t W = N.Width;
  // Reuse the prepared output's allocation. Operand slots precede the
  // output, so assigning the first operand cannot overwrite another input.
  auto Start = [&](uint64_t Identity) {
    if (NumArgs)
      Result = Arg(0);
    else
      Result = llvm::APInt(W, Identity, true);
  };

  switch (N.Op) {
  case SymOp::Const:
    Result = Ctx.constValue(R);
    return;
  case SymOp::Var:
    Result = llvm::APInt(W, 0);
    return;
  case SymOp::Add: {
    Start(0);
    for (size_t I = 1; I < NumArgs; ++I)
      Result += Arg(I);
    return;
  }
  case SymOp::Mul: {
    Start(1);
    for (size_t I = 1; I < NumArgs && !Result.isZero(); ++I)
      Result *= Arg(I);
    return;
  }
  case SymOp::And: {
    Start(UINT64_MAX);
    for (size_t I = 1; I < NumArgs; ++I)
      Result &= Arg(I);
    return;
  }
  case SymOp::Or: {
    Start(0);
    for (size_t I = 1; I < NumArgs; ++I)
      Result |= Arg(I);
    return;
  }
  case SymOp::Xor: {
    Start(0);
    for (size_t I = 1; I < NumArgs; ++I)
      Result ^= Arg(I);
    return;
  }
  case SymOp::Not:
    Result = ~Arg(0);
    return;
  case SymOp::Shl:
    Result =
        Arg(1).uge(W) ? llvm::APInt(W, 0) : Arg(0).shl(Arg(1).getZExtValue());
    return;
  case SymOp::LShr:
    Result =
        Arg(1).uge(W) ? llvm::APInt(W, 0) : Arg(0).lshr(Arg(1).getZExtValue());
    return;
  case SymOp::AShr:
    if (Arg(1).uge(W)) {
      Result =
          Arg(0).isNegative() ? llvm::APInt::getAllOnes(W) : llvm::APInt(W, 0);
      return;
    }
    Result = Arg(0).ashr(Arg(1).getZExtValue());
    return;
  case SymOp::UDiv:
    Result = Arg(1).isZero() ? llvm::APInt::getAllOnes(W) : Arg(0).udiv(Arg(1));
    return;
  case SymOp::SDiv:
    if (Arg(1).isZero()) {
      Result = Arg(0).isNonNegative() ? llvm::APInt::getAllOnes(W)
                                      : llvm::APInt(W, 1);
      return;
    }
    if (Arg(0).isMinSignedValue() && Arg(1).isAllOnes()) {
      Result = Arg(0);
      return;
    }
    Result = Arg(0).sdiv(Arg(1));
    return;
  case SymOp::URem:
    Result = Arg(1).isZero() ? Arg(0) : Arg(0).urem(Arg(1));
    return;
  case SymOp::SRem:
    if (Arg(1).isZero()) {
      Result = Arg(0);
      return;
    }
    if (Arg(0).isMinSignedValue() && Arg(1).isAllOnes()) {
      Result = llvm::APInt(W, 0);
      return;
    }
    Result = Arg(0).srem(Arg(1));
    return;
  case SymOp::Rol:
    Result = Arg(0).rotl(Arg(1).urem(W));
    return;
  case SymOp::Ror:
    Result = Arg(0).rotr(Arg(1).urem(W));
    return;
  case SymOp::Extract:
    Result = Arg(0).extractBits(W, static_cast<uint32_t>(N.Aux));
    return;
  case SymOp::Concat: {
    llvm::APInt Acc(W, 0);
    uint32_t Shift = W;
    for (size_t I = 0; I < NumArgs; ++I) {
      const auto &A = Arg(I);
      Shift -= A.getBitWidth();
      Acc |= A.zext(W).shl(Shift);
    }
    Result = Acc;
    return;
  }
  case SymOp::ZExt:
    Result = Arg(0).zext(W);
    return;
  case SymOp::SExt:
    Result = Arg(0).sext(W);
    return;
  case SymOp::Ite:
    Result = Arg(0).isZero() ? Arg(2) : Arg(1);
    return;
  case SymOp::Eq:
    Result = llvm::APInt(1, Arg(0) == Arg(1));
    return;
  case SymOp::Ult:
    Result = llvm::APInt(1, Arg(0).ult(Arg(1)));
    return;
  case SymOp::Ule:
    Result = llvm::APInt(1, Arg(0).ule(Arg(1)));
    return;
  case SymOp::Slt:
    Result = llvm::APInt(1, Arg(0).slt(Arg(1)));
    return;
  case SymOp::Sle:
    Result = llvm::APInt(1, Arg(0).sle(Arg(1)));
    return;
  }
  llvm_unreachable("unhandled SymOp in evalNodeAP");
}
} // namespace

llvm::APInt SymEvalPlan::evalWith(
    llvm::function_ref<const llvm::APInt *(uint32_t)> Lookup) {
  if (NativeState != NativePreparation::Done)
    prepareNativeReuse();
  // Native-width measurements never use APInt scratch. Allocate it only
  // when this entry point is called, retaining it for subsequent evaluations.
  if (ScratchAP.empty()) {
    ScratchAP.resize(numSteps());
    // Complete immutable slots before invoking an external input lookup.
    // A lookup may throw; the same plan must remain usable on the next call.
    for (const auto &[Slot, Node] : ConstantSlots)
      ScratchAP[Slot] = Ctx.constValue(Node);
    // Prefix slots fit in APInt's inline word. Their scaled-product consumers
    // multiply the complete prefix before restoring the wide coefficient;
    // every other full-width use already prevented this narrowing.
    for (const Step &St : Steps)
      if (St.Width <= 64 && preservesLowBits(St.Op))
        ScratchAP[St.OutputSlot] = llvm::APInt(St.Width, 0);
  }
  for (const Step &St : Steps) {
    const auto I = St.OutputSlot;
    const uint32_t *A = ArgSlots.data() + St.FirstArg;

    if (St.Op == SymOp::Var) {
      const llvm::APInt *V = Lookup(static_cast<uint32_t>(St.Aux));
      if (V && V->getBitWidth() == St.Width)
        ScratchAP[I] = *V;
      else
        ScratchAP[I] = V ? V->zextOrTrunc(St.Width) : llvm::APInt(St.Width, 0);
      continue;
    }

    if (St.Width <= 64 && preservesLowBits(St.Op)) {
      auto Word = [&](size_t K) { return ScratchAP[A[K]].getRawData()[0]; };
      uint64_t Value = Word(0);
      switch (St.Op) {
      case SymOp::Add:
        for (size_t K = 1; K < St.NumArgs; ++K)
          Value += Word(K);
        break;
      case SymOp::Mul:
        for (size_t K = 1; K < St.NumArgs; ++K)
          Value *= Word(K);
        break;
      case SymOp::And:
        for (size_t K = 1; K < St.NumArgs; ++K)
          Value &= Word(K);
        break;
      case SymOp::Or:
        for (size_t K = 1; K < St.NumArgs; ++K)
          Value |= Word(K);
        break;
      case SymOp::Xor:
        for (size_t K = 1; K < St.NumArgs; ++K)
          Value ^= Word(K);
        break;
      case SymOp::Not:
        Value = ~Value;
        break;
      default:
        llvm_unreachable("not a low-prefix operator");
      }
      ScratchAP[I] = Value & (~uint64_t(0) >> (64 - St.Width));
      continue;
    }

    if (St.Op == SymOp::Mul && St.Aux) {
      // All nonconstant factors are observed modulo the prepared mask.
      // Multiplying their prefixes first is exact in that ring. Restoring
      // the original coefficient then needs just one wide multiplication,
      // and preserves every output bit any parent can observe.
      uint64_t Product = ScratchAP[A[1]].getRawData()[0];
      for (size_t K = 2; K < St.NumArgs; ++K)
        Product *= ScratchAP[A[K]].getRawData()[0];
      ScratchAP[I] = ScratchAP[A[0]];
      ScratchAP[I] *= Product & St.Aux;
      continue;
    }

    // Every operand precedes this output slot. Read it directly through
    // the shared operator evaluator instead of copying wide APInts into a
    // temporary argument vector for every step and validation assignment.
    evalNodeAPImpl(
        Ctx, St.Node, St.NumArgs, ScratchAP[I],
        [&](size_t K) -> const llvm::APInt & { return ScratchAP[A[K]]; });
  }

  return ScratchAP[RootSlot];
}

llvm::APInt evalNodeAP(const SymContext &Ctx, SymRef R,
                       llvm::ArrayRef<llvm::APInt> Args) {
  llvm::APInt Result;
  evalNodeAPImpl(Ctx, R, Args.size(), Result,
                 [&](size_t I) -> const llvm::APInt & { return Args[I]; });
  return Result;
}

} // namespace neverd::symbolic
