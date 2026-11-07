//===- HighExprUseCountsTests.cpp - Shared expression reads --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check bounded use counts against independently expanded expression trees.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/high/HighExprUseCounts.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <vector>

namespace {
using namespace neverd;

ExprPtr variable(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  return HighExpr::makeVar(V);
}

void expandedReads(const ExprPtr &E, VarKeyMap<int> &Reads) {
  if (!E)
    return;
  if (E->Kind == ExprKind::Var)
    ++Reads[varKey(E->Var)];
  for (const auto &Child : E->Operands)
    expandedReads(Child, Reads);
  expandedReads(E->IndirectTarget, Reads);
}

TEST(HighExprUseCounts, LeavesRetainDistinctAndRepeatedVariableReads) {
  HighExprUseVisits Visits;
  VarKeyMap<int> Uses;
  const auto X = variable(1);
  const auto Y = variable(2);
  std::vector<ExprPtr> Roots = {X, Y};
  for (unsigned I = 0; I < 64; ++I) {
    Roots.push_back(HighExpr::makeConst(I, 8));
    Roots.push_back(HighExpr::makeUndef(8));
    Roots.push_back(nullptr);
    Roots.push_back(X);
    Roots.push_back(variable(1));
  }
  for (const auto &Root : Roots)
    countExprVarUsesUpToTwo(Root, Uses, Visits);
  ASSERT_EQ(Uses.size(), 2u);
  EXPECT_EQ(Uses.at(varKey(X->Var)), 2);
  EXPECT_EQ(Uses.at(varKey(Y->Var)), 1);
}

TEST(HighExprUseCounts, EmptyArgumentsKeepTheIndirectTargetRead) {
  HighExprUseVisits Visits;
  VarKeyMap<int> Uses;
  const auto Target = variable(1);
  auto Call = HighExpr::makeCall("", 0, {});
  Call->IsIndirectCall = true;
  Call->IndirectTarget = Target;
  countExprVarUsesUpToTwo(Call, Uses, Visits);
  ASSERT_EQ(Uses.size(), 1u);
  EXPECT_EQ(Uses.at(varKey(Target->Var)), 1);
  countExprVarUsesUpToTwo(Call, Uses, Visits);
  countExprVarUsesUpToTwo(Call, Uses, Visits);
  EXPECT_EQ(Uses.at(varKey(Target->Var)), 2);
}

TEST(HighExprUseCounts, SharedGraphsMatchExpandedTreeReads) {
  for (unsigned Seed = 0; Seed < 64; ++Seed) {
    SCOPED_TRACE(Seed);
    std::vector<ExprPtr> Nodes = {variable(0), variable(1), variable(2),
                                  HighExpr::makeConst(7, 8)};
    for (unsigned I = 0; I < 12; ++I) {
      auto Left = Nodes[(Seed + I * 5) % Nodes.size()];
      auto Right = Nodes[(Seed * 3 + I) % Nodes.size()];
      Nodes.push_back(HighExpr::makeBinop(NdOp::INT_ADD, Left, Right));
    }
    HighExprUseVisits Visits;
    VarKeyMap<int> Uses;
    VarKeyMap<int> Expected;
    for (const auto &Root :
         {Nodes.back(), Nodes[Seed % Nodes.size()], Nodes.back()}) {
      expandedReads(Root, Expected);
      countExprVarUsesUpToTwo(Root, Uses, Visits);
    }
    for (auto &[Key, Count] : Expected)
      Count = std::min(2, Count);
    EXPECT_EQ(Uses, Expected);
  }
}

} // namespace
