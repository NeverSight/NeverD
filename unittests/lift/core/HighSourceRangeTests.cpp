#include "gtest/gtest.h"

#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>

using namespace neverd;
namespace {
ExprPtr input(unsigned Id = 0, uint16_t Bytes = 4) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Bytes;
  return HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
}
HighStmt result(ExprPtr Value, va_t Address = 0) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  S.Addr = Address;
  return S;
}
HighStmt branch(ExprPtr Test, std::vector<HighStmt> Yes,
                std::vector<HighStmt> No = {}) {
  HighStmt S;
  S.Kind = No.empty() ? StmtKind::If : StmtKind::IfElse;
  S.Cond = std::move(Test);
  S.Body = std::move(Yes);
  S.ElseBody = std::move(No);
  return S;
}
ExprPtr compare(NdOp Op, ExprPtr Left, ExprPtr Right) {
  auto E = HighExpr::makeBinop(Op, std::move(Left), std::move(Right));
  E->Type = NdType::makeInt(1, false);
  return E;
}
HighStmt write(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}

TEST(HighSourceRanges, UnsignedGuardBoundsAgreeWithEveryByteInput) {
  for (auto Op : {NdOp::INT_LESS, NdOp::INT_LESSEQUAL})
    for (bool ConstantFirst : {false, true})
      for (uint64_t Limit : {0, 1, 17, 255}) {
        auto Index = input(0, 1);
        auto Constant = HighExpr::makeConst(Limit, 1);
        auto Test = compare(Op, ConstantFirst ? Constant : Index,
                            ConstantFirst ? Index : Constant);
        HighFunc F;
        F.Body = {branch(Test, {result(Index)}, {result(Index)})};
        const auto Bounds = highSourceUnsignedUpperBounds(
            F, {{&F.Body[0].Body[0], Index}, {&F.Body[0].ElseBody[0], Index}});
        std::optional<uint64_t> Oracle[2];
        for (uint64_t Value = 0; Value < 256; ++Value) {
          auto Left = ConstantFirst ? Limit : Value;
          auto Right = ConstantFirst ? Value : Limit;
          const bool Taken =
              Op == NdOp::INT_LESS ? Left < Right : Left <= Right;
          Oracle[Taken ? 0 : 1] = Value;
        }
        ASSERT_EQ(Bounds.size(), 2U);
        EXPECT_EQ(Bounds[0], Oracle[0]);
        EXPECT_EQ(Bounds[1], Oracle[1]);
      }
}

TEST(HighSourceRanges, IntegerComparisonResultsKeepTheirNativeCarrierWidth) {
  for (uint16_t Bytes : {1, 2, 4, 8}) {
    auto Index = input();
    auto Guard = compare(NdOp::INT_LESS, HighExpr::makeConst(8, 4), Index);
    Guard->Type = NdType::makeInt(Bytes, false);
    HighFunc F;
    F.Body = {branch(Guard, {result(HighExpr::makeConst(2, 8))}),
              result(Index)};
    const auto Bounds = highSourceUnsignedUpperBounds(F, {{&F.Body[1], Index}});
    ASSERT_EQ(Bounds.size(), 1U);
    EXPECT_EQ(Bounds[0], 8U);
  }
}

TEST(HighSourceRanges, CompoundPredicatesPreserveEveryShortCircuitArm) {
  for (bool Conjunction : {false, true})
    for (uint64_t Limit : {0, 1, 17, 255}) {
      auto Index = input(0, 1);
      auto Constant = HighExpr::makeConst(Limit, 1);
      auto Left = compare(Conjunction ? NdOp::INT_LESSEQUAL : NdOp::INT_LESS,
                          Conjunction ? Constant : Index,
                          Conjunction ? Index : Constant);
      auto Right = compare(Conjunction ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL,
                           Index, Constant);
      auto Guard =
          compare(Conjunction ? NdOp::BOOL_AND : NdOp::BOOL_OR, Left, Right);
      Guard->Type = NdType::makeInt(4);
      HighFunc F;
      F.Body = {branch(Guard, {result(Index)}, {result(Index)})};
      const auto Bounds = highSourceUnsignedUpperBounds(
          F, {{&F.Body[0].Body[0], Index}, {&F.Body[0].ElseBody[0], Index}});
      std::optional<uint64_t> Oracle[2];
      for (uint64_t Value = 0; Value < 256; ++Value) {
        const bool Taken = Conjunction ? (Limit <= Value && Value != Limit)
                                       : (Value < Limit || Value == Limit);
        Oracle[Taken ? 0 : 1] = Value;
      }
      // An upper-only domain may retain an impossible intersection; every
      // concretely reachable arm must still have its exact maximum here.
      for (unsigned Arm = 0; Arm < 2; ++Arm)
        if (Oracle[Arm])
          EXPECT_EQ(Bounds[Arm], Oracle[Arm]);
    }
}

TEST(HighSourceRanges, PureOpaquePredicatePreservesIndependentGuard) {
  auto Index = input(0, 8);
  auto OutOfRange = compare(NdOp::INT_LESS, HighExpr::makeConst(9, 8),
                            Index);
  auto Opaque = HighExpr::makeBinop(
      NdOp::INT_DIV, Index, HighExpr::makeConst(2, 8));
  auto Other = compare(NdOp::INT_EQUAL, Opaque,
                       HighExpr::makeConst(0, 8));
  auto Guard = compare(NdOp::BOOL_OR, OutOfRange, Other);
  HighFunc F;
  F.Body = {branch(Guard, {result(HighExpr::makeConst(0, 8))},
                   {result(Index)})};
  auto Bounds = highSourceUnsignedUpperBounds(
      F, {{&F.Body[0].ElseBody[0], Index}});
  ASSERT_EQ(Bounds.size(), 1U);
  EXPECT_EQ(Bounds[0], 9U);

  // A hidden register write during the other operand invalidates the fact.
  Other->IntrinsicOutputs.push_back(Index->Var);
  Bounds = highSourceUnsignedUpperBounds(F, {{&F.Body[0].ElseBody[0], Index}});
  EXPECT_EQ(Bounds[0], UINT64_MAX);
}

TEST(HighSourceRanges, WrappedIndicesMasksAndWidthsKeepTheirOwnBounds) {
  auto Index =
      HighExpr::makeBinop(NdOp::INT_SUB, input(), HighExpr::makeConst(3, 4));
  HighFunc F;
  F.Body = {branch(compare(NdOp::INT_LESS, HighExpr::makeConst(8, 4), Index),
                   {result(HighExpr::makeConst(2, 8))}, {result(Index)})};
  auto Bounds =
      highSourceUnsignedUpperBounds(F, {{&F.Body[0].ElseBody[0], Index}});
  ASSERT_EQ(Bounds.size(), 1U);
  EXPECT_EQ(Bounds[0], 8U);
  for (uint16_t Bytes : {1, 2, 4, 8}) {
    auto Masked = HighExpr::makeBinop(NdOp::INT_AND, input(0, Bytes),
                                      HighExpr::makeConst(7, Bytes));
    auto Shifted =
        HighExpr::makeBinop(NdOp::INT_LEFT, Masked, HighExpr::makeConst(3, 1));
    Shifted->Type = NdType::makeInt(Bytes, false);
    F.Body = {result(Shifted)};
    Bounds = highSourceUnsignedUpperBounds(F, {{&F.Body[0], Shifted}});
    EXPECT_EQ(Bounds[0], 56U);
  }
}

TEST(HighSourceRanges, EveryEmittedWriteInvalidatesTheMatchingLocal) {
  for (bool Renamed : {false, true}) {
    auto Index = input();
    auto Destination = input();
    if (Renamed) {
      Index->Var.RenameTag = Destination->Var.RenameTag = 7;
      Destination->Var.Id = 9;
    }
    HighFunc F;
    F.Body = {branch(
        compare(NdOp::INT_LESSEQUAL, Index, HighExpr::makeConst(8, 4)),
        {write(Destination, HighExpr::makeConst(99, 4)), result(Index)})};
    const auto Bounds =
        highSourceUnsignedUpperBounds(F, {{&F.Body[0].Body[1], Index}});
    EXPECT_EQ(Bounds[0], UINT32_MAX);
  }
}

TEST(HighSourceRanges, UnguardedEntriesAndSharedExpressionsCannotBorrowFacts) {
  auto Index = input();
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  HighFunc F;
  auto Guard = compare(NdOp::INT_LESSEQUAL, Index, HighExpr::makeConst(8, 4));
  F.Body = {branch(input(1), {Jump}),
            branch(Guard, {result(Index, 0x2000)}, {result(Index)})};
  auto Bounds = highSourceUnsignedUpperBounds(
      F, {{&F.Body[1].Body[0], Index}, {&F.Body[1].ElseBody[0], Index}});
  EXPECT_EQ(Bounds[0], UINT32_MAX);
  EXPECT_EQ(Bounds[1], UINT32_MAX);
  F.Body.erase(F.Body.begin());
  Bounds = highSourceUnsignedUpperBounds(
      F, {{&F.Body[0].Body[0], Index}, {&F.Body[0].ElseBody[0], Index}});
  EXPECT_EQ(Bounds[0], 8U);
  EXPECT_EQ(Bounds[1], UINT32_MAX);
}

TEST(HighSourceRanges, LoopGuardsAreRecheckedAfterEachIndexWrite) {
  auto Index = input();
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = compare(NdOp::INT_LESSEQUAL, Index, HighExpr::makeConst(8, 4));
  HighStmt Read;
  Read.Kind = StmtKind::ExprStmt;
  Read.Val = Index;
  Loop.Body = {Read,
               write(Index, HighExpr::makeBinop(NdOp::INT_ADD, Index,
                                                HighExpr::makeConst(1, 4)))};
  HighFunc F;
  F.Body = {Loop, result(Index)};
  const auto Bounds = highSourceUnsignedUpperBounds(
      F, {{&F.Body[0].Body[0], Index}, {&F.Body[1], Index}});
  EXPECT_EQ(Bounds[0], 8U);
  EXPECT_EQ(Bounds[1], UINT32_MAX);
}

TEST(HighSourceRanges, SignedGuardsEffectsEscapesAndIncompleteFlowGiveNoProof) {
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    auto Index = input();
    HighFunc F;
    auto Guard =
        compare(NdOp::INT_SLESSEQUAL, Index, HighExpr::makeConst(8, 4));
    F.Body = {branch(Guard, {result(Index)})};
    if (Variant == 1)
      Index->Type = NdType::makeFloat(4);
    if (Variant == 2) {
      Index->Kind = ExprKind::Load;
      Index->Operands = {HighExpr::makeConst(0x1000, 8)};
    }
    if (Variant == 3) {
      auto Address = std::make_shared<HighExpr>();
      Address->Kind = ExprKind::Addr;
      Address->Type = NdType::makePtr(NdType::makeInt(4));
      Address->Operands = {Index};
      HighStmt Escape;
      Escape.Kind = StmtKind::ExprStmt;
      Escape.Val = Address;
      F.Body.push_back(Escape);
    }
    if (Variant == 4) {
      HighStmt Jump;
      Jump.Kind = StmtKind::Goto;
      Jump.GotoTarget = 0x1234;
      F.Body.push_back(Jump);
    }
    if (Variant == 5)
      F.StructuredExceptionRegions = 1;
    const auto Bounds =
        highSourceUnsignedUpperBounds(F, {{&F.Body[0].Body[0], Index}});
    if (!Variant)
      EXPECT_EQ(Bounds[0], UINT32_MAX);
    else
      EXPECT_EQ(Bounds[0], std::nullopt) << Variant;
  }
}

TEST(HighSourceRanges, EffectfulBooleanOperandsInvalidateEarlierGuardFacts) {
  auto Index = input();
  auto Effect = HighExpr::makeCall("overwrite", 0x5000, {});
  Effect->Type = NdType::makeInt(1, false);
  Effect->IntrinsicOutputs = {Index->Var};
  auto Guard = compare(
      NdOp::BOOL_AND,
      compare(NdOp::INT_LESSEQUAL, Index, HighExpr::makeConst(8, 4)), Effect);
  HighFunc F;
  F.Body = {branch(Guard, {result(Index)})};
  const auto Bounds =
      highSourceUnsignedUpperBounds(F, {{&F.Body[0].Body[0], Index}});
  ASSERT_EQ(Bounds.size(), 1U);
  EXPECT_EQ(Bounds[0], UINT32_MAX);
}

TEST(HighSourceRanges, WritesWithinTheQueryStatementInvalidateEntryFacts) {
  auto Index = input();
  auto Effect = HighExpr::makeCall("overwrite", 0x5000, {});
  Effect->Type = NdType::makeInt(4, false);
  Effect->IntrinsicOutputs = {Index->Var};
  HighFunc F;
  F.Body = {
      branch(compare(NdOp::INT_LESSEQUAL, Index, HighExpr::makeConst(8, 4)),
             {result(HighExpr::makeBinop(NdOp::INT_ADD, Effect, Index))})};
  const auto Bounds =
      highSourceUnsignedUpperBounds(F, {{&F.Body[0].Body[0], Index}});
  ASSERT_EQ(Bounds.size(), 1U);
  EXPECT_EQ(Bounds[0], UINT32_MAX);
}

TEST(HighSourceRanges, TrailingLoopQueriesUseEveryConditionEntry) {
  for (bool Overwrite : {false, true})
    for (bool Continue : {false, true}) {
      auto Index = input();
      HighStmt Loop;
      Loop.Kind = StmtKind::DoWhile;
      Loop.Cond = compare(NdOp::INT_LESS, Index, HighExpr::makeConst(2, 4));
      if (Overwrite)
        Loop.Body.push_back(write(Index, HighExpr::makeConst(99, 4)));
      if (Continue) {
        HighStmt Jump;
        Jump.Kind = StmtKind::Continue;
        Loop.Body.push_back(Jump);
      }
      HighFunc F;
      F.Body = {
          branch(compare(NdOp::INT_LESSEQUAL, Index, HighExpr::makeConst(8, 4)),
                 {Loop})};
      const auto Bounds =
          highSourceUnsignedUpperBounds(F, {{&F.Body[0].Body[0], Index}});
      ASSERT_EQ(Bounds.size(), 1U);
      EXPECT_EQ(Bounds[0], Overwrite ? UINT32_MAX : 8U);
    }
}

TEST(HighSourceRanges, ExhaustedBudgetsDiscardTheWholeQueryBatch) {
  auto Index = input();
  HighFunc F;
  F.Body = {result(Index)};
  auto Bounds = highSourceUnsignedUpperBounds(
      F, std::vector<HighSourceUnsignedRangeQuery>(129, {&F.Body[0], Index}));
  EXPECT_TRUE(
      std::all_of(Bounds.begin(), Bounds.end(), [](auto B) { return !B; }));
  HighStmt Padding;
  Padding.Kind = StmtKind::ExprStmt;
  Padding.Val = HighExpr::makeConst(1, 1);
  F.Body.insert(F.Body.begin(), 100001, Padding);
  Bounds = highSourceUnsignedUpperBounds(F, {{&F.Body.back(), Index}});
  ASSERT_EQ(Bounds.size(), 1U);
  EXPECT_FALSE(Bounds[0]);
}
} // namespace
