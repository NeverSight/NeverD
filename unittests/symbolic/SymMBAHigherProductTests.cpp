//===- SymMBAHigherProductTests.cpp - Shared higher-degree cofactors ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent word oracles and refusal checks for higher MBA products.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymParse.h"

#include <array>
#include <bit>
#include <random>
#include <string>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct Expansion {
  SymRef Input;
  SymRef Expected;
};

Expansion partitioned(SymContext &C, unsigned Width, unsigned Degree,
                      unsigned Pieces, unsigned Scale = 37,
                      bool Perturbed = false) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
  auto V = C.mkVar("v", Width);
  std::array<SymRef, 8> Words{
      C.mkXor(X, Y), C.mkOr(Y, Z),  C.mkOr(X, Z), C.mkAnd(X, Y),
      C.mkXor(X, Z), C.mkXor(Y, Z), C.mkOr(X, Y), C.mkNot(C.mkAnd(X, Z))};
  std::array<SymRef, 6> Axes{Z, X, Y, W, V, Z};
  llvm::SmallVector<SymRef, 16> Whole;
  for (unsigned I = 0; I != Degree; ++I)
    Whole.push_back(I < Words.size() ? Words[I] : X);
  llvm::SmallVector<SymRef, 64> Terms;
  SymRef Error = C.mkConst(Width, 0);
  for (unsigned Mask = 0; Mask != (1u << Pieces); ++Mask) {
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned I = 0; I != Degree; ++I)
      Factors.push_back(
          I < Pieces
              ? C.mkAnd(Whole[I], Mask & (1u << I) ? Axes[I] : C.mkNot(Axes[I]))
              : Whole[I]);
    SymRef Term = C.mkMul(Factors);
    if (Perturbed && Mask == 0) {
      Error = C.mkMul(C.mkConst(Width, 2 * Scale), Term);
      Term = C.mkMul(C.mkConst(Width, 3), Term);
    }
    Terms.push_back(Term);
  }
  auto K = C.mkConst(Width, Scale);
  auto Residual =
      C.mkAdd(C.mkMul({C.mkConst(Width, 13), X, Y, Z}),
              C.mkAnd(X, C.mkConst(llvm::APInt::getSignMask(Width))));
  return {C.mkAdd({C.mkMul(K, C.mkAdd(Terms)), C.mkXor(X, Y),
                   C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)), Residual}),
          C.mkAdd({C.mkMul(K, C.mkMul(Whole)), X, Y, Residual, Error})};
}

llvm::APInt oracle(llvm::ArrayRef<llvm::APInt> V, unsigned Degree,
                   unsigned Scale) {
  auto X = V[0], Y = V[1], Z = V[2];
  unsigned Width = X.getBitWidth();
  std::array<llvm::APInt, 8> Words{X ^ Y, Y | Z, X | Z, X & Y,
                                   X ^ Z, Y ^ Z, X | Y, ~(X & Z)};
  llvm::APInt Product(Width, Scale, false, true);
  for (unsigned I = 0; I != Degree; ++I)
    Product *= I < Words.size() ? Words[I] : X;
  return Product + X + Y + llvm::APInt(Width, 13, false, true) * X * Y * Z +
         (X & llvm::APInt::getSignMask(Width));
}

TEST(SymMBAHigherProduct, RecoversPartitionsThroughTwelveFactorsAndWideWords) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u})
    for (unsigned Degree : {4u, 5u, 6u, 8u, 12u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Degree);
      SymContext C;
      auto [Input, Expected] = partitioned(C, Width, Degree, 4);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected)) << C.toString(R.Expr);
      std::mt19937_64 Random(0x48494748);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        llvm::SmallVector<llvm::APInt, 5> Values;
        for (unsigned I = 0; I != 5; ++I) {
          llvm::APInt Value(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
          Values.push_back(Value);
        }
        auto Want = oracle(Values, Degree, 37);
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(R.Expr, Values), Want);
      }
    }
}

TEST(SymMBAHigherProduct, SharesProvedQuotientsAcrossSixtyFourProducts) {
  for (unsigned Scale : {1u, 37u, 182u, 0x80000000u}) {
    SymContext C;
    auto [Input, Expected] = partitioned(C, 32, 8, 6, Scale);
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    EXPECT_LT(R.Work, Options.MaxWork);
    EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
  }
}

TEST(SymMBAHigherProduct, RecoversNonlinearQuotientsAndRepeatedFactors) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u})
    for (const char *Text :
         {"(x&y)*(x|y)*(z&w)*(z|w)+(x&~y)*(~x&y)*(z&w)*(z|w)"
          "+(x&y)*(x|y)*(z&~w)*(~z&w)+(x&~y)*(~x&y)*(z&~w)*(~z&w)",
          "182*((x&y)*(x|y)*x*x*x*x+(x&~y)*(~x&y)*x*x*x*x)"}) {
      SymContext C;
      auto Input = parseSymExpr(C, Text, Width);
      ASSERT_TRUE(Input.ok());
      auto Expected = parseSymExpr(
          C, Text[0] == '1' ? "182*x*x*x*x*x*y" : "x*y*z*w", Width);
      ASSERT_TRUE(Expected.ok());
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input.Root, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected.Root));
      for (unsigned I = 0; I != 256; ++I) {
        std::array<llvm::APInt, 4> Values{
            llvm::APInt(Width, I, false, true),
            llvm::APInt(Width, I * 37, false, true),
            llvm::APInt(Width, I * 11, false, true),
            llvm::APInt(Width, I * 79, false, true)};
        EXPECT_EQ(C.eval(Input.Root, Values), C.eval(Expected.Root, Values));
        EXPECT_EQ(C.eval(R.Expr, Values), C.eval(Expected.Root, Values));
      }
    }
}

TEST(SymMBAHigherProduct,
     KeepsChangedCoefficientsAndPartialBudgetResultsExact) {
  for (bool Perturbed : {false, true}) {
    SymContext C;
    auto [Input, Expected] = partitioned(C, 3, 4, 4, 37, Perturbed);
    for (size_t Work : {0u, 1u, 128u, 1024u, 4096u, 65536u}) {
      detail::WorkBudget Budget(Work);
      detail::SolveReport Report;
      MBAOptions Options;
      Options.VerifySamples = 0;
      SymRef R =
          detail::solveProductCofactors(C, Input, Options, Budget, Report);
      EXPECT_LE(Budget.used(), Work);
      EXPECT_LE(C.readabilityCost(R), C.readabilityCost(Input));
      if (R != Input)
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Before(C, Input), After(C, R), Want(C, Expected);
      for (unsigned Assignment = 0; Assignment != 4096; ++Assignment) {
        std::array<uint64_t, 5> Values{Assignment & 7u, (Assignment >> 3) & 7u,
                                       (Assignment >> 6) & 7u, Assignment >> 9,
                                       0};
        EXPECT_EQ(Before.evalU64(Values), Want.evalU64(Values));
        EXPECT_EQ(After.evalU64(Values), Want.evalU64(Values));
      }
    }
  }
}

TEST(SymMBAHigherProduct, RefusesBeforeAllocationAndRequiresDistributionProof) {
  SymContext C;
  auto [Input, Expected] = partitioned(C, 32, 4, 1);
  for (size_t Bytes : {0u, 128u}) {
    MBAOptions Options;
    Options.MaxTableBytes = Bytes;
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    size_t Nodes = C.numNodes();
    EXPECT_EQ(detail::solveProductCofactors(C, Input, Options, Budget, Report),
              Input);
    EXPECT_EQ(C.numNodes(), Nodes);
  }
  detail::WorkBudget Full(65536);
  detail::SolveReport Report;
  auto R = detail::solveProductCofactors(C, Input, {}, Full, Report);
  ASSERT_NE(R, Input);
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
    detail::WorkBudget Budget(Work);
    detail::SolveReport Limited;
    auto Value = detail::solveProductCofactors(C, Input, {}, Budget, Limited);
    if (Work <= 1)
      EXPECT_EQ(Value, Input);
    else if (Work == Full.used())
      EXPECT_EQ(Value, R);
    else {
      // A complete distribution may now precede the final bookkeeping unit.
      // A partial budget may retain that proved answer, never an unproved one.
      EXPECT_TRUE(Value == Input || Value == R);
      if (Value != Input)
        EXPECT_EQ(Limited.Evidence, MBAEvidence::Derivation);
    }
    EXPECT_LE(Budget.used(), Work);
  }
}

TEST(SymMBAHigherProduct, IndependentSmallWordZ3Proofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {2u, 3u})
    for (unsigned Degree : {4u, 6u}) {
      SymContext C;
      auto [Input, Expected] = partitioned(C, Width, Degree, 3);
      MBAOptions Options;
      Options.MaxWork = 65536;
      auto R = simplifyMBADeep(C, Input, Options);
      ASSERT_NE(R.Expr, Input);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(C, Input, R.Expr), SynthVerification::Equivalent);
    }
}

Expansion disturbedProduct(SymContext &C, unsigned Width, unsigned Degree,
                           const llvm::APInt &Scale, const llvm::APInt &Delta,
                           bool Second = false) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
  std::array<SymRef, 4> P{C.mkAnd(X, Y), C.mkOr(X, Y), C.mkAnd(X, C.mkNot(Y)),
                          C.mkAnd(C.mkNot(X), Y)};
  std::array<SymRef, 4> Q{C.mkAnd(Z, W), C.mkOr(Z, W), C.mkAnd(Z, C.mkNot(W)),
                          C.mkAnd(C.mkNot(Z), W)};
  std::array<SymRef, 4> Tail{C.mkXor(X, Z), C.mkXor(Y, Z), C.mkOr(X, Y),
                             C.mkNot(C.mkAnd(X, Z))};
  llvm::SmallVector<SymRef, 4> Terms, Errors;
  llvm::SmallVector<SymRef, 16> Whole{C.mkConst(Scale), X, Y, Z, W};
  for (unsigned I = 4; I < Degree; ++I)
    Whole.push_back(Tail[(I - 4) % Tail.size()]);
  for (unsigned A = 0; A != 2; ++A)
    for (unsigned B = 0; B != 2; ++B) {
      llvm::SmallVector<SymRef, 16> Factors{P[2 * A], P[2 * A + 1], Q[2 * B],
                                            Q[2 * B + 1]};
      for (unsigned I = 4; I < Degree; ++I)
        Factors.push_back(Tail[(I - 4) % Tail.size()]);
      auto Difference = A == 0 && B == 0 ? Delta
                                         : (Second && A == 1 && B == 1
                                                ? -Delta * llvm::APInt(Width, 3)
                                                : llvm::APInt(Width, 0));
      auto Product = C.mkMul(Factors);
      Terms.push_back(C.mkMul(C.mkConst(Scale + Difference), Product));
      if (!Difference.isZero())
        Errors.push_back(C.mkMul(C.mkConst(Difference), Product));
    }
  Errors.push_back(C.mkMul(Whole));
  Errors.push_back(X);
  Errors.push_back(Y);
  Terms.push_back(C.mkXor(X, Y));
  Terms.push_back(C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)));
  return {C.mkAdd(Terms), C.mkAdd(Errors)};
}

llvm::APInt disturbedOracle(llvm::ArrayRef<llvm::APInt> Values, unsigned Degree,
                            const llvm::APInt &Scale, const llvm::APInt &Delta,
                            bool Second = false) {
  auto X = Values[0], Y = Values[1], Z = Values[2], W = Values[3];
  auto Product =
      Scale * X * Y * Z * W + Delta * (X & Y) * (X | Y) * (Z & W) * (Z | W);
  if (Second)
    Product -= llvm::APInt(X.getBitWidth(), 3) * Delta * (X & ~Y) * (~X & Y) *
               (Z & ~W) * (~Z & W);
  std::array<llvm::APInt, 4> Tail{X ^ Z, Y ^ Z, X | Y, ~(X & Z)};
  for (unsigned I = 4; I < Degree; ++I)
    Product *= Tail[(I - 4) % Tail.size()];
  return Product + X + Y;
}

TEST(SymMBAHigherProduct, SparseCoefficientDifferencesFinishBelowOriginalCap) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u})
    for (unsigned Degree : {4u, 6u, 8u, 12u, 16u, 24u, 32u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Degree);
      SymContext C;
      llvm::APInt Scale(Width, 182, false, true),
          Delta(Width, 364, false, true);
      auto [Input, Expected] = disturbedProduct(C, Width, Degree, Scale, Delta);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
      std::mt19937_64 Random(0x434f4546);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::array<llvm::APInt, 4> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        }
        auto Want = disturbedOracle(Values, Degree, Scale, Delta);
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(R.Expr, Values), Want);
      }
    }
}

TEST(SymMBAHigherProduct, CoefficientDifferencesKeepModularZerosAndHighBits) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u})
    for (auto Scale : {llvm::APInt(Width, 37, false, true),
                       -llvm::APInt(Width, 11, false, true),
                       llvm::APInt::getOneBitSet(Width, Width - 2)})
      for (auto Delta : {-Scale, llvm::APInt::getSignMask(Width)}) {
        SymContext C;
        auto [Input, Expected] =
            disturbedProduct(C, Width, 8, Scale, Delta, true);
        MBAOptions Options;
        Options.MaxWork = 65536;
        Options.VerifySamples = 0;
        auto R = simplifyMBADeep(C, Input, Options);
        EXPECT_LE(R.SizeAfter, R.SizeBefore);
        std::mt19937_64 Random(0x44454c54);
        for (unsigned Sample = 0; Sample != 128; ++Sample) {
          std::array<llvm::APInt, 4> Values;
          for (auto &Value : Values) {
            Value = llvm::APInt(Width, 0);
            for (unsigned Bit = 0; Bit < Width; Bit += 64)
              Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
          }
          auto Want = disturbedOracle(Values, 8, Scale, Delta, true);
          EXPECT_EQ(C.eval(Input, Values), Want);
          EXPECT_EQ(C.eval(R.Expr, Values), Want);
        }
      }
}

TEST(SymMBAHigherProduct, SparseDifferencePartialProofsRemainExact) {
  for (bool Second : {false, true}) {
    SymContext C;
    llvm::APInt Scale(3, 6), Delta(3, 4);
    auto [Input, Expected] = disturbedProduct(C, 3, 4, Scale, Delta, Second);
    for (size_t Work : {0u, 1u, 128u, 1024u, 4096u, 8192u, 65536u}) {
      detail::WorkBudget Budget(Work);
      detail::SolveReport Report;
      auto R = detail::solveProductCofactors(C, Input, {}, Budget, Report);
      EXPECT_LE(Budget.used(), Work);
      EXPECT_LE(C.readabilityCost(R), C.readabilityCost(Input));
      if (R != Input)
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Before(C, Input), After(C, R);
      for (unsigned Bits = 0; Bits != 4096; ++Bits) {
        std::array<uint64_t, 4> Values{Bits & 7u, (Bits >> 3) & 7u,
                                       (Bits >> 6) & 7u, (Bits >> 9) & 7u};
        std::array<llvm::APInt, 4> Words;
        for (unsigned I = 0; I != 4; ++I)
          Words[I] = llvm::APInt(3, Values[I]);
        auto Want =
            disturbedOracle(Words, 4, Scale, Delta, Second).getZExtValue();
        EXPECT_EQ(Before.evalU64(Values), Want);
        EXPECT_EQ(After.evalU64(Values), Want);
      }
    }
  }
}

TEST(SymMBAHigherProduct, SparseDifferenceHasIndependentSMTProof) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {2u, 3u})
    for (unsigned Degree : {4u, 8u}) {
      SymContext C;
      auto [Input, Expected] = disturbedProduct(
          C, Width, Degree, llvm::APInt(Width, 1), llvm::APInt(Width, 2));
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      ASSERT_NE(R.Expr, Input);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(C, Input, R.Expr), SynthVerification::Equivalent);
    }
}

TEST(SymMBAHigherProduct, HighCoefficientObservesOnlyProductParity) {
  for (unsigned Width : {2u, 3u, 8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Word = C.mkXor(C.mkAdd(X, Z), C.mkMul(Y, Z));
    auto Scale = llvm::APInt::getSignMask(Width);
    auto Input = C.mkMul({C.mkConst(Scale), X, Y, Z, Word, C.mkOr(X, Y)});
    MBAOptions Options;
    Options.MaxWork = 16384;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    EXPECT_LT(R.SizeAfter, R.SizeBefore);
    EXPECT_LT(R.Work, Options.MaxWork);
    std::mt19937_64 Random(0x50415249);
    for (unsigned Sample = 0; Sample != 256; ++Sample) {
      std::array<llvm::APInt, 3> Values;
      for (auto &Value : Values) {
        Value = llvm::APInt(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
      }
      auto A = Values[0], B = Values[1], D = Values[2];
      auto Want = Scale * A * B * D * ((A + D) ^ (B * D)) * (A | B);
      EXPECT_EQ(C.eval(Input, Values), Want);
      EXPECT_EQ(C.eval(R.Expr, Values), Want);
    }
  }
}

TEST(SymMBAHigherProduct, AdjacentCoefficientStillObservesCarries) {
  for (unsigned Width : {2u, 3u, 8u, 16u, 32u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Scale = llvm::APInt::getOneBitSet(Width, Width - 2);
    auto Input = C.mkMul({C.mkConst(Scale), X, Y});
    MBAOptions Options;
    Options.MaxWork = 16384;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    for (unsigned A = 0; A != 8; ++A)
      for (unsigned B = 0; B != 8; ++B) {
        std::array<llvm::APInt, 2> Values{llvm::APInt(Width, A, false, true),
                                          llvm::APInt(Width, B, false, true)};
        EXPECT_EQ(C.eval(R.Expr, Values), Scale * Values[0] * Values[1]);
      }
    std::array<llvm::APInt, 2> Counterexample{llvm::APInt(Width, 3),
                                              llvm::APInt(Width, 3)};
    EXPECT_NE(C.eval(R.Expr, Counterexample),
              Scale * (Counterexample[0] & Counterexample[1]));
  }
}

TEST(SymMBAHigherProduct, ProductParityHasIndependentFullWidthSMTProof) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Word = C.mkXor(C.mkAdd(X, Z), C.mkMul(Y, Z));
    auto Input = C.mkMul({C.mkConst(llvm::APInt::getSignMask(Width)), X, Y, Z,
                          Word, C.mkOr(X, Y)});
    MBAOptions Options;
    Options.MaxWork = 16384;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    ASSERT_NE(R.Expr, Input);
    solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verifier(C, Input, R.Expr), SynthVerification::Equivalent);
  }
}

TEST(SymMBAHigherProduct, RefusesFlatteningAnOversizedOpaqueQuotient) {
  SymContext C;
  llvm::SmallVector<SymRef, 512> Wide;
  for (unsigned I = 0; I != 512; ++I)
    Wide.push_back(C.mkVar("v" + std::to_string(I), 32));
  auto A = C.mkVar("a", 32), B = C.mkVar("b", 32);
  auto D = C.mkVar("d", 32), X = C.mkVar("x", 32);
  auto Input =
      C.mkAdd(C.mkMul({A, B, D, C.mkAdd(Wide)}), C.mkMul({A, B, D, X}));
  size_t Nodes = C.numNodes();
  detail::WorkBudget Budget(65536);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveProductCofactors(C, Input, {}, Budget, Report), Input);
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_LE(C.numNodes(), Nodes + 4);
  EXPECT_LT(Budget.used(), 512u);
}

TEST(SymMBAHigherProduct, CoefficientBoundsAdmitIndependentModularExpansions) {
  std::mt19937_64 Random(0x424f554e44);
  for (unsigned Width : {2u, 8u, 32u, 64u, 257u})
    for (unsigned Degree : {4u, 5u, 6u, 8u})
      for (unsigned Sample = 0; Sample != 16; ++Sample) {
        llvm::APInt Scale = Sample % 3 == 0
                                ? llvm::APInt::getOneBitSet(Width, Width - 2)
                                : llvm::APInt(Width, Random() | 1, false, true);
        if (Sample % 3 == 2)
          Scale <<= 1;
        std::vector<unsigned> Other(Degree);
        for (auto &Value : Other)
          Value = 1 + Random() % 3;
        detail::HigherCoefficients H;
        // Enumerate choices independently of coefficient bounds or inverses.
        for (unsigned Mask = 0; Mask != (1u << Degree); ++Mask) {
          detail::Monomial Key;
          for (unsigned I = 0; I != Degree; ++I)
            Key.push_back(Mask & (1u << I) ? Other[I] : 0);
          llvm::sort(Key);
          auto [It, New] = H.try_emplace(Key, Scale);
          if (!New)
            It->second += Scale;
        }
        std::erase_if(H,
                      [](const auto &Entry) { return Entry.second.isZero(); });
        detail::WorkBudget Full(65536);
        ASSERT_TRUE(
            detail::productCoefficientsFitScale(H, Degree, Scale, Full));
        detail::WorkBudget SupportBudget(65536);
        auto Support =
            detail::recoverProductSupport(H, Degree, 2, SupportBudget, 65536);
        ASSERT_TRUE(Support);
        ASSERT_EQ(Support->size(), Degree);
        std::vector<unsigned> Actual, Expected;
        for (const auto &Factor : *Support) {
          unsigned Bits = 0;
          for (unsigned I = 0; I != 4; ++I)
            Bits |= unsigned(Factor.at(I)) << I;
          Actual.push_back(Bits);
        }
        for (auto Value : Other)
          Expected.push_back(1u | (1u << Value));
        llvm::sort(Actual);
        llvm::sort(Expected);
        EXPECT_EQ(Actual, Expected);
        detail::WorkBudget Short(Full.used() - 1);
        EXPECT_FALSE(
            detail::productCoefficientsFitScale(H, Degree, Scale, Short));
      }
  for (unsigned Scale : {1u, 2u}) {
    detail::HigherCoefficients Bad;
    Bad.try_emplace(detail::Monomial{0, 0, 0, 1}, llvm::APInt(32, 5));
    detail::WorkBudget Budget(65536);
    EXPECT_FALSE(detail::productCoefficientsFitScale(
        Bad, 4, llvm::APInt(32, Scale), Budget));
  }
}

TEST(SymMBAHigherProduct, CancelsExactResidualsBeforePolynomialProofExpansion) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned I = 0; I != 16; ++I)
      Factors.push_back(I % 2 ? C.mkOr(Y, Z) : C.mkXor(X, Y));
    auto Residual = C.mkMul(Factors);
    auto Scale = C.mkConst(Width, 37);
    auto Input =
        C.mkMul(Scale, C.mkAdd({Residual, C.mkXor(X, Y),
                                C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y))}));
    auto Expected =
        C.mkAdd(C.mkMul(Scale, Residual), C.mkMul(Scale, C.mkAdd(X, Y)));
    detail::WorkBudget Full(4096);
    ASSERT_TRUE(detail::provePolynomialIdentity(C, Input, Expected, 3, Full));
    detail::WorkBudget Wrong(4096);
    EXPECT_FALSE(detail::provePolynomialIdentity(
        C, Input, C.mkAdd(Expected, C.mkOne(Width)), 3, Wrong));
    detail::WorkBudget NoStorage(4096);
    EXPECT_FALSE(
        detail::provePolynomialIdentity(C, Input, Expected, 3, NoStorage, 0));
  }
}

TEST(SymMBAHigherProduct, ExtremalScaleRejectionKeepsGeneralExpansionExact) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u})
    for (unsigned Degree : {3u, 4u}) {
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      llvm::SmallVector<uint32_t, 2> Ids;
      C.collectVars(C.mkAdd(X, Y), Ids);
      llvm::SmallVector<detail::PolyTerm, 3> Terms{
          {llvm::APInt(Width, 1), {X, X, X, X}},
          {llvm::APInt(Width, 3), {Y, Y, Y, Y}}};
      for (auto &Term : Terms)
        Term.Factors.resize(Degree);
      detail::WorkBudget General(4096), Single(4096);
      auto Form = detail::expandOverMinterms(C, Terms, Ids, Width, General);
      ASSERT_TRUE(Form);
      EXPECT_FALSE(detail::expandOverMinterms(C, Terms, Ids, Width, Single,
                                              /*Optional=*/true,
                                              /*SingleProduct=*/true));
      EXPECT_LT(Single.used(), 1024u);
      // Cancelling a leading coefficient makes the scale check inconclusive.
      // It must still expand the surviving polynomial with its full scale.
      Terms.push_back({llvm::APInt::getAllOnes(Width), {X, X, X, X}});
      Terms.back().Factors.resize(Degree);
      detail::WorkBudget Cancelled(4096), Unrestricted(4096);
      auto Actual = detail::expandOverMinterms(C, Terms, Ids, Width, Cancelled,
                                               true, true);
      auto Expected =
          detail::expandOverMinterms(C, Terms, Ids, Width, Unrestricted);
      ASSERT_TRUE(Actual);
      ASSERT_TRUE(Expected);
      EXPECT_EQ(Actual->Higher, Expected->Higher);
    }
}

TEST(SymMBAHigherProduct, AvoidsExpandingTheTwoSlowMixedDegreeTails) {
  for (unsigned Width : {3u, 8u, 16u, 32u, 64u, 257u})
    for (bool FifthDegree : {false, true}) {
      SymContext C;
      const char *Tail =
          FifthDegree
              ? "182*x*y*z*w*(x^z)+364*(x&y)*(z&w)*(z|w)*(x^z)*(x|y)"
              : "19*x*x*x*x*x*y*z+7*(x^y)*(x&y)*(y|z)*(x|z)*(x^z)*(y^z)*~(x&z)";
      auto Parsed = parseSymExpr(C, Tail, Width);
      ASSERT_TRUE(Parsed.ok());
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto Input = C.mkAdd({Parsed.Root, C.mkXor(X, Y),
                            C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y))});
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_LT(R.Work, 20000u) << Width << " " << FifthDegree;
      EXPECT_LE(R.SizeAfter, C.readabilityCost(C.mkAdd({Parsed.Root, X, Y})));
      std::mt19937_64 Random(0x45585452);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::array<llvm::APInt, 4> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        }
        auto A = Values[0], B = Values[1], Z = Values[2], W = Values[3];
        auto Want = A + B;
        if (FifthDegree)
          Want +=
              llvm::APInt(Width, 182, false, true) * A * B * Z * W * (A ^ Z) +
              llvm::APInt(Width, 364, false, true) * (A & B) * (Z & W) *
                  (Z | W) * (A ^ Z) * (A | B);
        else
          Want +=
              llvm::APInt(Width, 19, false, true) * A * A * A * A * A * B * Z +
              llvm::APInt(Width, 7, false, true) * (A ^ B) * (A & B) * (B | Z) *
                  (A | Z) * (A ^ Z) * (B ^ Z) * ~(A & Z);
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(R.Expr, Values), Want);
      }
    }
}

Expansion holedPartitions(SymContext &C, unsigned Width, unsigned Degree,
                          unsigned Pieces, bool Opaque = false,
                          bool Disturbed = false) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
  auto V = C.mkVar("v", Width);
  std::array<SymRef, 8> Words{
      C.mkXor(X, Y), C.mkOr(Y, Z),  C.mkOr(X, Z), C.mkAnd(X, Y),
      C.mkXor(X, Z), C.mkXor(Y, Z), C.mkOr(X, Y), C.mkNot(C.mkAnd(X, Z))};
  if (Opaque) {
    Words[0] = C.mkXor(C.mkAdd(X, Y), Z);
    Words[1] = C.mkOr(C.mkMul(Y, Z), X);
  }
  std::array<SymRef, 6> Axes{Z, X, Y, W, V, Z};
  llvm::SmallVector<SymRef, 16> Whole, Terms, Residuals;
  for (unsigned I = 0; I != Degree; ++I)
    Whole.push_back(Words[I % Words.size()]);
  auto Scale = C.mkConst(Width, 37);
  Residuals.push_back(C.mkMul(Scale, C.mkMul(Whole)));
  for (unsigned Mask = 0; Mask != (1u << Pieces); ++Mask) {
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned I = 0; I != Degree; ++I)
      Factors.push_back(
          I < Pieces
              ? C.mkAnd(Whole[I], Mask & (1u << I) ? Axes[I] : C.mkNot(Axes[I]))
              : Whole[I]);
    SymRef Product = C.mkMul(Factors);
    if (Mask == 0 || Mask + 1 == (1u << Pieces)) {
      Residuals.push_back(C.mkNeg(C.mkMul(Scale, Product)));
      continue;
    }
    auto Coefficient = llvm::APInt(Width, 37, false, true);
    if (Disturbed && Mask == 1) {
      auto Delta = llvm::APInt::getSignMask(Width);
      Coefficient += Delta;
      Residuals.push_back(C.mkMul(C.mkConst(Delta), Product));
    }
    Terms.push_back(C.mkMul(C.mkConst(Coefficient), Product));
  }
  Terms.push_back(C.mkXor(X, Y));
  Terms.push_back(C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)));
  // The cofactor stage preserves the linear tail; the later deep walk owns
  // its independent reduction to x+y.
  Residuals.push_back(C.mkXor(X, Y));
  Residuals.push_back(C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)));
  return {C.mkAdd(Terms), C.mkAdd(Residuals)};
}

llvm::APInt holedOracle(llvm::ArrayRef<llvm::APInt> Values, unsigned Degree,
                        unsigned Pieces, bool Opaque, bool Disturbed) {
  const auto &X = Values[0], &Y = Values[1], &Z = Values[2];
  const unsigned Width = X.getBitWidth();
  std::array<llvm::APInt, 8> Words{X ^ Y, Y | Z, X | Z, X & Y,
                                   X ^ Z, Y ^ Z, X | Y, ~(X & Z)};
  if (Opaque) {
    Words[0] = (X + Y) ^ Z;
    Words[1] = (Y * Z) | X;
  }
  std::array<llvm::APInt, 6> Axes{Z, X, Y, Values[3], Values[4], Z};
  llvm::APInt Whole(Width, 37, false, true),
      MissingLow = Whole, MissingHigh = Whole,
      Difference = llvm::APInt::getSignMask(Width);
  for (unsigned I = 0; I != Degree; ++I) {
    auto Word = Words[I % Words.size()];
    Whole *= Word;
    MissingLow *= I < Pieces ? Word & ~Axes[I] : Word;
    MissingHigh *= I < Pieces ? Word & Axes[I] : Word;
    Difference *= I < Pieces ? Word & (I == 0 ? Axes[I] : ~Axes[I]) : Word;
  }
  return Whole - MissingLow - MissingHigh + X + Y +
         (Disturbed ? Difference : llvm::APInt(Width, 0));
}

TEST(SymMBAHigherProduct, CompletesMissingCornersThroughSixtyFourFactors) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u})
    for (unsigned Degree : {4u, 8u, 12u, 16u, 24u, 32u, 48u, 64u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Degree);
      SymContext C;
      auto [Input, Expected] = holedPartitions(C, Width, Degree, 3);
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget Budget(65536);
      detail::SolveReport Report;
      SymRef Output =
          detail::solveProductCofactors(C, Input, Options, Budget, Report);
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(Budget.used(), 65536u);
      EXPECT_LE(C.readabilityCost(Output), C.readabilityCost(Expected));
      std::mt19937_64 Random(0x484f4c45);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::array<llvm::APInt, 5> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        }
        auto Want = holedOracle(Values, Degree, 3, false, false);
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(Output, Values), Want);
      }
    }
}

TEST(SymMBAHigherProduct, CompletionKeepsOpaqueWordsAndCoefficientDifferences) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u})
    for (unsigned Pieces : {3u, 4u, 5u, 6u}) {
      SymContext C;
      auto [Input, Expected] =
          holedPartitions(C, Width, 12, Pieces, true, true);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_LE(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, R.SizeBefore);
      std::mt19937_64 Random(0x434f4d50);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::array<llvm::APInt, 5> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        }
        auto Want = holedOracle(Values, 12, Pieces, true, true);
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(R.Expr, Values), Want);
      }
    }
}

TEST(SymMBAHigherProduct, CompletionPartialBudgetsRemainExact) {
  SymContext C;
  auto [Input, Expected] = holedPartitions(C, 3, 4, 3, false, true);
  for (size_t Work : {0u, 1u, 128u, 1024u, 4096u, 8192u, 65536u}) {
    detail::WorkBudget Budget(Work);
    detail::SolveReport Report;
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Output =
        detail::solveProductCofactors(C, Input, Options, Budget, Report);
    EXPECT_LE(Budget.used(), Work);
    EXPECT_LE(C.readabilityCost(Output), C.readabilityCost(Input));
    SymEvalPlan Before(C, Input), After(C, Output);
    for (unsigned Bits = 0; Bits != 4096; ++Bits) {
      std::array<uint64_t, 5> Values{Bits & 7u, (Bits >> 3) & 7u,
                                     (Bits >> 6) & 7u, Bits >> 9, 0};
      std::array<llvm::APInt, 5> Words;
      for (unsigned I = 0; I != Words.size(); ++I)
        Words[I] = llvm::APInt(3, Values[I]);
      auto Want = holedOracle(Words, 4, 3, false, true).getZExtValue();
      EXPECT_EQ(Before.evalU64(Values), Want);
      EXPECT_EQ(After.evalU64(Values), Want);
    }
  }
}

TEST(SymMBAHigherProduct, CompletionWithoutALinearTailPreservesTheWordWidth) {
  for (unsigned Width : {3u, 32u, 257u}) {
    SymContext C;
    auto [Input, Expected] = holedPartitions(C, Width, 8, 3);
    auto X = C.varRef(0), Y = C.varRef(1);
    auto Linear =
        C.mkAdd(C.mkXor(X, Y), C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)));
    Input = C.mkSub(Input, Linear);
    Expected = C.mkSub(Expected, Linear);
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    auto Output = detail::solveProductCofactors(C, Input, {}, Budget, Report);
    EXPECT_EQ(C.width(Output), Width);
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(C.readabilityCost(Output), C.readabilityCost(Expected));
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 5> Values{llvm::APInt(Width, I, false, true),
                                        llvm::APInt(Width, I * 37, false, true),
                                        llvm::APInt(Width, I * 11, false, true),
                                        llvm::APInt(Width, I * 79, false, true),
                                        llvm::APInt(Width, 0)};
      auto Want =
          holedOracle(Values, 8, 3, false, false) - Values[0] - Values[1];
      EXPECT_EQ(C.eval(Input, Values), Want);
      EXPECT_EQ(C.eval(Output, Values), Want);
    }
  }
}

TEST(SymMBAHigherProduct, CompletionHasIndependentSmallWordSMTProof) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {2u, 3u}) {
    SymContext C;
    auto [Input, Expected] = holedPartitions(C, Width, 4, 3, false, true);
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    auto Output = detail::solveProductCofactors(C, Input, {}, Budget, Report);
    ASSERT_NE(Output, Input);
    solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verifier(C, Input, Output), SynthVerification::Equivalent);
  }
}

TEST(SymMBAHigherProduct, GeneratedParityQuotientsFinishBelowOriginalCap) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
    auto Scale = C.mkConst(llvm::APInt::getSignMask(Width));
    auto Input =
        C.mkAdd({C.mkXor(X, Y), C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)),
                 C.mkMul({Scale, C.mkAnd(X, Y), C.mkOr(X, Y), C.mkAnd(Z, W),
                          C.mkOr(Z, W)}),
                 C.mkMul({X, Y, Z, W})});
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_LT(R.Work, Options.MaxWork);
    auto Expected = C.mkAdd(
        {X, Y, C.mkMul({X, Y, Z, W}), C.mkMul(Scale, C.mkAnd({X, Y, Z, W}))});
    EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
  }
}

Expansion nestedMissingCorner(SymContext &C, unsigned Width, unsigned Degree) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto Z = C.mkVar("z", Width);
  auto A = C.mkXor(X, Y), B = C.mkOr(Y, Z), D = C.mkOr(X, Z);
  auto First = C.mkAdd(C.mkMul(C.mkAnd(B, X), C.mkAnd(A, C.mkNot(Z))),
                       C.mkMul(C.mkAnd(B, C.mkNot(X)), A));
  auto Partial = C.mkAdd(C.mkMul(First, C.mkAnd(D, Y)),
                         C.mkMul({B, A, C.mkAnd(D, C.mkNot(Y))}));
  llvm::SmallVector<SymRef, 8> Tail{
      C.mkConst(llvm::APInt(Width, 37, false, true)), C.mkAnd(X, Y)};
  for (unsigned I = 4; I < Degree; ++I)
    Tail.push_back(I % 2 ? C.mkXor(Y, Z) : C.mkXor(X, Z));
  auto Common = C.mkMul(Tail);
  auto Linear =
      C.mkAdd({X, Y, C.mkAnd(X, C.mkConst(llvm::APInt::getSignMask(Width)))});
  // Both retained products may carry the coefficient while their word tail
  // is shared. The contract is recovery of the partitions, not scalar layout.
  auto Scale = Tail.front();
  auto Complete =
      C.mkSub(C.mkMul({Scale, A, B, D}),
              C.mkMul({Scale, C.mkAnd(A, Z), C.mkAnd(B, X), C.mkAnd(D, Y)}));
  Tail.erase(Tail.begin());
  return {C.mkAdd(Linear, C.mkMul(Common, Partial)),
          C.mkAdd(Linear, C.mkMul(C.mkMul(Tail), Complete))};
}

TEST(SymMBAHigherProduct, ReopensNestedProductPartitionsForSourceEmitters) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 257u})
    for (unsigned Degree : {4u, 6u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Degree);
      SymContext C;
      auto [Input, Expected] = nestedMissingCorner(C, Width, Degree);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
      std::mt19937_64 Random(0x4e455354);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        }
        auto X = Values[0], Y = Values[1], Z = Values[2];
        auto A = X ^ Y, B = Y | Z, D = X | Z;
        auto Want = llvm::APInt(Width, 37, false, true) * (X & Y) *
                    (A * B * D - (A & Z) * (B & X) * (D & Y));
        for (unsigned I = 4; I < Degree; ++I)
          Want *= I % 2 ? (Y ^ Z) : (X ^ Z);
        Want += X + Y + (X & llvm::APInt::getSignMask(Width));
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(R.Expr, Values), Want);
      }
    }
}

TEST(SymMBAHigherProduct,
     NestedPartitionsKeepPartialBudgetsAndIndependentProof) {
  SymContext C;
  auto [Input, Expected] = nestedMissingCorner(C, 3, 4);
  for (size_t Bytes : {0u, 128u}) {
    MBAOptions Options;
    Options.MaxTableBytes = Bytes;
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    size_t Nodes = C.numNodes();
    EXPECT_EQ(detail::solveProductCofactors(C, Input, Options, Budget, Report),
              Input);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_TRUE(Report.BudgetExhausted);
  }
  for (size_t Work : {0u, 1u, 128u, 1024u, 8192u, 65536u}) {
    MBAOptions Options;
    Options.MaxWork = Work;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_LE(R.Work, Work);
    EXPECT_LE(R.SizeAfter, R.SizeBefore);
    SymEvalPlan Before(C, Input), After(C, R.Expr), Want(C, Expected);
    for (unsigned Bits = 0; Bits != 512; ++Bits) {
      std::array<uint64_t, 3> Values{Bits & 7u, (Bits >> 3) & 7u,
                                     (Bits >> 6) & 7u};
      EXPECT_EQ(Before.evalU64(Values), Want.evalU64(Values));
      EXPECT_EQ(After.evalU64(Values), Want.evalU64(Values));
    }
  }
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  MBAOptions Options;
  Options.MaxWork = 65536;
  auto R = simplifyMBADeep(C, Input, Options);
  ASSERT_NE(R.Expr, Input);
  solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
  EXPECT_EQ(Verifier(C, Input, R.Expr), SynthVerification::Equivalent);
}

TEST(SymMBAHigherProduct, DeclinesExponentialFrontiersBeforeTheDeepWalk) {
  for (unsigned Width : {8u, 32u, 64u})
    for (unsigned Power : {7u, 16u}) {
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto Z = C.mkVar("z", Width);
      auto Partition = C.mkAdd(C.mkAnd(X, Y), C.mkAnd(X, C.mkNot(Y)));
      llvm::SmallVector<SymRef, 32> Factors{Z}, Expected{Z};
      for (unsigned I = 0; I != Power; ++I) {
        Factors.push_back(Partition);
        Expected.push_back(X);
      }
      auto Input = C.mkMul(Factors);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_LT(R.Work, 16384u);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(C.mkMul(Expected)));
      std::mt19937_64 Random(0x504f5752);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values)
          Value = llvm::APInt(Width, Random(), false, true);
        auto Want = Values[2];
        for (unsigned I = 0; I != Power; ++I)
          Want *= Values[0];
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(R.Expr, Values), Want);
      }
    }
}

} // namespace

namespace {

TEST(SymMBAHigherProduct, CorrelatedEvenFactorsProveModularZero) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u})
    for (bool Arithmetic : {false, true}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Arithmetic);
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto A = Arithmetic ? C.mkAdd(X, Y) : C.mkXor(X, Y);
      auto B = Arithmetic ? C.mkNot(C.mkXor(X, Y)) : C.mkAnd(X, Y);
      auto Scale = llvm::APInt::getOneBitSet(Width, Width - 4);
      auto Input = C.mkMul({C.mkConst(Scale), A, A, A, A, B, B, B, B});
      size_t Storage = MBAOptions{}.MaxTableBytes;
      detail::WorkBudget Full(65536);
      auto Output = detail::foldProductValuations(C, Input, Full, Storage);
      ASSERT_TRUE(C.isConstZero(Output));
      EXPECT_LT(Full.used(), 256u);
      for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
        detail::WorkBudget Budget(Work);
        size_t Bytes = MBAOptions{}.MaxTableBytes;
        auto Actual = detail::foldProductValuations(C, Input, Budget, Bytes);
        EXPECT_EQ(Actual, Work == Full.used() ? Output : Input);
        EXPECT_LE(Budget.used(), Work);
      }
      size_t NoStorage = 0;
      detail::WorkBudget Refused(65536);
      EXPECT_EQ(detail::foldProductValuations(C, Input, Refused, NoStorage),
                Input);
      std::mt19937_64 Random(0x56414c55);
      for (unsigned I = 0; I != 256; ++I) {
        std::array<llvm::APInt, 2> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        }
        auto Left = Arithmetic ? Values[0] + Values[1] : Values[0] ^ Values[1];
        auto Right =
            Arithmetic ? ~(Values[0] ^ Values[1]) : Values[0] & Values[1];
        auto Want = Scale;
        for (unsigned J = 0; J != 4; ++J)
          Want *= Left * Right;
        EXPECT_TRUE(Want.isZero());
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(Output, Values), Want);
      }
    }
}

TEST(SymMBAHigherProduct, ParityCountsRetainCarriesAndOpaqueInputLimits) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto A = C.mkXor(X, Y), B = C.mkAnd(X, Y);
    auto Scale = llvm::APInt::getOneBitSet(Width, Width - 5);
    auto Input = C.mkMul({C.mkConst(Scale), A, A, A, A, B, B, B, B});
    size_t Storage = MBAOptions{}.MaxTableBytes;
    detail::WorkBudget Budget(65536);
    EXPECT_EQ(detail::foldProductValuations(C, Input, Budget, Storage), Input);
    std::array<llvm::APInt, 2> Values{llvm::APInt(Width, 1),
                                      llvm::APInt(Width, 3)};
    EXPECT_EQ(C.eval(Input, Values), llvm::APInt::getSignMask(Width));

    llvm::SmallVector<SymRef, 16> Factors{
        C.mkConst(llvm::APInt::getSignMask(Width)), X, C.mkNot(X)};
    for (unsigned I = 0; I != 7; ++I)
      Factors.push_back(C.mkVar("opaque" + std::to_string(I), Width));
    auto WideDomain = C.mkMul(Factors);
    EXPECT_EQ(detail::foldProductValuations(C, WideDomain, Budget, Storage),
              WideDomain);
  }
}

TEST(SymMBAHigherProduct, ProductZeroProofHasIndependentFullWidthSMTCheck) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto A = C.mkAdd(X, Y), B = C.mkNot(C.mkXor(X, Y));
    auto Input =
        C.mkMul({C.mkConst(llvm::APInt::getOneBitSet(Width, Width - 4)), A, A,
                 A, A, B, B, B, B});
    size_t Storage = MBAOptions{}.MaxTableBytes;
    detail::WorkBudget Budget(65536);
    auto Output = detail::foldProductValuations(C, Input, Budget, Storage);
    ASSERT_TRUE(C.isConstZero(Output));
    solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verifier(C, Input, Output), SynthVerification::Equivalent);
  }
}

TEST(SymMBAHigherProduct, CancelsUnchangedSummandsBeforeDistributionProof) {
  for (unsigned Width : {3u, 8u, 16u, 32u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width);
    llvm::SmallVector<SymRef, 40> BeforeTerms, AfterTerms;
    for (unsigned I = 0; I != 32; ++I) {
      const auto Word = C.mkVar("unrelated" + std::to_string(I), Width);
      llvm::SmallVector<SymRef, 16> Factors(12, Word);
      Factors.push_back(C.mkConst(Width, 2 * I + 3));
      BeforeTerms.push_back(C.mkMul(Factors));
    }
    AfterTerms = BeforeTerms;
    BeforeTerms.push_back(C.mkMul(X, Z));
    BeforeTerms.push_back(C.mkMul(Y, Z));
    AfterTerms.push_back(C.mkMul(C.mkAdd(X, Y), Z));
    const auto Before = C.mkAdd(BeforeTerms), After = C.mkAdd(AfterTerms);
    detail::WorkBudget Full(4096);
    ASSERT_TRUE(detail::proveArithmeticIdentity(C, Before, After, Full, 1 << 20,
                                                false));
    EXPECT_LT(Full.used(), 2048u);
    for (size_t Work : {size_t(0), Full.used() - 1}) {
      detail::WorkBudget Short(Work);
      EXPECT_FALSE(detail::proveArithmeticIdentity(C, Before, After, Short,
                                                   1 << 20, false));
    }
    detail::WorkBudget Exact(Full.used());
    EXPECT_TRUE(detail::proveArithmeticIdentity(C, Before, After, Exact,
                                                1 << 20, false));
    detail::WorkBudget NoStorage(4096), Wrong(4096);
    EXPECT_FALSE(
        detail::proveArithmeticIdentity(C, Before, After, NoStorage, 0, false));
    EXPECT_FALSE(detail::proveArithmeticIdentity(
        C, Before, C.mkAdd(After, C.mkOne(Width)), Wrong, 1 << 20, false));
  }
}

TEST(SymMBAHigherProduct, ExtractsCompleteSharedProductsBeforeQuotientSearch) {
  for (unsigned Width : {3u, 8u, 16u, 32u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
    const auto U = C.mkVar("u", Width), Odd = C.mkOr(Z, C.mkOne(Width));
    llvm::SmallVector<SymRef, 16> Factors(8, Odd);
    Factors.append(4, W);
    const auto Common = C.mkMul(Factors), Scale = C.mkConst(Width, 37);
    const auto Input =
        C.mkAdd({C.mkMul({Scale, Common, C.mkAnd(X, Y)}),
                 C.mkMul({Scale, Common, C.mkAnd(X, C.mkNot(Y))}), U});
    const auto Expected = C.mkAdd(C.mkMul({Scale, Common, X}), U);
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    const auto Output =
        detail::solveStructuralFactors(C, Input, Options, Budget, Report);
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(C.readabilityCost(Output), C.readabilityCost(Expected));
    EXPECT_LT(Budget.used(), 8192u);
    std::mt19937_64 Random(0x434F4D4D4F4E);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      llvm::SmallVector<llvm::APInt, 5> Values;
      for (unsigned I = 0; I != 5; ++I) {
        llvm::APInt Value(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        Values.push_back(Value);
      }
      auto Want = llvm::APInt(Width, 37, false, true) * Values[0];
      for (unsigned I = 0; I != 8; ++I)
        Want *= Values[2] | llvm::APInt(Width, 1);
      for (unsigned I = 0; I != 4; ++I)
        Want *= Values[3];
      Want += Values[4];
      EXPECT_EQ(C.eval(Input, Values), Want);
      EXPECT_EQ(C.eval(Output, Values), Want);
    }
  }
}

TEST(SymMBAHigherProduct, RejectsInconsistentProductDiagonalsWithoutExpansion) {
  for (unsigned Width : {3u, 8u, 16u, 32u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width);
    const auto Input =
        C.mkAdd(C.mkMul({X, Y, Z}), C.mkMul({C.mkConst(Width, 2), C.mkOr(X, Y),
                                             C.mkOr(Y, Z), C.mkOr(X, Z)}));
    auto Terms = detail::splitIntoTerms(C, Input);
    ASSERT_TRUE(Terms);
    llvm::SmallVector<uint32_t, 3> Atoms;
    C.collectVars(Input, Atoms);
    detail::WorkBudget Search(1024), Proof(4096);
    EXPECT_FALSE(detail::expandOverMinterms(C, *Terms, Atoms, Width, Search,
                                            true, true));
    EXPECT_LT(Search.used(), 1024u);
    // This is a rejection of single-product synthesis only. General identity
    // proofs must still expand every coefficient of the original polynomial.
    auto Form = detail::expandOverMinterms(C, *Terms, Atoms, Width, Proof,
                                           false, false);
    ASSERT_TRUE(Form);
    EXPECT_FALSE(Form->Higher.empty());
  }
}

TEST(SymMBAHigherProduct, RecoversQuadraticsOverRepeatedMembershipClasses) {
  for (unsigned Width : {3u, 8u, 64u, 257u}) {
    SymContext C;
    llvm::SmallVector<SymRef, 8> Vars;
    llvm::SmallVector<uint32_t, 8> Ids;
    for (unsigned I = 0; I != 8; ++I) {
      Vars.push_back(C.mkVar("x" + std::to_string(I), Width));
      Ids.push_back(C.varId(Vars.back()));
    }
    auto A = C.mkXor({Vars[0], Vars[1], Vars[2]});
    auto B = C.mkOr({Vars[3], Vars[4], Vars[5]});
    for (auto Scale : {llvm::APInt(Width, 37, false, true),
                       llvm::APInt::getSignMask(Width)}) {
      llvm::SmallVector<detail::PolyTerm, 4> Terms;
      for (unsigned I = 0; I != 4; ++I)
        Terms.push_back({Scale,
                         {C.mkAnd(A, I & 1 ? Vars[6] : C.mkNot(Vars[6])),
                          C.mkAnd(B, I & 2 ? Vars[7] : C.mkNot(Vars[7]))}});
      detail::WorkBudget Budget(8192);
      bool Determined = false;
      auto Product = detail::recoverQuadraticTerms(
          C, Terms, Ids, Width, Budget, MBAOptions{}.MaxTableBytes, Determined);
      ASSERT_TRUE(Product);
      ASSERT_TRUE(Determined);
      EXPECT_LT(Budget.used(), 8192u);
      for (unsigned I = 0; I != 256; ++I)
        for (unsigned J = I; J != 256; ++J) {
          auto Left = [](unsigned M) { return std::popcount(M & 7) % 2 != 0; };
          auto Right = [](unsigned M) { return (M & 56) != 0; };
          unsigned Want = Left(I) && Right(J);
          unsigned Actual =
              Product->Factors[0].at(I) && Product->Factors[1].at(J);
          if (I != J) {
            Want += Left(J) && Right(I);
            Actual += Product->Factors[0].at(J) && Product->Factors[1].at(I);
          }
          EXPECT_EQ(Scale * llvm::APInt(Width, Want),
                    Product->Coefficient * llvm::APInt(Width, Actual));
        }
      Terms[0].Coeff += 1;
      detail::WorkBudget NearMiss(8192);
      Determined = false;
      EXPECT_FALSE(detail::recoverQuadraticTerms(C, Terms, Ids, Width, NearMiss,
                                                 MBAOptions{}.MaxTableBytes,
                                                 Determined));
      EXPECT_TRUE(Determined);
    }
  }
}

TEST(SymMBAHigherProduct, ReadsWideAffineSumsWithoutCornerEnumeration) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext C;
    llvm::SmallVector<SymRef, 16> Terms;
    for (unsigned I = 0; I != 13; ++I)
      Terms.push_back(C.mkMul(C.mkConst(Width, 2 * I + 1),
                              C.mkVar("x" + std::to_string(I), Width)));
    auto Input = C.mkAdd(Terms);
    MBAOptions Options;
    Options.VerifySamples = 0;
    Options.MaxWork = 8192;
    auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_LT(R.Work, Options.MaxWork);
    EXPECT_LE(R.SizeAfter, R.SizeBefore);
    std::mt19937_64 Random(0x41464649);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      llvm::SmallVector<llvm::APInt, 16> Values;
      llvm::APInt Expected(Width, 0);
      for (unsigned I = 0; I != 13; ++I) {
        llvm::APInt V(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          V |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        Expected += llvm::APInt(Width, 2 * I + 1) * V;
        Values.push_back(V);
      }
      EXPECT_EQ(C.eval(R.Expr, Values), Expected);
    }
  }
}

TEST(SymMBAHigherProduct,
     FinishesMixedDegreePartitionsWithChangedCoefficients) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width),
               Z = C.mkVar("z", Width), W = C.mkVar("w", Width),
               V = C.mkVar("v", Width);
    std::array<SymRef, 4> Words{
        C.mkXor(C.mkAdd(X, Y), Z), C.mkOr(C.mkMul(Y, Z), W),
        C.mkAnd(C.mkAdd(X, Z), C.mkOr(Y, V)), C.mkXor(C.mkAdd(W, V), X)};
    std::array<SymRef, 4> Common{C.mkOr(C.mkAdd(X, Y), C.mkOne(Width)),
                                 C.mkOr(C.mkMul(Z, W), C.mkOne(Width)),
                                 C.mkNot(C.mkAnd(C.mkXor(X, V), Z)),
                                 C.mkOr(C.mkAdd(Y, V), C.mkOne(Width))};
    std::array<SymRef, 3> Axes{X, Z, V};
    llvm::SmallVector<SymRef, 24> Terms;
    for (unsigned Block = 0; Block != 3; ++Block) {
      const unsigned Degree = 48 + 16 * Block;
      const auto Scale = Block == 0   ? llvm::APInt(Width, 1).shl(Width - 2)
                         : Block == 1 ? llvm::APInt(Width, 37)
                                      : -llvm::APInt(Width, 11);
      for (unsigned Corner = 1; Corner != 7; ++Corner) {
        auto Coefficient = Scale;
        if (Corner == 1)
          Coefficient += 13;
        if (Corner == 4)
          Coefficient -= 2 * Scale;
        if (Corner == 2)
          Coefficient += llvm::APInt::getSignMask(Width);
        llvm::SmallVector<SymRef, 96> Factors{C.mkConst(Coefficient)};
        for (unsigned I = 0; I != Degree - 3; ++I)
          Factors.push_back(Common[(I + Block) % 4]);
        for (unsigned I = 0; I != 3; ++I)
          Factors.push_back(
              C.mkAnd(Words[(I + Block) % 4],
                      Corner & (1u << I) ? Axes[I] : C.mkNot(Axes[I])));
        Terms.push_back(C.mkMul(Factors));
      }
    }
    Terms.append({C.mkXor(X, Y), C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)),
                  C.mkOr(Z, W), C.mkAnd(Z, W),
                  C.mkMul({C.mkConst(Width, 19), X, Y, Z})});
    const auto Input = C.mkAdd(Terms);
    MBAOptions Options;
    Options.MaxWork = Width <= 64 ? 65536 : 262144;
    Options.VerifySamples = 0;
    const auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_LT(R.Work, Options.MaxWork);
    EXPECT_LT(R.SizeAfter, R.SizeBefore / 2);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    std::mt19937_64 Random(0x4D49584544);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      llvm::SmallVector<llvm::APInt, 5> Values;
      for (unsigned I = 0; I != 5; ++I) {
        llvm::APInt Value(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        if (Sample < 8)
          Value = llvm::APInt(Width, Sample);
        Values.push_back(Value);
      }
      const auto &A = Values[0], &B = Values[1], &D = Values[2], &E = Values[3],
                 &F = Values[4];
      std::array<llvm::APInt, 4> Source{(A + B) ^ D, (B * D) | E,
                                        (A + D) & (B | F), (E + F) ^ A};
      std::array<llvm::APInt, 4> Prefix{(A + B) | 1, (D * E) | 1,
                                        ~((A ^ F) & D), (B + F) | 1};
      std::array<llvm::APInt, 3> Select{A, D, F};
      llvm::APInt Expected = A + B + D + E + 19 * A * B * D;
      for (unsigned Block = 0; Block != 3; ++Block) {
        const auto Scale = Block == 0   ? llvm::APInt(Width, 1).shl(Width - 2)
                           : Block == 1 ? llvm::APInt(Width, 37)
                                        : -llvm::APInt(Width, 11);
        llvm::APInt Power(Width, 1);
        for (unsigned I = 0; I != 45 + 16 * Block; ++I)
          Power *= Prefix[(I + Block) % 4];
        for (unsigned Corner = 1; Corner != 7; ++Corner) {
          auto Product = Scale;
          if (Corner == 1)
            Product += 13;
          if (Corner == 4)
            Product -= 2 * Scale;
          if (Corner == 2)
            Product += llvm::APInt::getSignMask(Width);
          Product *= Power;
          for (unsigned I = 0; I != 3; ++I)
            Product *= Source[(I + Block) % 4] &
                       (Corner & (1u << I) ? Select[I] : ~Select[I]);
          Expected += Product;
        }
      }
      EXPECT_EQ(C.eval(R.Expr, Values), Expected);
    }
  }
}

} // namespace
