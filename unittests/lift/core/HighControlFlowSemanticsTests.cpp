#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/ADT/APInt.h"

#include <optional>
#include <stdexcept>

namespace neverd {
void structureIfElse(HighFunc &, int, const MedFunc * = nullptr);
void detectAndConvertLoops(HighFunc &, const std::unordered_map<va_t, int> &,
                           const MedFunc &, bool);
void inlineSingleDefSingleUse(std::vector<HighStmt> &);
void resolveRegAliases(std::vector<HighStmt> &);
void simplifyAllExprs(std::vector<HighStmt> &);
void recoverSwitchStatements(HighFunc &);
void removeUnreachableCode(std::vector<HighStmt> &);
void eliminateRegAliasCopies(HighFunc &);
void elimConsecutiveDeadStores(std::vector<HighStmt> &);
void postRenameCleanup(std::vector<HighStmt> &);
void renameVars(std::vector<HighStmt> &);
void eliminateUnusedValues(std::vector<HighStmt> &);
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
std::optional<uint64_t> execute(const HighFunc &F, uint64_t Condition,
                                bool RequireExactTargets = false) {
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
      if (E->Op == NdOp::BOOL_AND)
        return uint64_t(Value(E->Operands[0]) && Value(E->Operands[1]));
      if (E->Op == NdOp::BOOL_OR)
        return uint64_t(Value(E->Operands[0]) || Value(E->Operands[1]));
      auto A = Value(E->Operands[0]), B = Value(E->Operands[1]);
      switch (E->Op) {
      case NdOp::INT_ADD:
        return A + B;
      case NdOp::INT_SUB:
        return A - B;
      case NdOp::INT_MULT:
        return A * B;
      case NdOp::INT_AND:
        return A & B;
      case NdOp::INT_EQUAL:
        return uint64_t(A == B);
      case NdOp::INT_NOTEQUAL:
        return uint64_t(A != B);
      case NdOp::SUBBYTES:
        return llvm::APInt(E->Operands[0]->Type->Size * 8, A)
            .lshr(B * 8)
            .zextOrTrunc(E->Type->Size * 8)
            .getZExtValue();
      case NdOp::CONCAT:
        return llvm::APInt(E->Operands[0]->Type->Size * 8, A)
            .concat(llvm::APInt(E->Operands[1]->Type->Size * 8, B))
            .getZExtValue();
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
      if (S.Kind == StmtKind::Block) {
        auto R = Run(S.Body);
        if (R.Return || R.Target || R.Break || R.Continue)
          return R;
      }
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
  for (unsigned Steps = 0; Steps != 10000 && Position < F.Body.size();
       ++Steps) {
    auto R = Run({F.Body[Position]});
    if (R.Return)
      return R.Return;
    if (!R.Target) {
      ++Position;
      continue;
    }
    auto I = std::find_if(F.Body.begin(), F.Body.end(),
                          [&](const auto &S) { return S.Addr == R.Target; });
    // Handwritten pass inputs can identify a removed instruction inside an
    // address range. Complete lowering must instead preserve an exact entry,
    // including when blocks are emitted in a different physical order.
    if (I == F.Body.end() && !RequireExactTargets)
      I = std::find_if(F.Body.begin(), F.Body.end(),
                       [&](const auto &S) { return S.Addr >= R.Target; });
    if (I == F.Body.end())
      return {};
    Position = I - F.Body.begin();
  }
  return {};
}

HighFunc guardedPhiCopy() {
  HighFunc F;
  HighStmt Define;
  Define.Kind = StmtKind::IfElse;
  Define.Cond = local(0);
  Define.Body = {assign(0x1004, 1, 42)};
  auto Copy = assign(0x1008, 1, 0);
  Copy.Val = local(2);
  Copy.IsPhiCopy = true;
  Define.ElseBody = {Copy};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Cond = local(0);
  Use.Body = {result(0x1010, local(1))};
  F.Body = {Define, Use, result(0x1014, HighExpr::makeConst(7, 8))};
  return F;
}

TEST(HighControlFlowSemantics, DeadGuardedPhiReadIsRemovedBeforeExecution) {
  auto F = guardedPhiCopy();
  EXPECT_THROW(execute(F, 0), std::runtime_error);
  ASSERT_TRUE(eliminateHighDeadPhiCopies(F));
  for (uint64_t Condition :
       {uint64_t{0}, uint64_t{1}, uint64_t{2}, ~uint64_t{0}})
    EXPECT_EQ(execute(F, Condition), Condition ? 42u : 7u);
}

TEST(HighControlFlowSemantics, RewrittenGuardKeepsTheReachingPhiValue) {
  auto F = guardedPhiCopy();
  F.Body.insert(F.Body.begin(), assign(0, 2, 19));
  F.Body.insert(F.Body.begin() + 2, assign(0, 0, 1));
  ASSERT_EQ(execute(F, 0), 19u);
  ASSERT_EQ(execute(F, 1), 42u);
  EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
  EXPECT_EQ(execute(F, 0), 19u);
  EXPECT_EQ(execute(F, 1), 42u);
}

HighFunc relationalPhiCopy(bool Swapped, bool Inverted) {
  auto F = guardedPhiCopy();
  F.Body[0].Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), local(3));
  F.Body[1].Cond =
      HighExpr::makeBinop(Inverted ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL,
                          local(Swapped ? 3 : 0), local(Swapped ? 0 : 3));
  if (Inverted) {
    F.Body[1].Kind = StmtKind::IfElse;
    F.Body[1].ElseBody = std::move(F.Body[1].Body);
    F.Body[1].Body.clear();
  }
  F.Body.insert(F.Body.begin(), assign(0, 3, 7));
  return F;
}

TEST(HighControlFlowSemantics, RepeatedEqualityRemovesOnlyDeadEdgeCopies) {
  for (bool Swapped : {false, true})
    for (bool Inverted : {false, true}) {
      auto F = relationalPhiCopy(Swapped, Inverted);
      EXPECT_THROW(execute(F, 0), std::runtime_error);
      ASSERT_TRUE(eliminateHighDeadPhiCopies(F));
      for (uint64_t Value :
           {uint64_t{0}, uint64_t{7}, uint64_t{19}, UINT64_MAX})
        EXPECT_EQ(execute(F, Value), Value == 7 ? 42u : 7u);
    }
}

TEST(HighControlFlowSemantics, EitherEqualityOperandWriteInvalidatesTheFact) {
  for (unsigned Operand : {0U, 3U})
    for (bool Swapped : {false, true}) {
      auto F = relationalPhiCopy(Swapped, false);
      F.Body.insert(F.Body.begin() + 2, assign(0, Operand, Operand ? 0 : 7));
      F.Body.insert(F.Body.begin(), assign(0, 2, 19));
      const auto Before = execute(F, 0);
      ASSERT_EQ(Before, 19u);
      EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
      EXPECT_EQ(execute(F, 0), Before);
    }
}

TEST(HighControlFlowSemantics, EscapedEqualityOperandsDoNotCarryFacts) {
  for (unsigned Operand : {0U, 3U}) {
    auto F = relationalPhiCopy(false, false);
    auto Address = std::make_shared<HighExpr>();
    Address->Kind = ExprKind::Addr;
    Address->Type = NdType::makePtr();
    Address->Operands = {local(Operand)};
    HighStmt Escape;
    Escape.Kind = StmtKind::Call;
    Escape.CallExpr = HighExpr::makeCall("mutate", 0x5000, {Address});
    F.Body.insert(F.Body.begin() + 2, Escape);
    EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
  }
}

TEST(HighControlFlowSemantics, EqualityFactsRequireConsistentWidths) {
  auto F = relationalPhiCopy(false, false);
  for (auto &Operand : F.Body[2].Cond->Operands) {
    Operand->Type = NdType::makeInt(4);
    Operand->Var.Size = 4;
  }
  EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
}

TEST(HighControlFlowSemantics, EqualityViewsCannotHideNarrowingOrPromotion) {
  for (unsigned Bytes : {1U, 2U, 4U}) {
    auto F = relationalPhiCopy(false, false);
    auto View = std::make_shared<HighExpr>();
    View->Kind = ExprKind::Cast;
    View->Type = View->CastTo = NdType::makeInt(Bytes, false);
    View->Operands = {F.Body[2].Cond->Operands[0]};
    F.Body[2].Cond->Operands[0] = View;
    EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
    if (Bytes < 4) {
      // Even an equal-width cast changes C's integer promotion of a negative
      // byte or short. It cannot borrow a signed comparison's edge fact.
      for (unsigned Statement : {1U, 2U})
        for (auto &Operand : F.Body[Statement].Cond->Operands) {
          auto Base =
              Operand->Kind == ExprKind::Cast ? Operand->Operands[0] : Operand;
          Base->Type = NdType::makeInt(Bytes);
          Base->Var.Size = Bytes;
        }
      EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
    }
  }
}

ExprPtr byteSlice(ExprPtr Base, unsigned Offset, unsigned Size) {
  auto Slice = HighExpr::makeBinop(NdOp::SUBBYTES, std::move(Base),
                                   HighExpr::makeConst(Offset, 4));
  Slice->Type = NdType::makeInt(Size, false);
  return Slice;
}

ExprPtr concatenate(ExprPtr High, ExprPtr Low) {
  const auto Bytes = High->Type->Size + Low->Type->Size;
  auto Joined =
      HighExpr::makeBinop(NdOp::CONCAT, std::move(High), std::move(Low));
  Joined->Type = NdType::makeInt(Bytes, false);
  return Joined;
}

TEST(HighControlFlowSemantics, AdjacentLocalSlicesPreserveEveryBit) {
  for (unsigned Offset : {0U, 1U, 2U})
    for (unsigned Bytes : {2U, 4U, 8U}) {
      if (Offset + Bytes > 8)
        continue;
      auto Joined = byteSlice(local(0), Offset, 1);
      for (unsigned Size = 1; Size < Bytes; Size *= 2)
        Joined = concatenate(byteSlice(local(0), Offset + Size, Size), Joined);
      HighFunc F;
      F.Body = {result(0, Joined)};
      std::vector<uint64_t> Inputs{0, UINT64_MAX, UINT64_C(0x8765432101234567)};
      for (unsigned Bit = 0; Bit != 64; ++Bit)
        Inputs.push_back(uint64_t{1} << Bit);
      std::vector<std::optional<uint64_t>> Expected;
      for (auto Input : Inputs)
        Expected.push_back(execute(F, Input));
      simplifyAllExprs(F.Body);
      EXPECT_NE(F.Body[0].RetVal->Op, NdOp::CONCAT);
      EXPECT_FALSE(F.Body[0].RetVal->Type->IsSigned);
      for (size_t I = 0; I != Inputs.size(); ++I)
        EXPECT_EQ(execute(F, Inputs[I]), Expected[I]);
    }
}

TEST(HighControlFlowSemantics, SliceJoiningRejectsDifferentOrObservableValues) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    auto High = byteSlice(local(0), 4, 4);
    auto Low = byteSlice(local(0), 0, 4);
    if (Case == 0)
      High->Operands[1] = HighExpr::makeConst(3, 4); // Overlap.
    if (Case == 1)
      Low->Type = NdType::makeInt(2, false); // Gap.
    if (Case == 2)
      High->Operands[0] = local(1);
    if (Case == 3)
      std::swap(High, Low);
    if (Case >= 4) {
      auto Base = Case == 4 ? HighExpr::makeLoad(local(0), NdType::makeInt(8))
                            : HighExpr::makeCall("next_value", 0x4000, {});
      Base->Type = NdType::makeInt(8);
      High->Operands[0] = Low->Operands[0] = Base;
    }
    HighFunc F;
    F.Body = {result(0, concatenate(High, Low))};
    simplifyAllExprs(F.Body);
    EXPECT_EQ(F.Body[0].RetVal->Op, NdOp::CONCAT) << Case;
  }
}

TEST(HighControlFlowSemantics, LowSlicesIgnoreOnlyUnobservedConcatPadding) {
  for (unsigned LowBytes : {1U, 2U, 4U}) {
    for (unsigned HighBytes : {1U, 2U, 4U}) {
      auto High = byteSlice(local(0), 0, HighBytes);
      auto Low = byteSlice(local(0), HighBytes, LowBytes);
      auto Joined = concatenate(High, Low);
      HighFunc Function;
      Function.Body = {result(0, byteSlice(Joined, 0, LowBytes))};
      const auto Before = Function;
      simplifyAllExprs(Function.Body);
      EXPECT_EQ(Function.Body[0].RetVal, Low);
      for (unsigned Bit = 0; Bit != 64; ++Bit) {
        const auto Input = uint64_t{1} << Bit;
        EXPECT_EQ(execute(Function, Input), execute(Before, Input));
        EXPECT_EQ(execute(Function, ~Input), execute(Before, ~Input));
      }
    }
  }
  for (const bool Cast : {false, true}) {
    auto Unknown = std::make_shared<HighExpr>();
    Unknown->Kind = ExprKind::Undef;
    Unknown->Type = NdType::makeInt(4, false);
    auto Low = byteSlice(local(0), 0, 4);
    auto Joined = concatenate(Unknown, Low);
    auto View = byteSlice(Joined, 0, 4);
    if (Cast) {
      View->Kind = ExprKind::Cast;
      View->CastTo = View->Type;
      View->Operands = {Joined};
    }
    HighFunc Function;
    Function.Body = {result(0, View), result(0, Joined)};
    simplifyAllExprs(Function.Body);
    EXPECT_EQ(Function.Body[0].RetVal, Low);
    EXPECT_EQ(Function.Body[1].RetVal, Joined);
    EXPECT_EQ(Joined->Operands[0], Unknown);
  }
}

TEST(HighControlFlowSemantics, VectorCarrierSlicesKeepOnlyProvenLowBytes) {
  for (unsigned LowBytes : {4U, 8U})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation)
      for (const bool Cast : {false, true}) {
        auto High = HighExpr::makeUndef(16 - LowBytes);
        auto Low = HighExpr::makeCall("low_effect", 0x4000, {});
        Low->Type = NdType::makeInt(LowBytes, Mutation == 8);
        if (Mutation == 1)
          High = HighExpr::makeLoad(local(0), NdType::makeInt(16 - LowBytes));
        if (Mutation == 2) {
          High = HighExpr::makeCall("high_effect", 0x5000, {});
          High->Type = NdType::makeInt(16 - LowBytes);
        }
        if (Mutation == 3)
          High->MemoryOrdering = NdMemoryOrdering::Acquire;
        if (Mutation == 4)
          High->IntrinsicOutputs = {local(2)->Var};
        auto Joined = concatenate(High, Low);
        if (Mutation == 5)
          Joined->Type = NdType::makeInt(15, false);
        auto View = byteSlice(Joined, 0, Mutation == 6 ? 16 : LowBytes);
        if (Cast) {
          View->Kind = ExprKind::Cast;
          View->CastTo = View->Type;
          View->Operands = {Joined};
        }
        if (Mutation == 7) {
          if (Cast)
            View->CastTo = NdType::makeFloat(LowBytes);
          else
            View->Operands[1]->ConstVal = 1;
        }
        HighFunc F;
        F.Body = {result(0, View), result(0, Joined)};
        simplifyAllExprs(F.Body);
        if (!Mutation)
          EXPECT_EQ(F.Body[0].RetVal, Low);
        else if (Mutation == 8) {
          ASSERT_EQ(F.Body[0].RetVal->Kind, ExprKind::Cast);
          EXPECT_EQ(F.Body[0].RetVal->Operands[0], Low);
          EXPECT_EQ(F.Body[0].RetVal->CastTo->Size, LowBytes);
          EXPECT_FALSE(F.Body[0].RetVal->CastTo->IsSigned);
        } else
          EXPECT_NE(F.Body[0].RetVal, Low) << Mutation;
        EXPECT_EQ(F.Body[1].RetVal, Joined);
        EXPECT_EQ(Joined->Operands[0], High);
        EXPECT_EQ(Joined->Operands[1], Low);
      }
}

TEST(HighControlFlowSemantics, LowSliceFoldingKeepsEffectsAndObservedUnknowns) {
  for (unsigned Case = 0; Case < 7; ++Case) {
    auto High = byteSlice(local(0), 4, 4);
    auto Low = byteSlice(local(0), 0, 4);
    if (Case == 0 || Case == 1) {
      High = HighExpr::makeLoad(local(1), NdType::makeInt(4, false));
      if (Case == 1)
        High->MemoryOrdering = NdMemoryOrdering::Acquire;
    }
    if (Case == 2) {
      High = HighExpr::makeCall("effect", 0x4000, {});
      High->Type = NdType::makeInt(4, false);
    }
    if (Case == 3) {
      High =
          HighExpr::makeBinop(NdOp::INT_DIV, High, HighExpr::makeConst(0, 4));
      High->Type = NdType::makeInt(4, false);
    }
    auto Joined = concatenate(High, Low);
    auto View = byteSlice(Joined, Case == 4 ? 1 : 0, Case == 5 ? 8 : 4);
    if (Case == 6)
      Joined->Type = NdType::makeInt(4, false);
    HighFunc Function;
    Function.Body = {result(0, View)};
    simplifyAllExprs(Function.Body);
    EXPECT_NE(Function.Body[0].RetVal, Low) << Case;
  }
}

TEST(HighControlFlowSemantics, MaskedLowBitsDiscardOnlyUnobservedConcatHigh) {
  for (unsigned ViewCase = 0; ViewCase != 3; ++ViewCase) {
    auto Low = byteSlice(local(0), 0, 1);
    auto Joined = concatenate(HighExpr::makeUndef(7), Low);
    ExprPtr Value = Joined;
    const unsigned ResultBytes = ViewCase ? 4 : 8;
    if (ViewCase == 1) {
      auto Cast = std::make_shared<HighExpr>();
      Cast->Kind = ExprKind::Cast;
      Cast->Type = Cast->CastTo = NdType::makeInt(ResultBytes, false);
      Cast->Operands = {Joined};
      Value = std::move(Cast);
    } else if (ViewCase == 2)
      Value = byteSlice(Joined, 0, ResultBytes);
    auto Masked = HighExpr::makeBinop(
        NdOp::INT_AND, Value,
        HighExpr::makeConst(1, ViewCase == 1 ? 8 : ResultBytes));
    Masked->Type = NdType::makeInt(ResultBytes, false);
    HighFunc Function;
    Function.Body = {result(0, Masked)};

    simplifyAllExprs(Function.Body);

    ASSERT_EQ(Function.Body[0].RetVal, Masked);
    ASSERT_EQ(Masked->Operands[0]->Kind, ExprKind::UnaryOp);
    EXPECT_EQ(Masked->Operands[0]->Op, NdOp::INT_ZEXT);
    EXPECT_EQ(Masked->Operands[0]->Type->Size, ResultBytes);
    EXPECT_EQ(Masked->Operands[0]->Operands[0], Low);
    for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{2},
                           uint64_t{0xff}, UINT64_MAX})
      EXPECT_EQ(execute(Function, Input), Input & 1);
  }
}

TEST(HighControlFlowSemantics, NarrowBitTestDiscardsShiftedUnknownHigh) {
  for (unsigned ViewCase = 0; ViewCase != 2; ++ViewCase) {
    auto Low = byteSlice(local(0), 0, 1);
    auto Joined = concatenate(HighExpr::makeUndef(ViewCase ? 7 : 3), Low);
    ExprPtr Value = ViewCase ? byteSlice(Joined, 0, 4) : Joined;
    auto Shifted =
        HighExpr::makeBinop(NdOp::INT_RIGHT, Value, HighExpr::makeConst(0, 4));
    Shifted->Type = NdType::makeInt(4, false);
    auto Masked =
        HighExpr::makeBinop(NdOp::INT_AND, Shifted, HighExpr::makeConst(1, 4));
    Masked->Type = NdType::makeInt(ViewCase ? 4 : 1, false);
    HighFunc Function;
    Function.Body = {result(0, Masked)};

    simplifyAllExprs(Function.Body);

    ASSERT_EQ(Function.Body[0].RetVal, Masked);
    EXPECT_NE(Masked->Operands[0], Shifted) << ViewCase;
    for (uint64_t Input :
         {uint64_t{0}, uint64_t{1}, uint64_t{2}, uint64_t{0xff}, UINT64_MAX})
      EXPECT_EQ(execute(Function, Input), Input & 1) << ViewCase;
  }
}

TEST(HighControlFlowSemantics, ShiftedMaskKeepsObservedOrEffectfulHigh) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    ExprPtr High = HighExpr::makeUndef(3);
    if (Case == 1) {
      High = HighExpr::makeCall("effect", 0x4000, {});
      High->Type = NdType::makeInt(3, false);
    }
    auto Joined = concatenate(High, byteSlice(local(0), 0, 1));
    auto Shifted = HighExpr::makeBinop(
        NdOp::INT_RIGHT, Joined, HighExpr::makeConst(Case == 2 ? 1 : 0, 4));
    Shifted->Type = NdType::makeInt(4, false);
    auto Masked = HighExpr::makeBinop(
        NdOp::INT_AND, Shifted, HighExpr::makeConst(Case == 0 ? 0x100 : 1, 4));
    Masked->Type = NdType::makeInt(4, false);
    HighFunc Function;
    Function.Body = {result(0, Masked)};

    simplifyAllExprs(Function.Body);

    EXPECT_EQ(Masked->Operands[0], Shifted) << Case;
  }
}

TEST(HighControlFlowSemantics, MaskedConcatKeepsObservedOrEffectfulHigh) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    ExprPtr High = HighExpr::makeUndef(7);
    if (Case == 1) {
      High = HighExpr::makeCall("effect", 0x4000, {});
      High->Type = NdType::makeInt(7, false);
    }
    auto Joined = concatenate(High, byteSlice(local(0), 0, 1));
    auto Masked = HighExpr::makeBinop(
        NdOp::INT_AND, Joined,
        HighExpr::makeConst(Case == 0 ? 0x100 : 1, 8));
    Masked->Type = NdType::makeInt(8, false);
    if (Case == 2)
      Joined->Type = NdType::makeInt(7, false);
    HighFunc Function;
    Function.Body = {result(0, Masked)};

    simplifyAllExprs(Function.Body);

    EXPECT_EQ(Masked->Operands[0], Joined) << Case;
  }
}

TEST(HighControlFlowSemantics, ReconstructedEqualityKeepsOnlyFeasibleUses) {
  auto F = relationalPhiCopy(true, true);
  F.Body[2].Cond->Operands[1] =
      concatenate(byteSlice(local(0), 4, 4), byteSlice(local(0), 0, 4));
  EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
  simplifyAllExprs(F.Body);
  ASSERT_TRUE(eliminateHighDeadPhiCopies(F));
  for (uint64_t Value : {uint64_t{0}, uint64_t{7}, UINT64_MAX})
    EXPECT_EQ(execute(F, Value), Value == 7 ? 42u : 7u);
}

TEST(HighControlFlowSemantics, RetainedCopyKeepsDependenciesInEveryContext) {
  auto F = guardedPhiCopy();
  auto Copy = assign(0, 3, 0);
  Copy.Val = local(1);
  Copy.IsPhiCopy = true;
  F.Body.insert(F.Body.begin() + 1, Copy);
  F.Body[2].Body[0].RetVal = local(3);
  // Although the false path never uses local 3, its source assignment remains
  // because the true path does. Its RHS must remain valid in both contexts.
  EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
  EXPECT_THROW(execute(F, 0), std::runtime_error);
  EXPECT_EQ(execute(F, 1), 42u);
}

TEST(HighControlFlowSemantics, PhiAnnotationCannotDiscardObservableLoads) {
  auto F = guardedPhiCopy();
  auto &Copy = F.Body[0].ElseBody[0];
  Copy.Val =
      HighExpr::makeLoad(HighExpr::makeConst(0x4000, 8), NdType::makeInt(8));
  EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Kind, ExprKind::Load);
  Copy.Val = HighExpr::makeCall("side_effect", 0x5000, {});
  EXPECT_FALSE(eliminateHighDeadPhiCopies(F));
}

TEST(HighControlFlowSemantics, ConditionalDefaultKeepsItsExclusivePath) {
  for (bool PhiCopies : {false, true})
    for (uint64_t Lookup : {uint64_t{0}, uint64_t{9}}) {
      HighFunc F;
      auto Null = assign(0x1028, 3, 0);
      Null.IsPhiCopy = PhiCopies;
      auto Entry = conditional(0x1000, Null.Addr);
      Entry.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                       HighExpr::makeConst(0, 8));
      auto Existing = conditional(0x1014, 0x102c);
      Existing.Cond =
          HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(PhiCopies ? 1 : 3),
                              HighExpr::makeConst(0, 8));
      auto Copy = assign(0x1014, 3, 0);
      Copy.Val = local(1);
      Copy.IsPhiCopy = true;
      if (PhiCopies)
        Existing.Body.insert(Existing.Body.begin(), Copy);
      F.Body = {Entry, assign(0x1010, PhiCopies ? 1 : 3, Lookup), Existing,
                assign(0x1020, PhiCopies ? 2 : 3, 19)};
      if (PhiCopies) {
        auto Created = Copy;
        Created.Addr = 0x1024;
        Created.Val = local(2);
        F.Body.push_back(Created);
      }
      F.Body.push_back(jump(0x1024, 0x102c));
      F.Body.push_back(Null);
      F.Body.push_back(result(0x102c, local(3)));
      ASSERT_EQ(execute(F, 0), 0u);
      ASSERT_EQ(execute(F, 1), Lookup ? 9u : 19u);
      structureIfElse(F, 10);
      EXPECT_EQ(execute(F, 0), 0u);
      EXPECT_EQ(execute(F, 1), Lookup ? 9u : 19u);
    }
}

TEST(HighControlFlowSemantics, SharedMergeDoesNotHideAnotherFalseArmEntry) {
  HighFunc F;
  auto Enter = conditional(0x1004, 0x1020);
  Enter.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(0, 8));
  auto Skip = conditional(0x1010, 0x1040);
  Skip.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  auto Increment = assign(0x1020, 1, 0);
  Increment.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  F.Body = {
      assign(0x1000, 1, 5),    Enter, Skip, Increment, jump(0x1030, 0x1040),
      result(0x1040, local(1))};
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{2}})
    ASSERT_EQ(execute(F, Input), Input == 1 ? 5u : 6u);
  structureIfElse(F, 10);
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{2}})
    EXPECT_EQ(execute(F, Input), Input == 1 ? 5u : 6u);
}

TEST(HighControlFlowSemantics, NestedDiamondKeepsOuterSinglePredecessorEntry) {
  HighFunc F;
  F.Entry = 0x1000;
  auto Outer = conditional(0x1000, 0x1040);
  Outer.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                   HighExpr::makeConst(0, 8));
  auto Inner = conditional(0x1010, 0x1050);
  Inner.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                   HighExpr::makeConst(1, 8));
  F.Body = {Outer, Inner, assign(0x1020, 1, 2), jump(0x1030, 0x1060),
            assign(0x1040, 1, 1), jump(0x1044, 0x1060),
            assign(0x1050, 1, 0), result(0x1060, local(1))};

  MedFunc Med;
  Med.Entry = F.Entry;
  const va_t Starts[] = {0x1000, 0x1010, 0x1020, 0x1040, 0x1050, 0x1060};
  const va_t Ends[] = {0x1000, 0x1010, 0x1030, 0x1044, 0x1050, 0x1060};
  Med.Blocks.resize(6);
  for (int I = 0; I < 6; ++I) {
    auto &Block = Med.Blocks[I];
    Block.Id = I;
    Block.StartAddr = Starts[I];
    MedOp Last;
    Last.Addr = Ends[I];
    Last.Opcode = I < 2 ? NdOp::COND_BR
                  : I == 5 ? NdOp::RETURN
                           : NdOp::BRANCH;
    Block.Ops = {Last};
  }
  Med.Blocks[0].Succs = {1, 3};
  Med.Blocks[1].Preds = {0};
  Med.Blocks[1].Succs = {2, 4};
  Med.Blocks[2].Preds = {1};
  Med.Blocks[2].Succs = {5};
  Med.Blocks[3].Preds = {0};
  Med.Blocks[3].Succs = {5};
  Med.Blocks[4].Preds = {1};
  Med.Blocks[4].Succs = {5};
  Med.Blocks[5].Preds = {2, 3, 4};

  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{2}})
    ASSERT_EQ(execute(F, Input), Input == 0 ? 1u : Input == 1 ? 0u : 2u);
  structureIfElse(F, 10, &Med);
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{2}})
    EXPECT_EQ(execute(F, Input), Input == 0 ? 1u : Input == 1 ? 0u : 2u);
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
  Exit.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(1), HighExpr::makeConst(3, 8));
  Exit.Body = {Break};
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Addr = Loop.LoopHeaderAddr = 0x1050;
  Loop.Cond = HighExpr::makeConst(1, 1);
  Loop.Body = {Increment, Exit};
  F.Body = {
      conditional(0x1000, 0x1040), jump(0x1004, 0x1080),
      assign(0x1044, 1, 0),        Loop,
      result(0x1060, local(1)),    result(0x1080, HighExpr::makeConst(7, 8))};

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
                  : I == 1         ? NdOp::BRANCH
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
  EXPECT_TRUE(
      std::any_of(F.Body.front().Body.begin(), F.Body.front().Body.end(),
                  [](const HighStmt &S) { return S.Kind == StmtKind::While; }));
  expectUniqueGotoTargets(F);
}

TEST(HighControlFlowSemantics,
     UnprovenLoopPredecessorsCannotHideTheSharedTail) {
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
    EXPECT_TRUE(
        std::any_of(F.Body.begin(), F.Body.end(), [](const HighStmt &S) {
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
  Outer.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(0, 8));
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1004;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
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

std::pair<HighFunc, MedFunc> branchWithScalarContinuation(bool SharedHeader) {
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  auto Add = [](va_t Address, int Dst, int Src, uint64_t Increment) {
    auto S = assign(Address, Dst, 0);
    S.Val = HighExpr::makeBinop(NdOp::INT_ADD, local(Src),
                                HighExpr::makeConst(Increment, 8));
    return S;
  };
  HighStmt VectorLoop;
  VectorLoop.Kind = StmtKind::While;
  VectorLoop.Addr = VectorLoop.LoopHeaderAddr = 0x1040;
  VectorLoop.Cond = HighExpr::makeConst(1, 1);
  VectorLoop.Body = {Add(0x1044, 1, 1, 10), Break};
  auto Early = conditional(0x1050, 0x1120);
  Early.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Early.Body = {assign(0x1054, 4, 111), jump(0, 0x1120)};
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1004;
  Inner.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                                   HighExpr::makeConst(1, 8));
  Inner.Body = {assign(0x1020, 1, 100), VectorLoop, Early, assign(0x1058, 2, 0),
                jump(0, 0x1100)};
  Inner.ElseBody = {assign(0x1008, 1, 10), assign(0x100C, 2, 0),
                    jump(0x1010, 0x1100)};
  Inner.Body[3].IsPhiCopy = true;
  Inner.ElseBody[0].IsPhiCopy = true;
  Inner.ElseBody[1].IsPhiCopy = true;
  HighStmt Outer;
  Outer.Kind = StmtKind::IfElse;
  Outer.Addr = 0x1000;
  Outer.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(0, 8));
  Outer.ElseBody = {Inner};
  HighStmt ScalarLoop;
  ScalarLoop.Kind = StmtKind::While;
  ScalarLoop.Addr = ScalarLoop.LoopHeaderAddr = 0x1100;
  ScalarLoop.Cond = HighExpr::makeConst(1, 1);
  auto Exit = conditional(0x1114, 0);
  Exit.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(2), HighExpr::makeConst(2, 8));
  Exit.Body = {Break};
  ScalarLoop.Body = {Add(SharedHeader ? 0x1100 : 0x1104, 3, 1, 0),
                     Add(SharedHeader ? 0x1100 : 0x1108, 1, 3, 1),
                     Add(0x1110, 2, 2, 1), Exit};
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x1120;
  HighFunc F;
  F.Body = {Outer,      result(0x1080, HighExpr::makeConst(7, 8)),
            ScalarLoop, Add(0x111C, 4, 1, 0),
            Label,      result(0x1124, local(4))};

  MedFunc Med;
  Med.Blocks.resize(3);
  for (int I = 0; I < 3; ++I)
    Med.Blocks[I].Id = I;
  MedOp Branch;
  Branch.Opcode = NdOp::COND_BR;
  Branch.Addr = 0x1000;
  Med.Blocks[0].StartAddr = 0x0FFC;
  Med.Blocks[0].Ops = {Branch};
  Med.Blocks[0].Succs = {1, 2};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1080;
  Med.Blocks[1].StartAddr = 0x1070;
  Med.Blocks[1].Ops = {Return};
  Med.Blocks[1].Preds = {0};
  Med.Blocks[2].StartAddr = 0x1004;
  return {std::move(F), std::move(Med)};
}

TEST(HighControlFlowSemantics, NestedContinuationsPreserveBothLoopsAndReturns) {
  for (bool SharedHeader : {false, true}) {
    for (bool ReverseArms : {false, true}) {
      auto [F, Med] = branchWithScalarContinuation(SharedHeader);
      if (ReverseArms) {
        std::swap(F.Body[0].Body, F.Body[0].ElseBody);
        F.Body[0].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, F.Body[0].Cond);
      }
      const uint64_t Expected[] = {7, 12, 111, 112, 112};
      for (uint64_t Input = 0; Input < 5; ++Input)
        ASSERT_EQ(execute(F, Input), Expected[Input]);
      const size_t Before = statementCount(F);
      foldStructuredContinuations(F, &Med);
      for (uint64_t Input = 0; Input < 5; ++Input)
        EXPECT_EQ(execute(F, Input), Expected[Input]);
      size_t Loops = 0, Gotos = 0, PhiCopies = 0;
      walkStmts(F.Body, [&](const HighStmt &S) {
        Loops += S.Kind == StmtKind::While;
        Gotos += S.Kind == StmtKind::Goto;
        PhiCopies += S.IsPhiCopy;
      });
      EXPECT_EQ(Loops, 2u);
      EXPECT_EQ(Gotos, 0u);
      EXPECT_EQ(PhiCopies, 3u);
      EXPECT_LE(statementCount(F), Before);
      expectUniqueGotoTargets(F);
    }
  }
}

TEST(HighControlFlowSemantics, ContinuationFoldingPreservesExternalEntries) {
  for (va_t Entry : {0x1010, 0x1080, 0x1070}) {
    auto [F, Med] = branchWithScalarContinuation(false);
    HighStmt Dispatch;
    Dispatch.Kind = StmtKind::Switch;
    Dispatch.SwitchExpr = local(0);
    Dispatch.Cases.push_back({99, {jump(0, Entry)}});
    F.Body.insert(F.Body.begin(), Dispatch);
    foldStructuredContinuations(F, &Med);
    EXPECT_EQ(execute(F, 0), 7u);
    EXPECT_EQ(execute(F, 1), 12u);
    EXPECT_EQ(execute(F, 2), 111u);
    size_t Entries = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Entries +=
          S.Addr == Entry && (Entry == 0x1010 ? S.Kind == StmtKind::Goto
                                              : S.Kind == StmtKind::Return);
    });
    if (Entry == 0x1010)
      EXPECT_EQ(Entries, 1u);
    else {
      // The native block entry may already have been erased by DCE.
      EXPECT_TRUE(
          std::any_of(F.Body.begin(), F.Body.end(), [](const HighStmt &S) {
            return S.Kind == StmtKind::Return && S.Addr == 0x1080;
          }));
      EXPECT_EQ(execute(F, 99), 7u);
    }
  }
}

TEST(HighControlFlowSemantics, ContinuationFoldingKeepsDeadReturnEntryLabels) {
  for (bool SharedHeader : {false, true}) {
    for (bool ReverseArms : {false, true}) {
      auto [F, Med] = branchWithScalarContinuation(SharedHeader);
      HighStmt Label;
      Label.Kind = StmtKind::Block;
      Label.Addr = Med.Blocks[1].StartAddr;
      F.Body.insert(F.Body.begin() + 1, Label);
      if (ReverseArms) {
        std::swap(F.Body[0].Body, F.Body[0].ElseBody);
        F.Body[0].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, F.Body[0].Cond);
      }
      const uint64_t Expected[] = {7, 12, 111, 112, 112};
      for (uint64_t Input = 0; Input < 5; ++Input)
        ASSERT_EQ(execute(F, Input), Expected[Input]);
      foldStructuredContinuations(F, &Med);
      for (uint64_t Input = 0; Input < 5; ++Input)
        EXPECT_EQ(execute(F, Input), Expected[Input]);
      size_t Gotos = 0, Labels = 0;
      walkStmts(F.Body, [&](const HighStmt &S) {
        Gotos += S.Kind == StmtKind::Goto;
        Labels += S.Kind == StmtKind::Block && S.Addr == Label.Addr;
      });
      EXPECT_EQ(Gotos, 0u);
      EXPECT_EQ(Labels, 1u);
      expectUniqueGotoTargets(F);
    }
  }
}

TEST(HighControlFlowSemantics, ReturnEntryLabelsRequireExclusiveNativeOwner) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    auto [F, Med] = branchWithScalarContinuation(false);
    HighStmt Label;
    Label.Kind = StmtKind::Block;
    Label.Addr = Med.Blocks[1].StartAddr;
    if (Mutation == 0)
      F.Entry = Label.Addr;
    else if (Mutation == 1)
      Med.Entry = Label.Addr;
    else if (Mutation == 2)
      Label.Addr = 0x1074; // Not an operation or entry in the return block.
    else if (Mutation == 3)
      Label.Body = {assign(0x1074, 5, 42)};
    else if (Mutation == 4)
      Label.ElseBody = {assign(0x1074, 5, 42)};
    else if (Mutation == 5)
      F.Body.push_back(Label); // A duplicate is not an exclusive label.
    else {
      HighStmt Dispatch;
      Dispatch.Kind = StmtKind::Switch;
      Dispatch.SwitchExpr = local(0);
      Dispatch.Cases.push_back({99, {jump(0, Label.Addr)}});
      F.Body.push_back(Dispatch);
    }
    F.Body.insert(F.Body.begin() + 1, Label);
    foldStructuredContinuations(F, &Med);
    size_t Gotos = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Gotos += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1100;
    });
    EXPECT_EQ(Gotos, 1u) << Mutation;
    EXPECT_EQ(F.Body[1].Addr, Label.Addr) << Mutation;
    EXPECT_EQ(F.Body[2].Kind, StmtKind::Return) << Mutation;
  }
}

TEST(HighControlFlowSemantics, ContinuationFoldingRequiresExactLoopEntry) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto [F, Med] = branchWithScalarContinuation(true);
    auto &Loop = F.Body[2];
    if (Mutation == 0)
      Loop.Cond = local(0);
    else if (Mutation == 1)
      Loop.Body.insert(Loop.Body.begin(), assign(0x1104, 5, 42));
    else if (Mutation == 2)
      Loop.Body.push_back(assign(0x1100, 5, 42));
    else if (Mutation == 3)
      F.Body.push_back(assign(0x1100, 5, 42));
    else
      Loop.LoopHeaderAddr = 0x1104;
    foldStructuredContinuations(F, &Med);
    size_t Gotos = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Gotos += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1100;
    });
    EXPECT_EQ(Gotos, 1u) << Mutation;
    EXPECT_EQ(F.Body[1].Kind, StmtKind::Return) << Mutation;
  }
}

TEST(HighControlFlowSemantics, ContinuationFoldingRequiresNativeReturnOwner) {
  for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
    auto [F, Med] = branchWithScalarContinuation(false);
    if (Mutation == 0)
      Med.Blocks[1].Preds.push_back(2);
    else if (Mutation == 1)
      Med.Blocks[0].Succs = {2};
    else if (Mutation == 2)
      Med.Blocks[0].Id = 2;
    else if (Mutation == 3)
      Med.Blocks[0].Ops.clear();
    else if (Mutation == 4)
      Med.Blocks[0].Ops.back().Addr = 0x1004;
    else if (Mutation == 5)
      Med.Blocks[1].ExceptionalPreds.push_back({});
    else if (Mutation == 6)
      F.Entry = 0x1080;
    else if (Mutation == 7)
      Med.Entry = 0x1070;
    else
      Med.Entry = 0x1080;
    foldStructuredContinuations(F, &Med);
    EXPECT_EQ(F.Body[1].Kind, StmtKind::Return) << Mutation;
    EXPECT_EQ(F.Body[1].Addr, 0x1080u) << Mutation;
    for (uint64_t Input : {0, 1, 2, 3})
      EXPECT_EQ(execute(F, Input), Input == 0   ? 7u
                                   : Input == 1 ? 12u
                                   : Input == 2 ? 111u
                                                : 112u);
  }
  auto [F, Med] = branchWithScalarContinuation(false);
  F.Entry = 0x1080;
  foldStructuredContinuations(F);
  EXPECT_EQ(F.Body[1].Kind, StmtKind::Return);
  EXPECT_EQ(F.Body[1].Addr, 0x1080u);
}

TEST(HighControlFlowSemantics, ReturnContinuationRejectsEffectsAndAmbiguity) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto [F, Med] = branchWithScalarContinuation(false);
    if (Mutation == 0)
      F.Body[4].Body = {assign(0x1120, 4, 42)};
    else if (Mutation == 1)
      F.Body.back().RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x8000, 8),
                                                NdType::makeInt(8));
    else if (Mutation == 2)
      F.Body.push_back(assign(0x1120, 4, 42));
    else if (Mutation == 3)
      F.Body.back().RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
    else
      F.Body.insert(F.Body.begin() + 5, assign(0x1122, 4, 42));
    foldStructuredContinuations(F, &Med);
    size_t Gotos = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Gotos += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1120;
    });
    EXPECT_EQ(Gotos, 1u) << Mutation;
  }
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

TEST(HighControlFlowSemantics, FlagPhisKeepTheirReachingDefinitions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    MedFunc Med;
    Med.Entry = 0x1000;
    Med.Name = "flag_phi";
    Med.ReturnType = NdType::makeInt(8, false);
    auto Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = TRI.IntParamRegs[0];
    Med.Params = {Input};
    auto Incoming = machineValue(1, Architecture);
    Incoming.Kind = MedVar::Flag;
    Incoming.Size = 1;
    Incoming.RegOff = TRI.FlagZF;
    auto Joined = Incoming;
    Joined.SSAVer = 1;
    auto TrueReturn = machineValue(2, Architecture);
    TrueReturn.Kind = MedVar::Reg;
    TrueReturn.RegOff = TRI.IntReturnReg;
    auto FalseReturn = TrueReturn;
    FalseReturn.Id = 3;
    FalseReturn.SSAVer = 1;
    auto C = [](uint64_t Value) { return MedVar::makeConst(Value, 8); };

    Med.Blocks.resize(4);
    for (int I = 0; I != 4; ++I) {
      Med.Blocks[I].Id = I;
      Med.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      Med.Blocks[I].EndAddr = Med.Blocks[I].StartAddr + 0x10;
    }
    Med.Blocks[0].Succs = {1};
    Med.Blocks[0].Ops = {
        operation(NdOp::INT_EQUAL, 0x1000, Incoming, {Input, C(0)}),
        operation(NdOp::BRANCH, 0x1004, {}, {C(0x1100)})};
    Med.Blocks[1].Preds = {0};
    Med.Blocks[1].Succs = {2, 3};
    Med.Blocks[1].Phis = {{Joined, {{0, Incoming}}}};
    Med.Blocks[1].Ops = {
        operation(NdOp::COND_BR, 0x1100, {}, {C(0x1200), Joined})};
    Med.Blocks[2].Preds = {1};
    Med.Blocks[2].Ops = {
        operation(NdOp::COPY, 0x1200, TrueReturn, {C(1)}),
        operation(NdOp::RETURN, 0x1204, {}, {TrueReturn})};
    Med.Blocks[3].Preds = {1};
    Med.Blocks[3].Ops = {
        operation(NdOp::COPY, 0x1300, FalseReturn, {C(0)}),
        operation(NdOp::RETURN, 0x1304, {}, {FalseReturn})};

    const auto Function = MedToHighConverter().convert(Med, Architecture);
    const auto Flow = analyzeHighSourceFlow(Function, true);
    EXPECT_TRUE(Flow.Complete);
    EXPECT_TRUE(Flow.Items.empty());
    EXPECT_EQ(execute(Function, 0), 1U);
    EXPECT_EQ(execute(Function, 7), 0U);
  }
}

TEST(HighControlFlowSemantics, ConditionalFalseEdgeKeepsNonlexicalSuccessor) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    MedFunc Med;
    Med.Entry = 0x1000;
    Med.Name = "nonlexical_false_edge";
    Med.ReturnType = NdType::makeInt(8, false);
    auto Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
    Med.Params = {Input};
    auto First = machineValue(1, Architecture);
    auto Second = machineValue(2, Architecture);
    auto Zero = machineValue(3, Architecture);
    auto Bits = machineValue(4, Architecture);
    auto Join = machineValue(5, Architecture);
    auto Updated = machineValue(6, Architecture);
    auto Result = machineValue(7, Architecture);
    auto Return = machineValue(8, Architecture);
    Return.Kind = MedVar::Reg;
    Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    auto C = [](uint64_t Value) { return MedVar::makeConst(Value, 8); };
    Med.Blocks.resize(5);
    for (int I = 0; I < 5; ++I) {
      Med.Blocks[I].Id = I;
      Med.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      Med.Blocks[I].EndAddr = Med.Blocks[I].StartAddr + 0x20;
    }
    Med.Blocks[0].Succs = {1, 2};
    Med.Blocks[0].Ops = {
        operation(NdOp::INT_AND, 0x1000, First, {Input, C(2)}),
        operation(NdOp::INT_AND, 0x1004, Second, {Input, C(1)}),
        operation(NdOp::COND_BR, 0x1008, {}, {C(0x1200), First})};
    Med.Blocks[1].Preds = {0};
    Med.Blocks[1].Succs = {4, 3};
    Med.Blocks[1].Ops = {
        operation(NdOp::COPY, 0x1100, Zero, {C(0)}),
        operation(NdOp::COND_BR, 0x1104, {}, {C(0x1300), Second})};
    Med.Blocks[2].Preds = {0};
    Med.Blocks[2].Succs = {4, 3};
    Med.Blocks[2].Ops = {
        operation(NdOp::COPY, 0x1200, Bits, {C(2048)}),
        operation(NdOp::COND_BR, 0x1204, {}, {C(0x1300), Second})};
    Med.Blocks[3].Preds = {1, 2};
    Med.Blocks[3].Succs = {4};
    Med.Blocks[3].Phis = {{Join, {{1, Zero}, {2, Bits}}}};
    Med.Blocks[3].Ops = {
        operation(NdOp::INT_ADD, 0x1300, Updated, {Join, C(0x80000)})};
    Med.Blocks[4].Preds = {1, 2, 3};
    Med.Blocks[4].Phis = {{Result, {{1, Zero}, {2, Bits}, {3, Updated}}}};
    Med.Blocks[4].Ops = {operation(NdOp::COPY, 0x1400, Return, {Result}),
                         operation(NdOp::RETURN, 0x1404, {}, {Return})};
    for (bool ReverseSuccessors : {false, true}) {
      auto Variant = Med;
      if (ReverseSuccessors)
        for (auto &Block : Variant.Blocks)
          std::reverse(Block.Succs.begin(), Block.Succs.end());
      const auto Function = MedToHighConverter().convert(Variant, Architecture);
      for (uint64_t Value = 0; Value < 16; ++Value)
        EXPECT_EQ(execute(Function, Value),
                  (Value & 2 ? 2048u : 0u) + (Value & 1 ? 0x80000u : 0u))
            << "reverse successors=" << ReverseSuccessors << " input=" << Value;
    }
  }
}

TEST(HighControlFlowSemantics, LayoutBackwardJoinDoesNotBecomeALoop) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    MedFunc Med;
    Med.Entry = 0x1000;
    Med.Name = "late_branch_shared_return";
    Med.ReturnType = NdType::makeInt(8, false);
    auto Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
    Med.Params = {Input};
    auto Updated = machineValue(1, Architecture);
    auto Joined = machineValue(2, Architecture);
    auto Return = machineValue(3, Architecture);
    Return.Kind = MedVar::Reg;
    Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    auto C = [](uint64_t Value) { return MedVar::makeConst(Value, 8); };
    Med.Blocks.resize(3);
    for (int I = 0; I < 3; ++I) {
      Med.Blocks[I].Id = I;
      Med.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      Med.Blocks[I].EndAddr = Med.Blocks[I].StartAddr + 0x10;
    }
    Med.Blocks[0].Succs = {1, 2};
    Med.Blocks[0].Ops = {
        operation(NdOp::COND_BR, 0x1000, {}, {C(0x1200), Input})};
    Med.Blocks[1].Preds = {0, 2};
    Med.Blocks[1].Phis = {{Joined, {{0, C(7)}, {2, Updated}}}};
    Med.Blocks[1].Ops = {operation(NdOp::COPY, 0x1100, Return, {Joined}),
                         operation(NdOp::RETURN, 0x1104, {}, {Return})};
    Med.Blocks[2].Preds = {0};
    Med.Blocks[2].Succs = {1};
    Med.Blocks[2].Ops = {
        operation(NdOp::INT_ADD, 0x1200, Updated, {Input, C(9)}),
        operation(NdOp::BRANCH, 0x1204, {}, {C(0x1100)})};
    for (bool ReverseSuccessors : {false, true}) {
      auto Variant = Med;
      if (ReverseSuccessors)
        std::reverse(Variant.Blocks[0].Succs.begin(),
                     Variant.Blocks[0].Succs.end());
      auto F = MedToHighConverter().convert(Variant, Architecture);
      std::function<void(const std::vector<HighStmt> &)> Check =
          [&](const auto &Body) {
            for (const auto &S : Body) {
              EXPECT_NE(S.Kind, StmtKind::While);
              EXPECT_NE(S.Kind, StmtKind::DoWhile);
              Check(S.Body);
              Check(S.ElseBody);
            }
          };
      Check(F.Body);
      for (uint64_t Value : {UINT64_C(0), UINT64_C(1), UINT64_C(37),
                             UINT64_C(0x8000000000000000), UINT64_MAX})
        EXPECT_EQ(execute(F, Value), Value ? Value + 9 : 7);
    }
  }
}

TEST(HighControlFlowSemantics, EarlyReturnMovesItsCompletePhiEdgePrefix) {
  for (va_t CopyAddress : {0U, 0x1000U, 0x1004U}) {
    HighFunc F;
    auto Copy = assign(CopyAddress, 1, 7);
    Copy.IsPhiCopy = true;
    F.Body = {conditional(0x1000, 0x1030), Copy, result(0x1010, local(1)),
              assign(0x1030, 1, 9), result(0x1034, local(1))};
    ASSERT_EQ(execute(F, 0), 7U);
    ASSERT_EQ(execute(F, 1), 9U);
    structureIfElse(F, 4);
    EXPECT_EQ(execute(F, 0), 7U);
    EXPECT_EQ(execute(F, 1), 9U);
    ASSERT_EQ(F.Body.front().Kind, StmtKind::If);
    ASSERT_FALSE(F.Body.front().Body.empty());
    EXPECT_TRUE(F.Body.front().Body.front().IsPhiCopy);
  }
}

TEST(HighControlFlowSemantics, EarlyReturnKeepsTransferPastOtherBranchEntries) {
  HighFunc F;
  auto First = conditional(0x1000, 0x1030);
  First.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(0, 8));
  auto Second = conditional(0x1010, 0x1040);
  Second.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  F.Body = {First, Second, result(0x1020, HighExpr::makeConst(30, 8)),
            result(0x1030, HighExpr::makeConst(10, 8)),
            result(0x1040, HighExpr::makeConst(20, 8))};
  for (unsigned Passes : {1U, 4U, 16U}) {
    auto Structured = F;
    structureIfElse(Structured, Passes);
    for (uint64_t Input : {0U, 1U, 2U, 3U})
      EXPECT_EQ(execute(Structured, Input), execute(F, Input)) << Passes;
  }
}

TEST(HighControlFlowSemantics, NearbyNestedFallthroughKeepsExactGotoTarget) {
  HighFunc F;
  F.Entry = 0x1000;
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1010;
  Inner.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                   HighExpr::makeConst(1, 8));
  Inner.Body = {result(0x1014, HighExpr::makeConst(3, 8))};
  Inner.ElseBody = {jump(0x1018, 0x1048)};
  HighStmt Outer;
  Outer.Kind = StmtKind::IfElse;
  Outer.Addr = F.Entry;
  Outer.Cond = local(0);
  Outer.Body = {Inner};
  Outer.ElseBody = {result(0x1008, HighExpr::makeConst(4, 8))};
  F.Body = {Outer, result(0x1040, HighExpr::makeConst(1, 8)),
            result(0x1048, HighExpr::makeConst(2, 8))};

  ASSERT_EQ(execute(F, 0, true), 4U);
  ASSERT_EQ(execute(F, 1, true), 3U);
  ASSERT_EQ(execute(F, 2, true), 2U);
  invertSkipGotos(F);
  EXPECT_EQ(execute(F, 0, true), 4U);
  EXPECT_EQ(execute(F, 1, true), 3U);
  EXPECT_EQ(execute(F, 2, true), 2U);
}

TEST(HighControlFlowSemantics, InlinedElseJoinPreservesPhiCopyBeforeWork) {
  HighFunc F;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8);

  auto Phi = assign(0, 2, 23);
  Phi.IsPhiCopy = true;
  HighStmt Choice;
  Choice.Kind = StmtKind::IfElse;
  Choice.Addr = F.Entry;
  Choice.Cond = local(0);
  Choice.Body = {assign(0, 3, 11), jump(0, 0x3000)};
  Choice.ElseBody = {Phi, jump(0, 0x2000)};

  auto JoinWork = assign(0x2000, 3, 0);
  JoinWork.Val = HighExpr::makeBinop(NdOp::INT_ADD, local(2),
                                     HighExpr::makeConst(5, 8));
  // The intervening return makes 0x2000 an exclusive jump target rather
  // than the next fallthrough statement of the conditional.
  F.Body = {Choice, result(0x1100, HighExpr::makeConst(99, 8)), JoinWork,
            jump(0x2004, 0x3000), result(0x3000, local(3))};
  ASSERT_EQ(execute(F, 0, true), 28U);
  ASSERT_EQ(execute(F, 1, true), 11U);

  invertSkipGotos(F);
  ASSERT_EQ(F.Body.front().Kind, StmtKind::IfElse);
  ASSERT_FALSE(F.Body.front().ElseBody.empty());
  EXPECT_TRUE(F.Body.front().ElseBody.front().IsPhiCopy);
  unsigned OldTransfers = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    OldTransfers += S.Kind == StmtKind::Goto && S.GotoTarget == 0x2000;
  });
  EXPECT_EQ(OldTransfers, 0U);
  EXPECT_EQ(execute(F, 0, true), 28U);
  EXPECT_EQ(execute(F, 1, true), 11U);
}

TEST(HighControlFlowSemantics, ExternalSkipInvertKeepsTakenPhiCopy) {
  HighFunc F;
  F.Entry = 0x1000;
  auto Branch = conditional(0x1000, 0x1040);
  auto TakenCopy = assign(0x1000, 1, 7);
  TakenCopy.IsPhiCopy = true;
  Branch.Body.insert(Branch.Body.begin(), std::move(TakenCopy));
  F.Body = {Branch, assign(0x1010, 1, 9), jump(0x1014, 0x1030),
            result(0x1030, local(1)), result(0x1040, local(1))};

  ASSERT_EQ(execute(F, 0), 9U);
  ASSERT_EQ(execute(F, 1), 7U);
  invertSkipGotos(F);
  EXPECT_EQ(execute(F, 0), 9U);
  EXPECT_EQ(execute(F, 1), 7U);
}

TEST(HighControlFlowSemantics, ExternalSkipInvertKeepsSharedTailEntry) {
  HighFunc F;
  F.Entry = 0x1000;
  auto Shared = assign(0x1020, 1, 0);
  Shared.Val = HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                   HighExpr::makeConst(1, 8));
  F.Body = {assign(0x1000, 1, 0), conditional(0x1004, 0x1040),
            assign(0x1010, 1, 5), Shared, jump(0x1024, 0x1030),
            result(0x1030, local(1)), jump(0x1040, 0x1020)};

  ASSERT_EQ(execute(F, 0, true), 6U);
  ASSERT_EQ(execute(F, 1, true), 1U);
  invertSkipGotos(F);
  EXPECT_EQ(execute(F, 0, true), 6U);
  EXPECT_EQ(execute(F, 1, true), 1U);
}

TEST(HighControlFlowSemantics, Arm64NestedCleanupKeepsOuterJoinValue) {
  HighFunc F;
  F.Entry = 0x1000;
  auto Store = HighStmt{};
  Store.Kind = StmtKind::Store;
  Store.Addr = 0x1014;
  Store.StoreAddr = HighExpr::makeConst(0x2000, 8);
  Store.StoreVal = HighExpr::makeConst(42, 8);
  auto Load = assign(0x1018, 1, 0);
  Load.Val = HighExpr::makeLoad(HighExpr::makeConst(0x2000, 8),
                                NdType::makeInt(8));
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x1020;
  Inner.Cond = local(2);
  Inner.Body = {assign(0x1024, 3, 1)};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1010;
  Outer.Cond = local(0);
  Outer.Body = {Store, Load, Inner};
  F.Body = {assign(0x1000, 1, 0), assign(0x1004, 2, 1), Outer,
            result(0x1040, local(1))};
  MedFunc Med;
  Med.CC = CallingConv::ARM_AAPCS;

  ASSERT_EQ(execute(F, 0), 0U);
  ASSERT_EQ(execute(F, 1), 42U);
  structureIfElse(F, 8, &Med);
  EXPECT_EQ(execute(F, 0), 0U);
  EXPECT_EQ(execute(F, 1), 42U);
}

TEST(HighControlFlowSemantics, MultiEntryCycleKeepsExplicitTransfers) {
  HighFunc F;
  F.Entry = 0x1000;
  F.Body = {conditional(0x1000, 0x1020), assign(0x1010, 1, 7),
            assign(0x1014, 0, 0),        jump(0x1018, 0x1020),
            conditional(0x1020, 0x1010), result(0x1030, local(1))};
  MedFunc Med;
  Med.Entry = F.Entry;
  Med.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    Med.Blocks[I].Id = I;
    Med.Blocks[I].StartAddr = 0x1000 + I * 0x10;
  }
  Med.Blocks[0].Succs = {1, 2};
  Med.Blocks[1].Succs = {2};
  Med.Blocks[2].Succs = {1, 3};
  for (const auto &S : F.Body) {
    MedOp Op;
    Op.Addr = S.Addr;
    Med.Blocks[(S.Addr - 0x1000) / 0x10].Ops.push_back(Op);
  }
  for (uint64_t Input : {0U, 1U, 19U})
    ASSERT_EQ(execute(F, Input), 7U);
  detectAndConvertLoops(F, {}, Med, false);
  for (const auto &S : F.Body)
    EXPECT_NE(S.Kind, StmtKind::While);
  for (uint64_t Input : {0U, 1U, 19U})
    EXPECT_EQ(execute(F, Input), 7U);
}

TEST(HighControlFlowSemantics, BranchStoresKeepTheirObservableContinuation) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    MedFunc M;
    M.Entry = 0x1000;
    M.Name = "store_before_tail_call";
    M.ReturnType = NdType::makeInt(8, false);
    auto Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
    M.Params = {Input};
    auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
    M.Blocks.resize(5);
    for (int I = 0; I < 5; ++I) {
      M.Blocks[I].Id = I;
      M.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      M.Blocks[I].EndAddr = M.Blocks[I].StartAddr + 0x20;
    }
    M.Blocks[0].Succs = {1, 2};
    M.Blocks[0].Ops = {
        operation(NdOp::STORE, 0x1000, {}, {C(0x8000), C(5)}),
        operation(NdOp::COND_BR, 0x1004, {}, {C(0x1200), Input})};
    M.Blocks[1].Preds = {0};
    M.Blocks[1].Succs = {3};
    M.Blocks[1].Ops = {operation(NdOp::STORE, 0x1100, {}, {C(0x8000), C(17)}),
                       operation(NdOp::BRANCH, 0x1104, {}, {C(0x1300)})};
    M.Blocks[2].Preds = {0};
    M.Blocks[2].Succs = {4};
    M.Blocks[2].Ops = {operation(NdOp::BRANCH, 0x1200, {}, {C(0x1400)})};
    for (int I = 3; I < 5; ++I) {
      auto &B = M.Blocks[I];
      B.Preds = {I - 2};
      auto Observed = machineValue(I, Architecture);
      auto Return = machineValue(I + 2, Architecture);
      Return.SSAVer = I;
      Return.Kind = MedVar::Reg;
      Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
      auto Call = operation(NdOp::CALL, B.StartAddr, Observed,
                            {C(0x2000), C(0), C(0x8000)});
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->Signature.ReturnType = M.ReturnType;
      Hint->Signature.Parameters = {
          {"unused", M.ReturnType, {}},
          {"address", NdType::makePtr(M.ReturnType), {}}};
      Call.SourceCallHint = Hint;
      B.Ops = {Call,
               operation(NdOp::INT_ADD, B.StartAddr + 4, Return,
                         {Observed, C((I - 2) * 100)}),
               operation(NdOp::RETURN, B.StartAddr + 8, {}, {Return})};
    }
    const std::map<va_t, std::string> Names{{0x2000, "observe"}};
    for (bool ReverseSuccessors : {false, true}) {
      auto Variant = M;
      if (ReverseSuccessors)
        std::reverse(Variant.Blocks[0].Succs.begin(),
                     Variant.Blocks[0].Succs.end());
      MedToHighConverter Converter;
      Converter.setFuncNames(&Names);
      auto F = Converter.convert(Variant, Architecture);
      EXPECT_EQ(execute(F, 0), 117u);
      EXPECT_EQ(execute(F, 1), 205u);
    }
  }
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

TEST(HighControlFlowSemantics, ConditionalLatchPreservesExitPhiAndBypass) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Med = loopFunction(Architecture, false, false);
    auto Condition = machineValue(10, Architecture);
    Condition.Size = 1;
    auto Output = machineValue(11, Architecture);
    auto &Entry = Med.Blocks[0];
    Entry.Succs = {1, 2};
    Entry.Ops = {operation(NdOp::INT_EQUAL, 0x1000, Condition,
                           {Med.Params[0], MedVar::makeConst(0, 8)}),
                 operation(NdOp::COND_BR, 0x1004, {},
                           {MedVar::makeConst(0x1200, 8), Condition})};
    auto &Exit = Med.Blocks[2];
    Exit.Preds = {0, 1};
    Exit.Phis = {
        {Output,
         {{0, MedVar::makeConst(99, 8)}, {1, machineValue(4, Architecture)}}}};
    Exit.Ops[0].Inputs[0] = Output;
    auto Function = MedToHighConverter().convert(Med, Architecture);
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      if (Statement.Kind == StmtKind::Goto)
        EXPECT_NE(Statement.GotoTarget, 0U);
    });
    for (unsigned Count = 0; Count != 9; ++Count)
      EXPECT_EQ(execute(Function, Count), Count ? 2 * Count + 1 : 99);
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

TEST(HighControlFlowSemantics,
     UnreachableCleanupDropsOnlyUnreferencedReturnsAfterSourceTraps) {
  for (const auto Id : {Intrinsic::Ud2, Intrinsic::ArmHlt, Intrinsic::Brk,
                        Intrinsic::Hlt_A64}) {
    HighStmt Trap;
    Trap.Kind = StmtKind::Call;
    Trap.Addr = 0x1000;
    Trap.CallExpr = HighExpr::makeCall("trap", 0, {});
    Trap.CallExpr->IntrinsicId = Id;
    auto UnknownReturn = result(0x1004, HighExpr::makeUndef(8));
    std::vector<HighStmt> Body{Trap, UnknownReturn};
    removeUnreachableCode(Body);
    ASSERT_EQ(Body.size(), 1u);
    EXPECT_EQ(Body.front().Kind, StmtKind::Call);

    Body = {conditional(0xffc, 0x1004), Trap, UnknownReturn};
    removeUnreachableCode(Body);
    ASSERT_EQ(Body.size(), 3u)
        << "a branch may enter the return after the trap";
  }

  HighStmt DebugTrap;
  DebugTrap.Kind = StmtKind::Call;
  DebugTrap.Addr = 0x1000;
  DebugTrap.CallExpr = HighExpr::makeCall("debug_trap", 0, {});
  DebugTrap.CallExpr->IntrinsicId = Intrinsic::Int3;
  std::vector<HighStmt> Body{DebugTrap, result(0x1004, HighExpr::makeUndef(8))};
  removeUnreachableCode(Body);
  EXPECT_EQ(Body.size(), 2u)
      << "a resumable debugger trap must retain its following return";
}

TEST(HighControlFlowSemantics,
     UnreachableCleanupPreservesIncomingTailBranches) {
  HighFunc F;
  auto First = conditional(0x1000, 0x1100);
  First.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  auto Second = conditional(0x1004, 0x1200);
  Second.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  F.Body = {First,
            Second,
            result(0x1008, HighExpr::makeConst(7, 8)),
            assign(0x1010, 1, 99),
            assign(0x1100, 1, 11),
            result(0x1104, local(1)),
            assign(0x1110, 1, 99),
            assign(0x1200, 1, 22),
            result(0x1204, local(1)),
            assign(0x1210, 1, 99)};
  for (uint64_t Input : {0, 1, 2})
    ASSERT_EQ(execute(F, Input), Input == 0 ? 7u : Input * 11);
  removeUnreachableCode(F.Body);
  expectUniqueGotoTargets(F);
  for (uint64_t Input : {0, 1, 2})
    EXPECT_EQ(execute(F, Input), Input == 0 ? 7u : Input * 11);
  walkStmts(F.Body, [&](const HighStmt &S) {
    EXPECT_NE(S.Addr, 0x1010u);
    EXPECT_NE(S.Addr, 0x1110u);
    EXPECT_NE(S.Addr, 0x1210u);
  });
}

TEST(HighControlFlowSemantics, UnreachableCleanupKeepsNestedIncomingEntries) {
  for (unsigned Edge = 0; Edge < 5; ++Edge) {
    SCOPED_TRACE(Edge);
    HighStmt Container;
    Container.Kind = StmtKind::IfElse;
    Container.Addr = 0x1100;
    Container.Cond = local(0);
    std::vector<HighStmt> Tail{result(0x1100, HighExpr::makeConst(1, 8)),
                               assign(0x1104, 1, 99), result(0x1108, local(1))};
    switch (Edge) {
    case 0:
      Container.Body = Tail;
      break;
    case 1:
      Container.ElseBody = Tail;
      break;
    case 2:
      Container.Kind = StmtKind::Switch;
      Container.SwitchExpr = local(0);
      Container.Cases.emplace_back();
      Container.Cases.back().Body = Tail;
      break;
    case 3:
      Container.Kind = StmtKind::Switch;
      Container.SwitchExpr = local(0);
      Container.DefaultBody = Tail;
      break;
    case 4:
      Container.Kind = StmtKind::CxxTry;
      Container.EHClauses.emplace_back();
      Container.EHClauses.back().Kind = HighEHClauseKind::CxxCatch;
      Container.EHClauseBodies.push_back(Tail);
      break;
    }
    HighFunc F;
    F.Body = {conditional(0x1000, 0x1104),
              result(0x1004, HighExpr::makeConst(7, 8)), Container};
    removeUnreachableCode(F.Body);
    expectUniqueGotoTargets(F);
    bool FoundValue = false;
    walkStmts(F.Body, [&](const HighStmt &S) {
      if (S.Addr == 0x1104) {
        ASSERT_EQ(S.Kind, StmtKind::Assign);
        ASSERT_TRUE(S.Val);
        EXPECT_EQ(S.Val->ConstVal, 99u);
        FoundValue = true;
      }
    });
    EXPECT_TRUE(FoundValue);
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

TEST(HighControlFlowSemantics, DeadValueCyclesAndLongChainsKeepBranchEntries) {
  HighFunc F;
  F.Body = {jump(0x1000, 0x1010), assign(0x1010, 1, 7)};
  for (unsigned I = 2; I <= 64; ++I) {
    auto Copy = assign(0x1010 + I * 4, I, 0);
    Copy.Val = local(I - 1);
    F.Body.push_back(Copy);
  }
  F.Body.push_back(assign(0x1200, 65, 1));
  F.Body.push_back(assign(0x1204, 66, 2));
  auto A = assign(0x1208, 65, 0);
  A.Val = local(66);
  auto B = assign(0x120c, 66, 0);
  B.Val = local(65);
  F.Body.push_back(A);
  F.Body.push_back(B);
  F.Body.push_back(result(0x1300, HighExpr::makeConst(73, 8)));
  EXPECT_EQ(execute(F, 0), 73U);
  eliminateUnusedValues(F.Body);
  EXPECT_EQ(execute(F, 0), 73U);
  expectUniqueGotoTargets(F);
  walkStmts(F.Body,
            [&](const HighStmt &S) { EXPECT_NE(S.Kind, StmtKind::Assign); });
}

TEST(HighControlFlowSemantics, StackCheckNameDoesNotAuthorizeDeletingCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    MedFunc M;
    M.Name = "observable_named_call";
    M.Entry = 0x1000;
    M.ReturnType = NdType::makeInt(8, false);
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = M.Entry;
    auto Call =
        operation(NdOp::CALL, 0x1000, {}, {MedVar::makeConst(0x2000, 8)});
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->Signature.ReturnType = NdType::makeVoid();
    Call.SourceCallHint = Hint;
    Block.Ops = {
        Call, operation(NdOp::RETURN, 0x1004, {}, {MedVar::makeConst(73, 8)})};
    M.Blocks = {Block};
    for (const char *Name : {"__stack_chk_fail", "app_stack_chk_fail_audit"}) {
      const std::map<va_t, std::string> Names{{0x2000, Name}};
      MedToHighConverter Converter;
      Converter.setFuncNames(&Names);
      auto F = Converter.convert(M, Architecture);
      unsigned Calls = 0;
      walkStmts(F.Body, [&](const HighStmt &S) {
        forEachExpr(S, [&](const ExprPtr &E) {
          if (E && E->Kind == ExprKind::Call && E->CallTarget == Name)
            ++Calls;
        });
      });
      EXPECT_EQ(Calls, 1U) << Name;
    }
  }
}

TEST(HighControlFlowSemantics, LiveValueCyclesAndNestedEffectsSurviveDCE) {
  for (unsigned Sink = 0; Sink < 5; ++Sink) {
    SCOPED_TRACE(Sink);
    HighFunc F;
    F.Body = {assign(0x1000, 1, 7), assign(0x1004, 2, 9)};
    auto A = assign(0x1008, 1, 0);
    A.Val = local(2);
    auto B = assign(0x100c, 2, 0);
    B.Val = local(1);
    F.Body.push_back(A);
    F.Body.push_back(B);
    auto Root = result(0x1010, local(2));
    if (Sink == 1) {
      Root = conditional(0x1010, 0x1020);
      Root.Cond = local(2);
    } else if (Sink == 2) {
      Root = assign(0x1010, 3, 0);
      Root.Val = HighExpr::makeBinop(
          NdOp::INT_ADD, HighExpr::makeCall("effect", 0x2000, {local(2)}),
          HighExpr::makeConst(1, 8));
    } else if (Sink == 3) {
      Root = assign(0x1010, 3, 0);
      Root.Val = HighExpr::makeLoad(local(2), NdType::makeInt(8),
                                    NdMemoryOrdering::Acquire);
    } else if (Sink == 4) {
      Root.Kind = StmtKind::Store;
      Root.RetVal.reset();
      Root.StoreAddr = local(2);
      Root.StoreVal = HighExpr::makeConst(1, 8);
    }
    F.Body.push_back(Root);
    F.Body.push_back(result(0x1020, HighExpr::makeConst(73, 8)));
    eliminateUnusedValues(F.Body);
    EXPECT_EQ(F.Body.size(), 6U);
    if (Sink == 1)
      expectUniqueGotoTargets(F);
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

TEST(HighControlFlowSemantics, IndirectCallTargetReceivesRegisterRename) {
  MedVar Receiver;
  Receiver.Kind = MedVar::Reg;
  Receiver.Id = 31;
  Receiver.SSAVer = 2;
  Receiver.Size = 8;
  Receiver.RegOff = 0;

  HighStmt Definition;
  Definition.Kind = StmtKind::Assign;
  Definition.Addr = 0x1000;
  Definition.Dst = HighExpr::makeVar(Receiver);
  Definition.Val = HighExpr::makeCall("acquire", 0x1000, {});

  HighStmt Use;
  Use.Kind = StmtKind::Call;
  Use.Addr = 0x1004;
  Use.CallExpr =
      HighExpr::makeCall("indirect", 0x1004, {HighExpr::makeVar(Receiver)});
  Use.CallExpr->IsIndirectCall = true;
  Use.CallExpr->IndirectTarget = HighExpr::makeVar(Receiver);

  HighFunc F;
  F.Body = {Definition, Use};
  renameVars(F.Body);

  ASSERT_EQ(F.Body[0].Dst->Kind, ExprKind::Var);
  const auto &Renamed = F.Body[0].Dst->Var;
  EXPECT_EQ(Renamed.SSAVer, 0);
  ASSERT_EQ(F.Body[1].CallExpr->Operands[0]->Kind, ExprKind::Var);
  ASSERT_EQ(F.Body[1].CallExpr->IndirectTarget->Kind, ExprKind::Var);
  EXPECT_EQ(F.Body[1].CallExpr->Operands[0]->Var, Renamed);
  EXPECT_EQ(F.Body[1].CallExpr->IndirectTarget->Var, Renamed);
}

TEST(HighControlFlowSemantics, IndirectCallTargetIsLiveAfterRenameCleanup) {
  HighStmt Use;
  Use.Kind = StmtKind::Call;
  Use.Addr = 0x1004;
  Use.CallExpr = HighExpr::makeCall("indirect", 0x1004, {});
  Use.CallExpr->IsIndirectCall = true;
  Use.CallExpr->IndirectTarget = local(8);

  HighFunc F;
  F.Body = {assign(0x1000, 8, 0x1234), Use};
  postRenameCleanup(F.Body);

  ASSERT_EQ(F.Body.size(), 2u);
  ASSERT_EQ(F.Body[0].Dst->Kind, ExprKind::Var);
  ASSERT_EQ(F.Body[1].CallExpr->IndirectTarget->Kind, ExprKind::Var);
  EXPECT_EQ(F.Body[0].Dst->Var, F.Body[1].CallExpr->IndirectTarget->Var);
}

// Preserve the CFG while varying the presence of compiler-generated edge
// statements. Their source address is not a valid loop exit identity.
TEST(HighControlFlowSemantics, ConditionalLoopExitExecutesUnlabeledEdgeCopies) {
  for (bool Unlabeled : {false, true}) {
    HighFunc F;
    F.Entry = 0x1000;
    auto Add = assign(0x1100, 1, 0);
    Add.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(3, 8));
    auto Decrement = assign(0x1104, 0, 0);
    Decrement.Val =
        HighExpr::makeBinop(NdOp::INT_SUB, local(0), HighExpr::makeConst(1, 8));
    auto Latch = conditional(0x1108, 0x1100);
    Latch.Body.front().Addr = 0;
    auto Copy = assign(Unlabeled ? 0 : 0x1200, 2, 0);
    Copy.Val = local(1);
    Copy.IsPhiCopy = true;
    F.Body = {assign(0x1000, 1, 0),    Add, Decrement, Latch, Copy,
              result(0x1204, local(2))};
    MedFunc Med;
    Med.Entry = F.Entry;
    Med.Blocks.resize(3);
    for (int I = 0; I < 3; ++I) {
      Med.Blocks[I].Id = I;
      Med.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    }
    Med.Blocks[0].Succs = {1};
    Med.Blocks[1].Succs = {1, 2};
    for (const auto &S : F.Body) {
      if (!S.Addr)
        continue;
      MedOp Op;
      Op.Addr = S.Addr;
      Med.Blocks[(S.Addr - 0x1000) / 0x100].Ops.push_back(Op);
    }
    for (unsigned Count = 1; Count <= 8; ++Count)
      ASSERT_EQ(execute(F, Count, true), Count * 3u);
    detectAndConvertLoops(F, {}, Med, false);
    for (unsigned Count = 1; Count <= 8; ++Count) {
      SCOPED_TRACE(Unlabeled);
      EXPECT_EQ(execute(F, Count, true), Count * 3u);
    }
    walkStmts(F.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto) {
        EXPECT_NE(S.GotoTarget, 0u);
        EXPECT_NE(S.GotoTarget, InvalidVA);
      }
    });
  }
}

TEST(HighControlFlowSemantics, LoopHeaderHasOneEntryForExternalBackedges) {
  HighFunc F;
  F.Entry = 0x1000;
  auto Split = conditional(0x1104, 0x1300);
  Split.Body.front().Addr = 0;
  auto Latch = conditional(0x1200, 0x1100);
  Latch.Body.front().Addr = 0;
  F.Body = {assign(0x1000, 1, 0),
            assign(0x1100, 1, 7),
            Split,
            Latch,
            jump(0x1204, 0x1400),
            jump(0x1300, 0x1100),
            result(0x1400, local(1))};
  MedFunc Med;
  Med.Entry = F.Entry;
  Med.Blocks.resize(5);
  for (int I = 0; I < 5; ++I) {
    Med.Blocks[I].Id = I;
    Med.Blocks[I].StartAddr = 0x1000 + I * 0x100;
  }
  Med.Blocks[0].Succs = {1};
  Med.Blocks[1].Succs = {2, 3};
  Med.Blocks[2].Succs = {1, 4};
  Med.Blocks[3].Succs = {1};
  for (const auto &S : F.Body) {
    MedOp Op;
    Op.Addr = S.Addr;
    Med.Blocks[(S.Addr - 0x1000) / 0x100].Ops.push_back(Op);
  }
  ASSERT_TRUE(buildHighSourceFlowGraph(F).Diagnostics.Complete);
  detectAndConvertLoops(F, {}, Med, false);
  size_t Entries = 0, Loops = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Entries += S.Addr == 0x1100;
    Loops += S.Kind == StmtKind::While;
  });
  EXPECT_EQ(Loops, 1u);
  EXPECT_EQ(Entries, 1u);
  const auto Graph = buildHighSourceFlowGraph(F);
  for (const auto &Item : Graph.Diagnostics.Items)
    ADD_FAILURE() << Item.Reason << " at " << Item.RelatedAddress;
  EXPECT_TRUE(Graph.Diagnostics.Complete);
}

TEST(HighControlFlowSemantics, LoopExitRequiresExactContinuationIdentity) {
  HighFunc F;
  F.Entry = 0x1000;
  auto Exit = conditional(0x1104, 0x1300);
  Exit.Body.front().Addr = 0;
  F.Body = {assign(0x1000, 1, 0),
            assign(0x1100, 1, 7),
            Exit,
            assign(0x1200, 0, 1),
            jump(0x1204, 0x1100),
            result(0x1308, HighExpr::makeConst(99, 8)),
            result(0x1300, HighExpr::makeConst(42, 8))};
  MedFunc Med;
  Med.Entry = F.Entry;
  Med.Blocks.resize(5);
  for (int I = 0; I < 5; ++I)
    Med.Blocks[I].Id = I;
  const int Owners[] = {0, 1, 1, 2, 2, 3, 4};
  for (size_t I = 0; I < F.Body.size(); ++I) {
    auto &Block = Med.Blocks[Owners[I]];
    if (!Block.StartAddr)
      Block.StartAddr = F.Body[I].Addr;
    MedOp Op;
    Op.Addr = F.Body[I].Addr;
    Block.Ops.push_back(Op);
  }
  Med.Blocks[0].Succs = {1};
  Med.Blocks[1].Succs = {2, 4};
  Med.Blocks[2].Succs = {1};
  for (unsigned Input : {0, 1, 19})
    ASSERT_EQ(execute(F, Input, true), 42u);
  detectAndConvertLoops(F, {}, Med, false);
  for (unsigned Input : {0, 1, 19})
    EXPECT_EQ(execute(F, Input, true), 42u);
}

TEST(HighControlFlowSemantics, OuterLoopTransfersKeepTheirNestedLoopScope) {
  HighFunc F;
  F.Entry = 0x1000;
  HighStmt Inner;
  Inner.Kind = StmtKind::While;
  Inner.Addr = 0x1104;
  Inner.Cond = HighExpr::makeConst(1, 1);
  Inner.Body = {jump(0x1108, 0x1300)};
  F.Body = {assign(0x1000, 1, 0), assign(0x1100, 1, 7), Inner,
            jump(0x1200, 0x1100), result(0x1300, local(1))};
  MedFunc Med;
  Med.Entry = F.Entry;
  Med.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    Med.Blocks[I].Id = I;
    Med.Blocks[I].StartAddr = 0x1000 + I * 0x100;
  }
  Med.Blocks[0].Succs = {1};
  Med.Blocks[1].Succs = {2, 3};
  Med.Blocks[2].Succs = {1};
  walkStmts(F.Body, [&](const HighStmt &S) {
    MedOp Op;
    Op.Addr = S.Addr;
    Med.Blocks[(S.Addr - 0x1000) / 0x100].Ops.push_back(Op);
  });
  ASSERT_EQ(execute(F, 0, true), 7u);
  detectAndConvertLoops(F, {}, Med, false);
  EXPECT_EQ(execute(F, 0, true), 7u);
  size_t Transfers = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Transfers += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1300;
  });
  EXPECT_EQ(Transfers, 1u);
}
} // namespace

TEST(HighControlFlowSemantics,
     CalleeSavedComputedValuesRemainObservableMemoryWrites) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64, Arch::ARM, Arch::X86}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    ASSERT_FALSE(TRI.CalleeSaveRegs.empty());
    std::vector<uint64_t> Registers(TRI.CalleeSaveRegs.begin(),
                                    TRI.CalleeSaveRegs.end());
    for (unsigned Index : {7U, 8U, 15U})
      if (Index < TRI.VecRegCount)
        Registers.push_back(TRI.VecRegBase + Index * TRI.VecRegStride);
    for (bool Computed : {false, true})
      for (uint64_t Register : Registers) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Register);
        MedFunc M;
        M.Entry = 0x1000;
        M.Name = "saved_register_is_not_a_dead_store";
        M.FrameSize = 64;
        M.ReturnType = NdType::makeInt(TRI.PointerSize, false);
        auto Input = machineValue(0, Architecture);
        Input.Kind = MedVar::Param;
        Input.Size = TRI.PointerSize;
        if (Computed)
          M.Params = {Input};
        auto Saved = machineValue(Computed ? 1 : 0, Architecture);
        Saved.Kind = MedVar::Reg;
        Saved.RegOff = Register;
        Saved.SSAVer = Computed ? 1 : 0;
        Saved.Size = TRI.PointerSize;
        auto Loaded = machineValue(2, Architecture);
        auto Sum = machineValue(3, Architecture);
        Loaded.Size = Sum.Size = TRI.PointerSize;
        Sum.Kind = MedVar::Reg;
        Sum.RegOff = TRI.IntReturnReg;
        Sum.SSAVer = 1;
        auto C = [&](uint64_t V) {
          return MedVar::makeConst(V, TRI.PointerSize);
        };
        M.Blocks.resize(1);
        auto &B = M.Blocks.front();
        B.Id = 0;
        B.StartAddr = 0x1000;
        B.EndAddr = 0x1014;
        B.Ops = {operation(NdOp::INT_ADD, 0x1000, Saved, {Input, C(1)}),
                 operation(NdOp::STORE, 0x1004, {}, {C(0x8000), Saved}),
                 operation(NdOp::LOAD, 0x1008, Loaded, {C(0x8000)}),
                 operation(NdOp::INT_ADD, 0x100c, Sum, {Loaded, Saved}),
                 operation(NdOp::RETURN, 0x1010, {}, {Sum})};
        if (!Computed)
          B.Ops.erase(B.Ops.begin());
        MedToHighConverter Converter;
        const auto High = Converter.convert(M, Architecture);
        size_t Stores = 0;
        walkStmts(High.Body, [&](const HighStmt &S) {
          Stores += S.Kind == StmtKind::Store;
        });
        EXPECT_EQ(Stores, 1U);
        for (uint64_t Value : {0U, 1U, 17U, 65536U})
          EXPECT_EQ(execute(High, Value), (Value + unsigned(Computed)) * 2);
      }
  }
}

TEST(HighControlFlowSemantics, JumpTableSuccessorsKeepCallsStoresAndPhiEdges) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Reverse);
      MedFunc M;
      M.Entry = 0x1000;
      M.Name = "dispatch_effects";
      M.ReturnType = NdType::makeInt(8, false);
      auto Input = machineValue(0, Architecture);
      Input.Kind = MedVar::Param;
      Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
      M.Params = {Input};
      auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
      M.Blocks.resize(4);
      for (int I = 0; I < 4; ++I) {
        M.Blocks[I].Id = I;
        M.Blocks[I].StartAddr = 0x1000 + I * 0x100;
        M.Blocks[I].EndAddr = M.Blocks[I].StartAddr + 0x20;
      }
      M.Blocks[0].Succs = {1, 2};
      M.Blocks[0].Ops = {operation(NdOp::INDIR_BR, 0x1000, {}, {Input})};
      M.SwitchSelectorPlans[0x1000] = {};
      M.SwitchSelectorPlans[0x1000].Selector = Input;
      M.SwitchSelectorPlans[0x1000].ResultSize = 8;
      auto First = machineValue(1, Architecture);
      auto Second = machineValue(2, Architecture);
      First.Kind = Second.Kind = MedVar::Reg;
      First.RegOff = Second.RegOff =
          getTargetRegInfo(Architecture).IntReturnReg;
      First.SSAVer = 1;
      Second.SSAVer = 2;
      for (int I = 1; I < 3; ++I) {
        auto &B = M.Blocks[I];
        B.Preds = {0};
        B.Succs = {3};
        auto Incoming = machineValue(10 + I, Architecture);
        B.Phis = {{Incoming, {{0, C(I == 1 ? 17 : 23)}}}};
        auto Call =
            operation(NdOp::CALL, B.StartAddr + 4, I == 1 ? First : Second,
                      {C(0x2000), C(0), C(0x8000)});
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->TargetAddress = 0x2000;
        Hint->Signature.ReturnType = M.ReturnType;
        Hint->Signature.Parameters = {
            {"unused", M.ReturnType, {}},
            {"address", NdType::makePtr(M.ReturnType), {}}};
        std::string Diagnostic;
        ASSERT_TRUE(assignDarwinScalarSourceABI(Hint->Signature, Architecture,
                                                Diagnostic))
            << Diagnostic;
        Call.SourceCallHint = Hint;
        B.Ops = {operation(NdOp::STORE, B.StartAddr, {}, {C(0x8000), Incoming}),
                 Call,
                 operation(NdOp::BRANCH, B.StartAddr + 8, {}, {C(0x1300)})};
      }
      auto Joined = machineValue(3, Architecture);
      auto Return = machineValue(4, Architecture);
      Return.Kind = MedVar::Reg;
      Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
      Return.SSAVer = 3;
      M.Blocks[3].Preds = {1, 2};
      M.Blocks[3].Phis = {{Joined, {{1, First}, {2, Second}}}};
      M.Blocks[3].Ops = {
          operation(NdOp::INT_ADD, 0x1300, Return, {Joined, C(100)}),
          operation(NdOp::RETURN, 0x1304, {}, {Return})};
      if (Reverse) {
        std::swap(M.Blocks[1], M.Blocks[2]);
        auto Remap = [](int Id) { return Id == 1 ? 2 : Id == 2 ? 1 : Id; };
        for (auto &B : M.Blocks) {
          B.Id = Remap(B.Id);
          for (auto &Id : B.Preds)
            Id = Remap(Id);
          for (auto &Id : B.Succs)
            Id = Remap(Id);
          for (auto &Phi : B.Phis)
            for (auto &[Id, Value] : Phi.Args)
              Id = Remap(Id);
        }
      }
      JumpTable Table;
      Table.InsnAddr = 0x1000;
      Table.Targets = {0x1100, 0x1200, 0x1100};
      Table.CaseLabels = {0, 1, 2};
      const std::map<va_t, std::string> Names{{0x2000, "observe"}};
      MedToHighConverter Converter;
      Converter.setJumpTables({Table});
      Converter.setFuncNames(&Names);
      const auto High = Converter.convert(M, Architecture);
      unsigned Stores = 0, Calls = 0;
      walkStmts(High.Body, [&](const HighStmt &S) {
        Stores += S.Kind == StmtKind::Store;
        forEachExpr(S, [&](const ExprPtr &E) {
          if (E->Kind == ExprKind::Call) {
            ++Calls;
            EXPECT_EQ(E->Operands.size(), 2U);
            EXPECT_TRUE(E->SourceCallHint);
          }
        });
      });
      EXPECT_EQ(Stores, 2U);
      EXPECT_EQ(Calls, 2U);
      EXPECT_TRUE(buildHighSourceFlowGraph(High).Diagnostics.Complete);
      for (unsigned Selector : {0U, 1U, 2U}) {
        SCOPED_TRACE(Selector);
        EXPECT_NO_THROW(EXPECT_EQ(execute(High, Selector, true),
                                  Selector == 1 ? 123U : 117U));
      }
    }
  }
}

TEST(HighControlFlowSemantics, JumpTableLoopEdgesPreserveParallelPhiSnapshots) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Reverse : {false, true}) {
      auto Med = loopFunction(Architecture, true, Reverse);
      auto &Loop = Med.Blocks[1];
      auto &Terminator = Loop.Ops.back();
      auto Selector = Terminator.Inputs[1];
      Terminator.Opcode = NdOp::INDIR_BR;
      Terminator.Inputs[0] = Selector;
      Terminator.NumInputs = 1;
      Med.SwitchSelectorPlans[Terminator.Addr] = {};
      Med.SwitchSelectorPlans[Terminator.Addr].Selector = Selector;
      Med.SwitchSelectorPlans[Terminator.Addr].ResultSize = Selector.Size;
      JumpTable Table;
      Table.InsnAddr = Terminator.Addr;
      Table.Targets = {0x1200, 0x1100};
      Table.CaseLabels = {0, 1};
      MedToHighConverter Converter;
      Converter.setJumpTables({Table});
      const auto High = Converter.convert(Med, Architecture);
      EXPECT_TRUE(buildHighSourceFlowGraph(High).Diagnostics.Complete);
      for (unsigned Count = 1; Count <= 8; ++Count) {
        SCOPED_TRACE(Count);
        EXPECT_NO_THROW(
            EXPECT_EQ(execute(High, Count, true), Count % 2 ? 102U : 201U));
      }
      // The loop successor's PHIs cannot be assigned to some other case if
      // its dispatch edge is missing. Incomplete metadata stays unsupported.
      Table.Targets[1] = 0x1200;
      Converter.setJumpTables({Table});
      const auto Incomplete = Converter.convert(Med, Architecture);
      EXPECT_FALSE(buildHighSourceFlowGraph(Incomplete).Diagnostics.Complete);
    }
}
