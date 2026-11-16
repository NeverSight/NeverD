//===- SymMBAProductProgressionTests.cpp - Modular product windows --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

using namespace neverd::symbolic;

namespace {

enum WindowVariation : unsigned {
  Perturbed = 1,
  SingleMask = 2,
  ComplementX = 4,
  LinearTail = 8,
  RepeatedIndex = 16
};

struct Window {
  unsigned Bits, Size, Start, XStep, ZStep, YShift, ZShift;
  unsigned Variations = 0, ClearYBit = 0;

  std::optional<unsigned> Hole = std::nullopt;

  bool has(WindowVariation V) const { return Variations & V; }

  unsigned indexAt(unsigned J) const {
    return Start + J + unsigned(has(RepeatedIndex) && J == Size / 3) +
           unsigned(Hole && J >= *Hole);
  }
};

SymRef build(SymContext &Ctx, unsigned Width, const Window &W) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  const auto XBase = W.has(ComplementX) ? Ctx.mkNot(X) : X;
  auto YMask = llvm::APInt::getHighBitsSet(Width, Width - W.YShift);
  if (W.ClearYBit)
    YMask.clearBit(W.ClearYBit);
  const auto ZMask = llvm::APInt::getHighBitsSet(Width, Width - W.ZShift);
  std::vector<unsigned> Order(W.Size);
  std::iota(Order.begin(), Order.end(), 0u);
  std::shuffle(Order.begin(), Order.end(), std::mt19937(399));
  llvm::SmallVector<SymRef, 64> Factors;
  for (unsigned J : Order) {
    unsigned I = W.indexAt(J);
    auto A = Ctx.mkAdd(
        XBase, Ctx.mkConst(Width, W.XStep * I + (W.has(Perturbed) && !J)));
    auto B = Ctx.mkAnd(Ctx.mkAdd(Y, Ctx.mkConst(Width, I)), Ctx.mkConst(YMask));
    auto C = Ctx.mkAnd(Ctx.mkAdd(Z, Ctx.mkConst(Width, W.ZStep * I)),
                       Ctx.mkConst(ZMask));
    Factors.push_back(Ctx.mkXor(A, W.has(SingleMask) ? B : Ctx.mkMul(B, C)));
  }
  auto Scale = Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - W.Bits));
  auto Product = Ctx.mkMul(Scale, Ctx.mkMul(Factors));
  return W.has(LinearTail)
             ? Ctx.mkAdd({X, Y, Z, Ctx.mkVar("w", Width), Product})
             : Product;
}

llvm::APInt evaluate(const Window &W, llvm::ArrayRef<llvm::APInt> Inputs) {
  const auto Width = Inputs.front().getBitWidth();
  auto Product = llvm::APInt(Width, 3).shl(Width - W.Bits);
  for (unsigned J = 0; J < W.Size; ++J) {
    const unsigned I = W.indexAt(J);
    auto X = (W.has(ComplementX) ? ~Inputs[0] : Inputs[0]) + W.XStep * I +
             (W.has(Perturbed) && !J);
    auto Y =
        (Inputs[1] + I) & llvm::APInt::getHighBitsSet(Width, Width - W.YShift);
    auto Z = (Inputs[2] + W.ZStep * I) &
             llvm::APInt::getHighBitsSet(Width, Width - W.ZShift);
    if (W.ClearYBit)
      Y.clearBit(W.ClearYBit);
    Product *= X ^ (W.has(SingleMask) ? Y : Y * Z);
  }
  return W.has(LinearTail)
             ? Product + Inputs[0] + Inputs[1] + Inputs[2] + Inputs[3]
             : Product;
}

TEST(SymMBAProgressions,
     EveryFiveBitInputAgreesForCompleteAndIncompleteWindows) {
  for (Window W :
       {Window{5, 16, 0, 2, 2, 1, 1}, Window{5, 8, 11, 6, 3, 1, 2},
        Window{5, 15, 9, 2, 1, 2, 1}, Window{5, 17, 7, 6, 2, 1, 1, Perturbed},
        Window{5, 16, 7, 6, 1, 1, 0, SingleMask},
        Window{5, 15, 3, 2, 1, 2, 0, SingleMask | ComplementX},
        Window{5, 16, 0, 2, 2, 1, 1, 0, 3},
        Window{5, 24, 13, 6, 3, 1, 1, RepeatedIndex}}) {
    SCOPED_TRACE(W.Size);
    SymContext Ctx;
    auto Input = build(Ctx, 8, W);
    MBAOptions Opt;
    Opt.MaxWork = 65536;
    const auto R = simplifyMBADeep(Ctx, Input, Opt);
    SymEvalPlan Plan(Ctx, R.Expr);
    for (unsigned X = 0; X < 32; ++X)
      for (unsigned Y = 0; Y < 32; ++Y)
        for (unsigned Z = 0; Z < 32; ++Z) {
          unsigned Product = 3u << 3;
          for (unsigned J = 0; J < W.Size; ++J) {
            const unsigned I = W.indexAt(J);
            const unsigned A = (W.has(ComplementX) ? ~X : X) + W.XStep * I +
                               (W.has(Perturbed) && !J);
            unsigned B = (Y + I) & -(1u << W.YShift);
            if (W.ClearYBit)
              B &= ~(1u << W.ClearYBit);
            const unsigned C = (Z + W.ZStep * I) & -(1u << W.ZShift);
            Product *= A ^ (W.has(SingleMask) ? B : B * C);
          }
          ASSERT_EQ(Plan.evalU64({X, Y, Z}), Product & 255);
        }
  }
}

TEST(SymMBAProgressions, ProvesShuffledFullHalfAndIncompleteProducts) {
  using namespace neverd;
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (Window W : {Window{5, 16, 0, 2, 2, 1, 1}, Window{5, 8, 11, 6, 3, 1, 2},
                   Window{5, 15, 9, 2, 1, 2, 1},
                   Window{5, 16, 7, 6, 1, 1, 0, SingleMask}}) {
    SCOPED_TRACE(W.Size);
    SymContext Ctx;
    const auto Input = build(Ctx, 65, W);
    MBAOptions Opt;
    Opt.MaxWork = 65536;
    auto R = simplifyMBADeep(Ctx, Input, Opt);
    ASSERT_NE(R.Expr, Input);
    solver::Z3SolverOptions Options;
    Options.TimeoutMs = 30000;
    solver::Z3Solver Verifier(Ctx, Options);
    ASSERT_TRUE(Verifier.assertDistinct(Input, R.Expr));
    EXPECT_EQ(Verifier.check(), solver::SatResult::Unsat)
        << Verifier.reasonUnknown();
  }
}

TEST(SymMBAProgressions,
     FullWidthProductsKeepHolesOddSelectorsAndPerturbations) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (Window W :
         {Window{8, 64, 0, 2, 2, 1, 1}, Window{8, 63, 5, 2, 2, 1, 2},
          Window{8, 126, 0, 2, 2, 1, 4}, Window{8, 192, 13, 6, 3, 1, 2},
          Window{7, 25, 3, 2, 2, 1, 1}, Window{8, 65, 0, 2, 2, 1, 1, Perturbed},
          Window{7, 30, 11, 6, 3, 1, 2},
          Window{8, 128, 7, 6, 1, 1, 0, SingleMask},
          Window{8, 126, 0, 2, 2, 1, 4, 0, 7},
          Window{8, 95, 0, 2, 2, 1, 4, ComplementX | LinearTail},
          Window{8, 127, 38, 2, 2, 1, 3, LinearTail, 0, 88},
          Window{8, 192, 13, 6, 3, 1, 2, RepeatedIndex}}) {
      SCOPED_TRACE(::testing::Message() << Width << "/" << W.Size);
      SymContext Ctx;
      const auto Input = build(Ctx, Width, W);
      auto Check = [&](const MBAOptions &Opt, bool RequireReduction) {
        const auto R = simplifyMBADeep(Ctx, Input, Opt);
        if (RequireReduction) {
          EXPECT_LT(R.Work, Opt.MaxWork);
          EXPECT_LT(R.SizeAfter, R.SizeBefore / 2);
          EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
        }
        SymEvalPlan Plan(Ctx, R.Expr);
        std::mt19937_64 Random(399);
        for (unsigned Trial = 0; Trial < 64; ++Trial) {
          llvm::SmallVector<llvm::APInt, 4> Values;
          for (unsigned V = 0; V < (W.has(LinearTail) ? 4u : 3u); ++V) {
            llvm::APInt Word(Width, Random());
            for (unsigned Bit = 64; Bit < Width; Bit += 64)
              Word |= llvm::APInt(Width, Random()).shl(Bit);
            if (Trial < 8)
              Word = (Trial >> V) & 1 ? llvm::APInt::getAllOnes(Width)
                                      : llvm::APInt(Width, 0);
            Values.push_back(std::move(Word));
          }
          EXPECT_EQ(Plan.eval(Values), evaluate(W, Values));
        }
      };
      MBAOptions Opt;
      Opt.MaxWork = 65536;
      Check(Opt, W.Size == 64 || W.Size == 126 || W.Size == 128 ||
                     W.Size == 192 || W.has(LinearTail));
      if (Width == 65 && W.Size == 63) {
        Opt.MaxWork = 96;
        Check(Opt, false);
        Opt.MaxWork = 65536;
        Opt.MaxTableBytes = 64;
        Check(Opt, false);
      }
    }
}

} // namespace
