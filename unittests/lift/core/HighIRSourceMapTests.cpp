//===- HighIRSourceMapTests.cpp - HighIR navigation ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Surviving identity, multi-output relationships and structured row ownership.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighIRPrint.h"
#include "neverd/ir/high/HighSourceMap.h"

using namespace neverd;

TEST(HighIRSources, SameInstructionStatementsHaveIndependentSurvivingIdentity) {
  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Addr = 0x1010;
  Call.Val = HighExpr::makeCall("query", 0, {});
  HighStmt Transport = Call;
  Transport.Val = HighExpr::makeConst(7, 4);
  const HighSourceMap Sources = {
      observeHighStatement(0x1000, {Call.Addr, 3}, Call),
      observeHighStatement(0x1000, {Call.Addr, 4}, Transport)};
  HighStatementSourceIndex Index(Sources);
  ASSERT_TRUE(Index.lookup(0x1000, Call));
  EXPECT_EQ(Index.lookup(0x1000, Call)->Sequence, 3);
  EXPECT_EQ(Index.lookup(0x1000, Transport)->Sequence, 4);
  Call.Kind = StmtKind::Call;
  Call.CallExpr = Call.Val;
  Call.Val.reset();
  EXPECT_EQ(Index.lookup(0x1000, Call)->Sequence, 3);
  EXPECT_FALSE(Index.lookup(0x2000, Call));
  Call.IsPhiCopy = true;
  EXPECT_FALSE(Index.lookup(0x1000, Call));
}

TEST(HighIRSources, ReplacedExpiredChangedKindAndAmbiguousExpressionsNeverMap) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = 0x1020;
  S.Val = HighExpr::makeCall("query", 0, {});
  auto Source = observeHighStatement(0x1000, {S.Addr, 1}, S);
  HighStatementSourceIndex Index({Source});
  const auto Original = S.Val;
  S.Val = HighExpr::makeCall("query", 0, {});
  EXPECT_FALSE(Index.lookup(0x1000, S));
  S.Val = Original;
  S.Val->Kind = ExprKind::Const;
  EXPECT_FALSE(Index.lookup(0x1000, S));
  S.Val->Kind = ExprKind::Call;
  auto Other = Source;
  Other.Occurrence.Sequence = 2;
  EXPECT_FALSE(HighStatementSourceIndex({Source, Other}).lookup(0x1000, S));
  S.Val.reset();
  EXPECT_FALSE(Index.lookup(0x1000, S));
  auto Expired = Source;
  {
    S.Val = HighExpr::makeCall("other_query", 0, {});
    Expired = observeHighStatement(0x1000, {S.Addr, 3}, S);
    S.Val.reset();
  }
  ASSERT_TRUE(Expired.Expression.expired());
  S.Val = HighExpr::makeCall("other_query", 0, {});
  EXPECT_FALSE(HighStatementSourceIndex({Expired}).lookup(0x1000, S));
}

TEST(HighIRSources, CpuidPrintsAllOutputBindingsAndComparesTheirIdentity) {
  auto Call = HighExpr::makeCall(
      "__cpuid", 0, {HighExpr::makeConst(7, 4), HighExpr::makeConst(1, 4)});
  Call->IntrinsicId = Intrinsic::Cpuid;
  for (int I = 0; I < 4; ++I) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = 10 + I;
    V.Size = 4;
    Call->IntrinsicOutputs.push_back(V);
  }
  const auto Text = Call->str();
  EXPECT_NE(Text.find(" -> ("), std::string::npos);
  for (const auto &V : Call->IntrinsicOutputs)
    EXPECT_NE(Text.find(V.display()), std::string::npos);
  HighExpr Other = *Call;
  EXPECT_TRUE(Call->structuralEq(Other));
  Other.IntrinsicOutputs[2].Id += 10;
  EXPECT_FALSE(Call->structuralEq(Other));
}

TEST(HighIRSources, StructuredRowsKeepChildrenAndExcludeBracesAndLabels) {
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeConst(1, 1);
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall("query", 0, {});
  Branch.Body = {Call};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(9, 4);
  Branch.ElseBody = {Return};
  std::vector<const HighStmt *> Owners;
  std::string Text;
  printHighStatementRows(Branch, 0,
                         [&](llvm::StringRef Row, const HighStmt *Owner) {
                           Text += Row.str();
                           Owners.push_back(Owner);
                         });
  EXPECT_EQ(Text, "if (1) {\n  query();\n} else {\n  return 9;\n}\n");
  ASSERT_EQ(Owners.size(), 5u);
  EXPECT_EQ(Owners[0], &Branch);
  EXPECT_EQ(Owners[1], &Branch.Body[0]);
  EXPECT_EQ(Owners[2], nullptr);
  EXPECT_EQ(Owners[3], &Branch.ElseBody[0]);
  EXPECT_EQ(Owners[4], nullptr);
}
