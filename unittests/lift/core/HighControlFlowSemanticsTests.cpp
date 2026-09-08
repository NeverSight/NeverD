#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/MedToHigh.h"

#include <optional>
#include <stdexcept>

namespace neverd {
void structureIfElse(HighFunc &, int, const MedFunc * = nullptr);
void inlineSingleDefSingleUse(std::vector<HighStmt> &);
void resolveRegAliases(std::vector<HighStmt> &);
void eliminateRegAliasCopies(HighFunc &);
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
  std::function<uint64_t(const ExprPtr &)> Value = [&](const ExprPtr &E) {
    if (E->Kind == ExprKind::Const)
      return E->ConstVal;
    if (E->Kind == ExprKind::Var) {
      auto I = Values.find(varKey(E->Var));
      if (I == Values.end()) {
        std::string Description = "undefined " + E->str() + " identity " +
                                  std::to_string(E->Var.Id) + ":" +
                                  std::to_string(E->Var.SSAVer) + "\n";
        for (const auto &S : F.Body)
          Description += S.str() + "\n";
        throw std::runtime_error(Description);
      }
      return I->second;
    }
    if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT)
      return uint64_t(!Value(E->Operands.at(0)));
    if (E->Kind == ExprKind::UnaryOp &&
        (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)) {
      auto Input = E->Operands[0];
      auto Bits = unsigned(Input->Type->Size * 8);
      uint64_t V = Value(Input);
      if (Bits < 64) {
        auto Mask = (uint64_t(1) << Bits) - 1;
        V &= Mask;
        if (E->Op == NdOp::INT_SEXT && (V & (uint64_t(1) << (Bits - 1))))
          V |= ~Mask;
      }
      return V;
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
      if (S.Kind == StmtKind::Assign)
        Values[varKey(S.Dst->Var)] = Value(S.Val);
      if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) {
        auto R = Run(Value(S.Cond) ? S.Body : S.ElseBody);
        if (R.Return || R.Target || R.Break || R.Continue)
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
} // namespace
