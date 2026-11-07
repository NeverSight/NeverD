//===- SymMBAWeightedMaskTests.cpp - Full-word masked coefficients --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise full-word mask synthesis, independent proofs and resource limits.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"

#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt randomWord(unsigned Width, std::mt19937_64 &Random) {
  llvm::APInt Result(Width, 0);
  for (unsigned Bit = 0; Bit < Width; Bit += 64)
    Result |= llvm::APInt(Width, Random()).shl(Bit);
  return Result;
}

SymRef partition(SymContext &C, SymRef Carrier, const llvm::APInt &Scale,
                 unsigned Count, std::mt19937_64 &Random) {
  const unsigned Width = C.width(Carrier);
  std::vector<llvm::APInt> Masks(Count, llvm::APInt(Width, 0));
  for (unsigned Bit = 0; Bit != Width; ++Bit)
    Masks[Random() % Count].setBit(Bit);
  llvm::SmallVector<SymRef, 32> Terms;
  for (const auto &Mask : Masks)
    Terms.push_back(
        C.mkMul(C.mkConst(Scale), C.mkAnd(Carrier, C.mkConst(Mask))));
  return C.mkAdd(Terms);
}

std::pair<SymRef, SymRef> inclusion(SymContext &C, SymRef Carrier,
                                    unsigned Count, std::mt19937_64 &Random,
                                    bool NearMiss = false) {
  const unsigned Width = C.width(Carrier);
  std::vector<llvm::APInt> Masks;
  llvm::APInt Union(Width, 0);
  for (unsigned I = 0; I != Count; ++I) {
    Masks.push_back(randomWord(Width, Random));
    Union |= Masks.back();
  }
  llvm::SmallVector<SymRef, 32> Terms;
  for (unsigned Subset = 1; Subset != (1u << Count); ++Subset) {
    llvm::APInt Mask = llvm::APInt::getAllOnes(Width);
    unsigned Members = 0;
    for (unsigned I = 0; I != Count; ++I) {
      if ((Subset >> I) & 1) {
        Mask &= Masks[I];
        ++Members;
      }
    }
    if (NearMiss && Subset == 1)
      Mask.flipBit(Width - 1);
    auto K =
        Members & 1 ? llvm::APInt(Width, 1) : llvm::APInt::getAllOnes(Width);
    Terms.push_back(C.mkMul(C.mkConst(K), C.mkAnd(Carrier, C.mkConst(Mask))));
  }
  return {C.mkAdd(Terms), C.mkAnd(Carrier, C.mkConst(Union))};
}

TEST(SymMBAWeightedMask, RecoversOverlappingInclusionExclusionAtFullWidth) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Carrier = C.mkXor(X, Y);
    std::mt19937_64 Random(0x48534d31);
    auto [Input, Expected] = inclusion(C, Carrier, 5, Random);
    MBAOptions Options;
    Options.MaxWork = 1 << 20;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input, Options);
    ASSERT_TRUE(Result.Changed) << C.toString(Result.Expr);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(C.readabilityCost(Result.Expr), C.readabilityCost(Expected));
    solver::SolverOptions ProofOptions;
    ProofOptions.Blast.MaxWidth = Width;
    solver::SymSynthVerifier Verify(ProofOptions);
    EXPECT_EQ(Verify(C, Expected, Result.Expr), SynthVerification::Equivalent);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      llvm::SmallVector<llvm::APInt, 8> Values(C.numVars(),
                                               llvm::APInt(Width, 0));
      Values[0] = randomWord(Width, Random);
      Values[1] = randomWord(Width, Random);
      EXPECT_EQ(C.eval(Input, Values), C.eval(Result.Expr, Values));
    }
  }
}

TEST(SymMBAWeightedMask, PreservesOpaqueNonlinearAndFlattenedCarriers) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    for (SymRef Carrier : {C.mkAdd(X, Y), C.mkMul(X, Y), C.mkAnd({X, Y, Z})}) {
      SCOPED_TRACE(C.toString(Carrier));
      for (int Weight : {37, -11, 182}) {
        SCOPED_TRACE(Weight);
        std::mt19937_64 Random(0x121357);
        const llvm::APInt Scale(Width, Weight, true);
        auto Input = partition(C, Carrier, Scale, 16, Random);
        auto Expected = C.mkMul(C.mkConst(Scale), Carrier);
        MBAOptions Options;
        Options.MaxWork = 1 << 20;
        Options.VerifySamples = 0;
        detail::WorkBudget Budget(Options.MaxWork);
        detail::SolveReport Report;
        auto Result =
            detail::solveWeightedMasks(C, Input, Options, Budget, Report);
        EXPECT_EQ(Result, Expected) << C.toString(Result);
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
        detail::WorkBudget Proof(1 << 20);
        EXPECT_TRUE(
            detail::proveMaskedSumIdentity(C, Input, Result, Carrier, Proof));
        for (unsigned Sample = 0; Sample != 64; ++Sample) {
          std::vector<llvm::APInt> Values(C.numVars(), llvm::APInt(Width, 0));
          for (auto &Value : Values)
            Value = randomWord(Width, Random);
          EXPECT_EQ(C.eval(Input, Values), C.eval(Result, Values));
        }
      }
    }
  }
}

TEST(SymMBAWeightedMask, CompletesSixteenPartitionsWithinSmallWorkAllowance) {
  SymContext C;
  auto Carrier = C.mkAdd(C.mkVar("x", 64), C.mkVar("y", 64));
  std::mt19937_64 Random(0x471246);
  auto Input = partition(C, Carrier, llvm::APInt(64, 37), 16, Random);
  auto Expected = C.mkMul(C.mkConst(llvm::APInt(64, 37)), Carrier);
  MBAOptions Options;
  Options.MaxWork = 8192;
  Options.VerifySamples = 0;
  auto Result = simplifyMBADeep(C, Input, Options);
  EXPECT_EQ(Result.Expr, Expected) << C.toString(Result.Expr);
  EXPECT_LT(Result.Work, Options.MaxWork);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBAWeightedMask, FoldedLiteralKeepsTheShortestCarrierOrientation) {
  std::mt19937_64 Random(0x464f4c4445444d41);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (bool Residual : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Residual);
      SymContext C;
      const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      const auto Z = C.mkVar("z", Width), Carrier = C.mkMul(X, Y);
      const auto Scale = llvm::APInt(Width, 182).shl(Width - 8);
      const auto Bias = llvm::APInt(Width, 162).shl(Width - 8);
      llvm::SmallVector<SymRef, 8> Terms{C.mkConst(Bias)};
      for (const auto &[Mask, Flip] :
           {std::pair{32u, 50u}, std::pair{132u, 56u}, std::pair{8u, 19u},
            std::pair{16u, 17u}})
        Terms.push_back(C.mkMul(
            C.mkConst(Scale),
            C.mkAnd(C.mkConst(Width, Mask),
                    C.mkNot(C.mkXor(Carrier, C.mkConst(Width, Flip))))));
      if (Residual)
        Terms.push_back(Z);
      const auto Input = C.mkAdd(Terms);
      auto Expected =
          C.mkMul(C.mkConst(Scale),
                  C.mkNot(C.mkAnd(C.mkConst(Width, 188),
                                  C.mkXor(Carrier, C.mkConst(Width, 48)))));
      if (Residual)
        Expected = C.mkAdd(Expected, Z);
      MBAOptions Options;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      EXPECT_LE(Result.SizeAfter, C.readabilityCost(Expected))
          << C.toString(Result.Expr);
      for (unsigned Trial = 0; Trial != 128; ++Trial) {
        std::vector<llvm::APInt> Values(C.numVars(), llvm::APInt(Width, 0));
        for (auto &V : Values)
          V = randomWord(Width, Random);
        const auto Word = Values[0] * Values[1];
        auto Want = Scale * ~(llvm::APInt(Width, 188) &
                              (llvm::APInt(Width, 48) ^ Word));
        if (Residual)
          Want += Values[2];
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(Result.Expr, Values), Want);
      }
    }
}

TEST(SymMBAWeightedMask, RecoversScaledAndNegatedGroupsWithOpaqueResiduals) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width), Carrier = C.mkXor(X, Y);
    std::mt19937_64 Random(0x742531);
    auto [Flat, Reduced] = inclusion(C, Carrier, 5, Random);
    llvm::SmallVector<SymRef, 32> Positive, Negative;
    for (auto Term : C.operands(Flat)) {
      if (C.op(Term) == SymOp::Mul && C.numOperands(Term) == 2 &&
          C.isConstOnes(C.operand(Term, 0)))
        Negative.push_back(C.operand(Term, 1));
      else
        Positive.push_back(Term);
    }
    ASSERT_FALSE(Positive.empty());
    ASSERT_FALSE(Negative.empty());
    auto Grouped = C.mkSub(C.mkAdd(Positive), C.mkAdd(Negative));
    auto Residual = C.mkXor(Z, C.mkLShr(Z, C.mkConst(llvm::APInt(Width, 3))));
    auto Scale = C.mkConst(llvm::APInt(Width, -5, true));
    auto Input = C.mkAdd(C.mkMul(Scale, Grouped), Residual);
    auto Expected = C.mkAdd(C.mkMul(Scale, Reduced), Residual);
    detail::WorkBudget Budget(1 << 20);
    detail::SolveReport Report;
    auto Result = detail::solveWeightedMasks(C, Input, {}, Budget, Report);
    EXPECT_EQ(Result, Expected);
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      std::vector<llvm::APInt> Values(C.numVars(), llvm::APInt(Width, 0));
      for (auto &Value : Values)
        Value = randomWord(Width, Random);
      EXPECT_EQ(C.eval(Input, Values), C.eval(Result, Values));
    }
  }
}

TEST(SymMBAWeightedMask, ResidualWordsMustCancelExactlyInTheIndependentProof) {
  SymContext C;
  auto X = C.mkVar("x", 64), Y = C.mkVar("y", 64), Z = C.mkVar("z", 64);
  auto Residual = C.mkXor(Y, Z);
  auto Before = C.mkSub(X, C.mkAdd(Y, Residual));
  auto After = C.mkAdd({X, C.mkNeg(Y), C.mkNeg(Residual)});
  detail::WorkBudget Proof(1 << 16);
  EXPECT_TRUE(detail::proveMaskedSumIdentity(C, Before, After, X, Proof));
  auto Wrong = C.mkAdd({X, C.mkNeg(Y), C.mkNeg(C.mkOr(Y, Z))});
  detail::WorkBudget Counterexample(1 << 16);
  EXPECT_FALSE(
      detail::proveMaskedSumIdentity(C, Before, Wrong, X, Counterexample));
}

TEST(SymMBAWeightedMask, ContinuesInsideRecoveredCarrierAndCachesCompleteRoot) {
  SymContext C;
  auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  auto Carrier = C.mkAdd(C.mkOr(X, Y), C.mkAnd(X, Y));
  std::mt19937_64 Random(0x984275);
  auto Input = partition(C, Carrier, llvm::APInt(32, 37), 8, Random);
  auto Expected = C.mkMul(C.mkConst(llvm::APInt(32, 37)), C.mkAdd(X, Y));
  MBARewriteCache Cache(C, 128);
  auto First = simplifyMBADeep(C, Input, {}, Cache);
  EXPECT_EQ(First.Expr, Expected);
  EXPECT_EQ(First.Evidence, MBAEvidence::Derivation);
  auto Again = simplifyMBADeep(C, Input, {}, Cache);
  EXPECT_EQ(Again.Expr, Expected);
  EXPECT_EQ(Again.Work, 1u);
  EXPECT_GT(Cache.reusedRoots(), 0u);
}

TEST(SymMBAWeightedMask, ReconstructsManyPreviouslyAbsentFlattenedCarriers) {
  SymContext C;
  constexpr unsigned Width = 16;
  auto Scale = C.mkConst(llvm::APInt(Width, 37));
  llvm::SmallVector<SymRef, 128> Terms, ExpectedTerms;
  std::mt19937_64 Random(0x793125);
  for (unsigned Group = 0; Group != 64; ++Group) {
    auto X = C.mkVar("x" + std::to_string(Group), Width);
    auto Y = C.mkVar("y" + std::to_string(Group), Width);
    auto Z = C.mkVar("z" + std::to_string(Group), Width);
    std::vector<llvm::APInt> Masks(4, llvm::APInt(Width, 0));
    for (unsigned Bit = 0; Bit != Width; ++Bit)
      Masks[Random() % Masks.size()].setBit(Bit);
    // Construct the masked flattened uses without interning their shared AND.
    for (const auto &Mask : Masks)
      Terms.push_back(C.mkMul(Scale, C.mkAnd({C.mkConst(Mask), X, Y, Z})));
    ExpectedTerms.push_back(X);
    ExpectedTerms.push_back(Y);
    ExpectedTerms.push_back(Z);
  }
  auto Input = C.mkAdd(Terms);
  detail::WorkBudget Budget(1 << 22);
  detail::SolveReport Report;
  auto Result = detail::solveWeightedMasks(C, Input, {}, Budget, Report);
  Terms.clear();
  for (size_t I = 0; I != ExpectedTerms.size(); I += 3)
    Terms.push_back(
        C.mkMul(Scale, C.mkAnd(llvm::ArrayRef(ExpectedTerms).slice(I, 3))));
  EXPECT_NE(Result, Input);
  EXPECT_EQ(Result, C.mkAdd(Terms));
  EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
  for (unsigned Sample = 0; Sample != 64; ++Sample) {
    std::vector<llvm::APInt> Values(C.numVars(), llvm::APInt(Width, 0));
    for (auto &Value : Values)
      Value = randomWord(Width, Random);
    EXPECT_EQ(C.eval(Input, Values), C.eval(Result, Values));
  }
}

TEST(SymMBAWeightedMask, IndependentVerifierRejectsHighBitAndNonlinearChanges) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Top = C.mkConst(llvm::APInt::getOneBitSet(Width, Width - 1));
    for (SymRef Wrong :
         {C.mkAdd(X, C.mkAnd(X, Top)), C.mkMul(X, X), C.mkAnd(X, Y)}) {
      detail::WorkBudget Proof(1 << 20);
      EXPECT_FALSE(detail::proveMaskedSumIdentity(C, X, Wrong, X, Proof));
    }
    // Twice the top bit really is zero modulo the complete word width.
    auto Overflow = C.mkMul(C.mkConst(llvm::APInt(Width, 2)), C.mkAnd(X, Top));
    detail::WorkBudget Proof(1 << 20);
    EXPECT_TRUE(
        detail::proveMaskedSumIdentity(C, Overflow, C.mkZero(Width), X, Proof));
    detail::WorkBudget Incomplete(1);
    EXPECT_FALSE(detail::proveMaskedSumIdentity(C, X, X, X, Incomplete));
    detail::WorkBudget NoStorage(1 << 20);
    EXPECT_FALSE(detail::proveMaskedSumIdentity(C, X, X, X, NoStorage, 0));
  }
}

TEST(SymMBAWeightedMask, ExhaustiveBytePairsKeepNearbyNonidentities) {
  for (bool NearMiss : {false, true}) {
    SymContext C;
    auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
    auto Carrier = C.mkXor(X, Y);
    std::mt19937_64 Random(0x781269);
    auto [Input, Expected] = inclusion(C, Carrier, 4, Random, NearMiss);
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input, Options);
    bool Different = false;
    for (unsigned A = 0; A != 256; ++A) {
      for (unsigned B = 0; B != 256; ++B) {
        llvm::SmallVector<uint64_t, 8> Values(C.numVars(), 0);
        Values[0] = A;
        Values[1] = B;
        auto Original = C.evalU64(Input, Values);
        ASSERT_EQ(C.evalU64(Result.Expr, Values), Original);
        Different |= Original != C.evalU64(Expected, Values);
      }
    }
    EXPECT_EQ(Different, NearMiss);
  }
}

TEST(SymMBAWeightedMask, ExhaustiveRandomWeightedOverlaps) {
  std::mt19937_64 Random(0x746321);
  for (unsigned Case = 0; Case != 64; ++Case) {
    SCOPED_TRACE(Case);
    SymContext C;
    auto X = C.mkVar("x", 8);
    llvm::SmallVector<SymRef, 16> Terms;
    for (unsigned I = 0; I != 16; ++I) {
      auto Weight = C.mkConst(randomWord(8, Random));
      auto Mask = C.mkConst(randomWord(8, Random));
      Terms.push_back(C.mkMul(Weight, C.mkAnd(X, Mask)));
    }
    auto Input = C.mkAdd(Terms);
    MBAOptions Options;
    Options.MaxWork = 32768;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input, Options);
    EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
    for (unsigned Value = 0; Value != 256; ++Value) {
      std::vector<uint64_t> Values(C.numVars(), Value);
      ASSERT_EQ(C.evalU64(Input, Values), C.evalU64(Result.Expr, Values));
    }
  }
}

TEST(SymMBAWeightedMask, KeepsGroupsSeparateAndPreservesOrdinaryTerms) {
  SymContext C;
  auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32), Z = C.mkVar("z", 32);
  std::mt19937_64 Random(0x781245);
  auto Left = partition(C, X, llvm::APInt(32, 7), 8, Random);
  auto Right = partition(C, Y, llvm::APInt(32, -11), 8, Random);
  auto Input = C.mkAdd({Left, Right, Z, C.mkConst(llvm::APInt(32, 97))});
  auto Expected = C.mkAdd({C.mkMul(C.mkConst(llvm::APInt(32, 7)), X),
                           C.mkMul(C.mkConst(llvm::APInt(32, -11)), Y), Z,
                           C.mkConst(llvm::APInt(32, 97))});
  auto Result = simplifyMBADeep(C, Input);
  solver::SymSynthVerifier Verify;
  EXPECT_EQ(Verify(C, Expected, Result.Expr), SynthVerification::Equivalent);
  EXPECT_LE(C.readabilityCost(Result.Expr), C.readabilityCost(Expected));
}

TEST(SymMBAWeightedMask,
     RefusesIncompleteWorkAndStorageWithoutPartialRewrites) {
  for (size_t Limit : {size_t(0), size_t(1), size_t(64), size_t(256)}) {
    SymContext C;
    auto X = C.mkVar("x", 8);
    std::mt19937_64 Random(0x51235);
    auto Input = partition(C, X, llvm::APInt(8, 37), 8, Random);
    MBAOptions Options;
    Options.MaxWork = Limit;
    auto Result = simplifyMBADeep(C, Input, Options);
    EXPECT_LE(Result.Work, Limit);
    solver::SymSynthVerifier Verify;
    EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
    for (size_t Bytes : {size_t(0), size_t(64), size_t(4096)}) {
      Options.MaxTableBytes = Bytes;
      detail::WorkBudget Budget(1 << 20);
      detail::SolveReport Report;
      EXPECT_EQ(detail::solveWeightedMasks(C, Input, Options, Budget, Report),
                Input);
    }
  }
}

TEST(SymMBAWeightedMask, DenseEvenWeightsHaveCompleteProofsWithinSmallBudgets) {
  for (unsigned Width : {32u, 64u}) {
    for (unsigned Count : {2u, 4u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Count);
      SymContext C;
      auto X = C.mkVar("x", Width);
      std::mt19937_64 Random(0x123487);
      Random.discard(2 * Width);
      auto Input = partition(C, X, llvm::APInt(Width, 182), Count, Random);
      MBAOptions Options;
      Options.MaxWork = 8192;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(Result.Expr, C.mkMul(C.mkConst(llvm::APInt(Width, 182)), X));
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      detail::WorkBudget Proof(8192);
      EXPECT_TRUE(
          detail::proveMaskedSumIdentity(C, Input, Result.Expr, X, Proof));
      auto check = [&](const llvm::APInt &Value) {
        std::vector<llvm::APInt> Values(C.numVars(), Value);
        EXPECT_EQ(C.eval(Input, Values), C.eval(Result.Expr, Values));
      };
      check(llvm::APInt::getAllOnes(Width));
      for (unsigned A = 0; A != Width; ++A)
        for (unsigned B = 0; B != Width; ++B)
          check(llvm::APInt::getOneBitSet(Width, A) |
                llvm::APInt::getOneBitSet(Width, B));
      for (unsigned Sample = 0; Sample != 512; ++Sample)
        check(randomWord(Width, Random));
    }
  }
}

TEST(SymMBAWeightedMask, IndependentZ3ProvesFullWidthWeightedPartitions) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 is unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width);
    std::mt19937_64 Random(0x123487);
    for (int Scale : {37, -11, 182}) {
      // Dense even products at 32/64 bits exceed the fixed SMT time allowance.
      // Keep those exact cases in the complete certificate test above.
      if (Scale == 182 && Width > 16)
        continue;
      SCOPED_TRACE(Scale);
      auto Input = partition(C, X, llvm::APInt(Width, Scale), 4, Random);
      auto Result = simplifyMBADeep(C, Input);
      ASSERT_TRUE(Result.Changed);
      EXPECT_EQ(Result.Expr, C.mkMul(C.mkConst(llvm::APInt(Width, Scale)), X));
      solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
    }
  }
}

} // namespace
