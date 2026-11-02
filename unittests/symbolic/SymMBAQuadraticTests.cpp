//===- SymMBAQuadraticTests.cpp - Direct quadratic factor recovery --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check factor recovery against independent enumeration and word arithmetic.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymParse.h"

#include <algorithm>
#include <array>
#include <map>
#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

// Independently multiply the selected minterms, including modular cancellation.
detail::HigherCoefficients multiplyTables(const TruthTable &A,
                                          const TruthTable &B,
                                          const llvm::APInt &Scale) {
  detail::HigherCoefficients Out;
  for (unsigned I = 0; I != A.entries(); ++I) {
    if (!A.at(I))
      continue;
    for (unsigned J = 0; J != B.entries(); ++J) {
      if (!B.at(J))
        continue;
      detail::Monomial Key{std::min(I, J), std::max(I, J)};
      auto [It, New] = Out.try_emplace(Key, Scale);
      if (!New)
        It->second += Scale;
    }
  }
  std::erase_if(Out, [](const auto &Entry) { return Entry.second.isZero(); });
  return Out;
}

TruthTable table(unsigned Arity, uint64_t Bits) {
  auto T = TruthTable::zero(Arity);
  for (unsigned I = 0; I != T.entries(); ++I)
    T.setValue(I, (Bits >> I) & 1);
  return T;
}

uint32_t matrixCode(const detail::HigherCoefficients &H) {
  uint32_t Code = 0;
  for (unsigned I = 0, Shift = 0; I != 4; ++I)
    for (unsigned J = I; J != 4; ++J, Shift += 2)
      if (auto It = H.find(detail::Monomial{I, J}); It != H.end())
        Code |= uint32_t(It->second.getZExtValue()) << Shift;
  return Code;
}

TEST(SymMBAQuadratic, MatchesExhaustiveTwoInputFactorOracle) {
  // All two-input functions and all nonzero scales modulo four. This oracle
  // does not classify diagonals, graph edges or connected components.
  std::map<uint32_t, detail::HigherCoefficients> Possible;
  for (unsigned A = 1; A != 16; ++A)
    for (unsigned B = 1; B != 16; ++B)
      for (unsigned Scale = 1; Scale != 4; ++Scale) {
        auto H =
            multiplyTables(table(2, A), table(2, B), llvm::APInt(2, Scale));
        Possible.emplace(matrixCode(H), H);
      }
  auto Check = [&](const detail::HigherCoefficients &H, bool Expected) {
    detail::WorkBudget Budget(512);
    auto R = detail::recoverQuadraticProduct(H, 2, 2, Budget, 4096);
    ASSERT_EQ(bool(R), Expected) << matrixCode(H);
    EXPECT_LT(Budget.used(), 512u);
    if (R)
      EXPECT_EQ(multiplyTables(R->Factors[0], R->Factors[1], R->Coefficient),
                H);
  };
  for (const auto &[Code, H] : Possible)
    Check(H, true);
  std::mt19937 Random(0x713829);
  for (unsigned Sample = 0; Sample != 4096; ++Sample) {
    const uint32_t Code = Random() & ((1u << 20) - 1);
    detail::HigherCoefficients H;
    for (unsigned I = 0, Shift = 0; I != 4; ++I)
      for (unsigned J = I; J != 4; ++J, Shift += 2)
        if (auto Value = (Code >> Shift) & 3)
          H.emplace(detail::Monomial{I, J}, llvm::APInt(2, Value));
    Check(H, Possible.contains(Code));
  }
}

TEST(SymMBAQuadratic, PreservesWideEvenScalesAndVanishingCrossTerms) {
  for (unsigned Width : {1u, 8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    for (auto Scale : {llvm::APInt(Width, 1), llvm::APInt::getAllOnes(Width),
                       llvm::APInt::getSignMask(Width)}) {
      for (unsigned Arity : {2u, 3u, 4u, 5u, 6u}) {
        SCOPED_TRACE(Arity);
        auto A = atomTruthTable(0, Arity);
        A ^= atomTruthTable(1, Arity);
        auto B = atomTruthTable(0, Arity) | atomTruthTable(1, Arity);
        auto H = multiplyTables(A, B, Scale);
        detail::WorkBudget Budget(16384);
        auto R = detail::recoverQuadraticProduct(H, Arity, Width, Budget, 8192);
        ASSERT_TRUE(R);
        EXPECT_LT(Budget.used(), 16384u);
        EXPECT_EQ(multiplyTables(R->Factors[0], R->Factors[1], R->Coefficient),
                  H);
      }
    }
  }
}

TEST(SymMBAQuadratic, RejectsIncompleteWorkStorageAndMalformedMatrices) {
  auto H = multiplyTables(table(3, 0x96), table(3, 0xE8), llvm::APInt(257, 7));
  detail::WorkBudget Full(4096);
  ASSERT_TRUE(detail::recoverQuadraticProduct(H, 3, 257, Full, 4096));
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
    detail::WorkBudget Budget(Work);
    auto R = detail::recoverQuadraticProduct(H, 3, 257, Budget, 4096);
    EXPECT_EQ(bool(R), Work == Full.used());
    EXPECT_LE(Budget.used(), Work);
  }
  for (size_t Bytes : {0u, 64u, 256u}) {
    detail::WorkBudget Budget(4096);
    EXPECT_FALSE(detail::recoverQuadraticProduct(H, 3, 257, Budget, Bytes));
  }
  auto Bad = [&](detail::Monomial Key, llvm::APInt Value) {
    auto Altered = H;
    Altered.insert_or_assign(std::move(Key), std::move(Value));
    detail::WorkBudget Budget(4096);
    EXPECT_FALSE(
        detail::recoverQuadraticProduct(Altered, 3, 257, Budget, 4096));
  };
  Bad({0, 8}, llvm::APInt(257, 7));
  Bad({3, 1}, llvm::APInt(257, 7));
  Bad({1, 2, 3}, llvm::APInt(257, 7));
  Bad({0, 0}, llvm::APInt(64, 7));
  Bad({0, 0}, llvm::APInt(257, 0));
  Bad({0, 0}, llvm::APInt::getSignMask(257));
}

TEST(SymMBAQuadratic, CompletesThePreviouslyCappedMaskedProductRegions) {
  const char *Inputs[] = {
      "7*((203&(x+y))+(29&(x+y))+(88&(x*y))+(211&(x*y))"
      "+(54&(x*y))-(9&(x+y))-(80&(x*y))-(18&(x*y)))+(x|y)",
      "7*((11291&(x+y))+(48483&(x+y))+(20559&(x+y))+3*(3&(x+y))"
      "+(58803&(x+y))+(41987&(x+y))+(63695&(x*y))+(31444&(x*y))"
      "+(34168&(x*y))+(64&(x*y))+(44707&(x*y))+(10368&(x*y))"
      "+(32768&(x*y))-((3&(x+y))+(44035&(x+y))+(11&(x+y))"
      "+(4163&(x+y))+(42003&(x+y))+(42275&(x+y))+(16387&(x+y))"
      "+(30916&(x*y))+(32840&(x*y))+(80&(x*y))+(43139&(x*y))"
      "+(10880&(x*y))+(33824&(x*y))))+(x|y)"};
  for (unsigned I = 0; I != 2; ++I) {
    SymContext C;
    const unsigned Width = I ? 16 : 8;
    auto Input = parseSymExpr(C, Inputs[I], Width);
    auto Want = parseSymExpr(
        C, I ? "(x|y)+7*((x*y)+(32255&(x+y)))" : "(x|y)+7*((x*y)+(223&(x+y)))",
        Width);
    ASSERT_TRUE(Input.ok()) << Input.Error;
    ASSERT_TRUE(Want.ok()) << Want.Error;
    MBAOptions Options;
    Options.MaxWork = 16384;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input.Root, Options);
    EXPECT_TRUE(R.Changed);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(R.SizeAfter, C.readabilityCost(Want.Root));
    EXPECT_LT(R.Work, Options.MaxWork);
    if (Width == 8) {
      SymEvalPlan Before(C, Input.Root), After(C, R.Expr),
          Expected(C, Want.Root);
      for (unsigned X = 0; X != 256; ++X)
        for (unsigned Y = 0; Y != 256; ++Y) {
          llvm::SmallVector<uint64_t, 2> Values{X, Y};
          EXPECT_EQ(Before.evalU64(Values), Expected.evalU64(Values));
          EXPECT_EQ(After.evalU64(Values), Expected.evalU64(Values));
        }
    }
  }
}

TEST(SymMBAQuadratic, RecoversProductsWithIndependentPartitionVariables) {
  for (unsigned Width : {4u, 8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto Input = parseSymExpr(C,
                              "((x^y)&z)*((x|y)&w) + ((x^y)&~z)*((x|y)&w) + "
                              "((x^y)&z)*((x|y)&~w) + ((x^y)&~z)*((x|y)&~w)",
                              Width);
    auto Want = parseSymExpr(C, "(x^y)*(x|y)", Width);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.MaxWork = 16384;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input.Root, Options);
    EXPECT_TRUE(R.Changed);
    EXPECT_LT(R.Work, Options.MaxWork);
    EXPECT_LE(R.SizeAfter, C.readabilityCost(Want.Root));
    std::mt19937_64 Random(0x63714);
    SymEvalPlan Before(C, Input.Root), After(C, R.Expr);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      llvm::SmallVector<llvm::APInt, 4> Values;
      for (unsigned I = 0; I != 4; ++I) {
        llvm::APInt Value(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        Values.push_back(Value);
      }
      // The parser creates x, y, z, w in that order. z and w partition the
      // operands but must not affect the recovered product.
      auto Expected = (Values[0] ^ Values[1]) * (Values[0] | Values[1]);
      EXPECT_EQ(C.eval(Input.Root, Values), Expected);
      EXPECT_EQ(C.eval(R.Expr, Values), Expected);
    }
    if (Width == 4) {
      for (unsigned Assignment = 0; Assignment != 65536; ++Assignment) {
        llvm::SmallVector<uint64_t, 4> Values;
        for (unsigned I = 0; I != 4; ++I)
          Values.push_back((Assignment >> (4 * I)) & 15);
        auto Expected =
            ((Values[0] ^ Values[1]) * (Values[0] | Values[1])) & 15;
        EXPECT_EQ(Before.evalU64(Values), Expected);
        EXPECT_EQ(After.evalU64(Values), Expected);
      }
    }
  }
}

TEST(SymMBAQuadratic, IndependentSmallWordZ3Proofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {2u, 4u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto Input = parseSymExpr(C,
                              "((x^y)&z)*((x|y)&w) + ((x^y)&~z)*((x|y)&w) + "
                              "((x^y)&z)*((x|y)&~w) + ((x^y)&~z)*((x|y)&~w)",
                              Width);
    auto R = simplifyMBADeep(C, Input.Root);
    solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verifier(C, Input.Root, R.Expr), SynthVerification::Equivalent);
  }
}

TEST(SymMBAQuadratic, RecoversFiveInputExpansionsBeforeLocalBudgetStarvation) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
    auto V = C.mkVar("v", Width);
    auto A = C.mkXor(X, Y), B = C.mkOr(Y, V);
    llvm::SmallVector<SymRef, 16> Terms;
    for (unsigned I = 0; I != 4; ++I)
      for (unsigned J = 0; J != 4; ++J)
        Terms.push_back(C.mkMul(
            C.mkAnd({A, I & 1 ? Z : C.mkNot(Z), I & 2 ? W : C.mkNot(W)}),
            C.mkAnd({B, J & 1 ? X : C.mkNot(X), J & 2 ? Z : C.mkNot(Z)})));
    for (unsigned Scale : {1u, 7u, 182u}) {
      SCOPED_TRACE(Scale);
      auto K = C.mkConst(Width, Scale);
      auto Error = C.mkAnd(X, C.mkConst(llvm::APInt::getSignMask(Width)));
      auto Input = C.mkAdd({C.mkMul(K, C.mkAdd(Terms)), X, Y, Error});
      auto Expected = C.mkAdd({C.mkMul({K, A, B}), X, Y, Error});
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_TRUE(R.Changed);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      detail::WorkBudget Proof(65536);
      EXPECT_TRUE(detail::provePolynomialIdentity(C, Input, R.Expr, 5, Proof));
    }
  }
}

TEST(SymMBAQuadratic, RootRecoveryRequiresTheCompleteIndependentProof) {
  SymContext C;
  auto Input = parseSymExpr(C,
                            "((x^y)&z)*((y|z)&x)+((x^y)&~z)*((y|z)&x)"
                            "+((x^y)&z)*((y|z)&~x)+((x^y)&~z)*((y|z)&~x)",
                            32);
  ASSERT_TRUE(Input.ok());
  detail::WorkBudget Full(65536);
  detail::SolveReport Complete;
  ASSERT_NE(detail::solveExpandedProducts(C, Input.Root, {}, Full, Complete),
            Input.Root);
  EXPECT_EQ(Complete.Evidence, MBAEvidence::Derivation);
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1}) {
    detail::WorkBudget Budget(Work);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveExpandedProducts(C, Input.Root, {}, Budget, Report),
              Input.Root);
    EXPECT_NE(Report.Evidence, MBAEvidence::Derivation);
  }
  MBAOptions Tiny;
  Tiny.MaxTableBytes = 0;
  detail::WorkBudget Budget(65536);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveExpandedProducts(C, Input.Root, Tiny, Budget, Report),
            Input.Root);
}

TEST(SymMBAQuadratic, UnaffordableOptionalTablesLeaveWorkForEnclosingRegions) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto Input = parseSymExpr(C, "x+y+7*(x^y)*(y|z)", Width);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input.Root, Options);
    EXPECT_EQ(R.Expr, Input.Root);
    EXPECT_LT(R.Work, 8192u);
    detail::WorkBudget Budget(4096);
    llvm::SmallVector<SymRef, 3> Atoms{C.mkVar("a", Width), C.mkVar("b", Width),
                                       C.mkVar("c", Width)};
    llvm::SmallVector<llvm::APInt, 8> Weights;
    for (unsigned K = 0; K != 8; ++K)
      Weights.push_back(-llvm::APInt(Width, llvm::popcount(K)));
    llvm::SmallVector<SymRef, 8> Forms;
    detail::booleanResidualCandidates(
        C, Weights, Atoms, 64, detail::resolveLimits(Options), Budget, Forms);
    EXPECT_FALSE(Budget.exhausted());
    EXPECT_LT(Budget.used(), 4096u);
  }
}

TEST(SymMBAQuadratic, SmallSelectorsUseAffordableExactConstructions) {
  for (unsigned Width : {1u, 8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    llvm::SmallVector<SymRef, 3> Atoms{C.mkVar("a", Width), C.mkVar("b", Width),
                                       C.mkVar("c", Width)};
    BitwiseSynthesisLimits Limits;
    // A three-input ANF needs 24 units, below the complete recipe bound.
    Limits.MaxWork = 24;
    for (unsigned Bits = 0; Bits != 256; ++Bits) {
      SCOPED_TRACE(Bits);
      auto R = synthesizeBitwise(C, table(3, Bits), Atoms, Limits);
      ASSERT_TRUE(R);
      for (unsigned K = 0; K != 8; ++K) {
        llvm::SmallVector<llvm::APInt, 3> Values;
        for (unsigned I = 0; I != 3; ++I)
          Values.push_back(K & (1u << I) ? llvm::APInt::getAllOnes(Width)
                                         : llvm::APInt(Width, 0));
        EXPECT_EQ(C.eval(*R, Values), (Bits >> K) & 1
                                          ? llvm::APInt::getAllOnes(Width)
                                          : llvm::APInt(Width, 0));
      }
    }
    auto Relation = atomTruthTable(0, 3) & atomTruthTable(1, 3);
    Relation ^= atomTruthTable(2, 3);
    auto R = synthesizeBitwise(C, Relation, Atoms, Limits);
    ASSERT_TRUE(R);
    EXPECT_EQ(*R, C.mkXor(C.mkAnd(Atoms[0], Atoms[1]), Atoms[2]));
    for (size_t Work : {size_t(0), size_t(1)}) {
      Limits.MaxWork = Work;
      const auto Before = C.numNodes();
      EXPECT_FALSE(synthesizeBitwise(C, Relation, Atoms, Limits));
      EXPECT_EQ(C.numNodes(), Before);
    }
  }
}

TEST(SymMBAQuadratic, PreparedRecipesAreCompleteAndOptimal) {
  // Independent enumeration by increasing printed size, rather than the
  // generator's iterative relaxation and traversal order.
  std::array<unsigned, 256> Minimum;
  Minimum.fill(100);
  for (unsigned Bits : {0u, 255u, 170u, 204u, 240u})
    Minimum[Bits] = 1;
  for (unsigned Cost = 2; Cost <= 10; ++Cost) {
    for (unsigned A = 0; A != 256; ++A)
      if (Minimum[A] + 1 == Cost)
        Minimum[A ^ 255] = std::min(Minimum[A ^ 255], Cost);
    for (unsigned A = 0; A != 256; ++A)
      for (unsigned B = 0; B != 256; ++B)
        if (Minimum[A] + Minimum[B] + 1 == Cost) {
          Minimum[A & B] = std::min(Minimum[A & B], Cost);
          Minimum[A | B] = std::min(Minimum[A | B], Cost);
          Minimum[A ^ B] = std::min(Minimum[A ^ B], Cost);
        }
  }
  for (unsigned Width : {1u, 8u, 64u, 65u, 257u}) {
    SymContext C;
    llvm::SmallVector<SymRef, 3> Atoms{C.mkVar("a", Width), C.mkVar("b", Width),
                                       C.mkVar("c", Width)};
    BitwiseSynthesisLimits Limits;
    Limits.MaxWork = 49;
    for (unsigned Bits = 0; Bits != 256; ++Bits) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Bits);
      auto Result = synthesizeBitwise(C, table(3, Bits), Atoms, Limits);
      ASSERT_TRUE(Result);
      ASSERT_LE(Minimum[Bits], 10u);
      EXPECT_EQ(C.readabilityCost(*Result), Minimum[Bits]);
      for (unsigned Seed = 0; Seed != 32; ++Seed) {
        llvm::SmallVector<llvm::APInt, 3> Values;
        for (unsigned I = 0; I != 3; ++I) {
          llvm::APInt Value(Width, (Seed + 1) * (37 + 71 * I));
          if (Seed & (1u << I))
            Value.flipAllBits();
          Values.push_back(Value);
        }
        llvm::APInt Expected(Width, 0);
        for (unsigned K = 0; K != 8; ++K) {
          if (!(Bits & (1u << K)))
            continue;
          llvm::APInt Lane = llvm::APInt::getAllOnes(Width);
          for (unsigned I = 0; I != 3; ++I)
            Lane &= K & (1u << I) ? Values[I] : ~Values[I];
          Expected |= Lane;
        }
        ASSERT_EQ(C.eval(*Result, Values), Expected);
      }
    }
  }
}

} // namespace
