#include "gtest/gtest.h"

#include "neverd/ir/SourceVariableIdentity.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighVariableEdits.h"
using namespace neverd;
namespace {
TypeRef word() { return NdType::makeInt(8, false); }
MedVar local(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  return V;
}
ExprPtr value(int Id) { return HighExpr::makeVar(local(Id), word()); }
HighStmt assign(int Id, ExprPtr E) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = value(Id);
  S.Val = E;
  return S;
}
HighStmt use(ExprPtr E) {
  HighStmt S;
  S.Kind = StmtKind::Store;
  S.StoreAddr = HighExpr::makeConst(0x5000, 8);
  S.StoreVal = E;
  return S;
}
HighFunc function(std::vector<HighStmt> Body) {
  HighFunc F;
  F.Entry = 0x1000;
  F.Body = std::move(Body);
  unsigned I = 0;
  for (auto &S : F.Body)
    S.Addr = 0x1000 + I++;
  return F;
}
ExprPtr constant(unsigned I) { return HighExpr::makeConst(I, 8); }
std::string id(int I) { return highSourceVariableId(local(I)); }
HighSourceMap statementSources(const HighFunc &F) {
  HighSourceMap Sources;
  for (const auto &S : F.Body)
    Sources.push_back({F.Entry,
                       {S.Addr, 0},
                       {},
                       S.Kind == StmtKind::Store ? HighSourceKind::Store
                                                 : HighSourceKind::Statement,
                       S.Kind});
  return Sources;
}
TEST(HighVariableEdits, MapsCopiesOnAPrivateGraphAndKeepsSourceObservations) {
  auto F =
      function({assign(1, constant(7)), assign(2, value(1)), use(value(2))});
  const auto Original = F.Body[1].Val;
  HighSourceObservation O;
  O.Function = F.Entry;
  O.Expression = Original;
  auto R = editHighVariable(F, {O}, {HighVariableEdit::Map, id(1), id(2)});
  ASSERT_TRUE(R) << R.Refusal;
  EXPECT_EQ(highSourceVariableId(F.Body[0].Dst->Var), id(1));
  EXPECT_EQ(F.Body[1].Val, Original);
  EXPECT_EQ(highSourceVariableId(R.Function.Body[0].Dst->Var), id(2));
  ASSERT_EQ(R.Sources.size(), 1u);
  EXPECT_NE(R.Sources[0].Expression.lock(), Original);
}
TEST(HighVariableEdits, SplitsOnlyIndependentUsedLifetimes) {
  auto F = function({assign(1, constant(7)), use(value(1)),
                     assign(1, constant(9)), use(value(1))});
  auto R = editHighVariable(F, {}, {HighVariableEdit::Split, id(1), {}});
  ASSERT_TRUE(R) << R.Refusal;
  ASSERT_EQ(R.Outputs.size(), 2u);
  EXPECT_NE(highSourceVariableId(R.Function.Body[0].Dst->Var),
            highSourceVariableId(R.Function.Body[2].Dst->Var));
  EXPECT_EQ(highSourceVariableId(R.Function.Body[0].Dst->Var),
            highSourceVariableId(R.Function.Body[1].StoreVal->Var));
  EXPECT_EQ(highSourceVariableId(R.Function.Body[2].Dst->Var),
            highSourceVariableId(R.Function.Body[3].StoreVal->Var));
  auto Overlap = function({assign(1, constant(7)), assign(2, value(1)),
                           assign(1, constant(9)), use(value(2))});
  EXPECT_FALSE(
      editHighVariable(Overlap, {}, {HighVariableEdit::Map, id(1), id(2)}));
}
TEST(HighVariableEdits, PreservesStatementAnchorsAcrossLocalAssociations) {
  const auto F = function({assign(1, constant(7)), use(value(1)),
                           assign(1, constant(9)), use(value(1))});
  auto Sources = statementSources(F);
  auto Foreign = Sources.front();
  Foreign.Function = 0x2000;
  Sources.push_back(Foreign);
  const auto R =
      editHighVariable(F, Sources, {HighVariableEdit::Split, id(1), {}});
  ASSERT_TRUE(R) << R.Refusal;
  ASSERT_EQ(R.Sources.size(), F.Body.size());
  for (size_t I = 0; I < R.Sources.size(); ++I) {
    EXPECT_EQ(R.Sources[I].Function, F.Entry);
    EXPECT_EQ(R.Sources[I].Occurrence, Sources[I].Occurrence);
    EXPECT_EQ(R.Sources[I].Kind, Sources[I].Kind);
    EXPECT_EQ(R.Sources[I].StatementKind, Sources[I].StatementKind);
  }
}

TEST(HighVariableEdits, PairedStoreDestinationsFollowPrivateGraphCopies) {
  auto F = function({assign(1, constant(7)), assign(2, value(1)), use(value(2)),
                     use(value(2))});
  F.Body[3].Addr = F.Body[2].Addr;
  F.Body[3].StoreAddr = HighExpr::makeConst(0x5008, 8);
  const HighSourceMap Sources = {
      observeHighStatement(F.Entry, {F.Body[2].Addr, 3}, F.Body[2]),
      observeHighStatement(F.Entry, {F.Body[3].Addr, 4}, F.Body[3])};
  const auto R =
      editHighVariable(F, Sources, {HighVariableEdit::Map, id(1), id(2)});
  ASSERT_TRUE(R) << R.Refusal;
  const HighStatementSourceIndex Index(R.Sources);
  for (size_t I : {2u, 3u}) {
    ASSERT_TRUE(Index.lookup(F.Entry, R.Function.Body[I]));
    EXPECT_EQ(Index.lookup(F.Entry, R.Function.Body[I])->Sequence, I + 1);
    EXPECT_NE(R.Function.Body[I].StoreAddr, F.Body[I].StoreAddr);
  }
}
ExprPtr address(int Offset) {
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  SP.Size = 8;
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::BinOp;
  E->Op = NdOp::INT_SUB;
  E->Type = word();
  E->Operands = {HighExpr::makeVar(SP, word()), constant(Offset)};
  return E;
}
ExprPtr load(int Offset) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Load;
  E->Type = word();
  E->Operands = {address(Offset)};
  return E;
}
HighStmt store(int Offset, ExprPtr E) {
  auto S = use(E);
  S.StoreAddr = address(Offset);
  return S;
}
TEST(HighVariableEdits, SplitsExactUnescapedNegativeStackSlots) {
  auto F = function({store(8, constant(1)), use(load(8)), store(8, constant(2)),
                     use(load(8))});
  F.FrameSize = 64;
  auto R = editHighVariable(F, {}, {HighVariableEdit::Split, "stack:-8", {}});
  ASSERT_TRUE(R) << R.Refusal;
  EXPECT_EQ(R.Function.Body[0].Kind, StmtKind::Assign);
  EXPECT_EQ(F.Body[0].Kind, StmtKind::Store);
  EXPECT_NE(highSourceVariableId(R.Function.Body[0].Dst->Var),
            highSourceVariableId(R.Function.Body[2].Dst->Var));
  F.Body.insert(F.Body.begin(), use(address(8)));
  auto Escaped =
      editHighVariable(F, {}, {HighVariableEdit::Split, "stack:-8", {}});
  EXPECT_FALSE(Escaped);
  EXPECT_NE(Escaped.Refusal.find("escaping"), std::string::npos);
}
TEST(HighVariableEdits, RefusesOverlappingStorageAndExceptionEntries) {
  auto F = function({store(8, constant(1)), use(load(8)), store(8, constant(2)),
                     use(load(8)), store(12, constant(3))});
  F.FrameSize = 64;
  auto R = editHighVariable(F, {}, {HighVariableEdit::Split, "stack:-8", {}});
  EXPECT_FALSE(R);
  EXPECT_NE(R.Refusal.find("overlap"), std::string::npos);
  F.StructuredExceptionRegions = 1;
  EXPECT_FALSE(
      editHighVariable(F, {}, {HighVariableEdit::Split, "stack:-8", {}}));
}
TEST(HighVariableEdits, DropsOnlyStoreAnchorsChangedByScalarization) {
  auto F = function({store(8, constant(1)), use(load(8)), store(8, constant(2)),
                     use(load(8))});
  F.FrameSize = 64;
  const auto Sources = statementSources(F);
  const auto R =
      editHighVariable(F, Sources, {HighVariableEdit::Split, "stack:-8", {}});
  ASSERT_TRUE(R) << R.Refusal;
  ASSERT_EQ(R.Sources.size(), 2u);
  EXPECT_EQ(R.Sources[0].Occurrence, Sources[1].Occurrence);
  EXPECT_EQ(R.Sources[1].Occurrence, Sources[3].Occurrence);
  for (const auto &Source : R.Sources) {
    EXPECT_EQ(Source.Kind, HighSourceKind::Store);
    EXPECT_EQ(Source.StatementKind, StmtKind::Store);
  }
}
} // namespace
