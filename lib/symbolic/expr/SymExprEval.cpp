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

#include "../SymFunnel.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <limits>
#include <tuple>
#include <utility>

namespace neverd::symbolic {

namespace {

bool preservesLowBits(SymOp Op) {
  return Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
         Op == SymOp::Or || Op == SymOp::Xor || Op == SymOp::Not;
}

unsigned literalSuffixZeros(const SymContext &Ctx, SymRef R) {
  const auto Words = Ctx.constWords(R);
  return std::min(Ctx.width(R), llvm::APInt::tcLSB(Words.data(), Words.size()));
}

// A concatenated literal tail or the common scale of both addends supplies
// a proven zero suffix. The coefficient itself still needs its complete value.
unsigned factorScaleZeros(const SymContext &Ctx, SymRef S) {
  if (Ctx.op(S) == SymOp::Concat) {
    const auto Tail = Ctx.operands(S).back();
    if (!Ctx.isConst(Tail))
      return 0;
    return literalSuffixZeros(Ctx, Tail);
  }
  if (Ctx.op(S) != SymOp::Add || Ctx.numOperands(S) != 2)
    return 0;
  unsigned Zeros = Ctx.width(S);
  for (SymRef T : Ctx.operands(S)) {
    if (Ctx.op(T) == SymOp::Mul)
      T = Ctx.operand(T, 0);
    if (!Ctx.isConst(T))
      return 0;
    Zeros = std::min(Zeros, literalSuffixZeros(Ctx, T));
    if (!Zeros)
      break;
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
      Zeros = literalSuffixZeros(Ctx, Operands.front());
    unsigned Coefficient = 0;
    if constexpr (Wide) {
      if (St.Op == SymOp::Mul && Demand > 64 && Zeros < Demand - 64) {
        for (unsigned I = 0; I < Operands.size(); ++I) {
          const unsigned Candidate = factorScaleZeros(Ctx, Operands[I]);
          if (Candidate > Zeros &&
              (Candidate >= Demand - 64 ||
               llvm::APInt::getNumWords(Demand - Candidate) <
                   llvm::APInt::getNumWords(Demand - Zeros))) {
            Zeros = Candidate;
            Coefficient = I;
            if (Candidate >= Demand - 64)
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
  unsigned ProductBits = 0;
  const auto Needed = visitRequiredPrefixes(
      Ctx, Root, RootSlot, NumSlots, Steps, Args,
      [&](const Step &St, unsigned Demand, unsigned Zeros, unsigned) {
        if (!Zeros || Zeros >= Demand)
          return;
        const unsigned Bits = Demand - Zeros;
        // More factors than distinct residues makes repeated prefixes likely.
        // NumArgs is 32 bits, so no modulus of 2^32 or larger can qualify.
        if (Bits < 32 && St.NumArgs - 1 > (uint32_t(1) << Bits))
          ProductBits = std::max(ProductBits, Bits);
      });
  using ResidueKey = std::tuple<unsigned, unsigned, uint32_t, uint32_t>;
  llvm::DenseMap<ResidueKey, uint32_t> Residues;
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
    // Each consumer observes at most Needed[slot] bits. Two binary words
    // with identical projected inputs and operation can share that slot,
    // even when different constants made them distinct full-width nodes.
    unsigned Bits = Needed[St.OutputSlot];
    if (Bits <= ProductBits && St.NumArgs == 2 && preservesLowBits(St.Op)) {
      const auto First = Ctx.operand(St.Node, 0);
      const bool Constant = Ctx.isConst(First);
      uint32_t L = Args[St.FirstArg], R = Args[St.FirstArg + 1];
      if (Constant)
        L = Ctx.node(First).Aux & ((uint32_t(1) << Bits) - 1);
      else if (L > R)
        std::swap(L, R);
      auto [It, Inserted] = Residues.try_emplace(
          ResidueKey{2 * unsigned(St.Op) + Constant, Bits, L, R},
          St.OutputSlot);
      if (!Inserted) {
        Aliases[St.OutputSlot] = It->second;
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
        // If a product observes only the low lane, extension bits and the
        // other concatenated lanes cannot reach any use of this output.
        if (Demand <= 64 && Demand < St.Width &&
            (St.Op == SymOp::ZExt || St.Op == SymOp::SExt ||
             St.Op == SymOp::Concat) &&
            Demand <= Ctx.width(Ctx.operands(St.Node).back())) {
          St.FirstArg += St.NumArgs - 1;
          St.NumArgs = 1;
          St.Op = SymOp::Extract;
          St.Aux = 0;
          St.Width = Demand;
          return;
        }
        if (preservesLowBits(St.Op) && Demand <= 64)
          St.Width = Demand;
        if (Zeros && Demand > 64) {
          const unsigned FactorBits = Demand - std::min(Demand - 1, Zeros);
          // A flat product amortizes narrowing across its operands. For two
          // factors, keep the cheap scalar-literal multiply and project a
          // dynamic coefficient only when at least half the limbs disappear.
          const size_t PrefixWords = llvm::APInt::getNumWords(FactorBits);
          const size_t FullWords = llvm::APInt::getNumWords(St.Width);
          if (FactorBits <= 64 ||
              (St.NumArgs > 2
                   ? PrefixWords < FullWords
                   : !Ctx.isConst(Ctx.operand(St.Node, Coefficient)) &&
                         PrefixWords <= FullWords / 2)) {
            St.Aux = FactorBits;
            std::swap(ArgSlots[St.FirstArg],
                      ArgSlots[St.FirstArg + Coefficient]);
          }
        }
      });
}

// If any four weights on the same evaluation slot multiply to zero modulo
// 2^Bits, their affine product has degree at most three in that slot. Build
// the coefficients once and evaluate the original input at every assignment.
size_t groupAffineFactors(llvm::MutableArrayRef<uint32_t> Slots,
                          llvm::SmallVectorImpl<uint64_t> &Words,
                          llvm::MutableArrayRef<uint8_t> Uses, unsigned Bits) {
  const unsigned N = llvm::APInt::getNumWords(Bits);
  const size_t Stride = 1 + 2 * size_t(N);
  const unsigned MinZeros = llvm::divideCeil(Bits, 4u);
  auto Eligible = [&](size_t I) {
    const auto *Row = Words.data() + I * Stride;
    return Row[0] == 1 && llvm::APInt::tcLSB(Row + 1 + N, N) >= MinZeros;
  };
  llvm::fill(Uses, 0);
  bool CanGroup = false;
  for (auto [I, Slot] : llvm::enumerate(Slots.drop_front()))
    if (Uses[Slot] < 3 && Eligible(I))
      CanGroup |= ++Uses[Slot] == 3;
  if (!CanGroup)
    return Slots.size();

  struct Polynomial {
    uint32_t Slot;
    unsigned MinZeros;
  };
  llvm::DenseMap<uint32_t, unsigned> Index;
  llvm::SmallVector<Polynomial, 4> Groups;
  llvm::SmallVector<uint64_t, 32> Coefficients;
  llvm::SmallVector<uint64_t, 8> Scratch(2 * size_t(N));
  size_t Kept = 0;
  for (auto [I, Slot] : llvm::enumerate(Slots.drop_front())) {
    const auto Row = llvm::ArrayRef(Words).slice(I * Stride, Stride);
    if (Uses[Slot] < 3 || !Eligible(I)) {
      if (Kept != I) {
        Slots[Kept + 1] = Slot;
        llvm::copy(Row, Words.begin() + Kept * Stride);
      }
      ++Kept;
      continue;
    }
    const auto *A = Row.data() + 1;
    const auto *B = A + N;
    const unsigned Zeros = std::min(Bits, llvm::APInt::tcLSB(B, N));
    const auto [It, New] = Index.try_emplace(Slot, Groups.size());
    if (New) {
      Groups.push_back({Slot, Zeros});
      Coefficients.append(A, B + N);
      Coefficients.append(2 * size_t(N), 0);
      continue;
    }
    auto &G = Groups[It->second];
    G.MinZeros = std::min(G.MinZeros, Zeros);
    auto *C = Coefficients.data() + It->second * 4 * size_t(N);
    // Every coefficient of degree d carries at least d * MinZeros zero
    // bits. Accumulate descending degrees so the previous row stays live.
    const unsigned Degree = (Bits - 1) / G.MinZeros;
    if (N == 1) {
      for (unsigned K = Degree; K; --K)
        C[K] = C[K] * *A + C[K - 1] * *B;
      C[0] *= *A;
    } else {
      auto *First = Scratch.data(), *Second = First + N;
      for (unsigned K = Degree; K; --K) {
        auto *Current = C + K * N;
        llvm::APInt::tcMultiply(First, Current, A, N);
        llvm::APInt::tcMultiply(Second, Current - N, B, N);
        llvm::APInt::tcAdd(First, Second, 0, N);
        std::copy_n(First, N, Current);
      }
      llvm::APInt::tcMultiply(First, C, A, N);
      std::copy_n(First, N, C);
    }
  }
  Words.resize(Kept * Stride);
  const uint64_t LastMask = ~uint64_t(0) >> ((-Bits) % 64);
  for (const auto &[I, G] : llvm::enumerate(Groups)) {
    Slots[++Kept] = G.Slot;
    auto *C = Coefficients.data() + 4 * I * N;
    unsigned Degree = std::max(1u, (Bits - 1) / G.MinZeros);
    for (unsigned K = 0; K <= Degree; ++K)
      C[(K + 1) * N - 1] &= LastMask;
    while (Degree > 1 && llvm::all_of(llvm::ArrayRef(C + Degree * N, N),
                                      [](uint64_t Word) { return !Word; }))
      --Degree;
    Words.push_back(Degree == 1);
    if (Degree > 1)
      Words.push_back(Degree);
    Words.append(C, C + (Degree + 1) * N);
  }
  return Kept + 1;
}

// In C * product(A + B*x) only the low W-ctz(C) bits of each factor
// can reach the result. Lower each literal affine chain once in that ring.
// Each surviving source is still evaluated independently at every assignment.
template <typename Step, typename SlotMap>
size_t prepareAffineFactors(
    const SymContext &Ctx, SymRef Root, llvm::MutableArrayRef<Step> Steps,
    llvm::MutableArrayRef<uint32_t> Args, const SlotMap &Slot,
    llvm::SmallVectorImpl<uint64_t> &Words,
    llvm::SmallVectorImpl<std::pair<uint32_t, SymRef>> &Constants) {
  const auto Factors = Ctx.operands(Root);
  const auto Width = Ctx.width(Root);
  if (Factors.size() < 3 || !Ctx.isConst(Factors.front()) ||
      Ctx.op(Factors[1]) != SymOp::Add)
    return Steps.size();
  const unsigned Bits = Width - Ctx.constValue(Factors.front()).countr_zero();
  if (Bits == Width)
    return Steps.size();

  // The ordinary DAG already shares identical factors. Allocate one row
  // per distinct affine factor and retain its multiplicity in the product.
  size_t Unique = 0;
  SymRef Previous;
  for (auto F : Factors.drop_front()) {
    if (F == Previous)
      continue;
    if (Ctx.op(F) != SymOp::Add || Ctx.numOperands(F) != 2 ||
        !Ctx.isConst(Ctx.operand(F, 0)))
      return Steps.size();
    Previous = F;
    ++Unique;
  }
  // Stop peeling at a shared coordinate: its original slot computes it
  // once even when long affine chains feed many different factors.
  llvm::SmallVector<uint8_t, 64> Uses(Slot.size());
  for (const auto I : Args)
    Uses[I] = std::min<unsigned>(2, Uses[I] + 1);
  const auto N = llvm::APInt::getNumWords(Bits);
  auto Slots = Args.slice(Steps.back().FirstArg, Unique + 1);
  Slots.front() = Slot.find(Factors.front().index())->second - 1;
  Words.reserve((1 + 2 * size_t(N)) * Unique);
  struct Slice {
    unsigned Bits;
    uint32_t Slot;
  };
  llvm::DenseMap<std::pair<uint32_t, unsigned>, unsigned> SliceIndex;
  llvm::SmallVector<Slice, 8> Slices;
  llvm::SmallVector<std::pair<size_t, unsigned>, 16> SliceRows;
  size_t Out = 1;
  auto AppendSource = [&](SymRef F, unsigned Live) {
    const auto SourceSlot = Slot.find(F.index())->second - 1;
    // Only these low source bits survive multiplication. An existing wider
    // slice at the same position can supply them for every assignment.
    const bool Extract = Ctx.op(F) == SymOp::Extract;
    const unsigned Start =
        Extract ? Ctx.node(F).Aux : detail::sourceSuffixStart(Ctx, F);
    if (Extract || Start) {
      const auto Source = Ctx.operand(F, 0);
      const unsigned Available =
          Extract ? Ctx.width(F) : Ctx.width(Source) - Start;
      if (Live <= Available) {
        const auto [It, New] =
            SliceIndex.try_emplace({Source.index(), Start}, Slices.size());
        const auto Index = It->second;
        if (New)
          Slices.push_back({Available, SourceSlot});
        else if (Available > Slices[Index].Bits)
          Slices[Index] = {Available, SourceSlot};
        SliceRows.emplace_back(Out, Index);
      }
    }
    Slots[Out++] = SourceSlot;
  };
  for (size_t I = 1; I < Factors.size();) {
    auto F = Factors[I];
    const auto First = I++;
    while (I < Factors.size() && Factors[I] == F)
      ++I;
    size_t Copies = I - First;
    const auto Scaled = Ctx.operand(F, 1);
    if (Copies == 1 && Ctx.op(Scaled) == SymOp::Mul &&
        Ctx.numOperands(Scaled) == 2 && Ctx.isConst(Ctx.operand(Scaled, 0))) {
      const auto Weight = Ctx.constWords(Ctx.operand(Scaled, 0)).take_front(N);
      const unsigned Live =
          Bits - std::min(Bits, llvm::APInt::tcLSB(Weight.data(), N));
      auto Source = Ctx.operand(Scaled, 1);
      const auto Op = Ctx.op(Source);
      if (Op == SymOp::Concat || Op == SymOp::SExt || Op == SymOp::ZExt) {
        const auto Low = Ctx.operands(Source).back();
        if (Ctx.width(Low) >= Live)
          Source = Low;
      }
      // The remaining source is a real slice, shift or input. Its original
      // slot supplies every bit retained by the literal weight.
      const auto SourceOp = Ctx.op(Source);
      if (SourceOp == SymOp::Extract || SourceOp == SymOp::LShr ||
          SourceOp == SymOp::AShr || SourceOp == SymOp::Rol ||
          SourceOp == SymOp::Ror || SourceOp == SymOp::Var) {
        const auto Offset = Ctx.constWords(Ctx.operand(F, 0)).take_front(N);
        Words.push_back(1);
        Words.append(Offset.begin(), Offset.end());
        Words.append(Weight.begin(), Weight.end());
        AppendSource(Source, Live);
        continue;
      }
    }
    llvm::APInt A(Bits, Ctx.constWords(Ctx.operand(F, 0)).take_front(N));
    F = Ctx.operand(F, 1);
    llvm::APInt B(Bits, 1);
    while (Uses[Slot.find(F.index())->second - 1] == 1) {
      const auto Op = Ctx.op(F);
      const auto Children = Ctx.operands(F);
      if ((Op == SymOp::Add || Op == SymOp::Mul) && Children.size() == 2 &&
          Ctx.isConst(Children.front())) {
        llvm::APInt K(Bits, Ctx.constWords(Children.front()).take_front(N));
        if (Op == SymOp::Mul)
          B = B.isOne() ? std::move(K) : B * K;
        else
          A += B * K;
        F = Children.back();
        continue;
      }
      if ((Op == SymOp::Concat || Op == SymOp::ZExt || Op == SymOp::SExt) &&
          Ctx.width(Children.back()) >= Bits - B.countr_zero()) {
        F = Children.back();
        continue;
      }
      if (Op == SymOp::Extract && Ctx.node(F).Aux == 0 &&
          Ctx.width(F) >= Bits - B.countr_zero()) {
        F = Children.front();
        continue;
      }
      break;
    }
    // Shared scalar multiples can hide the power of two that makes the
    // weights nilpotent. Peel a single shared scale to expose that source;
    // shared affine chains otherwise keep their ordinary evaluation slots.
    if (Ctx.op(F) == SymOp::Mul && Ctx.numOperands(F) == 2 &&
        Ctx.isConst(Ctx.operand(F, 0))) {
      const auto Digits = Ctx.constWords(Ctx.operand(F, 0)).take_front(N);
      if (!(Digits.front() & 1)) {
        llvm::APInt Scale(Bits, Digits);
        B = B.isOne() ? std::move(Scale) : B * Scale;
        F = Ctx.operand(F, 1);
      }
    }
    // A shared scale or carrier can have other full-width consumers. Only
    // this factor can read through it when the weight discards its upper bits.
    if (Ctx.op(F) == SymOp::Concat || Ctx.op(F) == SymOp::ZExt ||
        Ctx.op(F) == SymOp::SExt) {
      const auto Low = Ctx.operands(F).back();
      if (Ctx.width(Low) >= Bits - B.countr_zero())
        F = Low;
    }
    if (Copies > 1 && B.countr_zero() >= Bits - Bits / 2) {
      // B squared is zero in this ring. The only nonconstant term of
      // (A+B*x)^m is m*A^(m-1)*B*x, including when A is even.
      const auto Others = llvm::APIntOps::pow(A, Copies - 1);
      A *= Others;
      B *= Others;
      B *= Copies;
      Copies = 1;
    }
    Words.push_back(Copies);
    AppendSource(F, Bits - B.countr_zero());
    Words.append(A.getRawData(), A.getRawData() + N);
    Words.append(B.getRawData(), B.getRawData() + N);
  }
  for (const auto &[Row, Index] : SliceRows)
    Slots[Row] = Slices[Index].Slot;
  Slots = Slots.take_front(groupAffineFactors(Slots, Words, Uses, Bits));
  Steps.back().NumArgs = Slots.size();
  Steps.back().Aux = Bits;
  // Consumers outside a factor still need their complete inputs. Every
  // variable lookup retains its original position, including discarded lanes.
  llvm::fill(Uses, 0);
  for (auto I : Slots)
    Uses[I] = 1;
  for (const auto &St : llvm::reverse(Steps.drop_back()))
    if (Uses[St.OutputSlot])
      for (auto I : Args.slice(St.FirstArg, St.NumArgs))
        Uses[I] = 1;
  llvm::erase_if(Constants, [&](const auto &C) { return !Uses[C.first]; });
  const auto RootStep = Steps.back();
  size_t Count = 0;
  for (const auto &St : Steps.drop_back())
    if (St.Op == SymOp::Var || Uses[St.OutputSlot])
      Steps[Count++] = St;
  Steps[Count++] = RootStep;
  return Count;
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
    PlanWords.resize(NextSlot);
    for (const auto &[Slot, Node] : ConstantSlots)
      PlanWords[Slot] = Ctx.node(Node).Aux;
  } else {
    NativeState = NativePreparation::Done;
    // A scaled wide product observes only a prefix of each remaining factor.
    // Other operators retain complete operands: a high bit can affect even
    // the lowest result bit of a comparison, division or right shift.
    prepareWidePrefixes(Ctx, Root, RootSlot, NextSlot,
                        llvm::MutableArrayRef<Step>(Steps), ArgSlots);
    if (Ctx.op(Root) == SymOp::Mul)
      Steps.resize(
          prepareAffineFactors(Ctx, Root, llvm::MutableArrayRef<Step>(Steps),
                               ArgSlots, Slot, PlanWords, ConstantSlots));
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
      Steps.resize(prepareNativeResidues(Ctx, Root, RootSlot, PlanWords.size(),
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

// Keep the less frequent width-dependent operators off the common evaluator's
// stack. Its caller masks the result to the declared width.
LLVM_ATTRIBUTE_NOINLINE
uint64_t evalGeneralU64(const SymContext &Ctx, SymRef Node,
                        llvm::ArrayRef<uint32_t> Args, const uint64_t *Values) {
  const auto Op = Ctx.op(Node);
  if (Op == SymOp::SExt)
    return llvm::SignExtend64(Values[Args[0]], Ctx.width(Ctx.operand(Node, 0)));
  if (Op == SymOp::Rol || Op == SymOp::Ror) {
    const unsigned Width = Ctx.width(Node);
    const unsigned Count = Values[Args[1]] % Width;
    const uint64_t Value = Values[Args[0]];
    if (!Count)
      return Value;
    const unsigned Left = Op == SymOp::Rol ? Count : Width - Count;
    return (Value << Left) | (Value >> (Width - Left));
  }
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
  uint64_t *S = PlanWords.data();

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
      Res = evalGeneralU64(Ctx, St.Node, {A, St.NumArgs}, S);
      break;
    }
    }

    S[I] = mask(Res, W);
  }

  return PlanWords[RootSlot];
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
    // A small signed coefficient times a power of two needs only one
    // word multiply. Restore its sign and shift in the original modulus.
    if (W > 64 && NumArgs == 2 && Ctx.isConst(Ctx.operand(R, 0))) {
      const auto &Scale = Arg(0);
      const unsigned Shift = Scale.countr_zero();
      if (Shift < W && Scale.getSignificantBits() - Shift <= 64) {
        const unsigned Bits = std::min(64u, W - Shift);
        uint64_t Multiplier = Scale.extractBitsAsZExtValue(Bits, Shift);
        const bool Negative = Scale.isNegative();
        if (Negative)
          Multiplier = -uint64_t(llvm::SignExtend64(Multiplier, Bits));
        Result = Arg(1);
        if (Multiplier != 1)
          Result *= Multiplier;
        if (Negative)
          Result.negate();
        Result <<= Shift;
        return;
      }
    }
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

// tcMultiply writes the low words into a disjoint destination. Alternating
// the buffers avoids allocating a full-width APInt for each factor.
llvm::APInt multiplyLowWords(llvm::ArrayRef<llvm::APInt> Values,
                             llvm::ArrayRef<uint32_t> Factors, unsigned Bits) {
  const unsigned Words = llvm::APInt::getNumWords(Bits);
  llvm::SmallVector<uint64_t, 16> Storage(2 * size_t(Words));
  auto *Product = Storage.data();
  auto *Next = Product + Words;
  std::copy_n(Values[Factors.front()].getRawData(), Words, Product);
  for (auto Slot : Factors.drop_front()) {
    llvm::APInt::tcMultiply(Next, Product, Values[Slot].getRawData(), Words);
    std::swap(Product, Next);
  }
  return llvm::APInt(Bits, llvm::ArrayRef(Product, Words));
}
// The low (Width - Bits) bits of Scale are zero. Multiply just its
// remaining bits and restore the scale in the original ring.
llvm::APInt multiplyShiftedPrefix(const llvm::APInt &Scale,
                                  const llvm::APInt &Prefix) {
  const unsigned Bits = Prefix.getBitWidth();
  const unsigned Shift = Scale.getBitWidth() - Bits;
  const unsigned N = Prefix.getNumWords();
  llvm::SmallVector<uint64_t, 8> Scratch(2 * size_t(N));
  auto *High = Scratch.data(), *Product = High + N;
  llvm::APInt::tcExtract(High, N, Scale.getRawData(), Bits, Shift);
  llvm::APInt::tcMultiply(Product, High, Prefix.getRawData(), N);
  llvm::APInt Result(Scale.getBitWidth(), 0);
  for (unsigned I = 0; I < N; ++I) {
    const unsigned Offset = I * 64;
    Result.insertBits(Product[I], Shift + Offset, std::min(64u, Bits - Offset));
  }
  return Result;
}

// Evaluate the literal affine factors directly in their common modulus.
// All three products use disjoint buffers, including arbitrary partial limbs.
llvm::APInt evaluateAffineFactors(llvm::ArrayRef<llvm::APInt> Values,
                                  llvm::ArrayRef<uint32_t> Sources,
                                  llvm::ArrayRef<uint64_t> Coefficients,
                                  unsigned Bits) {
  const unsigned N = llvm::APInt::getNumWords(Bits);
  llvm::SmallVector<uint64_t, 32> Storage(4 * size_t(N), 0);
  auto *Product = Storage.data(), *Next = Product + N;
  auto *Factor = Next + N, *Padded = Factor + N;
  Product[0] = 1;
  for (auto I : Sources.drop_front()) {
    const auto &X = Values[I];
    const auto *Raw = X.getRawData();
    if (X.getNumWords() < N) {
      std::copy_n(Raw, X.getNumWords(), Padded);
      std::fill(Padded + X.getNumWords(), Padded + N, 0);
      Raw = Padded;
    }
    const auto Repeat = Coefficients.front();
    Coefficients = Coefficients.drop_front();
    const unsigned Degree = Repeat ? 1 : Coefficients.front();
    if (!Repeat)
      Coefficients = Coefficients.drop_front();
    llvm::APInt::tcMultiply(Factor, Raw, Coefficients.data() + Degree * N, N);
    for (unsigned K = Degree - 1; K != 0; --K) {
      llvm::APInt::tcAdd(Factor, Coefficients.data() + K * N, 0, N);
      llvm::APInt::tcMultiply(Next, Raw, Factor, N);
      std::swap(Factor, Next);
    }
    llvm::APInt::tcAdd(Factor, Coefficients.data(), 0, N);
    for (uint64_t K = 0, Count = Repeat ? Repeat : 1; K < Count; ++K) {
      llvm::APInt::tcMultiply(Next, Product, Factor, N);
      std::swap(Product, Next);
    }
    Coefficients = Coefficients.drop_front((Degree + 1) * N);
  }
  const auto &Scale = Values[Sources.front()];
  return multiplyShiftedPrefix(Scale,
                               llvm::APInt(Bits, llvm::ArrayRef(Product, N)));
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
  const bool AffineProduct = !FitsU64 && !PlanWords.empty();
  for (const Step &St : llvm::ArrayRef(Steps).drop_back(AffineProduct)) {
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

    if (St.Op == SymOp::Extract && St.Width <= 64) {
      const auto &Word = ScratchAP[A[0]];
      ScratchAP[I] =
          llvm::APInt(St.Width, Word.extractBitsAsZExtValue(St.Width, St.Aux));
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
      // Evaluate the remaining factors in their common low-bit modulus.
      // Only the coefficient needs its original width to restore the result.
      const unsigned Bits = St.Aux;
      ScratchAP[I] = ScratchAP[A[0]];
      if (Bits <= 64) {
        uint64_t Product = ScratchAP[A[1]].getRawData()[0];
        for (size_t K = 2; K < St.NumArgs; ++K)
          Product *= ScratchAP[A[K]].getRawData()[0];
        ScratchAP[I] *= Product & (~uint64_t(0) >> (64 - Bits));
      } else {
        auto Prefix =
            St.NumArgs == 2
                ? ScratchAP[A[1]].zextOrTrunc(Bits)
                : multiplyLowWords(ScratchAP, {A + 1, St.NumArgs - 1}, Bits);
        const unsigned Shift = St.Width - Bits;
        if (ScratchAP[I].countr_zero() >= Shift) {
          ScratchAP[I] = multiplyShiftedPrefix(ScratchAP[I], Prefix);
        } else {
          ScratchAP[I] *= Prefix.zext(St.Width);
        }
      }
      continue;
    }

    // Every operand precedes this output slot. Read it directly through
    // the shared operator evaluator instead of copying wide APInts into a
    // temporary argument vector for every step and validation assignment.
    evalNodeAPImpl(
        Ctx, St.Node, St.NumArgs, ScratchAP[I],
        [&](size_t K) -> const llvm::APInt & { return ScratchAP[A[K]]; });
  }

  if (AffineProduct) {
    const auto &St = Steps.back();
    return evaluateAffineFactors(
        ScratchAP, llvm::ArrayRef(ArgSlots).slice(St.FirstArg, St.NumArgs),
        PlanWords, St.Aux);
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
