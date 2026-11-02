//===- SymMBAQuadraticRowTests.cpp - Lazy polynomial coefficient rows -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent coefficient oracles for lazy rows and shared-power recovery.
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace neverd::symbolic;

namespace {

SymRef fromTable(SymContext &C, llvm::ArrayRef<SymRef> Inputs, uint16_t Bits) {
  llvm::SmallVector<SymRef, 16> Terms;
  for (unsigned M = 0; M != 16; ++M) {
    if (!(Bits & (1u << M)))
      continue;
    llvm::SmallVector<SymRef, 4> Factors;
    for (unsigned I = 0; I != 4; ++I)
      Factors.push_back(M & (1u << I) ? Inputs[I] : C.mkNot(Inputs[I]));
    Terms.push_back(C.mkAnd(Factors));
  }
  return Terms.empty() ? C.mkConst(C.width(Inputs[0]), 0) : C.mkOr(Terms);
}

void checkProduct(const detail::QuadraticProduct &Product, uint16_t A,
                  uint16_t B, const llvm::APInt &Scale) {
  for (unsigned I = 0; I != 16; ++I)
    for (unsigned J = I; J != 16; ++J) {
      unsigned Want = unsigned((A >> I & 1) && (B >> J & 1));
      unsigned Got =
          unsigned(Product.Factors[0].at(I) && Product.Factors[1].at(J));
      if (I != J) {
        Want += unsigned((A >> J & 1) && (B >> I & 1));
        Got += unsigned(Product.Factors[0].at(J) && Product.Factors[1].at(I));
      }
      EXPECT_EQ(Scale * llvm::APInt(Scale.getBitWidth(), Want, false, true),
                Product.Coefficient *
                    llvm::APInt(Scale.getBitWidth(), Got, false, true));
    }
}

TEST(SymMBAQuadraticRows, RecoversSplitFactorsWithWideAndNoninvertibleScales) {
  std::mt19937 Random(0x726f7773);
  for (unsigned Width : {1u, 3u, 8u, 64u, 257u})
    for (unsigned Sample = 0; Sample != 32; ++Sample) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Sample);
      SymContext C;
      llvm::SmallVector<SymRef, 4> Inputs;
      llvm::SmallVector<uint32_t, 4> Ids;
      for (const char *Name : {"x", "y", "z", "w"}) {
        auto V = C.mkVar(Name, Width);
        Inputs.push_back(V);
        C.collectVars(V, Ids);
      }
      const uint16_t A = Random(), B = Random();
      auto Scale = Sample % 2 ? llvm::APInt::getSignMask(Width)
                              : llvm::APInt(Width, 3, false, true);
      llvm::SmallVector<detail::PolyTerm, 8> Terms;
      for (uint16_t L : {uint16_t(0xaaaa), uint16_t(0x5555)})
        for (uint16_t R : {uint16_t(0xcccc), uint16_t(0x3333)})
          Terms.push_back(
              {Scale,
               {fromTable(C, Inputs, A & L), fromTable(C, Inputs, B & R)}});
      detail::WorkBudget Budget(65536);
      bool Determined = false;
      auto Product = detail::recoverQuadraticTerms(C, Terms, Ids, Width, Budget,
                                                   1 << 20, Determined);
      ASSERT_TRUE(Product);
      EXPECT_TRUE(Determined);
      EXPECT_LT(Budget.used(), 65536u);
      checkProduct(*Product, A, B, Scale);
    }
}

TEST(SymMBAQuadraticRows, CanceledMintermsDoNotBecomeFactorInputs) {
  SymContext C;
  std::array<SymRef, 4> Inputs{C.mkVar("x", 64), C.mkVar("y", 64),
                               C.mkVar("z", 64), C.mkVar("w", 64)};
  llvm::SmallVector<uint32_t, 4> Ids;
  for (auto V : Inputs)
    C.collectVars(V, Ids);
  auto A = fromTable(C, Inputs, 0x0033);
  auto B = fromTable(C, Inputs, 0xcc00);
  auto Extra = fromTable(C, Inputs, 0xffff);
  llvm::SmallVector<detail::PolyTerm, 8> Terms{
      {llvm::APInt(64, 6), {A, B}},
      {llvm::APInt(64, 17), {Extra, Extra}},
      {-llvm::APInt(64, 17), {Extra, Extra}}};
  detail::WorkBudget Budget(65536);
  bool Determined = false;
  auto Product = detail::recoverQuadraticTerms(C, Terms, Ids, 64, Budget,
                                               1 << 20, Determined);
  ASSERT_TRUE(Product);
  EXPECT_TRUE(Determined);
  checkProduct(*Product, 0x0033, 0xcc00, llvm::APInt(64, 6));
}

TEST(SymMBAQuadraticRows, SingleProductKeepsItsFactorsWithoutMatrixExpansion) {
  for (unsigned Width : {1u, 8u, 9u, 63u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    llvm::SmallVector<SymRef, 8> Inputs;
    llvm::SmallVector<uint32_t, 8> Ids;
    for (const char *Name : {"a", "b", "c", "d", "e", "f", "g", "h"}) {
      Inputs.push_back(C.mkVar(Name, Width));
      C.collectVars(Inputs.back(), Ids);
    }
    // The declared truth-table order need not match the evaluator's variable
    // order. Include widths that leave an incomplete final packed word.
    std::rotate(Ids.begin(), Ids.begin() + 3, Ids.end());
    auto Assignment = [](unsigned M) { return ((M & 31) << 3) | (M >> 5); };
    auto A = C.mkOr(llvm::ArrayRef<SymRef>(Inputs).take_front(4));
    auto B = C.mkAnd(llvm::ArrayRef<SymRef>(Inputs).drop_front(4));
    auto Scale = llvm::APInt::getSignMask(Width);
    llvm::SmallVector<detail::PolyTerm, 1> Terms{{Scale, {A, B}}};
    detail::WorkBudget Full(2048);
    bool Determined = false;
    auto Product = detail::recoverQuadraticTerms(C, Terms, Ids, Width, Full,
                                                 1 << 20, Determined);
    ASSERT_TRUE(Product);
    ASSERT_TRUE(Determined);
    for (unsigned I = 0; I != 256; ++I)
      for (unsigned J = I; J != 256; ++J) {
        unsigned Want =
            unsigned((Assignment(I) & 15) && (Assignment(J) >> 4) == 15);
        unsigned Got =
            unsigned(Product->Factors[0].at(I) && Product->Factors[1].at(J));
        if (I != J) {
          Want += unsigned((Assignment(J) & 15) && (Assignment(I) >> 4) == 15);
          Got +=
              unsigned(Product->Factors[0].at(J) && Product->Factors[1].at(I));
        }
        ASSERT_EQ(Scale * llvm::APInt(Width, Want, false, true),
                  Product->Coefficient * llvm::APInt(Width, Got, false, true));
      }
    for (size_t Work : {size_t(0), Full.used() - 1, Full.used()}) {
      detail::WorkBudget Budget(Work);
      auto Result = detail::recoverQuadraticTerms(C, Terms, Ids, Width, Budget,
                                                  1 << 20, Determined);
      EXPECT_EQ(bool(Result), Work == Full.used());
      EXPECT_EQ(Determined, bool(Result));
      EXPECT_LE(Budget.used(), Work);
    }
    detail::WorkBudget Refusal(2048);
    EXPECT_FALSE(detail::recoverQuadraticTerms(C, Terms, Ids, Width, Refusal,
                                               1024, Determined));
    EXPECT_FALSE(Determined);
  }
}

TEST(SymMBAQuadraticRows,
     FactorCongruenceProvesWordsAndRejectsCornerIllusions) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    std::array<SymRef, 4> V{C.mkVar("x", Width), C.mkVar("y", Width),
                            C.mkVar("z", Width), C.mkVar("w", Width)};
    uint16_t ABits = 0, BBits = 0;
    for (unsigned M = 0; M != 16; ++M) {
      bool X = M & 1, Y = M & 2, Z = M & 4, W = M & 8;
      ABits |= uint16_t(((X || Y) != (Z && W)) << M);
      BBits |= uint16_t(((X && Z) || (Y && W)) << M);
    }
    auto A = C.mkXor(C.mkOr(V[0], V[1]), C.mkAnd(V[2], V[3]));
    auto B = C.mkOr(C.mkAnd(V[0], V[2]), C.mkAnd(V[1], V[3]));
    auto Scale = C.mkConst(Width, 6);
    auto Before =
        C.mkMul({Scale, fromTable(C, V, ABits), fromTable(C, V, BBits)});
    auto After = C.mkMul({Scale, A, B});
    ASSERT_NE(Before, After);
    detail::WorkBudget Proof(4096);
    ASSERT_TRUE(
        detail::provePolynomialIdentity(C, Before, After, 8, Proof, 1 << 20));
    for (size_t Work : {size_t(0), Proof.used() - 1, Proof.used()}) {
      detail::WorkBudget Boundary(Work);
      EXPECT_EQ(detail::provePolynomialIdentity(C, Before, After, 8, Boundary,
                                                1 << 20),
                Work == Proof.used());
      EXPECT_LE(Boundary.used(), Work);
    }
    // Keep the work allowance at the congruence proof's exact requirement;
    // the general coefficient proof must not mask a refused fast proof.
    detail::WorkBudget NoStorage(Proof.used());
    EXPECT_FALSE(
        detail::provePolynomialIdentity(C, Before, After, 8, NoStorage, 0));
    auto Changed = C.mkXor(B, fromTable(C, V, 1u << 2));
    detail::WorkBudget Bad(65536);
    EXPECT_FALSE(detail::provePolynomialIdentity(
        C, Before, C.mkMul({Scale, A, Changed}), 8, Bad, 1 << 20));
    // x*x+x vanishes at all-zero/all-ones corners but not on arbitrary words.
    // The abstraction must retain this arithmetic word as an independent atom.
    auto Illusion = C.mkMul(C.mkAdd(C.mkMul(V[0], V[0]), V[0]), B);
    detail::WorkBudget Nonlinear(65536);
    EXPECT_FALSE(detail::provePolynomialIdentity(C, Illusion, C.mkZero(Width),
                                                 8, Nonlinear, 1 << 20));
  }
}

TEST(SymMBAQuadraticRows, ChecksBeyondAnchorRowsAndHonorsRefusal) {
  SymContext C;
  std::array<SymRef, 4> Inputs{C.mkVar("x", 257), C.mkVar("y", 257),
                               C.mkVar("z", 257), C.mkVar("w", 257)};
  llvm::SmallVector<uint32_t, 4> Ids;
  for (auto V : Inputs)
    C.collectVars(V, Ids);
  const auto Scale = llvm::APInt(257, 6);
  llvm::SmallVector<detail::PolyTerm, 8> Terms{
      {Scale, {fromTable(C, Inputs, 0x0033), fromTable(C, Inputs, 0xcc00)}}};
  detail::WorkBudget Full(65536);
  bool Determined = false;
  ASSERT_TRUE(detail::recoverQuadraticTerms(C, Terms, Ids, 257, Full, 1 << 20,
                                            Determined));
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
    detail::WorkBudget Budget(Work);
    auto Product = detail::recoverQuadraticTerms(C, Terms, Ids, 257, Budget,
                                                 1 << 20, Determined);
    EXPECT_EQ(bool(Product), Work == Full.used());
    EXPECT_EQ(Determined, bool(Product));
    EXPECT_LE(Budget.used(), Work);
  }
  for (size_t Bytes : {0u, 64u, 2048u}) {
    detail::WorkBudget Budget(65536);
    EXPECT_FALSE(detail::recoverQuadraticTerms(C, Terms, Ids, 257, Budget,
                                               Bytes, Determined));
    EXPECT_FALSE(Determined);
  }
  // The first nonzero edge is (0,10). Change only (4,14), leaving both
  // proposal rows intact; the complete matrix must reject this perturbation.
  Terms.push_back(
      {llvm::APInt(257, 1),
       {fromTable(C, Inputs, 1u << 4), fromTable(C, Inputs, 1u << 14)}});
  detail::WorkBudget Bad(65536);
  EXPECT_FALSE(detail::recoverQuadraticTerms(C, Terms, Ids, 257, Bad, 1 << 20,
                                             Determined));
  EXPECT_TRUE(Determined);
}

TEST(SymMBAQuadraticRows, ShortKnownFactorsRetainTheImprovedLinearTail) {
  std::mt19937_64 Random(0x666163746f7273);
  for (unsigned Width : {8u, 9u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    llvm::SmallVector<SymRef, 8> Inputs;
    llvm::SmallVector<uint32_t, 8> Ids;
    for (const char *Name : {"a", "b", "c", "d", "e", "f", "g", "h"}) {
      Inputs.push_back(C.mkVar(Name, Width));
      C.collectVars(Inputs.back(), Ids);
    }
    auto A =
        C.mkAnd(C.mkOr(Inputs[0], Inputs[1]), C.mkOr(Inputs[2], Inputs[3]));
    auto B =
        C.mkAnd(C.mkOr(Inputs[4], Inputs[5]), C.mkOr(Inputs[6], Inputs[7]));
    auto Product = C.mkMul({C.mkConst(Width, 6), A, B});
    auto Source =
        C.mkAdd({Product, C.mkXor(Inputs[0], Inputs[1]),
                 C.mkMul(C.mkConst(Width, 2), C.mkAnd(Inputs[0], Inputs[1]))});
    auto Expected = C.mkAdd({Product, Inputs[0], Inputs[1]});
    MBAOptions Options;
    detail::WorkBudget Budget(4096);
    auto Result =
        detail::solvePolynomial(C, Source, Ids, Inputs, Options, Budget);
    ASSERT_TRUE(Result);
    EXPECT_LE(detail::readingCost(C, *Result),
              detail::readingCost(C, Expected));
    EXPECT_LT(detail::readingCost(C, *Result), detail::readingCost(C, Source));
    detail::WorkBudget Proof(65536);
    EXPECT_TRUE(detail::provePolynomialIdentity(C, Source, *Result, 8, Proof,
                                                Options.MaxTableBytes));
    for (unsigned Sample = 0; Sample != 384; ++Sample) {
      llvm::SmallVector<llvm::APInt, 8> Values;
      for (unsigned I = 0; I != 8; ++I) {
        std::array<uint64_t, 5> Words;
        for (auto &Word : Words)
          Word = Random();
        Values.push_back(Sample < 256 ? (Sample & (1u << I)
                                             ? llvm::APInt::getAllOnes(Width)
                                             : llvm::APInt(Width, 0))
                                      : llvm::APInt(Width, Words));
      }
      auto Left = (Values[0] | Values[1]) & (Values[2] | Values[3]);
      auto Right = (Values[4] | Values[5]) & (Values[6] | Values[7]);
      auto Want = llvm::APInt(Width, 6) * Left * Right + Values[0] + Values[1];
      EXPECT_EQ(C.eval(*Result, Values), Want);
    }
  }
}

TEST(SymMBAQuadraticRows, SparseLinearTailKeepsItsOwnSynthesisAllowance) {
  std::mt19937_64 Random(0x6c696e737570706f);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (bool ConstantOnly : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << ConstantOnly);
      SymContext C;
      C.mkVar("unused", 1);
      llvm::SmallVector<SymRef, 16> Inputs, Factors;
      llvm::SmallVector<uint32_t, 16> Ids;
      for (unsigned I = 0; I != 12; ++I) {
        Inputs.push_back(C.mkVar("v" + std::to_string(I), Width));
        Ids.push_back(C.varId(Inputs.back()));
        if (I != 1 && I != 10)
          Factors.push_back(Inputs.back());
      }
      const auto Product = C.mkMul(Factors);
      const auto Offset = C.mkConst(Width, 13);
      const auto X = Inputs[1], Y = Inputs[10];
      const auto Tail =
          ConstantOnly ? Offset
                       : C.mkAdd({Offset, C.mkXor(X, Y),
                                  C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y))});
      const auto Source = C.mkAdd(Product, Tail);
      const auto Expected = ConstantOnly ? C.mkAdd(Product, Offset)
                                         : C.mkAdd({Product, Offset, X, Y});
      // Noncontiguous ids and reversed coordinates must still bind the tail
      // correctly. Its two variables need four corners, not the product's
      // 4096: that larger table cannot fit this synthesis allowance.
      std::reverse(Ids.begin(), Ids.end());
      std::reverse(Inputs.begin(), Inputs.end());
      MBAOptions Options;
      Options.MaxWork = 512;
      detail::WorkBudget Budget(Options.MaxWork);
      auto Result =
          detail::solvePolynomial(C, Source, Ids, Inputs, Options, Budget);
      ASSERT_TRUE(Result);
      EXPECT_LE(C.readability(*Result), C.readability(Expected));
      EXPECT_LT(Budget.used(), Options.MaxWork);
      detail::WorkBudget Proof(65536);
      EXPECT_TRUE(detail::provePolynomialIdentity(C, Source, *Result, 12, Proof,
                                                  Options.MaxTableBytes));
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::vector<llvm::APInt> Values(C.numVars(), llvm::APInt(Width, 0));
        for (auto Input : Inputs) {
          std::array<uint64_t, 5> Words;
          for (auto &Word : Words)
            Word = Sample == 0 ? 0 : Sample == 1 ? UINT64_MAX : Random();
          Values[C.varId(Input)] = llvm::APInt(Width, Words);
        }
        llvm::APInt Want(Width, 1);
        for (auto Factor : Factors)
          Want *= Values[C.varId(Factor)];
        Want += 13;
        if (!ConstantOnly)
          Want += Values[C.varId(X)] + Values[C.varId(Y)];
        EXPECT_EQ(C.eval(*Result, Values), Want);
      }
    }
}

TEST(SymMBAQuadraticRows, CommonPowersLeaveOnlyTheLowDegreeProof) {
  for (unsigned Width : {3u, 8u, 64u, 257u}) {
    SymContext C;
    auto A = C.mkVar("a", Width), X = C.mkVar("x", Width);
    auto Y = C.mkVar("y", Width);
    auto Common = C.mkMul({A, A, A, A, A, A, A, A});
    auto Source = C.mkAdd(C.mkMul(Common, C.mkAnd(X, Y)),
                          C.mkMul(Common, C.mkAnd(X, C.mkNot(Y))));
    auto Expected = C.mkMul(Common, X);
    llvm::SmallVector<uint32_t, 4> Ids;
    C.collectVars(Source, Ids);
    llvm::SmallVector<SymRef, 4> Inputs;
    for (auto Id : Ids)
      Inputs.push_back(C.varRef(Id));
    MBAOptions Options;
    detail::WorkBudget Budget(4096);
    auto Result =
        detail::solvePolynomial(C, Source, Ids, Inputs, Options, Budget);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Expected);
    detail::WorkBudget Proof(4096);
    EXPECT_TRUE(detail::provePolynomialIdentity(C, Source, Expected, 8, Proof,
                                                Options.MaxTableBytes));
    EXPECT_LT(Proof.used(), 4096u);
    detail::WorkBudget BadProof(4096);
    EXPECT_FALSE(detail::provePolynomialIdentity(
        C, Source, C.mkAdd(Expected, C.mkConst(Width, 1)), 8, BadProof,
        Options.MaxTableBytes));
  }
}

TEST(SymMBAQuadraticRows, RepeatedNormalizedMonomialsKeepModularCoefficients) {
  for (unsigned Width : {3u, 8u, 64u, 257u})
    for (unsigned Count : {2u, 7u, 16u, 33u}) {
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto TwoX = C.mkMul(C.mkConst(Width, 2), X);
      auto Base =
          C.mkAdd(C.mkMul(C.mkAdd(TwoX, Y), C.mkSub(TwoX, Y)), C.mkMul(Y, Y));
      llvm::SmallVector<SymRef, 64> Factors(Count, Base);
      auto Source = C.mkMul(Factors);
      llvm::APInt Coefficient(Width, 1);
      for (unsigned I = 0; I != Count; ++I)
        Coefficient *= 4;
      Factors.assign(2 * Count, X);
      Factors.push_back(C.mkConst(Coefficient));
      auto Expected = C.mkMul(Factors);
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget Budget(65536);
      detail::SolveReport Report;
      EXPECT_EQ(detail::solveArithmetic(C, Source, Options, Budget, Report),
                Expected);
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    }
}

} // namespace
