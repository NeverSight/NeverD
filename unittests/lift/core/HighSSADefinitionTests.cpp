#include "gtest/gtest.h"

#include "neverd/ir/high/HighIR.h"

namespace neverd {
void elimConsecutiveDeadStores(std::vector<HighStmt> &Statements);
}
using namespace neverd;
namespace {
ExprPtr variable(int Version, uint16_t Size = 8, int Rename = -1) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = 8;
  V.SSAVer = Version;
  V.RegOff = 64;
  V.Size = Size;
  V.RenameTag = Rename;
  V.TheArch = Arch::AArch64;
  return HighExpr::makeVar(V, NdType::makeInt(Size, false));
}
HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}
HighStmt returning(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

TEST(HighSSADefinitions, SamePhysicalRegisterDoesNotOverwriteEarlierSSAValue) {
  auto A = variable(5), B = variable(8);
  auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, A, B);
  std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(93, 8)),
                             assign(B, HighExpr::makeConst(93, 8)),
                             returning(Sum)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Dst->Var.SSAVer, 5);
  EXPECT_EQ(Body[1].Dst->Var.SSAVer, 8);
}

TEST(HighSSADefinitions, WideningKeepsBothDistinctValuesAndTheConversion) {
  for (NdOp Extension : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
    auto A = variable(1, 4), B = variable(2, 8);
    auto Bits = HighExpr::makeConst(0x80000001, 4);
    auto Wide = HighExpr::makeUnary(Extension, Bits);
    Wide->Type = NdType::makeInt(8, Extension == NdOp::INT_SEXT);
    std::vector<HighStmt> Body{assign(A, Bits), assign(B, Wide), returning(B)};
    elimConsecutiveDeadStores(Body);
    ASSERT_EQ(Body.size(), 3U);
    EXPECT_EQ(Body[1].Val->Op, Extension);
    EXPECT_EQ(Body[2].RetVal->Var.SSAVer, 2);
    EXPECT_EQ(Body[2].RetVal->Type->Size, 8);
  }
}

TEST(HighSSADefinitions, RenamedLocalsAndWidthsDoNotBorrowAnOverwrite) {
  for (bool DifferentWidth : {false, true}) {
    auto A = variable(3, 4, 7);
    auto B = variable(3, DifferentWidth ? 8 : 4, DifferentWidth ? 7 : 8);
    std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(7, 4)),
                               assign(B, HighExpr::makeConst(9, B->Type->Size)),
                               returning(A)};
    elimConsecutiveDeadStores(Body);
    EXPECT_EQ(Body.size(), 3U);
  }
}

TEST(HighSSADefinitions, RealOverwriteKeepsCallEffectAndDependentRead) {
  auto A = variable(3);
  std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(7, 8)),
                             assign(A, HighExpr::makeConst(9, 8)),
                             returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 2U);
  EXPECT_EQ(Body[0].Val->ConstVal, 9U);
  Body = {assign(A, HighExpr::makeCall("effect", 0x2000, {})),
          assign(A, HighExpr::makeConst(9, 8)), returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Kind, StmtKind::Call);
  auto Call = HighExpr::makeCall("nested_effect", 0x3000, {});
  Call->Type = NdType::makeInt(8);
  Body = {assign(A, HighExpr::makeBinop(NdOp::INT_ADD, Call,
                                        HighExpr::makeConst(1, 8))),
          assign(A, HighExpr::makeConst(9, 8)), returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Kind, StmtKind::ExprStmt);
  EXPECT_EQ(Body[0].Val->Operands[0]->Kind, ExprKind::Call);
  Body = {assign(A, HighExpr::makeConst(7, 8)),
          assign(A, HighExpr::makeBinop(NdOp::INT_ADD, A,
                                        HighExpr::makeConst(9, 8))),
          returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
}
} // namespace
