//===- SymExprCancellationTests.cpp - Exact shared-word identities -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Checks exact modular cancellation and bounded bitwise absorption.

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/STLExtras.h"

#include <array>
#include <random>
#include <string>
#include <vector>

using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {
llvm::APInt word(std::mt19937_64 &Random, unsigned Width) {
  llvm::APInt Value(Width, 0);
  for (unsigned Bit = 0; Bit < Width; Bit += 64)
    Value |= llvm::APInt(Width, Random()).shl(Bit);
  return Value;
}
} // namespace

TEST(SymExprCancellation, SharedSumsCancelOpaqueValuesAtEveryWidth) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Common = C.mkMul(X, Y);
    auto A = C.mkAdd({Common, Y, C.mkConst(Width, 9)});
    auto B = C.mkAdd({Common, Z, C.mkConst(Width, 21)});
    auto Difference = C.mkSub(A, B);
    llvm::SmallVector<uint32_t, 4> Variables;
    C.collectVars(Difference, Variables);
    EXPECT_FALSE(llvm::is_contained(Variables, C.varId(X)));
    EXPECT_EQ(C.mkSub(A, A), C.mkZero(Width));
    auto Unequal = C.mkSub(C.mkAdd(C.mkMul(C.mkConst(Width, 3), X), Y),
                           C.mkAdd(C.mkMul(C.mkConst(Width, 2), X), Z));
    std::mt19937_64 Random(19);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 3> V{word(Random, Width), word(Random, Width),
                                   word(Random, Width)};
      const auto Product = V[0] * V[1];
      const auto Want = (Product + V[1] + llvm::APInt(Width, 9)) -
                        (Product + V[2] + llvm::APInt(Width, 21));
      EXPECT_EQ(C.eval(Difference, V), Want);
      EXPECT_EQ(C.eval(Unequal, V), V[0] + V[1] - V[2]);
    }
  }
}

TEST(SymExprCancellation, CoveredXorTermsCannotAddBitsToTheirCover) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Cover = C.mkAdd(X, C.mkConst(Width, 5));
    auto One = C.mkOr(Cover, C.mkXor(Cover, C.mkAnd(Cover, Y)));
    auto Many = C.mkOr(C.mkXor(C.mkAnd(Cover, Y), C.mkAnd(Z, Cover)), Cover);
    EXPECT_EQ(One, Cover);
    EXPECT_EQ(Many, Cover);
    auto Uncovered = C.mkOr(Cover, C.mkXor(C.mkAnd(Cover, Y), Z));
    std::mt19937_64 Random(23);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 3> V{word(Random, Width), word(Random, Width),
                                   word(Random, Width)};
      const auto Base = V[0] + llvm::APInt(Width, 5);
      EXPECT_EQ(C.eval(One, V), Base | (Base ^ (Base & V[1])));
      EXPECT_EQ(C.eval(Many, V), Base | ((Base & V[1]) ^ (Base & V[2])));
      EXPECT_EQ(C.eval(Uncovered, V), Base | ((Base & V[1]) ^ V[2]));
    }
  }
}

TEST(SymExprCancellation, ExhaustiveNarrowMixedOperandsRemainExact) {
  SymContext C;
  auto X = C.mkVar("x", 3), Y = C.mkVar("y", 3), Z = C.mkVar("z", 3);
  auto A = C.mkAdd(X, Y), B = C.mkAdd(X, Z);
  const std::array<SymRef, 4> Exprs{C.mkSub(A, B), C.mkSub(X, B),
                                    C.mkOr(X, C.mkXor(X, C.mkAnd(X, Y))),
                                    C.mkOr({X, C.mkXor(C.mkAnd(X, Y), Z), Y})};
  for (uint64_t AVal = 0; AVal != 8; ++AVal)
    for (uint64_t BVal = 0; BVal != 8; ++BVal)
      for (uint64_t CVal = 0; CVal != 8; ++CVal) {
        const std::array<uint64_t, 3> Values{AVal, BVal, CVal};
        const std::array<uint64_t, 4> Want{
            (AVal + BVal - (AVal + CVal)) & 7, (AVal - (AVal + CVal)) & 7,
            AVal | (AVal ^ (AVal & BVal)),
            AVal | ((AVal & BVal) ^ CVal) | BVal};
        for (unsigned I = 0; I != Exprs.size(); ++I)
          EXPECT_EQ(C.evalU64(Exprs[I], Values), Want[I]);
      }
}

TEST(SymExprCancellation, AdjacentIndexedBytesSeparateWithinSmallSolverBudget) {
  SymContext C;
  auto Root = C.mkVar("root", 64), Index = C.mkVar("index", 16);
  auto Current = C.mkAdd(Root, C.mkConst(64, uint64_t{0} - 80));
  auto Address = C.mkAdd(
      C.mkOr(Current, C.mkXor(Current, C.mkAnd(Current, C.mkZExt(Index, 64)))),
      C.mkZExt(Index, 64));
  auto Cell = C.mkAdd(Root, C.mkConst(64, uint64_t{0} - 88));
  auto Overlap = C.mkOr(C.mkUlt(C.mkSub(Address, Cell), C.mkConst(64, 8)),
                        C.mkUlt(C.mkSub(Cell, Address), C.mkConst(64, 1)));
  SolverOptions O;
  O.Blast.MaxGates = 512;
  O.Sat.MaxConflicts = 1000;
  O.Sat.MaxPropagations = 20000;
  O.Sat.MaxWatchVisits = 200000;
  BitVectorSolver S(C, O);
  ASSERT_TRUE(S.assertTrue(Overlap));
  EXPECT_EQ(S.check(), SatResult::Unsat);
  for (uint64_t R : {uint64_t{0}, uint64_t{7}, uint64_t{88},
                     uint64_t{0x80000000}, UINT64_MAX - 7, UINT64_MAX})
    for (uint64_t I : {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{8},
                       uint64_t{32768}, uint64_t{65535}}) {
      bool Want = false;
      for (unsigned B = 0; B != 8; ++B)
        Want |= R - 80 + I == R - 88 + B;
      EXPECT_EQ(C.evalU64(Overlap, std::array<uint64_t, 2>{R, I}), Want);
    }
  auto Aliasing = C.mkUlt(C.mkSub(C.mkAdd(Cell, C.mkZExt(Index, 64)), Cell),
                          C.mkConst(64, 8));
  BitVectorSolver Counterexample(C, O);
  ASSERT_TRUE(Counterexample.assertTrue(Aliasing));
  ASSERT_EQ(Counterexample.check(), SatResult::Sat);
  EXPECT_FALSE(
      C.eval(Aliasing, Counterexample.model().asVarValues(C)).isZero());
}

TEST(SymExprCancellation, ExhaustedSubsetWorkRetainsUncoveredBits) {
  SymContext C;
  llvm::SmallVector<SymRef, 32> Covers;
  for (unsigned I = 0; I != 300; ++I)
    Covers.push_back(C.mkVar("cover" + std::to_string(I), 64));
  auto Uncovered = C.mkVar("uncovered", 64);
  auto Term = C.mkXor(C.mkAnd(Covers[0], Covers[1]), Uncovered);
  Covers.push_back(Term);
  auto Result = C.mkOr(Covers);
  std::vector<uint64_t> Values(301, 0);
  Values.back() = 0x8000000000000001ULL;
  EXPECT_EQ(C.evalU64(Result, Values), Values.back());
}

TEST(SymExprCancellation, SharedOrMasksDifferOnlyAtChangedBits) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Common = C.mkOr({X, Y, C.mkMul(Z, C.mkConst(Width, 5))});
    auto LeftMask = llvm::APInt(Width, 2);
    auto RightMask = llvm::APInt(Width, 3);
    if (Width > 3)
      RightMask.setBit(Width - 1);
    auto Left = C.mkOr(Common, C.mkConst(LeftMask));
    auto Right = C.mkOr(Common, C.mkConst(RightMask));
    auto Difference = C.mkXor(Left, Right);
    auto ChangedBits =
        C.mkAnd(C.mkNot(Common), C.mkConst(LeftMask ^ RightMask));
    if (!C.isConst(Left) && !C.isConst(Right))
      EXPECT_EQ(Difference, ChangedBits) << C.toString(Difference);
    std::mt19937_64 Random(37);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 3> Inputs{
          word(Random, Width), word(Random, Width), word(Random, Width)};
      const auto Base =
          Inputs[0] | Inputs[1] | (Inputs[2] * llvm::APInt(Width, 5));
      EXPECT_EQ(C.eval(Difference, Inputs),
                (Base | LeftMask) ^ (Base | RightMask));
    }
  }
}

TEST(SymExprCancellation, ConstantMasksAbsorbCoveredBitwiseTerms) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 257u}) {
    SymContext C;
    llvm::SmallVector<SymRef, 16> Fields;
    for (unsigned I = 0; I != 12; ++I)
      Fields.push_back(C.mkVar("field" + std::to_string(I), Width));
    auto Common = C.mkOr(Fields);
    auto A = C.mkOr(Common, C.mkConst(Width, 2));
    auto B = C.mkOr(Common, C.mkConst(Width, 3));
    EXPECT_EQ(C.mkOr(C.mkConst(Width, 1), C.mkXor(A, B)), C.mkConst(Width, 1));
    EXPECT_EQ(C.mkOr(C.mkConst(Width, 3), C.mkAnd(Common, C.mkConst(Width, 1))),
              C.mkConst(Width, 3));
    EXPECT_EQ(C.mkAnd(C.mkConst(Width, 1), C.mkOr(Common, C.mkConst(Width, 3))),
              C.mkConst(Width, 1));
  }
}

TEST(SymExprCancellation, MaskVariantsMatchEveryThreeBitMaskAndInput) {
  SymContext C;
  auto X = C.mkVar("x", 3), Y = C.mkVar("y", 3);
  auto Common = C.mkOr(X, Y);
  for (uint64_t A = 0; A != 8; ++A)
    for (uint64_t B = 0; B != 8; ++B) {
      auto Difference =
          C.mkXor({C.mkOr(Common, C.mkConst(3, A)),
                   C.mkOr(Common, C.mkConst(3, B)), C.mkConst(3, 4)});
      auto CoveredOr = C.mkOr(C.mkConst(3, A), C.mkAnd(X, C.mkConst(3, B)));
      auto CoveredAnd = C.mkAnd(C.mkConst(3, A), C.mkOr(X, C.mkConst(3, B)));
      for (uint64_t U = 0; U != 8; ++U)
        for (uint64_t V = 0; V != 8; ++V) {
          std::array<uint64_t, 2> Inputs{U, V};
          EXPECT_EQ(C.evalU64(Difference, Inputs),
                    ((U | V | A) ^ (U | V | B)) ^ 4);
          EXPECT_EQ(C.evalU64(CoveredOr, Inputs), A | (U & B));
          EXPECT_EQ(C.evalU64(CoveredAnd, Inputs), A & (U | B));
        }
    }
}

TEST(SymExprCancellation, ConstantComplementMasksKeepTheCapturedWord) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Common = C.mkOr(X, C.mkMul(Y, C.mkConst(Width, 5)));
    std::mt19937_64 Random(41);
    for (unsigned I = 0; I != 64; ++I) {
      const auto Mask = word(Random, Width);
      const auto Constant = C.mkConst(Mask);
      auto And = C.mkNot(C.mkOr(Constant, C.mkNot(Common)));
      auto Or = C.mkNot(C.mkAnd(Constant, C.mkNot(Common)));
      EXPECT_EQ(And, C.mkAnd(C.mkConst(~Mask), Common));
      EXPECT_EQ(Or, C.mkOr(C.mkConst(~Mask), Common));
      std::array<llvm::APInt, 2> Inputs{word(Random, Width),
                                        word(Random, Width)};
      const auto Base = Inputs[0] | (Inputs[1] * llvm::APInt(Width, 5));
      EXPECT_EQ(C.eval(And, Inputs), ~(Mask | ~Base));
      EXPECT_EQ(C.eval(Or, Inputs), ~(Mask & ~Base));
    }
  }
}

TEST(SymExprCancellation, RemovingCapturedSetBitsDoesNotBorrow) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Base = C.mkAdd(X, C.mkConst(Width, 5));
    auto Mask = C.mkAnd(Y, Z);
    auto R = C.mkSub(Base, C.mkAnd(Base, Mask));
    EXPECT_EQ(R, C.mkAnd(Base, C.mkNot(Mask)));
    auto Neighbor = C.mkSub(Base, C.mkAnd(C.mkAdd(Base, C.mkOne(Width)), Mask));
    std::mt19937_64 Random(43);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 3> Inputs{
          word(Random, Width), word(Random, Width), word(Random, Width)};
      const auto Value = Inputs[0] + llvm::APInt(Width, 5);
      const auto Bits = Inputs[1] & Inputs[2];
      EXPECT_EQ(C.eval(R, Inputs), Value - (Value & Bits));
      EXPECT_EQ(C.eval(Neighbor, Inputs),
                Value - ((Value + llvm::APInt(Width, 1)) & Bits));
    }
  }
}

TEST(SymExprCancellation, XorOfCapturedMaskedWordReadsOnlyTheOtherBits) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
    auto Base = C.mkOr(X, C.mkMul(Y, C.mkConst(Width, 5)));
    auto Mask = C.mkAnd(Z, W);
    auto R = C.mkXor(Base, C.mkAnd(Base, Mask));
    auto Extra = C.mkXor({Base, C.mkAnd(Base, Mask), C.mkConst(Width, 3)});
    auto Neighbor = C.mkXor(Base, C.mkAnd(C.mkAdd(Base, C.mkOne(Width)), Mask));
    EXPECT_EQ(R, C.mkAnd(Base, C.mkNot(Mask)));
    std::mt19937_64 Random(47);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 4> Inputs{
          word(Random, Width), word(Random, Width), word(Random, Width),
          word(Random, Width)};
      const auto Value = Inputs[0] | (Inputs[1] * llvm::APInt(Width, 5));
      const auto Bits = Inputs[2] & Inputs[3];
      EXPECT_EQ(C.eval(R, Inputs), Value ^ (Value & Bits));
      EXPECT_EQ(C.eval(Extra, Inputs),
                Value ^ (Value & Bits) ^ llvm::APInt(Width, 3));
      EXPECT_EQ(C.eval(Neighbor, Inputs),
                Value ^ ((Value + llvm::APInt(Width, 1)) & Bits));
    }
  }
}

TEST(SymExprCancellation, ForcedOrBitsCancelAcrossACarryFreeMaskedSum) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width);
    auto Forced = llvm::APInt(Width, 2);
    auto Observed = llvm::APInt::getOneBitSet(Width, Width - 1);
    auto Keep = ~(Forced | Observed);
    auto Packed = C.mkOr(X, C.mkConst(Forced));
    auto Sum = C.mkAdd(C.mkConst(Forced), C.mkAnd(X, C.mkConst(Keep)));
    auto Difference = C.mkXor(Packed, Sum);
    EXPECT_EQ(Difference, C.mkAnd(X, C.mkConst(Observed)))
        << C.toString(Difference);
    auto Extra = C.mkXor({Packed, Sum, C.mkConst(Width, 1)});
    std::mt19937_64 Random(53);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 1> Values{word(Random, Width)};
      const auto Want = (Values[0] | Forced) ^ (Forced + (Values[0] & Keep));
      EXPECT_EQ(C.eval(Difference, Values), Want);
      EXPECT_EQ(C.eval(Extra, Values), Want ^ llvm::APInt(Width, 1));
    }
  }
}

TEST(SymExprCancellation, MaskedConstantSumsKeepOverlappingCarryBits) {
  SymContext C;
  auto X = C.mkVar("x", 3), Y = C.mkVar("y", 3);
  for (uint64_t K = 0; K != 8; ++K)
    for (uint64_t Mask = 0; Mask != 8; ++Mask) {
      auto Sum = C.mkAdd(C.mkConst(3, K), C.mkAnd(X, C.mkConst(3, Mask)));
      auto Difference = C.mkXor(C.mkOr(X, C.mkConst(3, K)), Sum);
      auto Pair =
          C.mkXor(C.mkOr(X, C.mkConst(3, K)), C.mkOr(Y, C.mkConst(3, K)));
      for (uint64_t A = 0; A != 8; ++A)
        for (uint64_t B = 0; B != 8; ++B) {
          std::array<uint64_t, 2> Values{A, B};
          EXPECT_EQ(C.evalU64(Sum, Values), (K + (A & Mask)) & 7);
          EXPECT_EQ(C.evalU64(Difference, Values),
                    ((K | A) ^ (K + (A & Mask))) & 7);
          EXPECT_EQ(C.evalU64(Pair, Values), (K | A) ^ (K | B));
        }
    }
}

TEST(SymExprCancellation, UnsignedZeroBoundsShareTheirExactZeroPredicate) {
  std::mt19937_64 Random(0x170b0d);
  for (const unsigned Width : {1u, 3u, 8u, 16u, 32u, 64u, 128u, 257u}) {
    SymContext C;
    const auto X = C.mkVar("x", Width), Zero = C.mkZero(Width);
    const auto Above = C.mkUlt(Zero, X), BelowOrEqual = C.mkUle(X, Zero);
    EXPECT_EQ(Above, C.mkNe(X, Zero));
    EXPECT_EQ(BelowOrEqual, C.mkEq(X, Zero));
    std::vector<llvm::APInt> Inputs{llvm::APInt(Width, 0),
                                    llvm::APInt(Width, 1),
                                    llvm::APInt::getAllOnes(Width)};
    for (unsigned I = 0; I < 64; ++I)
      Inputs.push_back(word(Random, Width));
    for (const auto &V : Inputs) {
      EXPECT_EQ(C.eval(Above, {V}).getBoolValue(), !V.isZero());
      EXPECT_EQ(C.eval(BelowOrEqual, {V}).getBoolValue(), V.isZero());
    }
  }
}
