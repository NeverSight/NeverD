//===- BitBlastArithTests.cpp - Repeated-bit arithmetic circuits ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Check arithmetic against independent integer values and original circuits,
// including repeated carries, known-zero prefixes and full-word products.
//
//===----------------------------------------------------------------------===//

#include "BitBlastDetail.h"
#include "gtest/gtest.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

using namespace neverd::solver;
namespace circuit = neverd::solver::detail;

namespace {
void originalAdder(CnfEncoder &E, circuit::LitSpan A, circuit::LitSpan B,
                   SatLit CarryIn, circuit::LitVec &Out, SatLit *CarryOut) {
  Out.clear();
  auto Carry = CarryIn;
  for (size_t I = 0; I < A.size(); ++I) {
    if (I + 1 == A.size() && !CarryOut) {
      const SatLit Inputs[] = {A[I], B[I], Carry};
      Out.push_back(E.mkXor(Inputs));
      return;
    }
    SatLit Sum, Next;
    E.mkFullAdder(A[I], B[I], Carry, Sum, Next);
    Out.push_back(Sum);
    Carry = Next;
  }
  if (CarryOut)
    *CarryOut = Carry;
}

TEST(RepeatedAdderCarry, IntegerOracleIncludesRunBoundariesAndCarryOut) {
  for (unsigned Width :
       {1U, 2U, 3U, 4U, 8U, 16U, 31U, 32U, 33U, 63U, 64U, 65U, 127U, 128U}) {
    for (unsigned Pattern = 0; Pattern < 12; ++Pattern) {
      SCOPED_TRACE(::testing::Message() << Width << ":" << Pattern);
      SatSolver S;
      CnfEncoder E(S);
      std::array<SatLit, 4> V{E.freshLit(), E.freshLit(), E.freshLit(),
                              E.freshLit()};
      circuit::LitVec A, B, Sum, Modular;
      for (unsigned I = 0; I < Width; ++I) {
        auto X = V[0], Y = V[1];
        switch (Pattern) {
        case 0:
          break;
        case 1:
          if (I % 2)
            std::swap(X, Y);
          break;
        case 2:
          if (I % 2)
            X = V[2];
          break;
        case 3:
          if (I % 2)
            Y = V[2];
          break;
        case 4:
          if (I == Width / 2)
            X = ~X;
          break;
        case 5:
          if (I == Width / 2)
            Y = ~Y;
          break;
        case 6:
          X = E.falseLit();
          break;
        case 7:
          X = E.trueLit();
          break;
        case 8:
          if (I < 3) {
            X = V[I];
            Y = ~V[2 - I];
          }
          break;
        case 9:
          X = E.constant(I < 16 && ((0x1ad5U >> I) & 1));
          Y = I < 3 ? V[I] : V[2];
          break;
        case 10:
          Y = X;
          break;
        case 11:
          Y = ~X;
          break;
        }
        A.push_back(X);
        B.push_back(Y);
      }
      SatLit Carry;
      circuit::addBits(E, A, B, V[3], Sum, &Carry);
      circuit::addBits(E, A, B, V[3], Modular);
      ASSERT_EQ(Sum.size(), Width);
      ASSERT_EQ(Modular.size(), Width);
      ASSERT_TRUE(Carry.isValid());
      for (unsigned Assignment = 0; Assignment < 16; ++Assignment) {
        SCOPED_TRACE(Assignment);
        circuit::LitVec Assumptions;
        for (unsigned I = 0; I < V.size(); ++I)
          Assumptions.push_back(V[I].withPolarity((Assignment >> I) & 1));
        ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);
        llvm::APInt X(Width + 1, 0), Y(Width + 1, 0), Got(Width + 1, 0);
        for (unsigned I = 0; I < Width; ++I) {
          ASSERT_NE(S.modelValue(A[I]), SatValue::Unknown);
          ASSERT_NE(S.modelValue(B[I]), SatValue::Unknown);
          ASSERT_NE(S.modelValue(Sum[I]), SatValue::Unknown);
          ASSERT_NE(S.modelValue(Modular[I]), SatValue::Unknown);
          if (S.modelValue(A[I]) == SatValue::True)
            X.setBit(I);
          if (S.modelValue(B[I]) == SatValue::True)
            Y.setBit(I);
          if (S.modelValue(Sum[I]) == SatValue::True)
            Got.setBit(I);
          EXPECT_EQ(S.modelValue(Sum[I]), S.modelValue(Modular[I]));
        }
        ASSERT_NE(S.modelValue(Carry), SatValue::Unknown);
        if (S.modelValue(Carry) == SatValue::True)
          Got.setBit(Width);
        EXPECT_EQ(Got, X + Y + llvm::APInt(Width + 1, (Assignment >> 3) & 1));
      }
    }
  }
}

TEST(RepeatedAdderCarry, SymbolicMiterKeepsIndependentLowBitsAndBreaks) {
  for (unsigned Width : {8U, 32U, 64U, 128U}) {
    for (unsigned Split : {0U, 1U, 4U}) {
      SatSolver S;
      CnfEncoder E(S);
      auto X = E.freshLit(), Y = E.freshLit(), Z = E.freshLit(),
           In = E.freshLit();
      circuit::LitVec A, B, Actual, Expected, Differences;
      for (unsigned I = 0; I < Width; ++I) {
        auto L = I < Split ? E.freshLit() : X;
        auto R = I < Split ? E.freshLit() : Y;
        if (I == Width / 2)
          R = Z;
        if (I % 2)
          std::swap(L, R);
        A.push_back(L);
        B.push_back(R);
      }
      SatLit ActualCarry, ExpectedCarry;
      circuit::addBits(E, A, B, In, Actual, &ActualCarry);
      originalAdder(E, A, B, In, Expected, &ExpectedCarry);
      for (unsigned I = 0; I < Width; ++I)
        Differences.push_back(E.mkXor(Actual[I], Expected[I]));
      Differences.push_back(E.mkXor(ActualCarry, ExpectedCarry));
      E.assertTrue(E.mkOr(Differences));
      EXPECT_EQ(S.solve(), SatResult::Unsat) << Width << ":" << Split;
    }
  }
}

TEST(RepeatedAdderCarry, SignExtensionDoesNotGrowRepeatedHighCarryChain) {
  size_t ShortGates = 0;
  for (unsigned Width : {16U, 32U, 64U, 128U}) {
    SatSolver S, ReferenceSolver;
    CnfEncoder E(S), Reference(ReferenceSolver);
    circuit::LitVec A, B, OldA, OldB, Out, OldOut;
    std::array<SatLit, 8> X, OldX;
    for (unsigned I = 0; I < 8; ++I) {
      X[I] = E.freshLit();
      OldX[I] = Reference.freshLit();
    }
    for (unsigned I = 0; I < Width; ++I) {
      const bool Bit = I < 16 && ((0x1ad5U >> I) & 1);
      A.push_back(E.constant(Bit));
      OldA.push_back(Reference.constant(Bit));
      B.push_back(X[std::min(I, 7U)]);
      OldB.push_back(OldX[std::min(I, 7U)]);
    }
    circuit::addBits(E, A, B, E.falseLit(), Out);
    originalAdder(Reference, OldA, OldB, Reference.falseLit(), OldOut, nullptr);
    if (Width == 16)
      ShortGates = E.numGates();
    else
      EXPECT_LE(E.numGates(), ShortGates + 2);
    if (Width >= 64)
      EXPECT_LT(E.numGates() + 16, Reference.numGates());
  }
}

TEST(RepeatedAdderCarry, EmptyInputPreservesInputCarry) {
  SatSolver S;
  CnfEncoder E(S);
  circuit::LitVec Out{E.trueLit()};
  const auto In = E.freshLit();
  SatLit Carry;
  circuit::addBits(E, {}, {}, In, Out, &Carry);
  EXPECT_TRUE(Out.empty());
  EXPECT_EQ(Carry, In);
  circuit::addBits(E, {}, {}, In, Out);
  EXPECT_TRUE(Out.empty());
}

void originalMultiplier(CnfEncoder &E, circuit::LitSpan A, circuit::LitSpan B,
                        circuit::LitVec &Out) {
  Out.assign(A.size(), E.falseLit());
  circuit::LitVec Row, Sum;
  for (size_t J = 0; J < B.size(); ++J) {
    Row.assign(A.size(), E.falseLit());
    for (size_t I = J; I < A.size(); ++I)
      Row[I] = E.mkAnd(A[I - J], B[J]);
    originalAdder(E, Out, Row, E.falseLit(), Sum, nullptr);
    Out.swap(Sum);
  }
}

TEST(ZeroPrefixProduct, IndependentCircuitProvesEveryOutputBit) {
  for (unsigned Width : {8u, 32u, 64u, 129u})
    for (unsigned AZeros : {0u, 1u, Width / 2, Width - 1, Width})
      for (unsigned BZeros : {0u, 1u, Width / 2, Width - 1, Width}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << AZeros << ':' << BZeros);
        SatSolver S;
        CnfEncoder E(S);
        circuit::LitVec A, B, Actual, Expected, Difference;
        for (unsigned I = 0; I < Width; ++I) {
          A.push_back(I < AZeros ? E.falseLit() : E.freshLit());
          B.push_back(I < BZeros ? E.falseLit() : E.freshLit());
        }
        circuit::multiplyBits(E, A, B, Actual);
        originalMultiplier(E, A, B, Expected);
        ASSERT_EQ(Actual.size(), Width);
        ASSERT_EQ(Expected.size(), Width);
        for (unsigned I = 0; I < Width; ++I)
          Difference.push_back(E.mkXor(Actual[I], Expected[I]));
        E.assertTrue(E.mkOr(Difference));
        EXPECT_EQ(S.solve(), SatResult::Unsat);
      }
}

TEST(ZeroPrefixProduct, IntegerOracleCoversTruncatedAndWideProducts) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 65u, 129u, 257u})
    for (unsigned AZeros : {0u, Width / 2, Width - 1, Width})
      for (unsigned BZeros : {0u, Width / 2, Width - 1, Width}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << AZeros << ':' << BZeros);
        SatSolver S;
        CnfEncoder E(S);
        const std::array Inputs{E.freshLit(), E.freshLit(), E.freshLit(),
                                E.freshLit()};
        circuit::LitVec A, B, Product;
        for (unsigned I = 0; I < Width; ++I) {
          A.push_back(I < AZeros ? E.falseLit() : Inputs[I % 2]);
          const auto Bit = Inputs[2 + I % 2];
          B.push_back(I < BZeros ? E.falseLit() : (I % 3 ? Bit : ~Bit));
        }
        circuit::multiplyBits(E, A, B, Product);
        ASSERT_EQ(Product.size(), Width);
        for (unsigned Assignment = 0; Assignment < 16; ++Assignment) {
          circuit::LitVec Assumptions;
          for (unsigned I = 0; I < Inputs.size(); ++I)
            Assumptions.push_back(
                Inputs[I].withPolarity((Assignment >> I) & 1));
          ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);
          llvm::APInt X(Width, 0), Y(Width, 0), Actual(Width, 0);
          for (unsigned I = 0; I < Width; ++I) {
            ASSERT_NE(S.modelValue(A[I]), SatValue::Unknown);
            ASSERT_NE(S.modelValue(B[I]), SatValue::Unknown);
            ASSERT_NE(S.modelValue(Product[I]), SatValue::Unknown);
            if (S.modelValue(A[I]) == SatValue::True)
              X.setBit(I);
            if (S.modelValue(B[I]) == SatValue::True)
              Y.setBit(I);
            if (S.modelValue(Product[I]) == SatValue::True)
              Actual.setBit(I);
          }
          EXPECT_EQ(Actual, X * Y) << Assignment;
        }
      }
}
} // namespace
