//===- SymExprNestedTagEqualityTests.cpp - Exact constructor regressions
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bitvector evaluations and constructor refusal controls.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace neverd::symbolic;

TEST(SymExpr, LiteralSelectionComparisonsRecoverTheSelector) {
  for (unsigned Width : {8u, 32u, 64u, 128u, 256u}) {
    SymContext C;
    const auto Selector = C.mkVar("selector", 8);
    const uint64_t Tags[] = {0x81, 0xa7, 0xd9, 0xe3, 0xf5, 0xfe};
    auto Choice = C.mkConst(Width, Tags[5]);
    for (unsigned I = 5; I-- > 0;)
      Choice = C.mkIte(C.mkEq(Selector, C.mkConst(8, I)),
                       C.mkConst(Width, Tags[I]), Choice);
    const auto First = C.mkEq(Selector, C.mkZero(8));
    EXPECT_EQ(C.mkEq(Choice, C.mkConst(Width, Tags[0])), First);
    EXPECT_EQ(C.mkEq(C.mkConst(Width, Tags[0]), Choice), First);
    for (unsigned Target = 0; Target != 7; ++Target) {
      const uint64_t Tag = Target == 6 ? 0x42 : Tags[Target];
      const auto Equal = C.mkEq(Choice, C.mkConst(Width, Tag));
      for (unsigned Value = 0; Value != 256; ++Value)
        EXPECT_EQ(C.eval(Equal, {llvm::APInt(8, Value)}).isOne(),
                  Tags[Value < 5 ? Value : 5] == Tag);
    }
  }
}

TEST(SymExpr, SelectedLiteralEqualityRetainsBranchOrderAndUnknownArms) {
  SymContext C;
  auto A = C.mkVar("a", 1), B = C.mkVar("b", 1);
  auto X = C.mkVar("x", 8);
  const auto Seven = C.mkConst(8, 7), Nine = C.mkConst(8, 9);
  const auto Inner = C.mkIte(B, X, Seven);
  const auto Choices = C.mkIte(A, Nine, Inner);
  const auto Equal = C.mkEq(Choices, Seven);
  for (unsigned Av = 0; Av != 2; ++Av)
    for (unsigned Bv = 0; Bv != 2; ++Bv)
      for (unsigned Xv = 0; Xv != 256; ++Xv)
        EXPECT_EQ(C.eval(Equal, {llvm::APInt(1, Av), llvm::APInt(1, Bv),
                                 llvm::APInt(8, Xv)})
                      .isOne(),
                  (Av   ? 9u
                   : Bv ? Xv
                        : 7u) == 7u);
  // The same tag in an earlier arm cannot make a later arm unconditional.
  const auto Repeated = C.mkIte(A, Seven, C.mkIte(B, Nine, Seven));
  const auto Matches = C.mkEq(Repeated, Seven);
  EXPECT_EQ(Matches, C.mkOr(A, C.mkNot(B)));
}

TEST(SymExpr, LiteralSelectionEqualityBoundsItsInspectionAndKeepsTheTail) {
  using namespace neverd::limits::symbolic;
  SymContext C;
  const auto Condition = C.mkVar("condition", 1);
  auto Choice = C.mkConst(16, 0x1234);
  for (unsigned I = 0; I != 4096; ++I)
    Choice = C.mkIte(Condition, C.mkConst(16, I), Choice);
  const auto Target = C.mkConst(16, 4095);
  const auto Before = C.numNodes();
  const auto Equal = C.mkEq(Choice, Target);
  EXPECT_LE(C.numNodes() - Before, 4 * kSymExprSelectionEqualityMaxSteps + 2);
  EXPECT_FALSE(C.eval(Equal, {llvm::APInt(1, 0)}).isOne());
  EXPECT_TRUE(C.eval(Equal, {llvm::APInt(1, 1)}).isOne());

  const auto Other = C.mkVar("other", 1);
  const auto Branch = C.mkIte(Other, C.mkConst(16, 7), C.mkConst(16, 9));
  const auto Fork = C.mkIte(Condition, Choice, Branch);
  const auto ForkBefore = C.numNodes();
  const auto Kept = C.mkEq(Fork, Target);
  EXPECT_EQ(C.numNodes() - ForkBefore, 1u);
  for (unsigned A = 0; A != 2; ++A)
    for (unsigned B = 0; B != 2; ++B)
      EXPECT_EQ(C.eval(Kept, {llvm::APInt(1, A), llvm::APInt(1, B)}).isOne(),
                bool(A));
}
