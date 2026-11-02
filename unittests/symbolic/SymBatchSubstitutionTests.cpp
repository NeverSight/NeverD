//===- SymBatchSubstitutionTests.cpp - Shared substitution walks ----------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

using namespace neverd::symbolic;

namespace {

TEST(SymBatchSubstitution, SimultaneousValuesAndOrderedDuplicateRoots) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8), Z = C.mkVar("z", 8);
  auto Sum = C.mkAdd(X, Y);
  SymRef Roots[] = {Sum, X, Sum, Y};
  auto Results = C.substituteAll(Roots, {{X.index(), Y}, {Y.index(), Z}});
  ASSERT_EQ(Results.size(), 4u);
  EXPECT_EQ(Results[0], C.mkAdd(Y, Z));
  EXPECT_EQ(Results[1], Y);
  EXPECT_EQ(Results[2], Results[0]);
  EXPECT_EQ(Results[3], Z);
}

TEST(SymBatchSubstitution, EmptyInputsAndUnchangedRoots) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  EXPECT_TRUE(C.substituteAll({}, {{X.index(), Y}}).empty());
  auto Root = C.mkUDiv(X, Y);
  SymRef Roots[] = {Root, X, Root};
  EXPECT_EQ(C.substituteAll(Roots, {}), std::vector<SymRef>({Root, X, Root}));
  auto Other = C.mkVar("other", 8);
  EXPECT_EQ(C.substituteAll(Roots, {{Other.index(), Y}}),
            std::vector<SymRef>({Root, X, Root}));
}

TEST(SymBatchSubstitution,
     RebuildsSharedOpaqueChildrenWithAnIndependentOracle) {
  SymContext C;
  auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4), Z = C.mkVar("z", 4);
  auto Shared = C.mkUDiv(X, Y);
  auto Sum = C.mkAdd(Shared, Y);
  auto Mix = C.mkXor(Shared, Z);
  auto Results = C.substituteAll({Sum, Mix}, {{X.index(), Z}});
  for (unsigned A = 0; A != 16; ++A)
    for (unsigned B = 0; B != 16; ++B) {
      uint64_t Inputs[] = {0, A, B};
      unsigned Quotient = A ? B / A : 15;
      EXPECT_EQ(C.evalU64(Results[0], Inputs), (Quotient + A) & 15);
      EXPECT_EQ(C.evalU64(Results[1], Inputs), Quotient ^ B);
    }
}

TEST(SymBatchSubstitution, RootsMayBorrowOperandStorageWhileBuildersGrowIt) {
  SymContext C;
  auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  std::vector<SymRef> Inputs;
  for (unsigned I = 0; I != 512; ++I) {
    auto Denominator = C.mkVar("d" + std::to_string(I), 32);
    Inputs.push_back(C.mkUDiv(X, Denominator));
  }
  auto Root = C.mkAdd(Inputs);
  auto Borrowed = C.operands(Root);
  std::vector<SymRef> Snapshot(Borrowed.begin(), Borrowed.end());
  auto Results = C.substituteAll(Borrowed, {{X.index(), Y}});
  ASSERT_EQ(Results.size(), Snapshot.size());
  for (unsigned I = 0; I != Results.size(); ++I)
    EXPECT_EQ(Results[I], C.mkUDiv(Y, C.operand(Snapshot[I], 1)));
}

TEST(SymBatchSubstitution, DeepSharedGraphsDoNotUseTheHostStack) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Deep = X;
  for (unsigned I = 0; I != 20000; ++I)
    Deep = C.mkUDiv(Deep, Y);
  auto Results = C.substituteAll({Deep, Deep}, {{X.index(), C.mkConst(8, 7)}});
  ASSERT_EQ(Results.size(), 2u);
  EXPECT_EQ(Results[0], Results[1]);
  for (unsigned I = 0; I != 20000; ++I) {
    ASSERT_EQ(C.op(Results[0]), SymOp::UDiv);
    Results[0] = C.operand(Results[0], 0);
  }
  EXPECT_EQ(Results[0], C.mkConst(8, 7));
}

TEST(SymBatchSubstitution, SharedWideGraphsKeepSimultaneousSubgraphOverrides) {
  for (unsigned Width : {8U, 64U, 65U, 257U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Shared = C.mkUDiv(X, Y);
    auto Overridden = C.mkAdd(X, Z);
    std::vector<SymRef> Roots;
    for (unsigned I = 0; I != 96; ++I)
      Roots.push_back(
          C.mkXor(C.mkAdd(Shared, C.mkConst(Width, I + 1)),
                  C.mkMul(Overridden, C.mkConst(Width, 2 * I + 1))));
    Roots.push_back(Roots.front());
    Roots.push_back(Shared);
    Roots.push_back(X);
    Roots.push_back(Y);
    // Overrides apply to the old graph simultaneously. In particular the X
    // supplied for Overridden must not be chased through the X -> Y entry.
    auto Results = C.substituteAll(
        Roots, {{X.index(), Y}, {Y.index(), Z}, {Overridden.index(), X}});
    ASSERT_EQ(Results.size(), Roots.size());
    EXPECT_EQ(Results[96], Results.front());
    EXPECT_EQ(Results[97], C.mkUDiv(Y, Z));
    EXPECT_EQ(Results[98], Y);
    EXPECT_EQ(Results[99], Z);
    for (unsigned Sample = 0; Sample != 20; ++Sample) {
      SCOPED_TRACE(Sample);
      llvm::APInt A(Width, 13 * Sample + 1);
      llvm::APInt B = llvm::APInt::getOneBitSet(Width, Width - 1) |
                      llvm::APInt(Width, 7 * Sample + 3);
      llvm::APInt D(Width, Sample);
      llvm::APInt Inputs[] = {A, B, D};
      auto Quotient = D.isZero() ? llvm::APInt::getAllOnes(Width) : B.udiv(D);
      for (unsigned I = 0; I != 96; ++I) {
        auto Expected = (Quotient + llvm::APInt(Width, I + 1)) ^
                        (A * llvm::APInt(Width, 2 * I + 1));
        EXPECT_EQ(C.eval(Results[I], Inputs), Expected) << I;
      }
    }
  }
}

} // namespace
