//===- SymMBAProductProgression.cpp - Prove modular factor windows --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymMBAProductProgression.h"

#include "Limits.h"

#include <algorithm>
#include <bit>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {
namespace {

template <typename T> void vote(T &Candidate, unsigned &Votes, const T &Value) {
  if (!Votes) {
    Candidate = Value;
    Votes = 1;
  } else if (Candidate == Value) {
    ++Votes;
  } else {
    --Votes;
  }
}

bool sameBasis(const ProductFactor &A, const ProductFactor &B) {
  return A.X == B.X && A.Y.Base == B.Y.Base && A.Z.Base == B.Z.Base &&
         A.Y.Shift == B.Y.Shift && A.Z.Shift == B.Z.Shift;
}

} // namespace

std::optional<ProductWindow> chooseProductWindow(unsigned Bits, size_t Count,
                                                 bool SingleMask) {
  if (Bits < limits::kMinProductProgressionBits)
    return std::nullopt;
  if (SingleMask) {
    // A single masked operand changes its top bit one half-period earlier
    // than a product of two even operands.
    const unsigned Period = 1u << Bits, Half = Period / 2;
    if (Count < Half - Half / 4 || Count > Half + Half / 4)
      return std::nullopt;
    return ProductWindow{Period, Half, 1, Half / 4 + 1};
  }
  const unsigned Period = 1u << (Bits - 1);
  const unsigned Half = Period / 2;
  const unsigned FullCopies = Count / Period;
  const unsigned Tail = Count % Period;
  const unsigned Nearest = FullCopies + (Tail >= Half);
  auto Near = [&](unsigned Length, unsigned Copies,
                  unsigned MaxExceptions) -> std::optional<ProductWindow> {
    const auto Target = size_t(Copies) * Length;
    const auto Delta = std::max(Count, Target) - std::min(Count, Target);
    if (Copies && Delta <= MaxExceptions)
      return ProductWindow{Period, Length, Copies, MaxExceptions};
    return std::nullopt;
  };

  // A small difference can use either a short inverse or the original tail.
  // Prefer complete periods, which have no half-period correction to build.
  if (auto W = Near(Period, Nearest,
                    Period / limits::kFullPeriodExceptionDivisor + 1))
    return W;
  if (auto W = Near(Half, 1, Half / limits::kHalfPeriodExceptionDivisor + 1))
    return W;
  if (!FullCopies && Count > Half)
    if (auto W = Near(Period, 1, Period / limits::kHalfPeriodExceptionDivisor))
      return W;

  // One complete period equals the square of either half under the parity
  // gate. Fold full periods and an adjacent half in a single odd power.
  // At equal distance keep an existing tail ahead of deriving its inverse.
  if (FullCopies && Tail > Half / 2)
    if (auto W = Near(Half, 2 * FullCopies + 1, Half / 2))
      return W;
  if (FullCopies && Tail <= Period / limits::kProductWindowTailDivisor)
    return ProductWindow{Period, Period, FullCopies, Period - 1};
  if (!FullCopies && Count >= Half / 2)
    return Near(Half, 1, Half - 1);
  return std::nullopt;
}

unsigned ProductProgression::basePrecision(unsigned Bits) const {
  while (Bits >= limits::kMinProductProgressionBits) {
    // At each recursive square the two halves are this many indices apart.
    // Both masked additions must commute with that shift.
    const auto Step =
        1u << (Bits -
               ((Window.Length == Window.Period || !Basis.Z.Base) ? 2 : 3));
    if ((Step & ((1u << Basis.Y.Shift) - 1)) ||
        ((Step * ZStep) & ((1u << Basis.Z.Shift) - 1)))
      break;
    --Bits;
  }
  return Bits;
}

std::optional<ProductProgression>
matchProductProgression(llvm::MutableArrayRef<ProductFactor> Factors,
                        ProductWindow Window, bool HasRetained,
                        llvm::function_ref<bool(size_t, size_t)> Charge) {
  const auto [Period, Length, Copies, MaxExceptions] = Window;
  const bool SingleMask = !Factors.front().Z.Base;
  const unsigned Mask = Period - 1,
                 WordMask = SingleMask ? Mask : 2 * Period - 1;
  unsigned Votes = 0;
  size_t Majority = 0;
  for (size_t I = 0; I < Factors.size(); ++I) {
    if (!Votes) {
      Majority = I;
      Votes = 1;
    } else if (sameBasis(Factors[I], Factors[Majority])) {
      ++Votes;
    } else {
      --Votes;
    }
  }

  // Either multiplicand can advance by one. Votes only choose a hypothesis.
  // Every consumed source must match both offset congruences below.
  for (unsigned Axis = 0; Axis != (SingleMask ? 1u : 2u); ++Axis) {
    if (!Charge(4 * Factors.size() + 5 * Period,
                Period *
                        (sizeof(const ProductFactor *) + 3 * sizeof(unsigned)) +
                    Factors.size() * 2 * sizeof(SymRef)))
      return std::nullopt;
    if (Axis)
      for (auto &F : Factors)
        std::swap(F.Y, F.Z);
    const auto &Basis = Factors[Majority];
    std::vector<const ProductFactor *> Slots(Period);
    for (const auto &F : Factors)
      if (sameBasis(F, Basis))
        Slots[F.Y.Offset & Mask] = &F;
    std::pair<unsigned, unsigned> Slopes{}, Intercepts{};
    Votes = 0;
    for (unsigned I = 0; I < Period; ++I) {
      const auto *A = Slots[I], *B = Slots[(I + 1) & Mask];
      if (A && B)
        vote(Slopes, Votes,
             std::pair{(B->Offset - A->Offset) & WordMask,
                       (B->Z.Offset - A->Z.Offset) & Mask});
    }
    if (!Votes || (Slopes.first & 1))
      continue;
    auto [DX, DZ] = Slopes;
    Votes = 0;
    for (const auto *F : Slots)
      if (F)
        vote(Intercepts, Votes,
             std::pair{(F->Offset - DX * F->Y.Offset) & WordMask,
                       (F->Z.Offset - DZ * F->Y.Offset) & Mask});
    if (!Votes)
      continue;
    const auto [OX, OZ] = Intercepts;
    auto Fits = [&](const ProductFactor &F) {
      const unsigned I = F.Y.Offset & Mask;
      return sameBasis(F, Basis) && F.Offset == ((OX + DX * I) & WordMask) &&
             (F.Z.Offset & Mask) == ((OZ + DZ * I) & Mask);
    };

    unsigned Start = 0;
    if (Length < Period) {
      // Equal-coverage windows can retain different spellings. Fold the
      // nonzero offsets first so the remaining source needs fewer additions.
      std::vector<unsigned> Present(Period), Weight(Period);
      for (const auto &F : Factors)
        if (Fits(F)) {
          const auto I = F.Y.Offset & Mask;
          ++Present[I];
          Weight[I] = unsigned(I != 0) + unsigned(F.Offset != 0) +
                      unsigned((F.Z.Offset & Mask) != 0);
        }
      for (unsigned I = 0; I < Period; ++I) {
        Present[I] = Present[I] > Copies / 2;
        if (!Present[I])
          Weight[I] = 0;
      }
      unsigned Coverage =
          std::count(Present.begin(), Present.begin() + Length, 1u);
      unsigned Cost = 0;
      for (unsigned I = 0; I < Length; ++I)
        Cost += Weight[I];
      unsigned Best = Coverage, BestCost = Cost;
      for (unsigned I = 1; I < Period; ++I) {
        Coverage += Present[(I + Length - 1) & Mask];
        Coverage -= Present[I - 1];
        Cost += Weight[(I + Length - 1) & Mask];
        Cost -= Weight[I - 1];
        if (Coverage > Best || (Coverage == Best && Cost > BestCost)) {
          Start = I;
          Best = Coverage;
          BestCost = Cost;
        }
      }
    }

    ProductProgression Result{Window, Basis, Start, OX, OZ, DX, DZ, {}, {}};
    std::vector<unsigned> Seen(Period);
    for (const auto &F : Factors) {
      const auto I = F.Y.Offset & Mask;
      const bool In = ((I - Start) & Mask) < Length;
      const auto Needed = Length == Period ? Copies : Copies / 2 + unsigned(In);
      if (Fits(F) && Seen[I] < Needed)
        ++Seen[I];
      else
        Result.Retained.push_back(F.Source);
    }
    const size_t Missing =
        size_t(Copies) * Length + Result.Retained.size() - Factors.size();
    // A partial group may already have removed a nonunit. Prefer the ordinary
    // mixed proof to a full-period inverse of several additional words.
    if (Missing + Result.Retained.size() > MaxExceptions ||
        (Length == Period && HasRetained &&
         Missing > limits::kMaxDirectOddInverseFactors))
      continue;
    if (!Charge(Length + Missing, Missing * sizeof(unsigned)))
      return std::nullopt;
    for (unsigned J = 0; J < (Copies == 1 ? Length : Period); ++J) {
      const unsigned I = (Start + J) & Mask;
      const auto Needed =
          Length == Period ? Copies : Copies / 2 + unsigned(J < Length);
      for (unsigned K = Seen[I]; K < Needed; ++K)
        Result.Missing.push_back(I);
    }
    return Result;
  }
  return std::nullopt;
}

// A few low-bit evaluations choose when to try the complete progression
// proof before affine expansion. Every accepted factor, exceptional word and
// parity gate is independently established in the decision solver.
bool hasMixedProgressionHint(const SymContext &Ctx, SymRef Root,
                             llvm::function_ref<bool(size_t)> Charge) {
  auto Args = Ctx.operands(Root);
  unsigned Bits = Ctx.width(Root);
  if (Ctx.isConst(Args.front())) {
    Bits -= Ctx.constValue(Args.front()).countr_zero();
    Args = Args.drop_front();
  }
  if (Bits < limits::kMinProductProgressionBits ||
      Bits > limits::kMaxAffineResidueTableBits ||
      Args.size() < (1u << (Bits - 3)))
    return false;
  auto Hint = [&](SymRef Word) {
    if (Ctx.op(Word) != SymOp::Xor || Ctx.numOperands(Word) > 3)
      return false;
    for (auto F : Ctx.operands(Word)) {
      if (Ctx.op(F) == SymOp::Not)
        F = Ctx.operand(F, 0);
      if (Ctx.op(F) != SymOp::Mul || Ctx.numOperands(F) != 2)
        continue;
      bool Masks = true;
      for (auto M : Ctx.operands(F)) {
        if (Ctx.op(M) == SymOp::Add && Ctx.numOperands(M) == 2 &&
            Ctx.isConst(Ctx.operand(M, 0)))
          M = Ctx.operand(M, 1);
        Masks &= Ctx.op(M) == SymOp::And && Ctx.numOperands(M) == 2 &&
                 Ctx.isConst(Ctx.operand(M, 0));
      }
      if (Masks)
        return true;
    }
    return false;
  };
  if (!Hint(Args[Args.size() / 2]) || !Hint(Args.back()))
    return false;
  auto Parity = [&](auto &&Self, SymRef F,
                    unsigned &Steps) -> std::optional<bool> {
    if (!Steps)
      return {};
    --Steps;
    if (Ctx.isConst(F))
      return Ctx.constValue(F)[0];
    if (Ctx.isVar(F))
      return false;
    const auto Op = Ctx.op(F);
    if (Op == SymOp::Not) {
      auto V = Self(Self, Ctx.operand(F, 0), Steps);
      if (V)
        return !*V;
      return {};
    }
    if (Op != SymOp::Mul && Op != SymOp::And && Op != SymOp::Add &&
        Op != SymOp::Xor)
      return {};
    const bool And = Op == SymOp::Mul || Op == SymOp::And;
    bool V = And;
    for (auto T : Ctx.operands(F)) {
      auto Low = Self(Self, T, Steps);
      if (!Low)
        return {};
      if (And && !*Low)
        return false;
      if (!And)
        V ^= *Low;
    }
    return V;
  };
  std::optional<bool> FirstParity;
  bool Mixed = false;
  for (auto F : {Args.front(), Args[Args.size() / 2], Args.back()}) {
    unsigned Steps = limits::kMaxProductProgressionHintNodes;
    auto V = Parity(Parity, F, Steps);
    if (!Charge(limits::kMaxProductProgressionHintNodes - Steps))
      return false;
    if (!V)
      continue;
    Mixed |= FirstParity && *FirstParity != *V;
    FirstParity = V;
  }
  return Mixed;
}

} // namespace neverd::symbolic::detail
