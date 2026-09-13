#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/ADT/APInt.h"

#include <optional>
#include <stdexcept>

namespace neverd {
void structureIfElse(HighFunc &, int, const MedFunc * = nullptr);
void inlineSingleDefSingleUse(std::vector<HighStmt> &);
void resolveRegAliases(std::vector<HighStmt> &);
void simplifyAllExprs(std::vector<HighStmt> &);
void recoverSwitchStatements(HighFunc &);
void removeUnreachableCode(std::vector<HighStmt> &);
void eliminateRegAliasCopies(HighFunc &);
void elimConsecutiveDeadStores(std::vector<HighStmt> &);
void postRenameCleanup(std::vector<HighStmt> &);
} // namespace neverd
using namespace neverd;

namespace {
ExprPtr local(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  return HighExpr::makeVar(V);
}
HighStmt assign(va_t Address, int Id, uint64_t Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = Address;
  S.Dst = local(Id);
  S.Val = HighExpr::makeConst(Value, 8);
  return S;
}
HighStmt jump(va_t Address, va_t Target) {
  HighStmt S;
  S.Kind = StmtKind::Goto;
  S.Addr = Address;
  S.GotoTarget = Target;
  return S;
}
HighStmt conditional(va_t Address, va_t Target) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Addr = Address;
  S.Cond = local(0);
  S.Body = {jump(Address, Target)};
  return S;
}
HighStmt result(va_t Address, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.Addr = Address;
  S.RetVal = std::move(Value);
  return S;
}

// An independent bounded interpreter checks observable branch results,
// including a missing shared return. It does not prescribe an attractive output
// shape.
std::optional<uint64_t> execute(const HighFunc &F, uint64_t Condition) {
  std::map<VarKey, uint64_t> Values{{{0, 0}, Condition}, {{0, -1}, Condition}};
  std::map<uint64_t, uint64_t> Memory;
  std::function<uint64_t(const ExprPtr &)> Value = [&](const ExprPtr &E) {
    if (E->Kind == ExprKind::Const)
      return E->ConstVal;
    if (E->Kind == ExprKind::Var) {
      auto I = Values.find(varKey(E->Var));
      if (I == Values.end()) {
        if (E->Var.Kind == MedVar::Reg &&
            getTargetRegInfo(E->Var.TheArch).isFrameReg(E->Var.RegOff))
          return UINT64_C(0x8000);

        std::string Description = "undefined " + E->str() + " identity " +
                                  std::to_string(E->Var.Id) + ":" +
                                  std::to_string(E->Var.SSAVer) + "\n";
        for (const auto &S : F.Body)
          Description += S.str() + "\n";
        throw std::runtime_error(Description);
      }
      return I->second;
    }
    if (E->Kind == ExprKind::Load)
      return Memory.at(Value(E->Operands.at(0)));
    if (E->Kind == ExprKind::Cast)
      return Value(E->Operands.at(0));
    if (E->Kind == ExprKind::Call && E->CallTarget == "observe")
      return Memory.at(Value(E->Operands.at(1)));
    if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT)
      return uint64_t(!Value(E->Operands.at(0)));
    if (E->Kind == ExprKind::UnaryOp &&
        (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)) {
      auto Input = E->Operands[0];
      const llvm::APInt V(Input->Type->Size * 8, Value(Input));
      const unsigned OutputBits = E->Type->Size * 8;
      return (E->Op == NdOp::INT_SEXT ? V.sextOrTrunc(OutputBits)
                                      : V.zextOrTrunc(OutputBits))
          .getZExtValue();
    }
    if (E->Kind == ExprKind::BinOp && E->Operands.size() == 2) {
      auto A = Value(E->Operands[0]), B = Value(E->Operands[1]);
      switch (E->Op) {
      case NdOp::INT_ADD:
        return A + B;
      case NdOp::INT_SUB:
        return A - B;
      case NdOp::INT_MULT:
        return A * B;
      case NdOp::INT_EQUAL:
        return uint64_t(A == B);
      case NdOp::INT_NOTEQUAL:
        return uint64_t(A != B);
      default:
        break;
      }
    }
    throw std::runtime_error("unsupported expression in control-flow oracle: " +
                             E->str());
  };
  struct Flow {
    std::optional<uint64_t> Return;
    va_t Target = 0;
    bool Break = false;
    bool Continue = false;
  };
  unsigned Budget = 10000;
  std::function<Flow(const std::vector<HighStmt> &)> Run =
      [&](const auto &Body) -> Flow {
    for (const auto &S : Body) {
      if (!Budget--)
        throw std::runtime_error(
            "control-flow oracle exceeded its instruction budget");
      if (S.Kind == StmtKind::Break)
        return {{}, 0, true, false};
      if (S.Kind == StmtKind::Continue)
        return {{}, 0, false, true};
      if (S.Kind == StmtKind::Return)
        return {Value(S.RetVal), 0};
      if (S.Kind == StmtKind::Goto)
        return {{}, S.GotoTarget};
      if (S.Kind == StmtKind::Store)
        Memory[Value(S.StoreAddr)] = Value(S.StoreVal);
      if (S.Kind == StmtKind::Call)
        (void)Value(S.CallExpr);
      if (S.Kind == StmtKind::Assign)
        Values[varKey(S.Dst->Var)] = Value(S.Val);
      if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) {
        auto R = Run(Value(S.Cond) ? S.Body : S.ElseBody);
        if (R.Return || R.Target || R.Break || R.Continue)
          return R;
      }
      if (S.Kind == StmtKind::Switch) {
        const auto Selector = Value(S.SwitchExpr);
        const std::vector<HighStmt> *Selected = &S.DefaultBody;
        for (const auto &Case : S.Cases)
          if (Case.Value == Selector) {
            Selected = &Case.Body;
            break;
          }
        auto R = Run(*Selected);
        if (R.Return || R.Target || R.Continue)
          return R;
      }
      if (S.Kind == StmtKind::While) {
        while (Value(S.Cond)) {
          if (!Budget--)
            throw std::runtime_error(
                "control-flow oracle exceeded its loop budget");
          auto R = Run(S.Body);
          if (R.Return || R.Target)
            return R;
          if (R.Break)
            break;
        }
      }
    }
    return {};
  };
  size_t Position = 0;
  for (unsigned Steps = 0; Steps != 30 && Position < F.Body.size(); ++Steps) {
    auto R = Run({F.Body[Position]});
    if (R.Return)
      return R.Return;
    if (!R.Target) {
      ++Position;
      continue;
    }
    auto I = std::find_if(F.Body.begin(), F.Body.end(),
                          [&](const auto &S) { return S.Addr >= R.Target; });
    if (I == F.Body.end())
      return {};
    Position = I - F.Body.begin();
  }
  return {};
}

TEST(HighControlFlowSemantics, TargetReturnIsNotAnEarlyFallthroughReturn) {
  HighFunc F;
  auto Nested = conditional(0x1004, 0x1040);
  Nested.Kind = StmtKind::IfElse;
  Nested.ElseBody = {jump(0x1008, 0x1040)};
  F.Body = {conditional(0x1000, 0x1020), Nested,
            result(0x1028, HighExpr::makeConst(7, 8)),
            result(0x1040, HighExpr::makeConst(9, 8))};
  ASSERT_EQ(execute(F, 0), 9u);
  ASSERT_EQ(execute(F, 1), 7u);
  structureIfElse(F, 1);
  EXPECT_EQ(execute(F, 0), 9u);
  EXPECT_EQ(execute(F, 1), 7u);
}

TEST(HighControlFlowSemantics, SharedReturnRemainsOnItsFallthroughPath) {
  HighFunc F;
  F.Body = {assign(0x1000, 1, 3), conditional(0x1004, 0x1040),
            assign(0x1008, 1, 11), result(0x1040, local(1))};
  ASSERT_EQ(execute(F, 0), 11u);
  ASSERT_EQ(execute(F, 1), 3u);
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), 11u);
  EXPECT_EQ(execute(F, 1), 3u);
}

TEST(HighControlFlowSemantics, ExclusiveReturnCanStillBeInlined) {
  HighFunc F;
  F.Body = {conditional(0x1000, 0x1020), jump(0x1004, 0x1040),
            result(0x1028, HighExpr::makeConst(7, 8)),
            result(0x1040, HighExpr::makeConst(9, 8))};
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), 9u);
  EXPECT_EQ(execute(F, 1), 7u);
  ASSERT_EQ(F.Body.front().Kind, StmtKind::If);
  ASSERT_FALSE(F.Body.front().Body.empty());
  EXPECT_EQ(F.Body.front().Body.back().Kind, StmtKind::Return);
}

size_t statementCount(const HighFunc &F) {
  size_t Count = 0;
  walkStmts(F.Body, [&](const HighStmt &) { ++Count; });
  return Count;
}

HighStmt nestedValue(va_t Address) {
  auto Tail = assign(0, 2, 17);
  for (unsigned Depth = 0; Depth < 8; ++Depth) {
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = HighExpr::makeConst(1, 1);
    Branch.Body.push_back(std::move(Tail));
    Branch.ElseBody = {assign(0, 2, 19)};
    Tail = std::move(Branch);
  }
  Tail.Addr = Address;
  return Tail;
}

TEST(HighControlFlowSemantics, SharedNestedTailKeepsPhiEdgesWithoutGrowth) {
  for (bool WithMed : {false, true}) {
    HighFunc F;
    auto Branch = conditional(0x1004, 0x1040);
    auto Phi = assign(0x1004, 1, 7);
    Phi.IsPhiCopy = true;
    Branch.Body.insert(Branch.Body.begin(), Phi);
    F.Body = {
        assign(0x1000, 1, 3), Branch, assign(0x1008, 1, 11),
        nestedValue(0x1040),
        result(0x1080, HighExpr::makeBinop(NdOp::INT_ADD, local(1), local(2)))};
    MedFunc Med;
    Med.Blocks.resize(1);
    Med.Blocks.front().StartAddr = 0x1040;
    Med.Blocks.front().Preds = {0};
    const size_t Before = statementCount(F);
    ASSERT_EQ(execute(F, 0), 28u);
    ASSERT_EQ(execute(F, 1), 24u);
    structureIfElse(F, 10, WithMed ? &Med : nullptr);
    EXPECT_EQ(execute(F, 0), 28u);
    EXPECT_EQ(execute(F, 1), 24u);
    EXPECT_LE(statementCount(F), Before);
  }
}

TEST(HighControlFlowSemantics, ExternalElseTailIsNotCloned) {
  HighFunc F;
  F.Body = {conditional(0x1000, 0x1080), jump(0x1004, 0x1040),
            nestedValue(0x1040), result(0x1048, local(2)),
            result(0x1080, HighExpr::makeConst(7, 8))};
  const size_t Before = statementCount(F);
  ASSERT_EQ(execute(F, 0), 17u);
  ASSERT_EQ(execute(F, 1), 7u);
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), 17u);
  EXPECT_EQ(execute(F, 1), 7u);
  EXPECT_LE(statementCount(F), Before);
}

TEST(HighControlFlowSemantics, BackwardTargetDoesNotConsumeItsOwnConditional) {
  HighFunc F;
  auto Increment = assign(0x1004, 1, 0);
  Increment.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  auto Branch = conditional(0x1008, 0x1004);
  Branch.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(1), local(0));
  F.Body = {assign(0x1000, 1, 0), Increment, Branch, result(0x1010, local(1))};
  const size_t Before = statementCount(F);
  for (unsigned Count = 1; Count <= 8; ++Count)
    ASSERT_EQ(execute(F, Count), Count);
  structureIfElse(F, 10);
  for (unsigned Count = 1; Count <= 8; ++Count)
    EXPECT_EQ(execute(F, Count), Count);
  EXPECT_LE(statementCount(F), Before);
}

TEST(HighControlFlowSemantics, SwitchEntryIntoTargetInteriorRemainsVisible) {
  for (bool DefaultEntry : {false, true}) {
    HighFunc F;
    HighStmt Dispatch;
    Dispatch.Kind = StmtKind::Switch;
    Dispatch.Addr = 0x1000;
    Dispatch.SwitchExpr = local(0);
    Dispatch.Cases.push_back(
        {0, {jump(0x1000, DefaultEntry ? 0x1010 : 0x1084)}});
    Dispatch.DefaultBody = {jump(0x1000, DefaultEntry ? 0x1084 : 0x1010)};
    F.Body = {Dispatch,
              conditional(0x1010, 0x1080),
              jump(0x1014, 0x10a0),
              assign(0x1080, 1, 7),
              result(0x1084, HighExpr::makeConst(9, 8)),
              result(0x10a0, HighExpr::makeConst(11, 8))};
    const auto Zero = execute(F, 0), One = execute(F, 1);
    ASSERT_EQ(Zero, DefaultEntry ? 11u : 9u);
    ASSERT_EQ(One, 9u);
    const size_t Before = statementCount(F);
    structureIfElse(F, 10);
    EXPECT_EQ(execute(F, 0), Zero);
    EXPECT_EQ(execute(F, 1), One);
    EXPECT_LE(statementCount(F), Before);
  }
}

TEST(HighControlFlowSemantics, OverlappingTargetRunRetainsItsSingleOwner) {
  HighFunc F;
  F.Body = {conditional(0x1000, 0x1004), jump(0x1004, 0x1020),
            result(0x1020, HighExpr::makeConst(7, 8))};
  const size_t Before = statementCount(F);
  ASSERT_EQ(execute(F, 0), 7u);
  ASSERT_EQ(execute(F, 1), 7u);
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), 7u);
  EXPECT_EQ(execute(F, 1), 7u);
  EXPECT_LE(statementCount(F), Before);
}

TEST(HighControlFlowSemantics, MovedBranchKeepsItsNonadjacentSuccessor) {
  for (bool ExplicitGoto : {false, true}) {
    HighFunc F;
    F.Body = {conditional(0x1000, 0x1080), assign(0x1004, 1, 3),
              jump(0x1008, 0x1100), result(0x1040, HighExpr::makeConst(91, 8)),
              assign(0x1080, 1, 7)};
    if (ExplicitGoto)
      F.Body.push_back(jump(0x1084, 0x1100));
    F.Body.push_back(result(0x1100, local(1)));
    const size_t Before = statementCount(F);
    ASSERT_EQ(execute(F, 0), 3u);
    ASSERT_EQ(execute(F, 1), 7u);
    structureIfElse(F, 10);
    EXPECT_EQ(execute(F, 0), 3u);
    EXPECT_EQ(execute(F, 1), 7u);
    EXPECT_LE(statementCount(F), Before);
  }
}

TEST(HighControlFlowSemantics, ImmediateTargetKeepsBothEdgesAndPhiCopies) {
  for (bool WithPhi : {false, true}) {
    HighFunc F;
    auto Branch = conditional(0x1004, 0x1008);
    if (WithPhi) {
      auto Phi = assign(0x1004, 1, 7);
      Phi.IsPhiCopy = true;
      Branch.Body.insert(Branch.Body.begin(), Phi);
    }
    F.Body = {assign(0x1000, 1, 3), Branch, result(0x1008, local(1))};
    ASSERT_EQ(execute(F, 0), 3u);
    ASSERT_EQ(execute(F, 1), WithPhi ? 7u : 3u);
    structureIfElse(F, 10);
    EXPECT_EQ(execute(F, 0), 3u);
    EXPECT_EQ(execute(F, 1), WithPhi ? 7u : 3u);
  }
}

TEST(HighControlFlowSemantics, ExceptionalTargetKeepsItsExternalEntry) {
  HighFunc F;
  F.Body = {conditional(0x1000, 0x1020), jump(0x1004, 0x1040),
            result(0x1028, HighExpr::makeConst(7, 8)),
            result(0x1040, HighExpr::makeConst(9, 8))};
  MedFunc Med;
  Med.Blocks.resize(3);
  for (int I = 0; I < 3; ++I) {
    Med.Blocks[I].Id = I;
    Med.Blocks[I].StartAddr = 0x1000 + I * 0x20;
  }
  Med.Blocks[0].Succs = {1, 2};
  Med.Blocks[1].Preds = {0};
  Med.Blocks[2].Preds = {0};
  ExceptionalEdge ToHandler;
  ToHandler.BlockId = 1;
  ToHandler.TargetVA = 0x1020;
  ToHandler.Kind = ExceptionalEdgeKind::ItaniumCatchPad;
  Med.Blocks[2].ExceptionalSuccs = {ToHandler};
  ToHandler.BlockId = 2;
  Med.Blocks[1].ExceptionalPreds = {ToHandler};
  auto EnterHandler = [](HighFunc Handler) {
    Handler.Body.insert(Handler.Body.begin(), jump(0, 0x1020));
    return execute(Handler, 0);
  };
  ASSERT_EQ(EnterHandler(F), 7u);
  structureIfElse(F, 10, &Med);
  EXPECT_EQ(execute(F, 0), 9u);
  EXPECT_EQ(execute(F, 1), 7u);
  EXPECT_EQ(EnterHandler(F), 7u);
}

void expectUniqueGotoTargets(const HighFunc &F) {
  std::map<va_t, size_t> Labels;
  walkStmts(F.Body, [&](const HighStmt &S) {
    if (S.Addr && S.Addr != InvalidVA)
      ++Labels[S.Addr];
  });
  walkStmts(F.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      EXPECT_EQ(Labels[S.GotoTarget], 1u) << "target " << S.GotoTarget;
  });
}

std::pair<HighFunc, MedFunc> branchWithInternalLoop() {
  HighFunc F;
  auto Increment = assign(0x1054, 1, 0);
  Increment.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  Break.Addr = 0x1058;
  HighStmt Exit;
  Exit.Kind = StmtKind::If;
  Exit.Addr = 0x1058;
  Exit.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(1),
                                HighExpr::makeConst(3, 8));
  Exit.Body = {Break};
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Addr = Loop.LoopHeaderAddr = 0x1050;
  Loop.Cond = HighExpr::makeConst(1, 1);
  Loop.Body = {Increment, Exit};
  F.Body = {conditional(0x1000, 0x1040), jump(0x1004, 0x1080),
            assign(0x1044, 1, 0), Loop, result(0x1060, local(1)),
            result(0x1080, HighExpr::makeConst(7, 8))};

  MedFunc Med;
  Med.Blocks.resize(5);
  const va_t Starts[] = {0x1000, 0x1040, 0x1050, 0x1060, 0x1080};
  const va_t Ends[] = {0x1000, 0x1044, 0x1058, 0x1060, 0x1080};
  for (int I = 0; I < 5; ++I) {
    auto &Block = Med.Blocks[I];
    Block.Id = I;
    Block.StartAddr = Starts[I];
    Block.EndAddr = Ends[I] + 4;
    MedOp Last;
    Last.Addr = Ends[I];
    Last.Opcode = I == 0 || I == 2 ? NdOp::COND_BR
                  : I == 1       ? NdOp::BRANCH
                                 : NdOp::RETURN;
    Block.Ops = {Last};
  }
  Med.Blocks[0].Succs = {1, 4};
  Med.Blocks[1].Preds = {0};
  Med.Blocks[1].Succs = {2};
  Med.Blocks[2].Preds = {1, 2};
  Med.Blocks[2].Succs = {2, 3};
  Med.Blocks[3].Preds = {2};
  Med.Blocks[4].Preds = {0};
  return {std::move(F), std::move(Med)};
}

TEST(HighControlFlowSemantics, InternalLoopPredecessorsKeepTheirBranchOwner) {
  auto [F, Med] = branchWithInternalLoop();
  const size_t Before = statementCount(F);
  ASSERT_EQ(execute(F, 0), 7u);
  ASSERT_EQ(execute(F, 1), 3u);
  structureIfElse(F, 10, &Med);
  EXPECT_EQ(execute(F, 0), 7u);
  EXPECT_EQ(execute(F, 1), 3u);
  EXPECT_LE(statementCount(F), Before);
  ASSERT_EQ(F.Body.front().Kind, StmtKind::If);
  EXPECT_TRUE(std::any_of(F.Body.front().Body.begin(), F.Body.front().Body.end(),
                          [](const HighStmt &S) {
                            return S.Kind == StmtKind::While;
                          }));
  expectUniqueGotoTargets(F);
}

TEST(HighControlFlowSemantics, UnprovenLoopPredecessorsCannotHideTheSharedTail) {
  for (unsigned Mode = 0; Mode < 8; ++Mode) {
    SCOPED_TRACE(Mode);
    auto [F, Med] = branchWithInternalLoop();
    if (Mode == 0) {
      MedBlock External;
      External.Id = 5;
      External.StartAddr = 0x1090;
      External.Succs = {2};
      MedOp Last;
      Last.Opcode = NdOp::BRANCH;
      Last.Addr = 0x1090;
      External.Ops = {Last};
      Med.Blocks.push_back(External);
      Med.Blocks[2].Preds.push_back(5);
    } else if (Mode == 1) {
      Med.Blocks[2].Preds.push_back(-1);
    } else if (Mode == 2) {
      HighStmt External;
      External.Addr = 0x1044;
      F.Body.push_back(External);
    } else if (Mode == 3) {
      Med.Blocks[0].Succs.push_back(2);
      Med.Blocks[2].Preds.push_back(0);
    } else if (Mode == 4) {
      Med.Blocks[1].Ops.clear();
    } else if (Mode == 5) {
      Med.Blocks[1].Id = 99;
    } else if (Mode == 6) {
      Med.Blocks[1].Succs.clear();
    } else {
      Med.Blocks[2].Id = 99;
    }
    const size_t Before = statementCount(F);
    structureIfElse(F, 10, &Med);
    EXPECT_EQ(execute(F, 0), 7u);
    EXPECT_EQ(execute(F, 1), 3u);
    EXPECT_LE(statementCount(F), Before);
    EXPECT_TRUE(std::any_of(F.Body.begin(), F.Body.end(), [](const HighStmt &S) {
      return S.Kind == StmtKind::While && S.Addr == 0x1050;
    }));
    expectUniqueGotoTargets(F);
  }
}

TEST(HighControlFlowSemantics, SharedPhiTailDoesNotRequireAnEliminatedLabel) {
  HighFunc F;
  auto Branch = conditional(0x1004, 0x1040);
  auto Phi = assign(0x1004, 1, 7);
  Phi.IsPhiCopy = true;
  Branch.Body.insert(Branch.Body.begin(), Phi);
  F.Body = {assign(0x1000, 1, 3), Branch, assign(0x1008, 1, 11),
            result(0x1048, HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                              HighExpr::makeConst(1, 8)))};
  const size_t Before = statementCount(F);
  ASSERT_EQ(execute(F, 0), 12u);
  ASSERT_EQ(execute(F, 1), 8u);
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), 12u);
  EXPECT_EQ(execute(F, 1), 8u);
  EXPECT_LE(statementCount(F), Before);
  ASSERT_EQ(F.Body.back().Kind, StmtKind::Return);
  EXPECT_EQ(F.Body.back().Addr, 0x1048u);
  expectUniqueGotoTargets(F);
}

HighFunc nestedDiamondWithSharedReturn() {
  auto Outer = conditional(0x1000, 0x1080);
  Outer.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                 HighExpr::makeConst(0, 8));
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1004;
  Inner.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                 HighExpr::makeConst(1, 8));
  Inner.Body = {assign(0x1040, 1, 1), jump(0x1044, 0x1100)};
  Inner.ElseBody = {assign(0x1060, 1, 2), jump(0x1064, 0x1100)};
  HighFunc F;
  F.Body = {Outer, Inner, assign(0x1080, 1, 0), result(0x1104, local(1))};
  return F;
}

TEST(HighControlFlowSemantics, NestedDiamondKeepsItsSharedReturnVisible) {
  auto F = nestedDiamondWithSharedReturn();
  const size_t Before = statementCount(F);
  for (uint64_t Input : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, Input), Input < 2 ? Input : 2u);
  structureIfElse(F, 10);
  for (uint64_t Input : {0, 1, 2, 3})
    EXPECT_EQ(execute(F, Input), Input < 2 ? Input : 2u);
  EXPECT_LE(statementCount(F), Before);
  expectUniqueGotoTargets(F);
}

TEST(HighControlFlowSemantics, CommonTransferKeepsItsReferencedEntry) {
  for (bool Exceptional : {false, true}) {
    auto F = nestedDiamondWithSharedReturn();
    MedFunc Med;
    if (Exceptional) {
      Med.Blocks.resize(1);
      ExceptionalEdge Entry;
      Entry.TargetVA = 0x1044;
      Med.Blocks[0].ExceptionalPreds = {Entry};
    } else {
      // A switch entry can enter this transfer without executing its arm.
      // Keep it present even though ordinary inputs below never take it.
      HighStmt Dispatch;
      Dispatch.Kind = StmtKind::Switch;
      Dispatch.SwitchExpr = local(0);
      Dispatch.Cases.push_back({99, {jump(0, 0x1044)}});
      F.Body.insert(F.Body.begin(), Dispatch);
    }
    structureIfElse(F, 10, Exceptional ? &Med : nullptr);
    for (uint64_t Input : {0, 1, 2, 3})
      EXPECT_EQ(execute(F, Input), Input < 2 ? Input : 2u);
    size_t EntryCount = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto && S.Addr == 0x1044 &&
          S.GotoTarget == 0x1100)
        ++EntryCount;
    });
    EXPECT_EQ(EntryCount, 1u);
  }
}

TEST(HighControlFlowSemantics, ExternalFalsePrefixEntryRemainsReachable) {
  HighFunc F;
  HighStmt Dispatch;
  Dispatch.Kind = StmtKind::Switch;
  Dispatch.Addr = 0x1004;
  Dispatch.SwitchExpr = local(0);
  Dispatch.Cases.push_back({0, {jump(0, 0x1020)}});
  Dispatch.DefaultBody = {jump(0, 0x1010)};
  F.Body = {assign(0x1000, 1, 3), Dispatch, conditional(0x1010, 0x1060),
            assign(0x1020, 1, 9), result(0x1068, local(1))};
  ASSERT_EQ(execute(F, 0), 9u);
  ASSERT_EQ(execute(F, 1), 3u);
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), 9u);
  EXPECT_EQ(execute(F, 1), 3u);
  EXPECT_TRUE(std::any_of(F.Body.begin(), F.Body.end(), [](const HighStmt &S) {
    return S.Kind == StmtKind::Assign && S.Addr == 0x1020;
  }));
}

MedVar machineValue(int Id, Arch Architecture) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  V.TheArch = Architecture;
  return V;
}
MedOp operation(NdOp Opcode, va_t Address, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp O;
  O.Opcode = Opcode;
  O.Addr = Address;
  O.Output = Output;
  for (const auto &Input : Inputs)
    O.addInput(Input);
  return O;
}
MedFunc loopFunction(Arch Architecture, bool Swap, bool ReversePhis) {
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "phi_edge_snapshot";
  F.ReturnType = NdType::makeInt(8, false);
  auto Count = machineValue(0, Architecture);
  Count.Kind = MedVar::Param;
  Count.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
  F.Params = {Count};
  auto A = machineValue(1, Architecture), B = machineValue(2, Architecture);
  auto N = machineValue(3, Architecture), Next = machineValue(4, Architecture);
  auto NextN = machineValue(5, Architecture),
       Condition = machineValue(6, Architecture);
  auto Combined = machineValue(7, Architecture),
       Return = machineValue(8, Architecture);
  Return.Kind = MedVar::Reg;
  Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
  F.Blocks.resize(3);
  for (int I = 0; I < 3; ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + 0x40;
  }
  auto &Entry = F.Blocks[0];
  Entry.Succs = {1};
  Entry.Ops = {
      operation(NdOp::BRANCH, 0x1000, {}, {MedVar::makeConst(0x1100, 8)})};
  auto &Loop = F.Blocks[1];
  Loop.Preds = {0, 1};
  Loop.Succs = {2, 1};
  Loop.Phis = {{A, {{0, MedVar::makeConst(1, 8)}, {1, Swap ? B : Next}}},
               {B, {{0, MedVar::makeConst(2, 8)}, {1, A}}},
               {N, {{0, Count}, {1, NextN}}}};
  if (ReversePhis)
    std::reverse(Loop.Phis.begin(), Loop.Phis.end());
  if (!Swap)
    Loop.Ops.push_back(
        operation(NdOp::INT_ADD, 0x1100, Next, {A, MedVar::makeConst(2, 8)}));
  Loop.Ops.push_back(
      operation(NdOp::INT_SUB, 0x1104, NextN, {N, MedVar::makeConst(1, 8)}));
  Condition.Size = 1;
  Loop.Ops.push_back(operation(NdOp::INT_NOTEQUAL, 0x1108, Condition,
                               {NextN, MedVar::makeConst(0, 8)}));
  Loop.Ops.push_back(operation(NdOp::COND_BR, 0x110c, {},
                               {MedVar::makeConst(0x1100, 8), Condition}));
  auto &Exit = F.Blocks[2];
  Exit.Preds = {1};
  if (Swap) {
    Exit.Ops.push_back(operation(NdOp::INT_MULT, 0x1200, Combined,
                                 {A, MedVar::makeConst(100, 8)}));
    Exit.Ops.push_back(operation(NdOp::INT_ADD, 0x1204, Return, {Combined, B}));
  } else {
    Exit.Ops.push_back(operation(NdOp::COPY, 0x1200, Return, {Next}));
  }
  Exit.Ops.push_back(operation(NdOp::RETURN, 0x1208, {}, {Return}));
  return F;
}

TEST(HighControlFlowSemantics, ParallelPhiCyclesRunOnlyOnTheirTakenEdge) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Reverse : {false, true}) {
      auto F = MedToHighConverter().convert(
          loopFunction(Architecture, true, Reverse), Architecture);
      for (unsigned Count = 1; Count <= 8; ++Count) {
        SCOPED_TRACE(Count);
        EXPECT_EQ(execute(F, Count), Count % 2 ? 102u : 201u);
      }
    }
}

TEST(HighControlFlowSemantics, LoopExpressionKeepsItsPrePhiSnapshot) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = MedToHighConverter().convert(
        loopFunction(Architecture, false, false), Architecture);
    for (unsigned Count = 1; Count <= 8; ++Count) {
      SCOPED_TRACE(Count);
      EXPECT_EQ(execute(F, Count), 1 + Count * 2);
    }
  }
}
TEST(HighControlFlowSemantics, SingleUseExpressionRetainsItsPhiSnapshot) {
  HighFunc F;
  auto First = assign(0x1000, 1, 10);
  First.IsPhiCopy = true;
  auto Snapshot = assign(0x1004, 2, 0);
  Snapshot.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(7, 8));
  auto Overwrite = assign(0x1008, 1, 20);
  Overwrite.IsPhiCopy = true;
  F.Body = {First, Snapshot, Overwrite, result(0x100c, local(2))};
  ASSERT_EQ(execute(F, 0), 17u);
  inlineSingleDefSingleUse(F.Body);
  EXPECT_EQ(execute(F, 0), 17u);
}

TEST(HighControlFlowSemantics, ImmutableSingleUseExpressionStillInlines) {
  HighFunc F;
  auto Calculation = assign(0x1004, 2, 0);
  Calculation.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(7, 8));
  F.Body = {assign(0x1000, 1, 10), Calculation, result(0x1008, local(2))};
  inlineSingleDefSingleUse(F.Body);
  EXPECT_EQ(execute(F, 0), 17u);
  EXPECT_EQ(F.Body.back().RetVal->Kind, ExprKind::BinOp);
}

TEST(HighControlFlowSemantics, SameRegisterAliasesPreserveExtensionSemantics) {
  for (bool AliasPass : {false, true})
    for (auto Op : {NdOp::INT_SEXT, NdOp::INT_ZEXT}) {
      HighFunc F;
      auto Narrow = machineValue(1, Arch::X64);
      Narrow.Kind = MedVar::Reg;
      Narrow.RegOff = 0;
      Narrow.Size = 4;
      auto Wide = Narrow;
      Wide.Id = 2;
      Wide.Size = 8;
      auto Set = assign(0x1000, 1, 0x80000001u);
      Set.Dst = HighExpr::makeVar(Narrow);
      Set.Val = HighExpr::makeConst(0x80000001u, 4);
      auto Extend = assign(0x1004, 2, 0);
      Extend.Dst = HighExpr::makeVar(Wide);
      Extend.Val = HighExpr::makeUnary(Op, Set.Dst);
      Extend.Val->Type = NdType::makeInt(8);
      F.Body = {Set, Extend, result(0x1008, Extend.Dst)};
      const uint64_t Expected =
          Op == NdOp::INT_SEXT ? 0xffffffff80000001ULL : 0x80000001ULL;
      ASSERT_EQ(execute(F, 0), Expected);
      if (AliasPass)
        eliminateRegAliasCopies(F);
      else
        resolveRegAliases(F.Body);
      EXPECT_EQ(execute(F, 0), Expected);
    }
}

TEST(HighControlFlowSemantics, NegativeConstantFoldingPreservesBoundaryValues) {
  for (uint64_t Constant : {UINT64_C(0x8000000000000000),
                            UINT64_C(0x8000000000000001), UINT64_MAX}) {
    for (uint64_t Input :
         {UINT64_C(0), UINT64_C(1), UINT64_C(0x7fffffffffffffff), UINT64_MAX}) {
      SCOPED_TRACE(Constant);
      SCOPED_TRACE(Input);
      HighFunc F;
      F.Body = {assign(0x1000, 1, Input),
                result(0x1004,
                       HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                           HighExpr::makeConst(Constant, 8)))};
      const uint64_t Expected = Input + Constant;
      ASSERT_EQ(execute(F, 0), Expected);
      simplifyAllExprs(F.Body);
      EXPECT_EQ(execute(F, 0), Expected);
    }
  }
}

TEST(HighControlFlowSemantics, RecoveredSwitchPreservesUnmatchedReturn) {
  HighFunc F;
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto Branch = conditional(0x1000 + 4 * Case, 0x1100 + 0x100 * Case);
    Branch.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                      HighExpr::makeConst(Case, 8));
    F.Body.push_back(std::move(Branch));
  }
  F.Body.push_back(result(0x1010, HighExpr::makeConst(99, 8)));
  for (unsigned Case = 0; Case < 3; ++Case)
    F.Body.push_back(
        result(0x1100 + 0x100 * Case, HighExpr::makeConst(Case + 10, 8)));
  for (uint64_t Input : {0, 1, 2, 3, 255})
    ASSERT_EQ(execute(F, Input), Input < 3 ? Input + 10 : 99);
  recoverSwitchStatements(F);
  for (uint64_t Input : {0, 1, 2, 3, 255}) {
    SCOPED_TRACE(Input);
    EXPECT_EQ(execute(F, Input), Input < 3 ? Input + 10 : 99);
  }
}

TEST(HighControlFlowSemantics, SwitchCleanupPreservesContinuationPaths) {
  for (StmtKind Exit : {StmtKind::Break, StmtKind::Goto, StmtKind::Return}) {
    for (bool HasDefault : {false, true}) {
      HighFunc F;
      HighStmt Switch;
      Switch.Kind = StmtKind::Switch;
      Switch.SwitchExpr = local(0);
      HighStmt End;
      End.Kind = Exit;
      End.GotoTarget = 0x1200;
      End.RetVal = HighExpr::makeConst(10, 8);
      SwitchCase Case;
      Case.Value = 0;
      Case.Body = {End};
      Switch.Cases.push_back(Case);
      if (HasDefault)
        Switch.DefaultBody = {End};
      F.Body = {Switch, result(0x1200, HighExpr::makeConst(99, 8))};
      const auto Matching = execute(F, 0);
      const auto Unmatched = execute(F, 1);
      removeUnreachableCode(F.Body);
      EXPECT_EQ(execute(F, 0), Matching);
      EXPECT_EQ(execute(F, 1), Unmatched);
      if (Exit == StmtKind::Return && HasDefault)
        EXPECT_EQ(F.Body.size(), 1u);
    }
  }
}

TEST(HighControlFlowSemantics, ArgumentValueDoesNotMakeFrameStoreDead) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    for (uint64_t FrameRegister : {TRI.StackPointer, TRI.FramePointer}) {
      for (bool ReadAfterCall : {false, true}) {
        for (uint64_t Argument : {5, 7}) {
          SCOPED_TRACE(static_cast<int>(Architecture));
          SCOPED_TRACE(FrameRegister);
          SCOPED_TRACE(ReadAfterCall);
          SCOPED_TRACE(Argument);
          MedFunc M;
          M.Entry = 0x1000;
          M.Name = "observable_frame_store";
          M.ReturnType = NdType::makeInt(8, false);
          auto Frame = machineValue(20, Architecture);
          Frame.Kind = MedVar::Reg;
          Frame.RegOff = FrameRegister;
          auto Return = machineValue(21, Architecture);
          Return.Kind = MedVar::Reg;
          Return.RegOff = TRI.IntReturnReg;
          MedBlock Block;
          Block.Id = 0;
          Block.StartAddr = M.Entry;
          Block.Ops.push_back(operation(NdOp::STORE, 0x1000, {},
                                        {Frame, MedVar::makeConst(99, 8)}));
          Block.Ops.push_back(operation(NdOp::STORE, 0x1004, {},
                                        {Frame, MedVar::makeConst(5, 8)}));
          auto Call = operation(NdOp::CALL, 0x1008, Return,
                                {MedVar::makeConst(0x2000, 8),
                                 MedVar::makeConst(Argument, 8), Frame});
          auto Hint = std::make_shared<SourceCallTypeHint>();
          Hint->Signature.ReturnType = M.ReturnType;
          Hint->Signature.Parameters = {
              {"value", M.ReturnType, {}},
              {"address", NdType::makePtr(M.ReturnType), {}}};
          Call.SourceCallHint = Hint;
          Block.Ops.push_back(Call);
          if (ReadAfterCall) {
            auto Loaded = Return;
            Loaded.Id = 22;
            Block.Ops.push_back(operation(NdOp::LOAD, 0x100c, Loaded, {Frame}));
            Return = Loaded;
          }
          Block.Ops.push_back(operation(NdOp::RETURN, 0x1010, {}, {Return}));
          M.Blocks.push_back(std::move(Block));
          const std::map<va_t, std::string> Names{{0x2000, "observe"}};
          MedToHighConverter Converter;
          Converter.setFuncNames(&Names);
          auto F = Converter.convert(M, Architecture);
          EXPECT_EQ(execute(F, 0), 5u);
        }
      }
    }
  }
}

MedFunc extensionConditionFunction(Arch Architecture, uint16_t FirstWidth,
                                   uint16_t SecondWidth, uint64_t Compared) {
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "extension_condition_widths";
  M.ReturnType = NdType::makeInt(8, false);
  auto P = machineValue(0, Architecture);
  P.Kind = MedVar::Param;
  P.Size = 1;
  P.RegOff = TRI.IntParamRegs[0];
  M.Params = {P};
  auto Frame = machineValue(20, Architecture);
  Frame.Kind = MedVar::Reg;
  Frame.RegOff = TRI.StackPointer;
  M.Blocks.resize(5);
  for (int I = 0; I < 5; ++I) {
    M.Blocks[I].Id = I;
    M.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    M.Blocks[I].EndAddr = M.Blocks[I].StartAddr + 0x40;
  }
  M.Blocks[0].Ops.push_back(
      operation(NdOp::STORE, 0x1000, {}, {Frame, MedVar::makeConst(0, 8)}));
  for (int I = 0; I < 2; ++I) {
    auto &Check = M.Blocks[I * 2];
    auto Extended = machineValue(1 + I * 2, Architecture);
    Extended.Size = I == 0 ? FirstWidth : SecondWidth;
    auto Condition = machineValue(2 + I * 2, Architecture);
    Condition.Size = 1;
    Check.Succs = {I * 2 + 1, I * 2 + 2};
    if (I)
      Check.Preds = {0, 1};
    Check.Ops.push_back(
        operation(NdOp::INT_SEXT, Check.StartAddr + 4, Extended, {P}));
    Check.Ops.push_back(
        operation(NdOp::INT_NOTEQUAL, Check.StartAddr + 8, Condition,
                  {Extended, MedVar::makeConst(Compared, Extended.Size)}));
    Check.Ops.push_back(
        operation(NdOp::COND_BR, Check.StartAddr + 12, {},
                  {MedVar::makeConst(Check.StartAddr + 0x200, 8), Condition}));
    auto &Body = M.Blocks[I * 2 + 1];
    Body.Preds = {I * 2};
    Body.Succs = {I * 2 + 2};
    Body.Ops.push_back(operation(NdOp::STORE, Body.StartAddr, {},
                                 {Frame, MedVar::makeConst(I + 1, 8)}));
    Body.Ops.push_back(
        operation(NdOp::BRANCH, Body.StartAddr + 4, {},
                  {MedVar::makeConst(Body.StartAddr + 0x100, 8)}));
  }
  auto R = machineValue(10, Architecture);
  R.Kind = MedVar::Reg;
  R.RegOff = TRI.IntReturnReg;
  auto &Exit = M.Blocks[4];
  Exit.Preds = {2, 3};
  Exit.Ops = {operation(NdOp::LOAD, 0x1400, R, {Frame}),
              operation(NdOp::RETURN, 0x1404, {}, {R})};
  return M;
}

TEST(HighControlFlowSemantics, ExtensionWidthsKeepConditionsDistinct) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (auto [FirstWidth, SecondWidth] :
         {std::pair<uint16_t, uint16_t>{2, 4}, {4, 8}, {2, 8}, {4, 4}})
      for (bool Reverse : {false, true}) {
        if (Reverse)
          std::swap(FirstWidth, SecondWidth);
        const uint64_t Compared =
            llvm::APInt::getAllOnes(std::min(FirstWidth, SecondWidth) * 8)
                .getZExtValue();
        auto F = MedToHighConverter().convert(
            extensionConditionFunction(Architecture, FirstWidth, SecondWidth,
                                       Compared),
            Architecture);
        for (unsigned Input = 0; Input < 256; ++Input) {
          SCOPED_TRACE(static_cast<int>(Architecture));
          SCOPED_TRACE(FirstWidth);
          SCOPED_TRACE(SecondWidth);
          SCOPED_TRACE(Input);
          const llvm::APInt Value(8, Input);
          uint64_t Expected =
              Value.sext(FirstWidth * 8).getZExtValue() == Compared ? 1 : 0;
          if (Value.sext(SecondWidth * 8).getZExtValue() == Compared)
            Expected = 2;
          EXPECT_EQ(execute(F, Input), Expected);
        }
      }
}

TEST(HighControlFlowSemantics, StructuralEqualityIncludesExpressionWidth) {
  auto Value = HighExpr::makeConst(0xff, 1);
  auto First = HighExpr::makeUnary(NdOp::INT_SEXT, Value);
  auto Second = HighExpr::makeUnary(NdOp::INT_SEXT, Value);
  First->Type = NdType::makeInt(4);
  Second->Type = NdType::makeInt(4);
  EXPECT_TRUE(First->structuralEq(*Second));
  Second->Type = NdType::makeInt(8);
  EXPECT_FALSE(First->structuralEq(*Second));
  EXPECT_FALSE(HighExpr::makeConst(0xff, 1)->structuralEq(
      *HighExpr::makeConst(0xff, 4)));
}

TEST(HighControlFlowSemantics, TypedViewsStillReferenceTheirVariable) {
  for (auto Cleanup : {elimConsecutiveDeadStores, postRenameCleanup})
    for (bool HasType : {false, true}) {
      HighFunc F;
      auto View = local(1);
      View->Var.Size = 4;
      View->Type = HasType ? NdType::makeInt(4) : nullptr;
      auto Update = assign(0x1004, 1, 0);
      Update.Val =
          HighExpr::makeBinop(NdOp::INT_ADD, View, HighExpr::makeConst(9, 4));
      F.Body = {assign(0x1000, 1, 7), Update, result(0x1008, local(1))};
      ASSERT_EQ(execute(F, 0), 16u);
      Cleanup(F.Body);
      EXPECT_NO_THROW({ EXPECT_EQ(execute(F, 0), 16u); });
    }
}
} // namespace
