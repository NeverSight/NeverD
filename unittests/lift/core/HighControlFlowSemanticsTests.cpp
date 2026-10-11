#include "../../../lib/ir/high/lower/CompareTreeSwitch.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd {
void structureIfElse(HighFunc &, int, const MedFunc * = nullptr);
bool sinkJoinDefaultsLate(HighFunc &Func);
void detectAndConvertLoops(HighFunc &, const std::unordered_map<va_t, int> &,
                           const MedFunc &, bool);
void inlineSingleDefSingleUse(std::vector<HighStmt> &);
void foldCopyChains(HighFunc &);
void resolveRegAliases(std::vector<HighStmt> &);
void simplifyAllExprs(std::vector<HighStmt> &);
void recoverSwitchStatements(HighFunc &);
void cleanupGuardBeforeSwitch(HighFunc &);
void eliminateGotoToLoop(std::vector<HighStmt> &);
void removeUnreachableCode(std::vector<HighStmt> &);
void eliminateRegAliasCopies(HighFunc &);
void elimConsecutiveDeadStores(std::vector<HighStmt> &);
void postRenameCleanup(std::vector<HighStmt> &);
void renameVars(std::vector<HighStmt> &);
void eliminateUnusedValues(std::vector<HighStmt> &);
void narrowUnreadRegisterBytes(HighFunc &, Arch Architecture = Arch::Unknown);
} // namespace neverd
using namespace neverd;

TEST(HighControlFlowSemantics, CalleeNameSnapshotKeepsLookupPrecedence) {
  BinaryImage Img;
  Img.Imports.push_back({"", "import_name", 0, 0x1000});
  Img.Imports.push_back({"", "sub_ignored", 0, 0x6000});
  Img.Exports.push_back({"export_name", 0, 0x3000});
  Img.Exports.push_back({"export_after_synthetic_import", 0, 0x6000});
  Img.Exports.push_back({"sub_7000", 0, 0x7000});
  Img.Exports.push_back({"", 0, 0x8000});
  Img.Exports.push_back({"ignored_later_export", 0, 0x8000});
  Img.Symbols.push_back({"symbol_behind_export", 0x3000});
  Img.Symbols.push_back({"symbol_name", 0x4000});
  Img.Symbols.push_back({"symbol_behind_synthetic", 0x7000});
  Img.Symbols.push_back({"symbol_after_empty_export", 0x8000});
  std::map<va_t, std::string> FunctionNames = {{0x1000, "sub_1000"},
                                               {0x2000, "declared_name"},
                                               {0x3000, "sub_3000"},
                                               {0x5000, "sub_5000"}};
  std::set<va_t> Targets = {0,      0x1000, 0x2000, 0x3000, 0x4000,
                            0x5000, 0x6000, 0x7000, 0x8000};

  MedToHighConverter Resolver;
  Resolver.setBinaryImage(&Img);
  Resolver.setFuncNames(&FunctionNames);
  std::map<va_t, std::string> Snapshot;
  Resolver.resolveCalleeNames(Targets, Snapshot);
  EXPECT_EQ(Snapshot.at(0), "sub_0");
  EXPECT_EQ(Snapshot.at(0x1000), "import_name");
  EXPECT_EQ(Snapshot.at(0x2000), "declared_name");
  EXPECT_EQ(Snapshot.at(0x3000), "export_name");
  EXPECT_EQ(Snapshot.at(0x4000), "symbol_name");
  EXPECT_EQ(Snapshot.at(0x5000), "sub_5000");
  EXPECT_EQ(Snapshot.at(0x6000), "export_after_synthetic_import");
  EXPECT_EQ(Snapshot.at(0x7000), "sub_7000");
  EXPECT_EQ(Snapshot.at(0x8000), "symbol_after_empty_export");

  MedToHighConverter Cached;
  Cached.setBinaryImage(&Img);
  Cached.setFuncNames(&FunctionNames);
  Cached.setResolvedCalleeNames(&Snapshot);
  Img.Imports[0].Name = "changed_import";
  Img.Symbols[1].Name = "changed_symbol";
  std::map<va_t, std::string> Reused;
  Cached.resolveCalleeNames({0x1000, 0x4000}, Reused);
  EXPECT_EQ(Reused.at(0x1000), "import_name");
  EXPECT_EQ(Reused.at(0x4000), "symbol_name");
}

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
      case NdOp::INT_LESS:
        return uint64_t(A < B);
      case NdOp::INT_LESSEQUAL:
        return uint64_t(A <= B);
      case NdOp::INT_SLESS:
        return uint64_t(static_cast<int64_t>(A) < static_cast<int64_t>(B));
      case NdOp::INT_SLESSEQUAL:
        return uint64_t(static_cast<int64_t>(A) <= static_cast<int64_t>(B));
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
      // No test statement raises an exception: a __try runs its body.
      if (S.Kind == StmtKind::Block || S.Kind == StmtKind::SEHTry) {
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
        for (size_t I = 0; I < S.Cases.size(); ++I)
          if (S.Cases[I].Value == Selector) {
            // `case A: case B: body` shares the next non-empty body.
            size_t J = I;
            while (J + 1 < S.Cases.size() && S.Cases[J].FallsThrough)
              ++J;
            Selected = &S.Cases[J].Body;
            break;
          }
        auto R = Run(*Selected);
        if (R.Return || R.Target || R.Continue)
          return R;
      }
      if (S.Kind == StmtKind::While) {
        // `while (1)` has no condition expression.
        while (!S.Cond || Value(S.Cond)) {
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
      if (S.Kind == StmtKind::DoWhile) {
        // The body runs before the first test; `continue` goes to the test.
        do {
          if (!Budget--)
            throw std::runtime_error(
                "control-flow oracle exceeded its loop budget");
          auto R = Run(S.Body);
          if (R.Return || R.Target)
            return R;
          if (R.Break)
            break;
        } while (!S.Cond || Value(S.Cond));
      }
      if (S.Kind == StmtKind::ExprStmt && S.Val)
        (void)Value(S.Val);
      switch (S.Kind) {
      case StmtKind::Assign:
      case StmtKind::ExprStmt:
      case StmtKind::If:
      case StmtKind::IfElse:
      case StmtKind::While:
      case StmtKind::DoWhile:
      case StmtKind::Switch:
      case StmtKind::Return:
      case StmtKind::Goto:
      case StmtKind::Block:
      case StmtKind::Store:
      case StmtKind::Call:
      case StmtKind::Nop:
      case StmtKind::Break:
      case StmtKind::Continue:
      case StmtKind::SEHTry:
        break;
      default:
        // A statement the oracle cannot run must not pass as a no-op.
        throw std::runtime_error(
            "control-flow oracle cannot run this statement kind");
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

TEST(HighControlFlowSemantics, DeadIntegerViewPhiCopiesPreserveEffectBarriers) {
  for (unsigned Variant = 0; Variant != 8; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = guardedPhiCopy();
    auto &Copy = F.Body[0].ElseBody[0];
    auto View = std::make_shared<HighExpr>();
    View->Kind = ExprKind::Cast;
    View->Type = View->CastTo = NdType::makeInt(8, false);
    View->Operands = {Copy.Val};
    Copy.Val = View;
    switch (Variant) {
    case 0:
      break;
    case 1:
      View->Operands[0] = HighExpr::makeCall("observe", 0x2000, {});
      View->Operands[0]->Type = NdType::makeInt(8);
      break;
    case 2:
      View->Operands[0] = HighExpr::makeLoad(local(2), NdType::makeInt(8));
      break;
    case 3:
      View->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 4:
      Copy.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 5:
      View->Type = View->CastTo = NdType::makeFloat(8);
      break;
    case 6:
      View->Type = View->CastTo = NdType::makeInt(4, false);
      break;
    case 7:
      View->IndirectTarget = HighExpr::makeConst(0x2000, 8);
      break;
    }
    EXPECT_EQ(eliminateHighDeadPhiCopies(F), Variant == 0);
    if (Variant == 0)
      for (uint64_t Input : {0ULL, 1ULL, 2ULL, 0xffffffffffffffffULL})
        EXPECT_EQ(execute(F, Input), Input ? 42u : 7u);
    else
      EXPECT_EQ(F.Body[0].ElseBody[0].Val, View);
  }
}

TEST(HighControlFlowSemantics, DeadUnknownPhiCopiesNeverDefineObservedBits) {
  for (unsigned Variant = 0; Variant != 12; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = guardedPhiCopy();
    auto &Copy = F.Body[0].ElseBody[0];
    Copy.Val = HighExpr::makeUndef(8);
    const auto Unknown = Copy.Val;
    switch (Variant) {
    case 0:
      break;
    case 1:
      F.Body[1].Cond = HighExpr::makeConst(1, 1);
      break;
    case 2:
      Copy.IsPhiCopy = false;
      break;
    case 3:
      Copy.Val = HighExpr::makeUndef(4);
      break;
    case 4:
      Unknown->Type = NdType::makeFloat(8);
      break;
    case 5:
      Unknown->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 6:
      Copy.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 7:
      Unknown->IndirectTarget = local(2);
      break;
    case 8:
      Unknown->Operands = {local(2)};
      break;
    case 9:
      Unknown->IntrinsicOutputs.push_back(local(2)->Var);
      break;
    case 10:
      Copy.Val = HighExpr::makeLoad(local(2), NdType::makeInt(8));
      break;
    case 11:
      Copy.Val = HighExpr::makeCall("observe", 0x2000, {});
      Copy.Val->Type = NdType::makeInt(8);
      break;
    }
    const auto Before = Copy.Val;
    EXPECT_EQ(eliminateHighDeadPhiCopies(F), Variant == 0);
    if (Variant == 0) {
      EXPECT_EQ(F.Body[0].ElseBody[0].Addr, 0x1008U);
      for (uint64_t Input : {0ULL, 1ULL, 2ULL, 0xffffffffffffffffULL})
        EXPECT_EQ(execute(F, Input), Input ? 42u : 7u);
    } else {
      EXPECT_EQ(F.Body[0].ElseBody[0].Val, Before);
    }
  }
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

TEST(HighControlFlowSemantics,
     BooleanEqualityNeedsNoUnrelatedScalarCopyToPruneDeadPhiRead) {
  auto F = guardedPhiCopy();
  auto Boolean = [](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.Size = 1;
    return HighExpr::makeVar(V);
  };
  auto Equality = [&](va_t Address, int Destination) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.Dst = Boolean(Destination);
    S.Val = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), local(3));
    return S;
  };
  F.Body[0].Cond = Boolean(5);
  F.Body[1].Cond = Boolean(8);
  F.Body.insert(F.Body.begin(), assign(0x1000, 3, 7));
  F.Body.insert(F.Body.begin() + 1, Equality(0x1002, 5));
  F.Body.insert(F.Body.begin() + 3, Equality(0x100c, 8));
  EXPECT_THROW(execute(F, 0), std::runtime_error);
  ASSERT_TRUE(eliminateHighDeadPhiCopies(F));
  for (uint64_t Value : {uint64_t{0}, uint64_t{7}, uint64_t{19}})
    EXPECT_EQ(execute(F, Value), Value == 7 ? 42u : 7u);
}

HighFunc copiedBooleanEquality(bool ReassignCopy) {
  auto Copy = [](va_t Address, int Destination, int Source) {
    auto S = assign(Address, Destination, 0);
    S.Val = local(Source);
    return S;
  };
  auto Boolean = [](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.Size = 1;
    return HighExpr::makeVar(V);
  };
  auto Equality = [&](va_t Address, int Destination, int Left, int Right) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.Dst = Boolean(Destination);
    S.Val = HighExpr::makeBinop(NdOp::INT_EQUAL, local(Left), local(Right));
    return S;
  };
  HighStmt Define;
  Define.Kind = StmtKind::If;
  Define.Addr = 0x1018;
  Define.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Boolean(5));
  Define.Body = {assign(0x101c, 9, 7)};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Addr = 0x1034;
  Use.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Boolean(8));
  Use.Body = {result(0x1038, local(9))};
  HighFunc F;
  F.Body = {assign(0x1000, 1, 1), assign(0x1004, 2, 1),      Copy(0x1008, 3, 1),
            Copy(0x100c, 4, 2),   Equality(0x1010, 5, 1, 2), Define};
  if (ReassignCopy)
    F.Body.push_back(assign(0x1020, 3, 2));
  F.Body.insert(F.Body.end(), {Copy(0x1024, 6, 3), Copy(0x1028, 7, 4),
                               Equality(0x102c, 8, 6, 7), Use,
                               result(0x103c, HighExpr::makeConst(0, 8))});
  return F;
}

TEST(HighControlFlowSemantics, BooleanEqualitySurvivesDominatingScalarCopies) {
  auto F = copiedBooleanEquality(false);
  const auto Report = analyzeHighSourceFlow(F, true);
  EXPECT_TRUE(Report.Complete);
  EXPECT_TRUE(Report.Items.empty());
}

TEST(HighControlFlowSemantics,
     BooleanEqualitySurvivesIdenticalCopiesOnBothBranchArms) {
  auto F = copiedBooleanEquality(false);
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  MedVar Parameter;
  Parameter.Kind = MedVar::Param;
  Parameter.Size = 8;
  Branch.Cond = HighExpr::makeVar(Parameter);
  Branch.Body = {F.Body[2], F.Body[3]};
  Branch.ElseBody = {F.Body[2], F.Body[3]};
  F.Body.erase(F.Body.begin() + 2, F.Body.begin() + 4);
  F.Body.insert(F.Body.begin() + 2, std::move(Branch));

  const auto Report = analyzeHighSourceFlow(F, true);
  EXPECT_TRUE(Report.Complete);
  EXPECT_TRUE(Report.Items.empty())
      << (Report.Items.empty() ? "" : Report.Items.front().Reason);
  for (uint64_t Choice : {uint64_t{0}, uint64_t{1}})
    EXPECT_EQ(execute(F, Choice), 0u);

  // The alias proof must not cross a join when one path never writes it.
  F.Body[2].ElseBody.pop_back();
  const auto MissingCopy = analyzeHighSourceFlow(F, true);
  EXPECT_TRUE(MissingCopy.Complete);
  EXPECT_FALSE(MissingCopy.Items.empty());
}

HighFunc mergedCopyEquality() {
  auto F = copiedBooleanEquality(false);
  MedVar Parameter;
  Parameter.Kind = MedVar::Param;
  Parameter.Id = 0;
  Parameter.Size = 8;
  const auto Input = HighExpr::makeVar(Parameter);
  F.Body[0].Val = Input;
  const auto Copy = [](va_t Address, int Destination, int Source) {
    auto S = assign(Address, Destination, 0);
    S.Val = local(Source);
    return S;
  };
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Addr = 0x1006;
  Branch.Cond = Input;
  Branch.Body = {Copy(0x2000, 10, 1), Copy(0x2004, 3, 10)};
  Branch.ElseBody = {Copy(0x2010, 11, 1), Copy(0x2014, 3, 11)};
  F.Body[2] = std::move(Branch);
  return F;
}

TEST(HighControlFlowSemantics, DistinctBranchCopiesShareOneDominatingRoot) {
  const auto F = mergedCopyEquality();
  const auto Report = analyzeHighSourceFlow(F, true);
  ASSERT_TRUE(Report.Complete);
  ASSERT_TRUE(Report.Items.empty())
      << (Report.Items.empty() ? "" : Report.Items.front().Reason);
  for (uint64_t Input : {0ULL, 1ULL, 2ULL, 7ULL, 0xffffffffULL,
                         0x8000000000000000ULL, 0xffffffffffffffffULL})
    EXPECT_EQ(execute(F, Input), Input == 1 ? 0u : 7u);
}

TEST(HighControlFlowSemantics, CopyViewsPreserveOnlyFullWidthIntegerBits) {
  for (unsigned Variant = 0; Variant != 9; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = mergedCopyEquality();
    auto &Copy = F.Body[2].ElseBody.back();
    auto View = std::make_shared<HighExpr>();
    View->Kind = ExprKind::Cast;
    View->Type = View->CastTo = NdType::makeInt(8, false);
    View->Operands = {Copy.Val};
    Copy.Val = View;
    switch (Variant) {
    case 0:
      break;
    case 1:
      View->Type = View->CastTo = NdType::makeInt(4, false);
      break;
    case 2:
      View->Type = View->CastTo = NdType::makeInt(16, false);
      break;
    case 3:
      View->Type = View->CastTo = NdType::makePtr();
      break;
    case 4:
      View->Type = View->CastTo = NdType::makeFloat(8);
      break;
    case 5:
      View->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 6:
      View->IndirectTarget = HighExpr::makeConst(0x1000, 8);
      break;
    case 7:
      View->CastTo = NdType::makeInt(8, true);
      break;
    case 8:
      View->Operands.push_back(HighExpr::makeConst(0, 8));
      break;
    }
    const auto Report = analyzeHighSourceFlow(F, true);
    EXPECT_TRUE(Report.Complete);
    EXPECT_EQ(Report.Items.empty(), Variant == 0);
    if (Variant == 0)
      for (uint64_t Input : {0ULL, 1ULL, 0xffffffffULL, 0x8000000000000000ULL,
                             0xffffffffffffffffULL})
        EXPECT_EQ(execute(F, Input), Input == 1 ? 0u : 7u);
  }
}

TEST(HighControlFlowSemantics, FullWidthCopySignednessKeepsEqualityBits) {
  auto F = mergedCopyEquality();
  walkStmts(F.Body, [&](HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      std::set<const HighExpr *> Seen;
      while (!Pending.empty()) {
        const auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Temp &&
            E->Var.Id == 3)
          E->Type = NdType::makeInt(8, false);
        E->forEachChildExpr(
            [&](const ExprPtr &Child) { Pending.push_back(Child); });
      }
    });
  });
  const auto Report = analyzeHighSourceFlow(F, true);
  EXPECT_TRUE(Report.Complete);
  EXPECT_TRUE(Report.Items.empty());
  for (uint64_t Input :
       {0ULL, 1ULL, 0x8000000000000000ULL, 0xffffffffffffffffULL})
    EXPECT_EQ(execute(F, Input), Input == 1 ? 0u : 7u);
}

TEST(HighControlFlowSemantics, PartitionBudgetRetainsAnAffordableGuardSubset) {
  for (bool OverwriteGuard : {false, true}) {
    SCOPED_TRACE(OverwriteGuard);
    auto F = mergedCopyEquality();
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    std::vector<HighStmt> Noise;
    for (unsigned I = 0; I < 8; ++I) {
      auto Definition = assign(0x3000 + I * 4, 20 + I, 0);
      Definition.Val =
          HighExpr::makeBinop(NdOp::INT_AND, HighExpr::makeVar(Parameter),
                              HighExpr::makeConst(uint64_t{1} << I, 8));
      F.Body.insert(F.Body.begin(), Definition);
    }
    for (unsigned Pass = 0; Pass != 2; ++Pass)
      for (unsigned I = 0; I != 8; ++I) {
        HighStmt Test;
        Test.Kind = StmtKind::If;
        Test.Addr = 0x3100 + Pass * 32 + I * 4;
        Test.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL,
                                        local(20 + (Pass ? 7 - I : I)),
                                        HighExpr::makeConst(0, 8));
        Noise.push_back(Test);
      }
    F.Body.insert(F.Body.end() - 2, Noise.begin(), Noise.end());
    if (OverwriteGuard) {
      auto Rewrite = assign(0x3200, 8, 0);
      Rewrite.Dst->Type = NdType::makeInt(1, false);
      Rewrite.Dst->Var.Size = 1;
      Rewrite.Val = HighExpr::makeConst(0, 1);
      F.Body.insert(F.Body.end() - 2, Rewrite);
      EXPECT_THROW(execute(F, 1), std::runtime_error);
    }
    const auto Report = analyzeHighSourceFlow(F, true);
    EXPECT_TRUE(Report.Complete);
    EXPECT_EQ(Report.Items.empty(), !OverwriteGuard)
        << (Report.Items.empty() ? "" : Report.Items.front().Reason);
    if (!OverwriteGuard)
      for (uint64_t Input = 0; Input != 256; ++Input)
        EXPECT_EQ(execute(F, Input), Input == 1 ? 0u : 7u);
  }
}

TEST(HighControlFlowSemantics, MergedCopyRootsNeedEveryDefinitionAndPath) {
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = mergedCopyEquality();
    auto &Branch = F.Body[2];
    switch (Variant) {
    case 0:
      // One edge carries a different stable root.
      Branch.ElseBody[0].Val = local(2);
      break;
    case 1:
      // One edge reads its alias before that alias has a definition.
      Branch.ElseBody.erase(Branch.ElseBody.begin());
      break;
    case 2:
      // A write to the original after the snapshot invalidates the relation.
      F.Body.insert(F.Body.begin() + 4, assign(0x2020, 1, 2));
      break;
    case 3:
      // The merged local is not defined on one incoming path.
      Branch.ElseBody.pop_back();
      break;
    case 4:
      // A cyclic copy graph supplies no stable root.
      Branch.ElseBody[0].Val = local(3);
      break;
    case 5:
      // A later conflicting write must not borrow the earlier alias fact.
      F.Body.insert(F.Body.begin() + 6, assign(0x2020, 3, 2));
      break;
    }
    const auto Report = analyzeHighSourceFlow(F, true);
    EXPECT_TRUE(Report.Complete);
    EXPECT_FALSE(Report.Items.empty());
  }
}

TEST(HighControlFlowSemantics, ReassignedScalarCopyInvalidatesEquality) {
  auto F = copiedBooleanEquality(true);
  const auto Report = analyzeHighSourceFlow(F, true);
  EXPECT_TRUE(Report.Complete);
  ASSERT_FALSE(Report.Items.empty());
  EXPECT_EQ(Report.Items[0].Issue, HighSourceFlowIssue::DefiniteAssignment);
}

TEST(HighControlFlowSemantics, SourceFlowSharedSwitchLabelsReachTheirBody) {
  MedVar Selector;
  Selector.Kind = MedVar::Param;
  Selector.Size = 8;
  HighStmt Dispatch;
  Dispatch.Kind = StmtKind::Switch;
  Dispatch.Addr = 0x1000;
  Dispatch.SwitchExpr = HighExpr::makeVar(Selector);
  Dispatch.Cases = {{0, {}, true}, {1, {}, true}, {2, {assign(0x1010, 1, 7)}}};
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  Break.Addr = 0x1014;
  Dispatch.Cases.back().Body.push_back(Break);
  Dispatch.DefaultBody = {result(0x1020, HighExpr::makeConst(99, 8))};
  HighFunc F;
  F.Body = {Dispatch, result(0x1030, local(1))};
  for (uint64_t Value : {0, 1, 2, 3})
    EXPECT_EQ(execute(F, Value), Value < 3 ? 7U : 99U);
  auto Flow = analyzeHighSourceFlow(F, true);
  ASSERT_TRUE(Flow.Complete);
  EXPECT_TRUE(Flow.Items.empty());
  F.Body[0].Cases[0].FallsThrough = false;
  Flow = analyzeHighSourceFlow(F, true);
  ASSERT_TRUE(Flow.Complete);
  ASSERT_FALSE(Flow.Items.empty());
  EXPECT_EQ(Flow.Items.front().Issue, HighSourceFlowIssue::DefiniteAssignment);
}

TEST(HighControlFlowSemantics, SourceFlowFallingCaseKeepsEffectsAndDefault) {
  HighStmt Dispatch;
  Dispatch.Kind = StmtKind::Switch;
  Dispatch.Addr = 0x1000;
  Dispatch.SwitchExpr = HighExpr::makeConst(0, 8);
  Dispatch.Cases = {{0, {assign(0x1010, 1, 7)}, true}, {1, {}, true}};
  Dispatch.DefaultBody = {result(0x1020, local(1))};
  HighFunc F;
  F.Body = {Dispatch, result(0x1030, local(2))};
  auto Flow = analyzeHighSourceFlow(F, true);
  ASSERT_TRUE(Flow.Complete);
  EXPECT_TRUE(Flow.Items.empty());
  const auto Graph = buildHighSourceFlowGraph(F);
  ASSERT_TRUE(Graph.Diagnostics.Complete);
  auto Definition = std::find_if(
      Graph.Nodes.begin(), Graph.Nodes.end(), [](const auto &Node) {
        return Node.Statement && Node.Statement->Addr == 0x1010;
      });
  ASSERT_NE(Definition, Graph.Nodes.end());
  ASSERT_EQ(Definition->Successors.size(), 1U);
  const auto &Next = Graph.Nodes[Definition->Successors.front()];
  ASSERT_TRUE(Next.Statement);
  EXPECT_EQ(Next.Statement->Addr, 0x1020U);
  // An explicit break bypasses the falling case and default, even when the
  // case's FallsThrough flag remains set. The follow reads undefined v2.
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  Break.Addr = 0x1014;
  F.Body[0].Cases[0].Body.push_back(Break);
  Flow = analyzeHighSourceFlow(F, true);
  ASSERT_TRUE(Flow.Complete);
  ASSERT_FALSE(Flow.Items.empty());
  EXPECT_EQ(Flow.Items.front().Issue, HighSourceFlowIssue::DefiniteAssignment);
  // Entering default directly cannot borrow the earlier case's definition.
  F.Body[0].Cases[0].Body.pop_back();
  F.Body[0].SwitchExpr = HighExpr::makeConst(99, 8);
  Flow = analyzeHighSourceFlow(F, true);
  ASSERT_TRUE(Flow.Complete);
  ASSERT_FALSE(Flow.Items.empty());
  EXPECT_EQ(Flow.Items.front().Issue, HighSourceFlowIssue::DefiniteAssignment);
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

TEST(HighControlFlowSemantics, FlagTestsOfOneCompareMergeIntoOneOrder) {
  // `cmp edi, 8; ja` reads !CF && !ZF: `!(x < 8) && !(x - 8 == 0)`, which is
  // `8 < x`.  Its negation `x < 8 || x - 8 == 0` is `x <= 8`.  Two different
  // values must stay two tests.
  auto view = [](ExprPtr Base) { return byteSlice(std::move(Base), 0, 4); };
  auto c32 = [](uint64_t V) { return HighExpr::makeConst(V, 4); };
  auto cmp = [](NdOp Op, ExprPtr A, ExprPtr B) {
    auto E = HighExpr::makeBinop(Op, std::move(A), std::move(B));
    E->Type = NdType::makeInt(1, false);
    return E;
  };
  auto logical = [](NdOp Op, ExprPtr A, ExprPtr B) {
    auto E = HighExpr::makeBinop(Op, std::move(A), std::move(B));
    E->Type = NdType::makeInt(1, false);
    return E;
  };
  auto difference = [&](ExprPtr X) {
    auto E = HighExpr::makeBinop(NdOp::INT_SUB, std::move(X), c32(8));
    E->Type = NdType::makeInt(4, false);
    return E;
  };
  auto negate = [](ExprPtr E) {
    auto N = HighExpr::makeUnary(NdOp::BOOL_NOT, std::move(E));
    N->Type = NdType::makeInt(1, false);
    return N;
  };
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    const bool Above = Case != 1;
    ExprPtr Other = Case == 2 ? view(local(1)) : view(local(0));
    ExprPtr Cond =
        Above ? logical(NdOp::BOOL_AND,
                        negate(cmp(NdOp::INT_LESS, view(local(0)), c32(8))),
                        negate(cmp(NdOp::INT_EQUAL, difference(Other), c32(0))))
              : logical(NdOp::BOOL_OR,
                        cmp(NdOp::INT_LESS, view(local(0)), c32(8)),
                        cmp(NdOp::INT_EQUAL, difference(Other), c32(0)));
    HighFunc F;
    F.Body = {result(0, Cond)};
    const std::vector<uint64_t> Inputs{0, 7, 8, 9, 0xffffffff, 0x100000008};
    std::vector<std::optional<uint64_t>> Expected;
    if (Case != 2)
      for (uint64_t Input : Inputs)
        Expected.push_back(execute(F, Input));
    simplifyAllExprs(F.Body);
    const ExprPtr &Root = F.Body[0].RetVal;
    if (Case == 2) {
      EXPECT_EQ(Root->Op, NdOp::BOOL_AND);
      continue;
    }
    EXPECT_EQ(Root->Op, Above ? NdOp::INT_LESS : NdOp::INT_LESSEQUAL);
    for (size_t I = 0; I != Inputs.size(); ++I)
      EXPECT_EQ(execute(F, Inputs[I]), Expected[I]) << Inputs[I];
  }
}

namespace {
HighStmt returning(uint64_t Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = HighExpr::makeConst(Value, 8);
  return S;
}

SwitchCase caseOf(uint64_t Value, std::vector<HighStmt> Body) {
  SwitchCase C;
  C.Value = Value;
  C.Body = std::move(Body);
  return C;
}

HighStmt dispatch(std::vector<SwitchCase> Cases,
                  std::vector<HighStmt> Default) {
  HighStmt S;
  S.Kind = StmtKind::Switch;
  S.Addr = 0x1010;
  S.SwitchExpr = local(0);
  S.SwitchExpr->Type = NdType::makeInt(8, false);
  S.Cases = std::move(Cases);
  S.DefaultBody = std::move(Default);
  return S;
}
} // namespace

TEST(HighControlFlowSemantics, SwitchTailMovesToItsOnlyExit) {
  // `switch (x) { case 1: return 10; case 2: break; default: return 30; }
  // return 20;` reaches the return after the switch only through case 2.
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  for (bool ImplicitExit : {false, true}) {
    SCOPED_TRACE(ImplicitExit);
    HighFunc F;
    std::vector<HighStmt> Two;
    if (!ImplicitExit)
      Two.push_back(Break);
    else
      Two.push_back(assign(0x1020, 5, 1));
    F.Body = {
        dispatch({caseOf(1, {returning(10)}), caseOf(2, Two)}, {returning(30)}),
        returning(20)};
    std::vector<std::optional<uint64_t>> Expected;
    for (uint64_t X : {1, 2, 3})
      Expected.push_back(execute(F, X));
    EXPECT_TRUE(moveSwitchTailsToTheirExit(F.Body));
    ASSERT_EQ(F.Body.size(), 1U);
    for (uint64_t X : {1, 2, 3})
      EXPECT_EQ(execute(F, X), Expected[X - 1]) << X;
  }
  // A second way out keeps the tail where it is.
  HighFunc F;
  F.Body = {dispatch({caseOf(1, {Break}), caseOf(2, {Break})}, {returning(30)}),
            returning(20)};
  EXPECT_FALSE(moveSwitchTailsToTheirExit(F.Body));
}

TEST(HighControlFlowSemantics, SwitchAbsorbsTheRangeCheckItsDefaultRepeats) {
  // `if (x - 10 <= 2) { switch (x) { case 10..12; default: return 9; } }
  // return 9;`: a value failing the check matches no case and takes the
  // default anyway.  A check that excludes a label must stay.
  auto guarded = [&](uint64_t Bound, uint64_t LastLabel) {
    auto Offset = HighExpr::makeBinop(NdOp::INT_SUB, local(0),
                                      HighExpr::makeConst(10, 8));
    Offset->Type = NdType::makeInt(8, false);
    auto Check = HighExpr::makeBinop(NdOp::INT_LESSEQUAL, Offset,
                                     HighExpr::makeConst(Bound, 8));
    Check->Type = NdType::makeInt(1, false);
    HighStmt Guard;
    Guard.Kind = StmtKind::If;
    Guard.Addr = 0x1000;
    Guard.Cond = Check;
    Guard.Body = {
        dispatch({caseOf(10, {returning(1)}), caseOf(11, {returning(2)}),
                  caseOf(LastLabel, {returning(3)})},
                 {returning(9)})};
    HighFunc F;
    F.Body = {Guard, returning(9)};
    return F;
  };
  HighFunc F = guarded(2, 12);
  std::vector<std::optional<uint64_t>> Expected;
  const std::vector<uint64_t> Inputs{0, 9, 10, 11, 12, 13, UINT64_MAX};
  for (uint64_t X : Inputs)
    Expected.push_back(execute(F, X));
  EXPECT_TRUE(absorbSwitchRangeGuards(F.Body));
  ASSERT_FALSE(F.Body.empty());
  EXPECT_EQ(F.Body.back().Kind, StmtKind::Switch);
  for (size_t I = 0; I < Inputs.size(); ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Expected[I]) << Inputs[I];

  HighFunc Narrow = guarded(1, 12);
  EXPECT_FALSE(absorbSwitchRangeGuards(Narrow.Body));

  // A case that leaves the switch still needs the code after it, so the
  // default's copy goes and unmatched values fall out to that code.
  HighFunc Leaving = guarded(2, 12);
  HighStmt &Inner = Leaving.Body[0].Body[0];
  Inner.Cases[1].Body = {assign(0x1030, 5, 7)};
  Expected.clear();
  for (uint64_t X : Inputs)
    Expected.push_back(execute(Leaving, X));
  EXPECT_TRUE(absorbSwitchRangeGuards(Leaving.Body));
  // The guard's label stays as an empty anchor before the switch.
  ASSERT_EQ(Leaving.Body.size(), 3U);
  EXPECT_EQ(Leaving.Body[1].Kind, StmtKind::Switch);
  EXPECT_TRUE(Leaving.Body[1].DefaultBody.empty());
  for (size_t I = 0; I < Inputs.size(); ++I)
    EXPECT_EQ(execute(Leaving, Inputs[I]), Expected[I]) << Inputs[I];

  // A default that only jumps to the code after the guard drops the jump.
  HighFunc Jumping = guarded(2, 12);
  Jumping.Body[1].Addr = 0x1040;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x1040;
  Jumping.Body[0].Body[0].DefaultBody = {Jump};
  Expected.clear();
  for (uint64_t X : Inputs)
    Expected.push_back(execute(Jumping, X));
  EXPECT_TRUE(absorbSwitchRangeGuards(Jumping.Body));
  ASSERT_EQ(Jumping.Body.size(), 3U);
  EXPECT_TRUE(Jumping.Body[1].DefaultBody.empty());
  for (size_t I = 0; I < Inputs.size(); ++I)
    EXPECT_EQ(execute(Jumping, Inputs[I]), Expected[I]) << Inputs[I];
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

TEST(HighControlFlowSemantics, MemoryBoundaryAncestorsMatchIndividualQueries) {
  for (const bool Atomic : {false, true}) {
    auto Memory = HighExpr::makeLoad(local(0), NdType::makeInt(8));
    if (Atomic)
      Memory->MemoryOrdering = NdMemoryOrdering::Acquire;
    else
      Memory->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    std::vector<ExprPtr> Roots{Memory, local(1)};
    // Many roots share most of their graph; the last roots also share an
    // ordered indirect target, which is not an ordinary call argument.
    for (unsigned I = 0; I < 256; ++I)
      Roots.push_back(
          HighExpr::makeBinop(NdOp::INT_ADD, Roots[I], Roots[I + 1]));
    auto Indirect = HighExpr::makeCall("indirect", 0x4000, {local(2)});
    Indirect->IsIndirectCall = true;
    Indirect->IndirectTarget = Memory;
    Roots.push_back(Indirect);
    Roots.push_back(nullptr);
    const auto Ancestors = findOrderedMemoryAncestors(Roots);
    for (const auto &Root : Roots)
      EXPECT_EQ(Ancestors.count(Root.get()) != 0,
                Root && Root->hasOrderedMemoryAccess());
    EXPECT_FALSE(Ancestors.count(Indirect->Operands.front().get()));
  }

  auto A = HighExpr::makeUnary(NdOp::INT_NEGATE, local(0));
  auto B = HighExpr::makeUnary(NdOp::INT_NEGATE, A);
  A->Operands[0] = B;
  EXPECT_TRUE(findOrderedMemoryAncestors({A, B}).empty());
  auto Memory = HighExpr::makeLoad(local(1), NdType::makeInt(8),
                                   NdMemoryOrdering::Acquire);
  A->Operands.push_back(Memory);
  const auto Ancestors = findOrderedMemoryAncestors({B});
  EXPECT_EQ(Ancestors.size(), 3U);
  EXPECT_TRUE(Ancestors.count(A.get()));
  EXPECT_TRUE(Ancestors.count(B.get()));
  EXPECT_TRUE(Ancestors.count(Memory.get()));
  A->Operands.clear(); // Break the deliberately malformed cycle.
}

TEST(HighControlFlowSemantics, SharedOrderedComparisonsRemainObservable) {
  for (const bool Reverse : {false, true}) {
    HighFunc F;
    auto Memory = HighExpr::makeLoad(local(0), NdType::makeInt(8),
                                     NdMemoryOrdering::Acquire);
    auto Shared = Memory;
    for (unsigned I = 0; I < 512; ++I) {
      Shared = HighExpr::makeBinop(NdOp::INT_ADD, Shared, local(1));
      auto Compare = HighExpr::makeBinop(NdOp::INT_LESS, Shared,
                                         HighExpr::makeConst(0, 8));
      F.Body.push_back(result(0, Compare));
    }
    if (Reverse)
      std::reverse(F.Body.begin(), F.Body.end());
    simplifyAllExprs(F.Body);
    for (const auto &Statement : F.Body) {
      EXPECT_EQ(Statement.RetVal->Op, NdOp::INT_LESS);
      EXPECT_TRUE(Statement.RetVal->hasOrderedMemoryAccess());
    }
    // Effect facts are specific to this invocation, not persistent node
    // annotations. Removing the ordering permits the next pass to fold.
    Memory->MemoryOrdering = NdMemoryOrdering::None;
    simplifyAllExprs(F.Body);
    for (const auto &Statement : F.Body) {
      EXPECT_EQ(Statement.RetVal->Kind, ExprKind::Const);
      EXPECT_EQ(Statement.RetVal->ConstVal, 0U);
    }
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

TEST(HighControlFlowSemantics, TruncatedWideArithmeticDropsUnreadUpperHalves) {
  // An x86-64 `int` parameter read through its 64-bit register has undefined
  // upper bytes: `(u32)(concat(?, a) OP concat(?, b))`.  The low bytes of a
  // sum, difference, product or bitwise result read only the operands' low
  // bytes, so the undefined halves drop out and OP runs at 32 bits.
  auto HasUndef = [](const ExprPtr &Root) {
    std::vector<const HighExpr *> Pending{Root.get()};
    while (!Pending.empty()) {
      const HighExpr *E = Pending.back();
      Pending.pop_back();
      if (!E)
        continue;
      if (E->Kind == ExprKind::Undef)
        return true;
      for (const ExprPtr &Operand : E->Operands)
        Pending.push_back(Operand.get());
    }
    return false;
  };
  const std::vector<uint64_t> Inputs = {0, UINT64_MAX,
                                        UINT64_C(0x8765432101234567),
                                        UINT64_C(0x00000001ffffffff)};
  for (NdOp Op : {NdOp::INT_ADD, NdOp::INT_SUB, NdOp::INT_MULT, NdOp::INT_AND,
                  NdOp::INT_OR, NdOp::INT_XOR})
    for (const bool Cast : {false, true}) {
      SCOPED_TRACE(Cast);
      auto A = byteSlice(local(0), 0, 4);
      auto B = byteSlice(local(0), 4, 4);
      auto Wide =
          HighExpr::makeBinop(Op, concatenate(HighExpr::makeUndef(4), A),
                              concatenate(HighExpr::makeUndef(4), B));
      Wide->Type = NdType::makeInt(8, false);
      ExprPtr View = byteSlice(Wide, 0, 4);
      if (Cast) {
        View->Kind = ExprKind::Cast;
        View->CastTo = View->Type;
        View->Operands = {Wide};
      }
      HighFunc F;
      F.Body = {result(0, View)};
      simplifyAllExprs(F.Body);
      const ExprPtr &Narrow = F.Body[0].RetVal;
      ASSERT_EQ(Narrow->Kind, ExprKind::BinOp);
      EXPECT_EQ(Narrow->Op, Op);
      EXPECT_EQ(Narrow->Type->Size, 4u);
      EXPECT_FALSE(HasUndef(Narrow));
      if (Op == NdOp::INT_OR || Op == NdOp::INT_XOR)
        continue;
      for (uint64_t Input : Inputs) {
        const uint64_t Lo = Input & 0xffffffff, Hi = Input >> 32;
        const uint64_t Expected = Op == NdOp::INT_ADD    ? Lo + Hi
                                  : Op == NdOp::INT_SUB  ? Lo - Hi
                                  : Op == NdOp::INT_MULT ? Lo * Hi
                                                         : Lo & Hi;
        EXPECT_EQ(*execute(F, Input) & 0xffffffff, Expected & 0xffffffff);
      }
    }

  // `(hi << 32) | lo` read at 32 bits is `lo`.
  auto Lo = byteSlice(local(0), 0, 4);
  auto Hi = HighExpr::makeUnary(NdOp::INT_ZEXT, byteSlice(local(0), 4, 4));
  Hi->Type = NdType::makeInt(8, false);
  auto Shifted =
      HighExpr::makeBinop(NdOp::INT_LEFT, Hi, HighExpr::makeConst(32, 8));
  Shifted->Type = NdType::makeInt(8, false);
  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Lo);
  Extended->Type = NdType::makeInt(8, false);
  auto Pair = HighExpr::makeBinop(NdOp::INT_OR, Shifted, Extended);
  Pair->Type = NdType::makeInt(8, false);
  HighFunc F;
  F.Body = {result(0, byteSlice(Pair, 0, 4))};
  simplifyAllExprs(F.Body);
  EXPECT_EQ(F.Body[0].RetVal, Lo);

  // All eight bytes are read: the undefined upper half stays observable.
  auto Full = concatenate(HighExpr::makeUndef(4), byteSlice(local(0), 0, 4));
  HighFunc Observed;
  Observed.Body = {result(0, Full)};
  simplifyAllExprs(Observed.Body);
  EXPECT_TRUE(HasUndef(Observed.Body[0].RetVal));
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
    for (uint64_t Input :
         {uint64_t{0}, uint64_t{1}, uint64_t{2}, uint64_t{0xff}, UINT64_MAX})
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
        NdOp::INT_AND, Joined, HighExpr::makeConst(Case == 0 ? 0x100 : 1, 8));
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
  Outer.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(0, 8));
  auto Inner = conditional(0x1010, 0x1050);
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  F.Body = {Outer,
            Inner,
            assign(0x1020, 1, 2),
            jump(0x1030, 0x1060),
            assign(0x1040, 1, 1),
            jump(0x1044, 0x1060),
            assign(0x1050, 1, 0),
            result(0x1060, local(1))};

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
    Last.Opcode = I < 2 ? NdOp::COND_BR : I == 5 ? NdOp::RETURN : NdOp::BRANCH;
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

TEST(HighControlFlowSemantics, DeepStableContainersPreserveEveryReturnPath) {
  // Stable trees must not require an exponential number of child traversals.
  // The independent interpreter checks both entered and bypassed paths; the
  // generous time limit distinguishes duplicate recursion from runner noise.
  for (auto Kind :
       {StmtKind::Block, StmtKind::While, StmtKind::Switch, StmtKind::SEHTry}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    HighStmt Leaf = result(0x2000, local(0));
    for (unsigned Depth = 0; Depth < 48; ++Depth) {
      HighStmt Parent;
      Parent.Kind = Kind;
      Parent.Addr = 0x1000 + 4 * Depth;
      if (Kind == StmtKind::While)
        Parent.Cond = local(0);
      if (Kind == StmtKind::Switch) {
        Parent.SwitchExpr = local(0);
        Parent.Cases.push_back({1, {std::move(Leaf)}});
        Parent.DefaultBody = {
            result(0x3000 + 4 * Depth, HighExpr::makeConst(19, 8))};
      } else {
        Parent.Body.push_back(std::move(Leaf));
      }
      Leaf = std::move(Parent);
    }
    HighFunc F;
    F.Body = {Leaf, result(0x4000, HighExpr::makeConst(99, 8))};
    std::vector<std::optional<uint64_t>> Before;
    for (uint64_t Input : {0u, 1u, 2u, 255u})
      Before.push_back(execute(F, Input));
    const auto Started = std::chrono::steady_clock::now();
    structureIfElse(F, 10);
    EXPECT_LT(std::chrono::steady_clock::now() - Started,
              std::chrono::seconds(5));
    size_t Index = 0;
    for (uint64_t Input : {0u, 1u, 2u, 255u})
      EXPECT_EQ(execute(F, Input), Before[Index++]);
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

TEST(HighControlFlowSemantics, DistantJoinReadKeepsTakenPhiCopy) {
  HighFunc F;
  auto Branch = conditional(0x1004, 0x1040);
  auto Copy = assign(0x1004, 1, 0);
  Copy.Val = local(2);
  Copy.IsPhiCopy = true;
  Branch.Body.insert(Branch.Body.begin(), Copy);
  HighStmt Work;
  Work.Kind = StmtKind::If;
  Work.Addr = 0x1008;
  Work.Cond = HighExpr::makeConst(1, 1);
  Work.Body = {assign(0x100c, 3, 5)};
  F.Body = {assign(0x1000, 1, 3), assign(0x1002, 2, 7), Branch, Work};
  for (int I = 0; I < 12; ++I)
    F.Body.push_back(assign(0x1040 + I * 4, 100 + I, I));
  F.Body.push_back(result(0x1080, local(1)));

  ASSERT_EQ(execute(F, 0), 3u);
  ASSERT_EQ(execute(F, 1), 7u);
  structureIfElse(F, 1);
  EXPECT_EQ(execute(F, 0), 3u);
  EXPECT_EQ(execute(F, 1), 7u);
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), 3u);
  EXPECT_EQ(execute(F, 1), 7u);
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

TEST(HighControlFlowSemantics, ThreadedSoleSuccessorKeepsItsTransferAndPhi) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    MedFunc M;
    M.Entry = 0x1000;
    M.Name = "threaded_single_successor";
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
    M.Blocks[0].Succs = {1, 2, 3};
    M.Blocks[0].Ops = {operation(NdOp::INDIR_BR, 0x1000, {}, {Input})};
    M.SwitchSelectorPlans[{0x1000, 0}] = {};
    M.SwitchSelectorPlans[{0x1000, 0}].Selector = Input;
    M.SwitchSelectorPlans[{0x1000, 0}].ResultSize = 8;
    auto Joined = machineValue(1, Architecture);
    auto Return = machineValue(2, Architecture);
    Return.Kind = MedVar::Reg;
    Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    Return.SSAVer = 1;
    M.Blocks[1].Preds = {0, 2};
    M.Blocks[1].Phis = {{Joined, {{0, C(7)}, {2, C(37)}}}};
    M.Blocks[1].Ops = {operation(NdOp::COPY, 0x1100, Return, {Joined}),
                       operation(NdOp::RETURN, 0x1104, {}, {Return})};
    M.Blocks[2].Preds = {0};
    M.Blocks[2].Succs = {1};
    // Threading an empty jump block leaves the edge in the CFG without a
    // BRANCH operation. Its successor is not the next block in source order.
    M.Blocks[2].Ops = {operation(NdOp::STORE, 0x1200, {}, {C(0x8000), C(19)})};
    M.Blocks[3].Preds = {0};
    auto OtherReturn = Return;
    OtherReturn.SSAVer = 2;
    M.Blocks[3].Ops = {operation(NdOp::COPY, 0x1300, OtherReturn, {C(93)}),
                       operation(NdOp::RETURN, 0x1304, {}, {OtherReturn})};
    JumpTable Table;
    Table.InsnAddr = 0x1000;
    Table.Targets = {0x1100, 0x1200, 0x1300};
    Table.CaseLabels = {0, 1, 2};
    MedToHighConverter Converter;
    Converter.setJumpTables({Table});
    const auto F = Converter.convert(M, Architecture);
    const uint64_t Expected[] = {7, 37, 93};
    for (uint64_t Condition : {0u, 1u, 2u}) {
      SCOPED_TRACE(Condition);
      EXPECT_NO_THROW(
          EXPECT_EQ(execute(F, Condition, true), Expected[Condition]));
    }
  }
}

TEST(HighControlFlowSemantics, SwitchPublishesTheCaseOfEachTablePosition) {
  // `switch (x) { case 10: case 11: case 12: }` dispatches on `x - 10`. The
  // recovered switch prints 10..12, and the listing labels each table entry
  // with the same case: publish the value that selects every position.
  const Arch Architecture = Arch::X64;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "offset_switch";
  M.ReturnType = NdType::makeInt(8, false);
  auto Input = machineValue(0, Architecture);
  Input.Kind = MedVar::Param;
  Input.RegOff = TRI.IntParamRegs[0];
  M.Params = {Input};
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  const auto Index = machineValue(1, Architecture);
  M.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    M.Blocks[I].Id = I;
    M.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    M.Blocks[I].EndAddr = M.Blocks[I].StartAddr + 0x20;
  }
  M.Blocks[0].Succs = {1, 2, 3};
  M.Blocks[0].Ops = {
      operation(NdOp::INT_ADD, 0x1000, Index, {Input, C(uint64_t(-10))}),
      operation(NdOp::INDIR_BR, 0x1008, {}, {Index})};
  M.SwitchSelectorPlans[{0x1008, 0}] = {};
  M.SwitchSelectorPlans[{0x1008, 0}].Selector = Index;
  M.SwitchSelectorPlans[{0x1008, 0}].ResultSize = 8;
  const uint64_t Results[] = {7, 37, 93};
  for (int I = 1; I < 4; ++I) {
    auto Return = machineValue(2, Architecture);
    Return.Kind = MedVar::Reg;
    Return.RegOff = TRI.IntReturnReg;
    Return.SSAVer = I;
    const va_t Start = M.Blocks[I].StartAddr;
    M.Blocks[I].Preds = {0};
    M.Blocks[I].Ops = {
        operation(NdOp::COPY, Start, Return, {C(Results[I - 1])}),
        operation(NdOp::RETURN, Start + 4, {}, {Return})};
  }
  JumpTable Table;
  Table.InsnAddr = 0x1008;
  Table.Targets = {0x1100, 0x1200, 0x1300};
  Table.CaseLabels = {0, 1, 2};
  MedToHighConverter Converter;
  Converter.setJumpTables({Table});
  const auto F = Converter.convert(M, Architecture);

  const auto Published = F.SwitchLabelsByJump.find(0x1008);
  ASSERT_NE(Published, F.SwitchLabelsByJump.end());
  EXPECT_EQ(Published->second.Values, (std::vector<uint64_t>{10, 11, 12}));
  EXPECT_EQ(Published->second.DefaultPosition, -1);
  EXPECT_EQ(Published->second.SelectorBits, 64u);
  for (size_t Position = 0; Position < Published->second.Values.size();
       ++Position) {
    SCOPED_TRACE(Position);
    EXPECT_NO_THROW(
        EXPECT_EQ(execute(F, Published->second.Values[Position], true),
                  Results[Position]));
  }
}

TEST(HighControlFlowSemantics, ThreadedFallthroughJumpsPastTheNextBlock) {
  // PiCMCaptureRegistryPropertyInputData: cold code falls into a `jmp` back
  // to the hot path.  Threading that jump-only block leaves a block without a
  // terminator whose sole successor is not the next block; it must not run
  // into the block laid out after it.
  const Arch Architecture = Arch::X64;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "threaded_fallthrough";
  M.ReturnType = NdType::makeInt(8, false);
  auto Input = machineValue(0, Architecture);
  Input.Kind = MedVar::Param;
  Input.RegOff = TRI.IntParamRegs[0];
  M.Params = {Input};
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  auto IsZero = machineValue(1, Architecture);
  IsZero.Size = 1;
  auto Result = [&](int Version) {
    auto V = machineValue(2, Architecture);
    V.Kind = MedVar::Reg;
    V.RegOff = TRI.IntReturnReg;
    V.SSAVer = Version;
    return V;
  };
  M.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    M.Blocks[I].Id = I;
    M.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    M.Blocks[I].EndAddr = M.Blocks[I].StartAddr + 0x20;
  }
  M.Blocks[0].Succs = {2, 1};
  M.Blocks[0].Ops = {operation(NdOp::INT_EQUAL, 0x1000, IsZero, {Input, C(0)}),
                     operation(NdOp::COND_BR, 0x1004, {}, {C(0x1200), IsZero})};
  M.Blocks[1].Preds = {0};
  M.Blocks[1].Succs = {3};
  M.Blocks[1].Ops = {operation(NdOp::COPY, 0x1100, Result(1), {C(5)})};
  M.Blocks[2].Preds = {0};
  M.Blocks[2].Succs = {3};
  M.Blocks[2].Ops = {operation(NdOp::COPY, 0x1200, Result(2), {C(7)})};
  M.Blocks[3].Preds = {1, 2};
  M.Blocks[3].Phis = {{Result(3), {{1, Result(1)}, {2, Result(2)}}}};
  M.Blocks[3].Ops = {operation(NdOp::RETURN, 0x1300, {}, {Result(3)})};
  const auto F = MedToHighConverter().convert(M, Architecture);
  for (uint64_t Condition : {0u, 1u}) {
    SCOPED_TRACE(Condition);
    EXPECT_NO_THROW(
        EXPECT_EQ(execute(F, Condition, true), Condition ? 5u : 7u));
  }
}

namespace {
// A function of blocks at 0x1000 + 0x100 * I that returns its result
// register; the interpreter's input is its first parameter.
MedFunc blockFunction(const char *Name, int Blocks) {
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = Name;
  M.ReturnType = NdType::makeInt(8, false);
  auto Input = machineValue(0, Arch::X64);
  Input.Kind = MedVar::Param;
  Input.RegOff = getTargetRegInfo(Arch::X64).IntParamRegs[0];
  M.Params = {Input};
  M.Blocks.resize(Blocks);
  for (int I = 0; I < Blocks; ++I) {
    M.Blocks[I].Id = I;
    M.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    M.Blocks[I].EndAddr = M.Blocks[I].StartAddr + 0x20;
  }
  return M;
}
MedVar resultValue(int Version) {
  auto V = machineValue(2, Arch::X64);
  V.Kind = MedVar::Reg;
  V.RegOff = getTargetRegInfo(Arch::X64).IntReturnReg;
  V.SSAVer = Version;
  return V;
}
size_t countTests(const HighFunc &F) {
  size_t Tests = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Tests += S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse;
  });
  return Tests;
}
} // namespace

TEST(HighControlFlowSemantics, ConstantBranchKeepsOnlyTheArmThatRuns) {
  // deregister_tm_clones compares two addresses of one object, so its branch
  // always goes one way, and `if (1) { ... } else { ... }` printed both
  // arms. Whichever way a constant branch goes and however its arms are
  // laid out, only the arm that runs remains, without a test.
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  for (uint64_t Condition : {0u, 1u}) {
    for (va_t Target : {va_t{0x1100}, va_t{0x1200}}) {
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << Condition << " " << Target << " " << Reverse);
        MedFunc M = blockFunction("constant_branch", 4);
        M.Blocks[0].Succs = {Target == 0x1100 ? 1 : 2,
                             Target == 0x1100 ? 2 : 1};
        if (Reverse)
          std::swap(M.Blocks[0].Succs[0], M.Blocks[0].Succs[1]);
        M.Blocks[0].Ops = {
            operation(NdOp::COND_BR, 0x1004, {},
                      {C(Target), MedVar::makeConst(Condition, 1)})};
        M.Blocks[1].Preds = {0};
        M.Blocks[1].Succs = {3};
        M.Blocks[1].Ops = {
            operation(NdOp::COPY, 0x1100, resultValue(1), {C(5)}),
            operation(NdOp::BRANCH, 0x1104, {}, {C(0x1300)})};
        M.Blocks[2].Preds = {0};
        M.Blocks[2].Succs = {3};
        M.Blocks[2].Ops = {
            operation(NdOp::COPY, 0x1200, resultValue(2), {C(7)})};
        M.Blocks[3].Preds = {1, 2};
        M.Blocks[3].Phis = {
            {resultValue(3), {{1, resultValue(1)}, {2, resultValue(2)}}}};
        M.Blocks[3].Ops = {
            operation(NdOp::RETURN, 0x1300, {}, {resultValue(3)})};
        const auto F = MedToHighConverter().convert(M, Arch::X64);
        EXPECT_EQ(countTests(F), 0u);
        const bool RunsBlock1 = (Target == 0x1100) == (Condition != 0);
        for (uint64_t Input : {0u, 1u})
          EXPECT_NO_THROW(
              EXPECT_EQ(execute(F, Input, true), RunsBlock1 ? 5u : 7u));
      }
    }
  }
}

TEST(HighControlFlowSemantics, AlwaysTakenBranchKeepsAnArmAJumpEnters) {
  // The arm the constant branch never takes is also the target of a later
  // test, so it still runs.
  MedFunc M = blockFunction("entered_arm", 5);
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  auto IsZero = machineValue(1, Arch::X64);
  IsZero.Size = 1;
  M.Blocks[0].Succs = {1, 2};
  M.Blocks[0].Ops = {operation(NdOp::COND_BR, 0x1004, {},
                               {C(0x1100), MedVar::makeConst(1, 1)})};
  M.Blocks[1].Preds = {0};
  M.Blocks[1].Succs = {3};
  M.Blocks[1].Ops = {operation(NdOp::COPY, 0x1100, resultValue(1), {C(5)}),
                     operation(NdOp::BRANCH, 0x1104, {}, {C(0x1300)})};
  M.Blocks[2].Preds = {0, 3};
  M.Blocks[2].Ops = {operation(NdOp::COPY, 0x1200, resultValue(2), {C(7)}),
                     operation(NdOp::RETURN, 0x1204, {}, {resultValue(2)})};
  M.Blocks[3].Preds = {1};
  M.Blocks[3].Succs = {2, 4};
  M.Blocks[3].Ops = {
      operation(NdOp::INT_EQUAL, 0x1300, IsZero, {M.Params.front(), C(0)}),
      operation(NdOp::COND_BR, 0x1304, {}, {C(0x1200), IsZero})};
  M.Blocks[4].Preds = {3};
  M.Blocks[4].Ops = {operation(NdOp::RETURN, 0x1400, {}, {resultValue(1)})};
  const auto F = MedToHighConverter().convert(M, Arch::X64);
  EXPECT_NO_THROW(EXPECT_EQ(execute(F, 0, true), 7u));
  EXPECT_NO_THROW(EXPECT_EQ(execute(F, 1, true), 5u));
}

TEST(HighControlFlowSemantics, AlwaysTakenSkipNeverRunsTheSkippedCall) {
  // A branch that always skips a call: the call never runs. The oracle's
  // `observe` reads memory nothing wrote, so running it throws.
  MedFunc M = blockFunction("always_skipped", 3);
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  M.Blocks[0].Succs = {2, 1};
  M.Blocks[0].Ops = {operation(NdOp::COPY, 0x1000, resultValue(1), {C(7)}),
                     operation(NdOp::COND_BR, 0x1004, {},
                               {C(0x1200), MedVar::makeConst(1, 1)})};
  M.Blocks[1].Preds = {0};
  M.Blocks[1].Succs = {2};
  auto Call = operation(NdOp::CALL, 0x1100, machineValue(3, Arch::X64),
                        {C(0x2000), C(0), C(0x9000)});
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->Signature.ReturnType = M.ReturnType;
  Hint->Signature.Parameters = {{"unused", M.ReturnType, {}},
                                {"address", NdType::makePtr(M.ReturnType), {}}};
  Call.SourceCallHint = Hint;
  M.Blocks[1].Ops = {Call};
  M.Blocks[2].Preds = {0, 1};
  M.Blocks[2].Ops = {operation(NdOp::RETURN, 0x1200, {}, {resultValue(1)})};
  const std::map<va_t, std::string> Names{{0x2000, "observe"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  const auto F = Converter.convert(M, Arch::X64);
  for (uint64_t Input : {0u, 1u}) {
    SCOPED_TRACE(Input);
    EXPECT_NO_THROW(EXPECT_EQ(execute(F, Input, true), 7u));
  }
}

TEST(HighControlFlowSemantics,
     IncomingRegisterBehindAVersionedSeedIsTheParameter) {
  // PiCMCaptureRegistryPropertyInputData: SSA versions a parameter
  // register's entry seed (`COPY RCX.1 = RCX` there, CL being seeded too)
  // while later reads still name the incoming register.  Those reads are the
  // first parameter, not an unknown register.
  const Arch Architecture = Arch::X64;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "versioned_seed";
  M.ReturnType = NdType::makeInt(8, false);
  auto Input = machineValue(0, Architecture);
  Input.Kind = MedVar::Param;
  Input.RegOff = TRI.IntParamRegs[0];
  M.Params = {Input};
  auto Incoming = machineValue(5, Architecture);
  Incoming.Kind = MedVar::Reg;
  Incoming.RegOff = TRI.IntParamRegs[0];
  auto Seeded = Incoming;
  Seeded.SSAVer = 1;
  auto Sum = machineValue(6, Architecture);
  auto Return = machineValue(7, Architecture);
  Return.Kind = MedVar::Reg;
  Return.RegOff = TRI.IntReturnReg;
  Return.SSAVer = 1;
  M.Blocks.resize(1);
  M.Blocks[0].Id = 0;
  M.Blocks[0].StartAddr = 0x1000;
  M.Blocks[0].EndAddr = 0x1020;
  M.Blocks[0].Ops = {operation(NdOp::COPY, 0x1000, Seeded, {Incoming}),
                     operation(NdOp::INT_ADD, 0x1004, Sum,
                               {Incoming, MedVar::makeConst(1, 8)}),
                     operation(NdOp::COPY, 0x1008, Return, {Sum}),
                     operation(NdOp::RETURN, 0x100c, {}, {Return})};
  const auto F = MedToHighConverter().convert(M, Architecture);
  EXPECT_NO_THROW(EXPECT_EQ(execute(F, 41, true), 42u));
}

TEST(HighControlFlowSemantics, GotoToReturnBlockKeepsItsStoreAndLoad) {
  // Both arms jump to `sink = v; r = sink; return r;`.  Folding the jump into
  // `return r` would skip the store and read an undefined value.
  const Arch Architecture = Arch::X64;
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "store_load_return_tail";
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
  M.SwitchSelectorPlans[{0x1000, 0}] = {};
  M.SwitchSelectorPlans[{0x1000, 0}].Selector = Input;
  M.SwitchSelectorPlans[{0x1000, 0}].ResultSize = 8;
  for (int I = 1; I < 3; ++I) {
    M.Blocks[I].Preds = {0};
    M.Blocks[I].Succs = {3};
    M.Blocks[I].Ops = {
        operation(NdOp::BRANCH, M.Blocks[I].StartAddr, {}, {C(0x1300)})};
  }
  auto Joined = machineValue(1, Architecture);
  auto Loaded = machineValue(2, Architecture);
  auto Return = machineValue(3, Architecture);
  Return.Kind = MedVar::Reg;
  Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
  Return.SSAVer = 1;
  M.Blocks[3].Preds = {1, 2};
  M.Blocks[3].Phis = {{Joined, {{1, C(17)}, {2, C(23)}}}};
  M.Blocks[3].Ops = {operation(NdOp::STORE, 0x1300, {}, {C(0x8000), Joined}),
                     operation(NdOp::LOAD, 0x1304, Loaded, {C(0x8000)}),
                     operation(NdOp::COPY, 0x1308, Return, {Loaded}),
                     operation(NdOp::RETURN, 0x130c, {}, {Return})};
  JumpTable Table;
  Table.InsnAddr = 0x1000;
  Table.Targets = {0x1100, 0x1200};
  Table.CaseLabels = {0, 1};
  MedToHighConverter Converter;
  Converter.setJumpTables({Table});
  const auto F = Converter.convert(M, Architecture);
  unsigned Stores = 0;
  walkStmts(F.Body,
            [&](const HighStmt &S) { Stores += S.Kind == StmtKind::Store; });
  EXPECT_GE(Stores, 1u);
  for (uint64_t Condition : {0u, 1u}) {
    SCOPED_TRACE(Condition);
    EXPECT_NO_THROW(
        EXPECT_EQ(execute(F, Condition, true), Condition ? 23u : 17u));
  }
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
    Med.Blocks[2].Ops = {operation(NdOp::COPY, 0x1200, TrueReturn, {C(1)}),
                         operation(NdOp::RETURN, 0x1204, {}, {TrueReturn})};
    Med.Blocks[3].Preds = {1};
    Med.Blocks[3].Ops = {operation(NdOp::COPY, 0x1300, FalseReturn, {C(0)}),
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

TEST(HighControlFlowSemantics, JoinUpdatesDoNotInventEntryValuesForArmLoads) {
  // A shared return sits before some of its predecessors in layout order.
  // Each arm loads its own base before updating the return PHI. An update
  // `joined = loaded + k` does not permit reading `loaded` before that arm.
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    MedFunc Med;
    Med.Entry = 0x1000;
    Med.Name = "branch_local_load_join";
    Med.ReturnType = NdType::makeInt(8, false);
    auto Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
    Med.Params = {Input};
    auto First = machineValue(1, Architecture);
    auto Second = machineValue(2, Architecture);
    auto Payload = machineValue(3, Architecture);
    auto Joined = machineValue(4, Architecture);
    auto Return = machineValue(5, Architecture);
    Return.Kind = MedVar::Reg;
    Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    Return.SSAVer = 1;
    auto C = [](uint64_t Value) { return MedVar::makeConst(Value, 8); };
    Med.Blocks.resize(6);
    for (int I = 0; I < 6; ++I) {
      Med.Blocks[I].Id = I;
      Med.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      Med.Blocks[I].EndAddr = Med.Blocks[I].StartAddr + 0x20;
    }
    Med.Blocks[0].Succs = {1, 2};
    Med.Blocks[0].Ops = {
        operation(NdOp::INT_ADD, 0x1000, Payload, {Input, C(71)}),
        operation(NdOp::STORE, 0x1004, {}, {C(0x8000), Payload}),
        operation(NdOp::INT_AND, 0x1008, First, {Input, C(1)}),
        operation(NdOp::INT_AND, 0x100c, Second, {Input, C(2)}),
        operation(NdOp::COND_BR, 0x1010, {}, {C(0x1100), First})};
    Med.Blocks[2].Preds = {0};
    Med.Blocks[2].Succs = {4, 5};
    Med.Blocks[2].Ops = {
        operation(NdOp::COND_BR, 0x1200, {}, {C(0x1400), Second})};
    for (int I : {1, 4, 5}) {
      auto Loaded = machineValue(10 + I, Architecture);
      auto Updated = machineValue(20 + I, Architecture);
      auto &Block = Med.Blocks[I];
      Block.Preds = {I == 1 ? 0 : 2};
      Block.Ops = {operation(NdOp::LOAD, Block.StartAddr, Loaded, {C(0x8000)}),
                   operation(NdOp::INT_ADD, Block.StartAddr + 4, Updated,
                             {Loaded, C(I == 1   ? 9
                                        : I == 4 ? 22
                                                 : 35)})};
      if (I == 5) {
        auto FinalReturn = Return;
        FinalReturn.SSAVer = 2;
        Block.Ops.push_back(
            operation(NdOp::COPY, Block.StartAddr + 8, FinalReturn, {Updated}));
        Block.Ops.push_back(
            operation(NdOp::RETURN, Block.StartAddr + 12, {}, {FinalReturn}));
      } else {
        Block.Succs = {3};
        Block.Ops.push_back(
            operation(NdOp::BRANCH, Block.StartAddr + 8, {}, {C(0x1300)}));
      }
    }
    Med.Blocks[3].Preds = {1, 4};
    Med.Blocks[3].Phis = {{Joined,
                           {{1, machineValue(21, Architecture)},
                            {4, machineValue(24, Architecture)}}}};
    Med.Blocks[3].Ops = {operation(NdOp::COPY, 0x1300, Return, {Joined}),
                         operation(NdOp::RETURN, 0x1304, {}, {Return})};
    const auto High = MedToHighConverter().convert(Med, Architecture);
    const auto Flow = analyzeHighSourceFlow(High, true);
    ASSERT_TRUE(Flow.Complete);
    EXPECT_TRUE(Flow.Items.empty());
    for (uint64_t Value : {0ULL, 1ULL, 2ULL, 3ULL, 91ULL, ~0ULL}) {
      SCOPED_TRACE(Value);
      const uint64_t Expected = Value + 71 +
                                (Value & 1   ? 9
                                 : Value & 2 ? 22
                                             : 35);
      EXPECT_NO_THROW(EXPECT_EQ(execute(High, Value, true), Expected));
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
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
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

TEST(HighControlFlowSemantics,
     SameTargetGuardsKeepDistinctPredicateCallsAndLaterValueUse) {
  HighFunc F;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8);
  auto Store = [](va_t Address, va_t Slot, ExprPtr Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Address;
    S.StoreAddr = HighExpr::makeConst(Slot, 8);
    S.StoreVal = std::move(Value);
    return S;
  };
  auto Observe = [](va_t Slot) {
    auto Call = HighExpr::makeCall(
        "observe", 0,
        {HighExpr::makeConst(0, 8), HighExpr::makeConst(Slot, 8)});
    Call->Type = NdType::makeInt(8);
    return Call;
  };
  auto FirstCall = assign(0x1008, 1, 0);
  FirstCall.Val = Observe(0x2000);
  auto FirstGuard = conditional(0x100c, 0x1040);
  FirstGuard.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(1), HighExpr::makeConst(0, 8));
  auto SecondCall = assign(0x1010, 2, 0);
  SecondCall.Val = Observe(0x2008);
  auto SecondGuard = conditional(0x1014, 0x1040);
  SecondGuard.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(2), HighExpr::makeConst(0, 8));
  F.Body = {Store(0x1000, 0x2000, HighExpr::makeConst(1, 8)),
            Store(0x1004, 0x2008, local(0)),
            FirstCall,
            FirstGuard,
            SecondCall,
            SecondGuard,
            result(0x1018, local(2)),
            result(0x1040, HighExpr::makeConst(7, 8))};

  ASSERT_EQ(execute(F, 0, true), 7U);
  ASSERT_EQ(execute(F, 1, true), 1U);
  invertSkipGotos(F);
  EXPECT_EQ(execute(F, 0, true), 7U);
  EXPECT_EQ(execute(F, 1, true), 1U);
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
  JoinWork.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(2), HighExpr::makeConst(5, 8));
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

TEST(HighControlFlowSemantics, SameTargetSkipKeepsTakenPhiCopy) {
  HighFunc F;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8);
  auto Choice = conditional(0x1000, 0x1040);
  auto TakenCopy = assign(0, 1, 7);
  TakenCopy.IsPhiCopy = true;
  Choice.Body.insert(Choice.Body.begin(), TakenCopy);
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.Addr = 0x1010;
  Store.StoreAddr = HighExpr::makeConst(0x2000, 8);
  Store.StoreVal = HighExpr::makeConst(1, 8);
  auto OtherCopy = assign(0, 1, 9);
  OtherCopy.IsPhiCopy = true;
  F.Body = {Choice, Store, OtherCopy, jump(0x1018, 0x1040),
            result(0x1040, local(1))};

  ASSERT_EQ(execute(F, 0, true), 9U);
  ASSERT_EQ(execute(F, 1, true), 7U);
  invertSkipGotos(F);
  EXPECT_EQ(execute(F, 0, true), 9U);
  EXPECT_EQ(execute(F, 1, true), 7U);
}

TEST(HighControlFlowSemantics, ExternalSkipInvertKeepsSharedTailEntry) {
  HighFunc F;
  F.Entry = 0x1000;
  auto Shared = assign(0x1020, 1, 0);
  Shared.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  F.Body = {assign(0x1000, 1, 0), conditional(0x1004, 0x1040),
            assign(0x1010, 1, 5), Shared,
            jump(0x1024, 0x1030), result(0x1030, local(1)),
            jump(0x1040, 0x1020)};

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
  Load.Val =
      HighExpr::makeLoad(HighExpr::makeConst(0x2000, 8), NdType::makeInt(8));
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

TEST(HighControlFlowSemantics, TableCallKeepsTheSelectedEntry) {
  // `fns[i % 3](x)` in a non-PIE build loads its target from
  // `fns(,%rdi,8)`. The table's symbol names the first slot only: a call by
  // that name would drop the index. One constant slot, `rip + disp` folded
  // in two addends, still names the pointer it holds.
  const Arch Architecture = Arch::X64;
  const std::map<va_t, std::string> Names{{0x404040, "fns"}};
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  auto callIn = [](const HighFunc &F) -> ExprPtr {
    for (const HighStmt &S : F.Body) {
      if (S.Kind == StmtKind::Call && S.CallExpr)
        return S.CallExpr;
      if (S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Call)
        return S.Val;
    }
    return nullptr;
  };
  for (bool Indexed : {true, false}) {
    SCOPED_TRACE(Indexed);
    MedFunc M;
    M.Entry = 0x1000;
    M.Name = "table_call";
    M.ReturnType = NdType::makeVoid();
    auto Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
    M.Params = {Input};
    const auto Offset = machineValue(1, Architecture);
    const auto Slot = machineValue(2, Architecture);
    const auto Target = machineValue(3, Architecture);
    M.Blocks.resize(1);
    M.Blocks[0].Id = 0;
    M.Blocks[0].StartAddr = 0x1000;
    M.Blocks[0].EndAddr = 0x1020;
    M.Blocks[0].Ops = {
        operation(NdOp::INT_MULT, 0x1000, Offset, {Input, C(8)}),
        Indexed
            ? operation(NdOp::INT_ADD, 0x1004, Slot, {Offset, C(0x404040)})
            : operation(NdOp::INT_ADD, 0x1004, Slot, {C(0x404000), C(0x40)}),
        operation(NdOp::LOAD, 0x1004, Target, {Slot}),
        operation(NdOp::INDIR_CALL, 0x100c, {}, {Target}),
        operation(NdOp::RETURN, 0x1010, {}, {})};
    MedToHighConverter Converter;
    Converter.setFuncNames(&Names);
    const auto F = Converter.convert(M, Architecture);
    const ExprPtr Call = callIn(F);
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->IsIndirectCall, Indexed);
    if (Indexed)
      EXPECT_TRUE(Call->IndirectTarget);
    else
      EXPECT_EQ(Call->CallTarget, "fns");
  }
}

TEST(HighControlFlowSemantics, SlotCallGoesThroughThePointerItHolds) {
  // x86 lifts `call [rip+disp]` as an INDIR_CALL of the slot address. A
  // symbol on a data slot names the variable, `int (*handler)(int)`, not the
  // callee: calling it by name would run the variable. A bound import slot
  // names its import, and a constant folded into `call rax` that lands in
  // code is the callee itself.
  const Arch Architecture = Arch::X64;
  BinaryImage Image;
  Image.Format = BinaryFormat::ELF;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Segment Code;
  Code.VA = 0x401000;
  Code.Size = Code.FileSz = 0x100;
  Code.Data.resize(0x100);
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(Code);
  Segment Data;
  Data.VA = 0x404000;
  Data.Size = Data.FileSz = 0x100;
  Data.Data.resize(0x100);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(Data);
  Symbol Handler;
  Handler.Name = "handler";
  Handler.Addr = 0x404018;
  Image.Symbols.push_back(Handler);
  Symbol Target;
  Target.Name = "target";
  Target.Addr = 0x401060;
  Image.Symbols.push_back(Target);
  Import Bound;
  Bound.Name = "puts";
  Bound.IATAddr = 0x404020;
  Image.Imports.push_back(Bound);
  // An ELF GLOB_DAT slot, and one that holds `import + 8`.
  ASSERT_TRUE(Image.recordImportStorageSlot(0x404028, "__libc_start_main", 0,
                                            ImportStorageEvidence::LoaderBind));
  ASSERT_TRUE(Image.recordImportStorageSlot(0x404030, "table", 8,
                                            ImportStorageEvidence::LoaderBind));
  // An unnamed API-set directory entry named by its slot symbol, and a slot
  // only the linker's `__imp_` symbol identifies.
  Import Unnamed;
  Unnamed.Module = "ext-ms-win-fs-clfs-l1-1-0.dll";
  Unnamed.IATAddr = 0x404038;
  Image.Imports.push_back(Unnamed);
  Symbol UnnamedSlot;
  UnnamedSlot.Name = "ClfsLsnInvalid";
  UnnamedSlot.Addr = 0x404038;
  Image.Symbols.push_back(UnnamedSlot);
  Symbol LinkerSlot;
  LinkerSlot.Name = "__imp_ZwClose";
  LinkerSlot.Addr = 0x404040;
  Image.Symbols.push_back(LinkerSlot);
  auto callIn = [](const HighFunc &F) -> ExprPtr {
    for (const HighStmt &S : F.Body) {
      if (S.Kind == StmtKind::Call && S.CallExpr)
        return S.CallExpr;
      if (S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Call)
        return S.Val;
    }
    return nullptr;
  };
  const std::map<va_t, std::string> Named{{0x404020, "puts"},
                                          {0x404028, "__libc_start_main"},
                                          {0x404038, "ClfsLsnInvalid"},
                                          {0x404040, "__imp_ZwClose"},
                                          {0x401060, "target"}};
  for (va_t Constant : {0x404018U, 0x404020U, 0x404028U, 0x404030U, 0x404038U,
                        0x404040U, 0x401060U}) {
    SCOPED_TRACE(Constant);
    MedFunc M;
    M.Entry = 0x401000;
    M.Name = "slot_call";
    M.ReturnType = NdType::makeVoid();
    M.Blocks.resize(1);
    M.Blocks[0].Id = 0;
    M.Blocks[0].StartAddr = 0x401000;
    M.Blocks[0].EndAddr = 0x401010;
    M.Blocks[0].Ops = {operation(NdOp::INDIR_CALL, 0x401000, {},
                                 {MedVar::makeConst(Constant, 8)}),
                       operation(NdOp::RETURN, 0x401006, {}, {})};
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto F = Converter.convert(M, Architecture);
    const ExprPtr Call = callIn(F);
    ASSERT_TRUE(Call);
    const auto Name = Named.find(Constant);
    if (Name == Named.end()) {
      EXPECT_TRUE(Call->IsIndirectCall);
      ASSERT_TRUE(Call->IndirectTarget);
      EXPECT_EQ(Call->IndirectTarget->Kind, ExprKind::Load);
      ASSERT_EQ(Call->IndirectTarget->Operands.size(), 1U);
      EXPECT_EQ(Call->IndirectTarget->Operands[0]->Kind, ExprKind::Const);
      EXPECT_EQ(Call->IndirectTarget->Operands[0]->ConstVal, Constant);
    } else {
      EXPECT_FALSE(Call->IsIndirectCall);
      EXPECT_EQ(Call->CallTarget, Name->second);
    }
  }
}

TEST(HighControlFlowSemantics, FixedImageStringArgumentIsAnAddress) {
  // `mov edi, 0x402000; call puts` in a non-PIE executable: no relocation
  // marks the immediate, but puts reads a C string there.  Only an image the
  // loader cannot move makes the number that string's address.
  const Arch Architecture = Arch::X64;
  const auto &TRI = getTargetRegInfo(Architecture);
  for (bool Fixed : {true, false}) {
    SCOPED_TRACE(Fixed);
    BinaryImage Image;
    Image.Format = BinaryFormat::ELF;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.LoadsAtLinkAddress = Fixed;
    Segment Rodata;
    Rodata.VA = 0x402000;
    Rodata.Data = {'t', 'e', 'n', 0};
    Rodata.Size = Rodata.FileSz = Rodata.Data.size();
    Rodata.Flags = SegmentFlags::Readable;
    Image.Segments.push_back(Rodata);
    MedFunc M;
    M.Entry = 0x401000;
    M.Name = "say";
    M.ReturnType = NdType::makeVoid();
    M.Blocks.resize(1);
    M.Blocks[0].Id = 0;
    M.Blocks[0].StartAddr = 0x401000;
    M.Blocks[0].EndAddr = 0x401010;
    auto Argument = machineValue(1, Architecture);
    Argument.Kind = MedVar::Reg;
    Argument.RegOff = TRI.IntParamRegs[0];
    Argument.SSAVer = 1;
    M.Blocks[0].Ops = {
        operation(NdOp::COPY, 0x401000, Argument,
                  {MedVar::makeConst(0x402000, 8)}),
        operation(NdOp::CALL, 0x401005, {}, {MedVar::makeConst(0x401100, 8)}),
        operation(NdOp::RETURN, 0x40100a, {}, {})};
    M.Blocks[0].Ops[0].Inputs[0].Provenance = ConstantAddressProvenance::Scalar;
    const std::map<va_t, std::string> Names{{0x401100, "puts"}};
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    Converter.setFuncNames(&Names);
    const auto F = Converter.convert(M, Architecture);
    ExprPtr Call;
    for (const HighStmt &S : F.Body)
      if (S.Kind == StmtKind::Call && S.CallExpr)
        Call = S.CallExpr;
    ASSERT_TRUE(Call);
    ASSERT_FALSE(Call->Operands.empty());
    const HighExpr *Argument0 = Call->Operands[0].get();
    while (Argument0 && Argument0->Kind != ExprKind::Const &&
           Argument0->Operands.size() == 1)
      Argument0 = Argument0->Operands[0].get();
    ASSERT_TRUE(Argument0);
    ASSERT_EQ(Argument0->Kind, ExprKind::Const);
    EXPECT_EQ(Argument0->ConstVal, 0x402000U);
    EXPECT_EQ(Argument0->ConstProvenance,
              Fixed ? ConstantAddressProvenance::DataAddress
                    : ConstantAddressProvenance::Scalar);
  }
}

TEST(HighControlFlowSemantics, SlotReadInAnotherBlockStillNamesItsImport) {
  // `mov rsi, [__imp_Sleep]` before a loop, `call rsi` inside it: the read
  // is in another block, and the callee is still the import.
  const Arch Architecture = Arch::X64;
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = 0x404000;
  Data.Size = Data.FileSz = 0x100;
  Data.Data.resize(0x100);
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(Data);
  Import Sleep;
  Sleep.Name = "Sleep";
  Sleep.IATAddr = 0x404010;
  Image.Imports.push_back(Sleep);
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "spin";
  M.ReturnType = NdType::makeVoid();
  auto Target = machineValue(0, Architecture);
  Target.Kind = MedVar::Reg;
  Target.RegOff = getTargetRegInfo(Architecture).IntParamRegs[1];
  Target.SSAVer = 1;
  M.Blocks.resize(2);
  for (int I = 0; I < 2; ++I) {
    M.Blocks[I].Id = I;
    M.Blocks[I].StartAddr = 0x1000 + I * 0x10;
    M.Blocks[I].EndAddr = M.Blocks[I].StartAddr + 0x10;
  }
  M.Blocks[0].Succs = {1};
  M.Blocks[0].Ops = {
      operation(NdOp::LOAD, 0x1000, Target, {MedVar::makeConst(0x404010, 8)})};
  M.Blocks[1].Preds = {0};
  M.Blocks[1].Ops = {operation(NdOp::INDIR_CALL, 0x1010, {}, {Target}),
                     operation(NdOp::RETURN, 0x1016, {}, {})};
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Image);
  const auto F = Converter.convert(M, Architecture);
  ExprPtr Call;
  std::function<void(const std::vector<HighStmt> &)> Find =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          if (S.Kind == StmtKind::Call && S.CallExpr)
            Call = S.CallExpr;
          Find(S.Body);
          Find(S.ElseBody);
        }
      };
  Find(F.Body);
  ASSERT_TRUE(Call);
  EXPECT_FALSE(Call->IsIndirectCall);
  EXPECT_EQ(Call->CallTarget, "Sleep");
}

namespace {
/// An i386 cdecl function reading and rewriting its incoming stack slots.
struct I386Frame {
  MedFunc M;
  MedVar EntrySP;
  int NextId = 10;
  explicit I386Frame(int Params) {
    M.Entry = 0x1000;
    M.Name = "i386_frame";
    M.ReturnType = NdType::makeInt(4, false);
    for (int I = 0; I < Params; ++I) {
      MedVar P;
      P.Kind = MedVar::Param;
      P.Id = I;
      P.RegOff = kNoParamReg;
      P.Size = 4;
      P.TheArch = Arch::X86;
      M.Params.push_back(P);
    }
    EntrySP.Kind = MedVar::Reg;
    EntrySP.Id = 3;
    EntrySP.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
    EntrySP.Size = 4;
    EntrySP.TheArch = Arch::X86;
    M.Blocks.resize(1);
    M.Blocks[0].Id = 0;
    M.Blocks[0].StartAddr = 0x1000;
    M.Blocks[0].EndAddr = 0x1040;
  }
  MedVar temp(uint16_t Size = 4) {
    MedVar T;
    T.Kind = MedVar::Temp;
    T.Id = NextId++;
    T.Size = Size;
    T.TheArch = Arch::X86;
    return T;
  }
  /// The address of the incoming stack slot at entry offset \p Offset.
  MedVar slot(va_t Address, int64_t Offset) {
    const MedVar Address32 = temp();
    M.Blocks[0].Ops.push_back(
        operation(NdOp::INT_ADD, Address, Address32,
                  {EntrySP, MedVar::makeConst(uint64_t(Offset), 4)}));
    return Address32;
  }
  MedVar returnRegister(int Version) const {
    MedVar R;
    R.Kind = MedVar::Reg;
    R.Id = 6;
    R.RegOff = getTargetRegInfo(Arch::X86).IntReturnReg;
    R.Size = 4;
    R.SSAVer = Version;
    R.TheArch = Arch::X86;
    return R;
  }
};

ExprPtr firstCall(const std::vector<HighStmt> &Body) {
  for (const HighStmt &S : Body) {
    if (S.Kind == StmtKind::Call && S.CallExpr)
      return S.CallExpr;
    if ((S.Kind == StmtKind::Assign || S.Kind == StmtKind::Return) && S.Val &&
        S.Val->Kind == ExprKind::Call)
      return S.Val;
    if (S.Kind == StmtKind::Return && S.RetVal) {
      const HighExpr *E = S.RetVal.get();
      while (E && E->Kind != ExprKind::Call && E->Operands.size() == 1)
        E = E->Operands[0].get();
      if (E && E->Kind == ExprKind::Call)
        return std::make_shared<HighExpr>(*E);
    }
  }
  return nullptr;
}

bool readsStack(const HighExpr &E) {
  if (E.Kind == ExprKind::Var && E.Var.Kind == MedVar::Reg &&
      E.Var.RegOff == getTargetRegInfo(Arch::X86).StackPointer)
    return true;
  for (const ExprPtr &Operand : E.Operands)
    if (Operand && readsStack(*Operand))
      return true;
  return false;
}
} // namespace

TEST(HighControlFlowSemantics, WrittenStackArgumentStartsAsTheArgument) {
  // `addl $1, 4(%esp)` keeps arg0's home a memory slot: the function writes
  // it. Its first read must see the argument, not an uninitialized local.
  I386Frame Frame(1);
  Frame.M.MutableStackParamHomes = {{0, 4}};
  const MedVar Slot = Frame.slot(0x1000, 4);
  const MedVar Old = Frame.temp(), New = Frame.temp(), Final = Frame.temp();
  auto &Ops = Frame.M.Blocks[0].Ops;
  Ops.push_back(operation(NdOp::LOAD, 0x1000, Old, {Slot}));
  Ops.push_back(
      operation(NdOp::INT_ADD, 0x1000, New, {Old, MedVar::makeConst(1, 4)}));
  Ops.push_back(operation(NdOp::STORE, 0x1000, {}, {Slot, New}));
  Ops.push_back(operation(NdOp::LOAD, 0x1008, Final, {Slot}));
  const MedVar Result = Frame.returnRegister(1);
  Ops.push_back(operation(NdOp::COPY, 0x1008, Result, {Final}));
  Ops.push_back(operation(NdOp::RETURN, 0x100c, {}, {Result}));
  const auto F = MedToHighConverter().convert(Frame.M, Arch::X86);
  EXPECT_NO_THROW(EXPECT_EQ(execute(F, 41), 42U));
}

TEST(HighControlFlowSemantics, TailJumpPassesTheRewrittenIncomingSlots) {
  // `int swapper(int a, int b) { return callee(b, a); }` at -O2 swaps its
  // own incoming slots and jumps: the callee reads 4(%esp) then 8(%esp).
  I386Frame Frame(2);
  Frame.M.MutableStackParamHomes = {{0, 4}, {1, 8}};
  const MedVar High = Frame.slot(0x1000, 8);
  const MedVar B = Frame.temp();
  auto &Ops = Frame.M.Blocks[0].Ops;
  Ops.push_back(operation(NdOp::LOAD, 0x1000, B, {High}));
  const MedVar Low = Frame.slot(0x1004, 4);
  const MedVar A = Frame.temp();
  Ops.push_back(operation(NdOp::LOAD, 0x1004, A, {Low}));
  Ops.push_back(operation(NdOp::STORE, 0x1008, {}, {Low, B}));
  Ops.push_back(operation(NdOp::STORE, 0x100c, {}, {High, A}));
  const MedVar Result = Frame.returnRegister(1);
  Ops.push_back(
      operation(NdOp::CALL, 0x1010, Result, {MedVar::makeConst(0x2000, 4)}));
  Ops.push_back(operation(NdOp::RETURN, 0x1010, {}, {Result}));
  const std::map<va_t, std::string> Names{{0x2000, "callee"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  const auto F = Converter.convert(Frame.M, Arch::X86);
  const ExprPtr Call = firstCall(F.Body);
  ASSERT_TRUE(Call);
  ASSERT_EQ(Call->Operands.size(), 2U);
  ASSERT_EQ(Call->Operands[0]->Kind, ExprKind::Var);
  ASSERT_EQ(Call->Operands[1]->Kind, ExprKind::Var);
  EXPECT_EQ(Call->Operands[0]->Var.Id, B.Id);
  EXPECT_EQ(Call->Operands[1]->Var.Id, A.Id);
}

TEST(HighControlFlowSemantics, CallTargetKeepsTheValueReadBeforeAStore) {
  // `return fns[i % 3](x);` reads i from 4(%esp), stores x there for the
  // tail call, then jumps through `fns(,%ecx,4)`. The target must use the
  // i read before the store, not reread the slot.
  I386Frame Frame(2);
  Frame.M.MutableStackParamHomes = {{0, 4}};
  const MedVar Low = Frame.slot(0x1000, 4);
  const MedVar I = Frame.temp(), Index = Frame.temp(), Offset = Frame.temp(),
               Entry = Frame.temp(), Target = Frame.temp();
  auto &Ops = Frame.M.Blocks[0].Ops;
  Ops.push_back(operation(NdOp::LOAD, 0x1000, I, {Low}));
  Ops.push_back(
      operation(NdOp::INT_AND, 0x1004, Index, {I, MedVar::makeConst(3, 4)}));
  Ops.push_back(operation(NdOp::STORE, 0x1008, {}, {Low, Frame.M.Params[1]}));
  Ops.push_back(operation(NdOp::INT_MULT, 0x100c, Offset,
                          {Index, MedVar::makeConst(4, 4)}));
  Ops.push_back(operation(NdOp::INT_ADD, 0x100c, Entry,
                          {Offset, MedVar::makeConst(0x804c040, 4)}));
  Ops.push_back(operation(NdOp::LOAD, 0x100c, Target, {Entry}));
  const MedVar Result = Frame.returnRegister(1);
  Ops.push_back(operation(NdOp::INDIR_CALL, 0x100c, Result, {Target}));
  Ops.push_back(operation(NdOp::RETURN, 0x100c, {}, {Result}));
  const auto F = MedToHighConverter().convert(Frame.M, Arch::X86);
  const ExprPtr Call = firstCall(F.Body);
  ASSERT_TRUE(Call);
  ASSERT_TRUE(Call->IndirectTarget);
  EXPECT_FALSE(readsStack(*Call->IndirectTarget))
      << Call->IndirectTarget->str();
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

TEST(HighControlFlowSemantics, SharedExpressionKeepsItsRepeatedInputUse) {
  HighFunc F;
  auto Calculation = assign(0x1004, 2, 0);
  Calculation.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(7, 8));
  auto Shared =
      HighExpr::makeBinop(NdOp::INT_ADD, local(2), HighExpr::makeConst(1, 8));
  auto FirstUse = assign(0x1008, 3, 0);
  FirstUse.Val = Shared;
  auto SecondUse = assign(0x100c, 4, 0);
  SecondUse.Val = Shared;
  F.Body = {
      assign(0x1000, 1, 10), Calculation, FirstUse, SecondUse,
      result(0x1010, HighExpr::makeBinop(NdOp::INT_ADD, local(3), local(4)))};
  ASSERT_EQ(execute(F, 0), 36u);
  inlineSingleDefSingleUse(F.Body);
  EXPECT_EQ(execute(F, 0), 36u);
  EXPECT_EQ(Shared->Operands[0]->Kind, ExprKind::Var);
  EXPECT_EQ(Shared->Operands[0]->Var.Id, 2u);
}

TEST(HighControlFlowSemantics, CopyChainKeepsItsSharedSourceDefinition) {
  HighFunc F;
  auto Shared = local(1);
  auto Copy = assign(0x1004, 2, 0);
  Copy.Val = Shared;
  F.Body = {
      assign(0x1000, 1, 7), Copy,
      result(0x1008, HighExpr::makeBinop(NdOp::INT_ADD, Shared, local(2)))};
  ASSERT_EQ(execute(F, 0), 14u);
  const auto Before = analyzeHighSourceFlow(F, true);
  ASSERT_TRUE(Before.Complete);
  ASSERT_TRUE(Before.Items.empty());

  // Sharing an expression object does not merge its two source use sites.
  foldCopyChains(F);

  const auto After = analyzeHighSourceFlow(F, true);
  EXPECT_TRUE(After.Complete);
  EXPECT_TRUE(After.Items.empty());
  EXPECT_EQ(execute(F, 0), 14u);
}

TEST(HighControlFlowSemantics, CopyChainKeepsDependenciesOfParallelRewrites) {
  HighFunc F;
  auto FirstCopy = assign(0x1004, 2, 0);
  FirstCopy.Val = local(1);
  auto SecondCopy = assign(0x1008, 3, 0);
  SecondCopy.Val = local(2);
  F.Body = {assign(0x1000, 1, 7), FirstCopy, SecondCopy,
            result(0x100c, local(3))};
  ASSERT_EQ(execute(F, 0), 7u);

  foldCopyChains(F);

  // Rewriting the second copy can still depend on the first copy's source.
  for (bool CleanDeadValues : {false, true}) {
    SCOPED_TRACE(CleanDeadValues);
    if (CleanDeadValues)
      eliminateUnusedValues(F.Body);
    const auto Flow = analyzeHighSourceFlow(F, true);
    EXPECT_TRUE(Flow.Complete);
    EXPECT_TRUE(Flow.Items.empty());
    EXPECT_EQ(execute(F, 0), 7u);
  }
}

TEST(HighControlFlowSemantics, CopyChainCountsUsesInEverySwitchArm) {
  for (bool InDefault : {false, true}) {
    SCOPED_TRACE(InDefault);
    HighFunc F;
    auto Copy = assign(0x1004, 2, 0);
    Copy.Val = local(1);
    auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, local(1), local(2));
    HighStmt Dispatch;
    Dispatch.Kind = StmtKind::Switch;
    Dispatch.Addr = 0x1008;
    MedVar Selector;
    Selector.Kind = MedVar::Param;
    Selector.Size = 8;
    Dispatch.SwitchExpr = HighExpr::makeVar(Selector);
    Dispatch.Cases.push_back({0, {result(0x100c, InDefault ? local(2) : Sum)}});
    Dispatch.DefaultBody = {result(0x1010, InDefault ? Sum : local(2))};
    F.Body = {assign(0x1000, 1, 7), Copy, Dispatch};
    for (uint64_t Selector : {0, 1})
      ASSERT_EQ(execute(F, Selector), bool(Selector) == InDefault ? 14u : 7u);

    foldCopyChains(F);
    eliminateUnusedValues(F.Body);

    const auto Flow = analyzeHighSourceFlow(F, true);
    EXPECT_TRUE(Flow.Complete);
    EXPECT_TRUE(Flow.Items.empty());
    for (uint64_t Selector : {0, 1})
      EXPECT_EQ(execute(F, Selector), bool(Selector) == InDefault ? 14u : 7u);
  }
}

TEST(HighControlFlowSemantics, CopyChainRetainsItsSourceBranchEntry) {
  HighFunc F;
  auto Copy = assign(0x1008, 2, 0);
  Copy.Val = local(1);
  F.Body = {jump(0x1000, 0x1004), assign(0x1004, 1, 7), Copy,
            result(0x100c, local(2))};
  ASSERT_EQ(execute(F, 0, true), 7u);

  foldCopyChains(F);

  for (bool CleanDeadValues : {false, true}) {
    SCOPED_TRACE(CleanDeadValues);
    if (CleanDeadValues)
      eliminateUnusedValues(F.Body);
    const auto Flow = analyzeHighSourceFlow(F, true);
    EXPECT_TRUE(Flow.Complete);
    EXPECT_TRUE(Flow.Items.empty());
    EXPECT_EQ(execute(F, 0, true), 7u);
  }
}

TEST(HighControlFlowSemantics, NestedCallRetainsItsPrecedingMemorySnapshot) {
  HighFunc F;
  const auto Address = HighExpr::makeConst(0x4000, 8);
  auto Store = [&](va_t Site, uint64_t Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Site;
    S.StoreAddr = Address;
    S.StoreVal = HighExpr::makeConst(Value, 8);
    return S;
  };
  auto Observe = HighExpr::makeCall(
      "observe", 0x5000,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0x4000, 8)});
  Observe->Type = NdType::makeInt(8);
  auto Snapshot = assign(0x1004, 2, 0);
  Snapshot.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, Observe, HighExpr::makeConst(1, 8));
  F.Body = {Store(0x1000, 5), Snapshot, Store(0x1008, 7),
            result(0x100c, local(2))};
  ASSERT_EQ(execute(F, 0), 6u);
  inlineSingleDefSingleUse(F.Body);
  EXPECT_EQ(execute(F, 0), 6u);
  EXPECT_EQ(F.Body.back().RetVal->Kind, ExprKind::Var);
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

TEST(HighControlFlowSemantics, RecoveredSwitchKeepsJumpsToASharedTail) {
  // `if (x == 0) goto C0; ... return 99; C0: v = 10; goto T; ... T: return
  // v + 1;` Each case jumps to a tail that is not what follows the switch;
  // breaking out instead would skip it (as in an Authz attribute copy loop).
  HighFunc F;
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto Branch = conditional(0x1000 + 4 * Case, 0x1100 + 0x100 * Case);
    Branch.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                      HighExpr::makeConst(Case, 8));
    F.Body.push_back(std::move(Branch));
  }
  F.Body.push_back(result(0x1010, HighExpr::makeConst(99, 8)));
  for (unsigned Case = 0; Case < 3; ++Case) {
    F.Body.push_back(assign(0x1100 + 0x100 * Case, 5, 10 * (Case + 1)));
    F.Body.push_back(jump(0x1104 + 0x100 * Case, 0x1400));
  }
  F.Body.push_back(
      result(0x1400, HighExpr::makeBinop(NdOp::INT_ADD, local(5),
                                         HighExpr::makeConst(1, 8))));
  auto Expected = [](uint64_t Input) -> uint64_t {
    return Input < 3 ? 10 * (Input + 1) + 1 : 99;
  };
  for (uint64_t Input : {0, 1, 2, 3, 255})
    ASSERT_EQ(execute(F, Input), Expected(Input));
  recoverSwitchStatements(F);
  for (uint64_t Input : {0, 1, 2, 3, 255}) {
    SCOPED_TRACE(Input);
    EXPECT_EQ(execute(F, Input), Expected(Input));
  }
}

TEST(HighControlFlowSemantics, RecoveredSwitchKeepsFallthroughIntoACase) {
  // Case zero's block falls into case one's; moving case one into the switch
  // would cut that path.
  HighFunc F;
  F.Body.push_back(assign(0x0ff0, 5, 100));
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto Branch = conditional(0x1000 + 4 * Case, 0x1100 + 0x100 * Case);
    Branch.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                      HighExpr::makeConst(Case, 8));
    F.Body.push_back(std::move(Branch));
  }
  F.Body.push_back(jump(0x100c, 0x1400));
  F.Body.push_back(assign(0x1100, 5, 10));
  auto AddFive = assign(0x1200, 5, 0);
  AddFive.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(5), HighExpr::makeConst(5, 8));
  F.Body.push_back(std::move(AddFive));
  F.Body.push_back(jump(0x1204, 0x1500));
  F.Body.push_back(assign(0x1300, 5, 30));
  F.Body.push_back(jump(0x1304, 0x1500));
  F.Body.push_back(assign(0x1400, 5, 0));
  F.Body.push_back(result(0x1500, local(5)));
  auto Expected = [](uint64_t Input) -> uint64_t {
    return Input == 0 ? 15 : Input == 1 ? 105 : Input == 2 ? 30 : 0;
  };
  for (uint64_t Input : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, Input), Expected(Input));
  recoverSwitchStatements(F);
  for (uint64_t Input : {0, 1, 2, 3}) {
    SCOPED_TRACE(Input);
    EXPECT_EQ(execute(F, Input), Expected(Input));
  }
}

/// `if (Guard) return 5; switch (x) { case 1: return 10; case 2: return 20;
/// default: return 5; }`
HighFunc guardedSwitch(ExprPtr Guard) {
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Addr = 0x1000;
  If.Cond = std::move(Guard);
  If.Body = {result(0x1004, HighExpr::makeConst(5, 8))};
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.Addr = 0x1008;
  Switch.SwitchExpr = local(0);
  Switch.SwitchExpr->Type = NdType::makeInt(8);
  for (uint64_t Value : {1, 2}) {
    SwitchCase Case;
    Case.Value = Value;
    Case.Body = {result(0x1100 * Value, HighExpr::makeConst(10 * Value, 8))};
    Switch.Cases.push_back(std::move(Case));
  }
  Switch.DefaultBody = {result(0x1300, HighExpr::makeConst(5, 8))};
  HighFunc F;
  F.Body = {If, Switch};
  return F;
}

TEST(HighControlFlowSemantics, SwitchGuardStaysWhenACaseSatisfiesIt) {
  // `x == 1` catches a case value; erasing it would return 10 for x == 1.
  auto Key = local(0);
  Key->Type = NdType::makeInt(8);
  HighFunc F = guardedSwitch(
      HighExpr::makeBinop(NdOp::INT_EQUAL, Key, HighExpr::makeConst(1, 8)));
  auto Expected = [](uint64_t X) -> uint64_t { return X == 2 ? 20 : 5; };
  for (uint64_t X : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, X), Expected(X));
  cleanupGuardBeforeSwitch(F);
  for (uint64_t X : {0, 1, 2, 3}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), Expected(X));
  }
}

TEST(HighControlFlowSemantics, SwitchGuardGoesWhenOnlyTheDefaultCatchesIt) {
  // `x > 2` catches no case value, so the default already covers it.
  auto Key = local(0);
  Key->Type = NdType::makeInt(8);
  HighFunc F = guardedSwitch(
      HighExpr::makeBinop(NdOp::INT_LESS, HighExpr::makeConst(2, 8), Key));
  auto Expected = [](uint64_t X) -> uint64_t {
    return X == 1 ? 10 : X == 2 ? 20 : 5;
  };
  for (uint64_t X : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, X), Expected(X));
  cleanupGuardBeforeSwitch(F);
  EXPECT_EQ(F.Body.size(), 1u);
  for (uint64_t X : {0, 1, 2, 3}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), Expected(X));
  }
}

TEST(HighControlFlowSemantics, JumpIntoALoopBodySkipsItsTest) {
  // `goto B; while (x < 3) { B: x = x + 1; }` enters the body before the
  // first test, like a do-while. Only a jump to the header, or into a loop
  // that always runs, is what falling into the loop does.
  auto MakeLoop = [](ExprPtr Cond) {
    HighStmt Loop;
    Loop.Kind = StmtKind::While;
    Loop.Addr = 0x1100;
    Loop.LoopHeaderAddr = 0x1100;
    Loop.Cond = std::move(Cond);
    auto Step = assign(0x1104, 0, 0);
    Step.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(0), HighExpr::makeConst(1, 8));
    Loop.Body = {Step};
    return Loop;
  };
  auto Less =
      HighExpr::makeBinop(NdOp::INT_LESS, local(0), HighExpr::makeConst(3, 8));
  auto Kept = [](const std::vector<HighStmt> &Body) {
    return !Body.empty() && Body.front().Kind == StmtKind::Goto;
  };
  std::vector<HighStmt> IntoBody = {jump(0x1000, 0x1104), MakeLoop(Less)};
  eliminateGotoToLoop(IntoBody);
  EXPECT_TRUE(Kept(IntoBody)) << "the jump skips the loop test";
  std::vector<HighStmt> ToHeader = {jump(0x1000, 0x1100), MakeLoop(Less)};
  eliminateGotoToLoop(ToHeader);
  EXPECT_FALSE(Kept(ToHeader)) << "the header runs the test either way";
  std::vector<HighStmt> Always = {jump(0x1000, 0x1104),
                                  MakeLoop(HighExpr::makeConst(1, 1))};
  eliminateGotoToLoop(Always);
  EXPECT_FALSE(Kept(Always)) << "an always-true loop skips nothing";
}

TEST(HighControlFlowSemantics, SameArmsMergeOnlyWhenTheyLeave) {
  // `if (x == 1) y = y + 1; if (x == 1) y = y + 1; return y;` runs the
  // increment twice; `if (x == 1 || x == 1)` would run it once.
  auto MakeIf = [](va_t Address) {
    auto Increment = assign(Address + 4, 1, 0);
    Increment.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
    HighStmt If;
    If.Kind = StmtKind::If;
    If.Addr = Address;
    If.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                  HighExpr::makeConst(1, 8));
    If.Body = {Increment};
    return If;
  };
  HighFunc F;
  F.Body = {assign(0x1000, 1, 0), MakeIf(0x1004), MakeIf(0x1010),
            result(0x1020, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t { return X == 1 ? 2 : 0; };
  for (uint64_t X : {0, 1})
    ASSERT_EQ(execute(F, X), Expected(X));
  reduceSingleUseGotos(F.Body, /*SpliceRegions=*/false);
  for (uint64_t X : {0, 1}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), Expected(X));
  }
}

namespace {
/// if (x & 1) { v = 1; <Then> } else { <Else> }  v = v + 10;  J: return v;
/// with one arm ending in `goto J`.
HighFunc armJumpsPastAStatement(bool FromElse) {
  auto Plus10 = assign(0x1010, 1, 0);
  Plus10.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(10, 8));
  HighStmt Arms;
  Arms.Kind = StmtKind::IfElse;
  Arms.Addr = 0x1000;
  Arms.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  if (FromElse) {
    Arms.Body = {assign(0x1004, 1, 1)};
    Arms.ElseBody = {assign(0x1008, 1, 3), jump(0x100c, 0x1020)};
  } else {
    // An if/else whose else arm is empty is a plain if.
    Arms.Body = {assign(0x1004, 1, 1), jump(0x1008, 0x1020)};
  }
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x1020;
  HighFunc F;
  F.Body = {assign(0x0ffc, 1, 0), Arms, Plus10, Label,
            result(0x1024, local(1))};
  return F;
}
} // namespace

TEST(HighControlFlowSemantics, ArmJumpPastStatementsBecomesStructured) {
  for (bool FromElse : {false, true}) {
    SCOPED_TRACE(FromElse);
    HighFunc F = armJumpsPastAStatement(FromElse);
    auto Expected = [&](uint64_t X) -> uint64_t {
      if (FromElse)
        return X & 1 ? 11 : 3;
      return X & 1 ? 1 : 10;
    };
    for (uint64_t X : {0, 1})
      ASSERT_EQ(execute(F, X), Expected(X));
    reduceSingleUseGotos(F.Body, /*SpliceRegions=*/true);
    for (uint64_t X : {0, 1})
      EXPECT_EQ(execute(F, X), Expected(X)) << X;
    size_t Gotos = 0;
    walkStmts(F.Body,
              [&](const HighStmt &S) { Gotos += S.Kind == StmtKind::Goto; });
    EXPECT_EQ(Gotos, 0u);
  }
}

TEST(HighControlFlowSemantics, ArmJumpPastALoopItAloneEntersBecomesStructured) {
  // v = 0; if (x & 1) { v = 1; } else { v = 3; goto Y; }
  // X: v = v + 10; if (v < 25) goto X;  Y: return v;
  // Only the skipped statements jump to X, so they move into the then arm
  // with their loop, and the else arm's jump goes away.
  auto Plus10 = assign(0x1010, 1, 0);
  Plus10.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(10, 8));
  HighStmt Arms;
  Arms.Kind = StmtKind::IfElse;
  Arms.Addr = 0x1000;
  Arms.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Arms.Body = {assign(0x1004, 1, 1)};
  Arms.ElseBody = {assign(0x1008, 1, 3), jump(0x100c, 0x1030)};
  auto Again = conditional(0x1014, 0x1010);
  Again.Cond =
      HighExpr::makeBinop(NdOp::INT_LESS, local(1), HighExpr::makeConst(25, 8));
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x1030;
  HighFunc F;
  F.Body = {assign(0x0ffc, 1, 0),    Arms, Plus10, Again, Label,
            result(0x1034, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t { return X & 1 ? 31 : 3; };
  for (uint64_t X : {0, 1})
    ASSERT_EQ(execute(F, X), Expected(X));
  reduceSingleUseGotos(F.Body, /*SpliceRegions=*/true);
  for (uint64_t X : {0, 1})
    EXPECT_EQ(execute(F, X), Expected(X)) << X;
  size_t Exits = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Exits += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1030;
  });
  EXPECT_EQ(Exits, 0u);
}

TEST(HighControlFlowSemantics, ArmJumpingBackAfterWorkBecomesALoop) {
  // v = x; X: v = v + 1; if (v < 5) { v = v + 1; goto X; }  return v;
  auto Bump = [](va_t Address) {
    auto S = assign(Address, 1, 0);
    S.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
    return S;
  };
  auto Start = assign(0x0ffc, 1, 0);
  Start.Val = local(0);
  HighStmt Again;
  Again.Kind = StmtKind::If;
  Again.Addr = 0x1004;
  Again.Cond =
      HighExpr::makeBinop(NdOp::INT_LESS, local(1), HighExpr::makeConst(5, 8));
  Again.Body = {Bump(0x1008), jump(0x100c, 0x1000)};
  HighFunc F;
  F.Body = {Start, Bump(0x1000), Again, result(0x1010, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t {
    uint64_t V = X + 1;
    while (V < 5)
      V += 2;
    return V;
  };
  for (uint64_t X : {0, 3, 10})
    ASSERT_EQ(execute(F, X), Expected(X));
  loopsForArmsJumpingBack(F.Body);
  for (uint64_t X : {0, 3, 10})
    EXPECT_EQ(execute(F, X), Expected(X)) << X;
  size_t Jumps = 0;
  walkStmts(F.Body,
            [&](const HighStmt &S) { Jumps += S.Kind == StmtKind::Goto; });
  EXPECT_EQ(Jumps, 0u);
}

TEST(HighControlFlowSemantics, EnteredDoWhileKeepsItsShape) {
  // `goto X; Top: do { y = y + 1; X: } while (y != 3); if (x) goto Top;`
  // The jump to Top runs the body before the test; `while (y != 3)` would
  // test first.
  auto Increment = assign(0x1104, 1, 0);
  Increment.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x1108;
  HighStmt Loop;
  Loop.Kind = StmtKind::DoWhile;
  Loop.Addr = 0x1100;
  Loop.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(1),
                                  HighExpr::makeConst(3, 8));
  Loop.Body = {Increment, Label};
  HighFunc F;
  F.Body = {jump(0x1004, 0x1108), Loop, conditional(0x1150, 0x1100),
            result(0x1200, local(1))};
  reduceSingleUseGotos(F.Body, /*SpliceRegions=*/false);
  bool TestsFirst = false;
  walkStmts(F.Body, [&](const HighStmt &S) {
    TestsFirst |= S.Kind == StmtKind::While && S.Addr == 0x1100;
  });
  EXPECT_FALSE(TestsFirst) << "a re-entry at Top would skip the increment";
}

TEST(HighControlFlowSemantics, SiblingSkipsKeepTheirOwnEdgeCopies) {
  // `if (x == 1) { v = 10; goto S; } if (x == 2) { v = 20; goto S; } v = 30;
  // S: return v;` Merging the guards into one would give x == 2 the first
  // guard's copy.
  auto Guard = [](va_t Address, uint64_t Value, uint64_t Copy) {
    HighStmt If;
    If.Kind = StmtKind::If;
    If.Addr = Address;
    If.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                  HighExpr::makeConst(Value, 8));
    auto EdgeCopy = assign(Address + 4, 1, Copy);
    EdgeCopy.IsPhiCopy = true;
    If.Body = {EdgeCopy, jump(Address + 8, 0x1100)};
    return If;
  };
  HighFunc F;
  F.Body = {Guard(0x1000, 1, 10), Guard(0x1010, 2, 20), assign(0x1020, 1, 30),
            result(0x1100, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t {
    return X == 1 ? 10 : X == 2 ? 20 : 30;
  };
  for (uint64_t X : {0, 1, 2})
    ASSERT_EQ(execute(F, X), Expected(X));
  invertSkipGotos(F);
  for (uint64_t X : {0, 1, 2}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), Expected(X));
  }
}

TEST(HighControlFlowSemantics, EnteredNopStillFallsIntoTheNextBlock) {
  // `if (x == 1) goto Y; goto X; return 7; Y: ; X: v = 1; return v + 10;`
  // The jump to Y runs into X, so X is not reached by jumps alone.
  HighStmt Entry;
  Entry.Kind = StmtKind::Nop;
  Entry.Addr = 0x1100;
  auto Test = conditional(0x1000, 0x1100);
  Test.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  HighFunc F;
  F.Body = {Test,
            jump(0x1004, 0x1108),
            result(0x1008, HighExpr::makeConst(7, 8)),
            Entry,
            assign(0x1108, 1, 1),
            result(0x110c, HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                               HighExpr::makeConst(10, 8)))};
  for (uint64_t X : {0, 1})
    ASSERT_EQ(execute(F, X), std::optional<uint64_t>(11));
  reduceSingleUseGotos(F.Body, /*SpliceRegions=*/false);
  for (uint64_t X : {0, 1}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(11));
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

TEST(HighControlFlowSemantics, LoopHeaderTestKeepsItsOwnExit) {
  // A search: `v = 3; H: if (v == 0) goto Miss; if (v == x) goto Hit;
  // v = v - 1; goto H; Hit: return v + 100; Miss: return 99;` The header
  // test exits to Miss, not to what follows the loop; hoisting it into
  // `while (v != 0)` would send a failed search into the hit path
  // (MiMakeIoRangePermanent read the NULL node's fields this way).
  HighFunc F;
  F.Entry = 0x1000;
  auto MissTest = conditional(0x1100, 0x1300);
  MissTest.Body.front().Addr = 0;
  MissTest.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(1), HighExpr::makeConst(0, 8));
  auto HitTest = conditional(0x1104, 0x1200);
  HitTest.Body.front().Addr = 0;
  HitTest.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(1), local(0));
  auto Step = assign(0x1108, 1, 0);
  Step.Val =
      HighExpr::makeBinop(NdOp::INT_SUB, local(1), HighExpr::makeConst(1, 8));
  F.Body = {assign(0x1000, 1, 3),
            MissTest,
            HitTest,
            Step,
            jump(0x110c, 0x1100),
            result(0x1200, HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                               HighExpr::makeConst(100, 8))),
            result(0x1300, HighExpr::makeConst(99, 8))};
  MedFunc Med;
  Med.Entry = F.Entry;
  Med.Blocks.resize(6);
  for (int I = 0; I < 6; ++I)
    Med.Blocks[I].Id = I;
  const int Owners[] = {0, 1, 2, 5, 5, 3, 4};
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
  Med.Blocks[2].Succs = {3, 5};
  Med.Blocks[5].Succs = {1};
  auto Expected = [](uint64_t X) -> uint64_t {
    return X >= 1 && X <= 3 ? X + 100 : 99;
  };
  for (uint64_t X : {0, 1, 2, 3, 5})
    ASSERT_EQ(execute(F, X, true), Expected(X));
  detectAndConvertLoops(F, {}, Med, false);
  for (uint64_t X : {0, 1, 2, 3, 5}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X, true), Expected(X));
  }
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
      M.SwitchSelectorPlans[{0x1000, 0}] = {};
      M.SwitchSelectorPlans[{0x1000, 0}].Selector = Input;
      M.SwitchSelectorPlans[{0x1000, 0}].ResultSize = 8;
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
      // A successor's tail may be copied to each case that jumps to it, and
      // every copy keeps its store and its call.
      EXPECT_GE(Stores, 2U);
      EXPECT_EQ(Calls, Stores);
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
      Med.SwitchSelectorPlans[{Terminator.Addr, Loop.Id}] = {};
      Med.SwitchSelectorPlans[{Terminator.Addr, Loop.Id}].Selector = Selector;
      Med.SwitchSelectorPlans[{Terminator.Addr, Loop.Id}].ResultSize =
          Selector.Size;
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

TEST(HighControlFlowSemantics, ChainFoldKeepsALoadADeeperTestReads) {
  // `if (c) { t1 = *base; r = t1; t2 = t1[4]; if (t2 == 7) { t3 = t1[12];
  // if (t3 == 7) { work; goto Join; } else r = t1; } else r = t1; } else
  // r = z; elseWork; Join:` folds into one chained test only when nothing
  // but the substituted work reads a folded value.  The deeper test, the kept
  // copy and the skipped arms read t1.
  auto Store = [](va_t Address, uint64_t Slot, ExprPtr Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Address;
    S.StoreAddr = HighExpr::makeConst(Slot, 8);
    S.StoreVal = std::move(Value);
    return S;
  };
  auto Load = [](ExprPtr Address) {
    return HighExpr::makeLoad(std::move(Address), NdType::makeInt(8));
  };
  auto Field = [&](uint64_t Offset) {
    return Load(HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                    HighExpr::makeConst(Offset, 8)));
  };
  auto Define = [](va_t Address, int Id, ExprPtr Value, bool Phi = false) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.Dst = local(Id);
    S.Val = std::move(Value);
    S.IsPhiCopy = Phi;
    return S;
  };
  auto Test = [](va_t Address, ExprPtr Cond, std::vector<HighStmt> Body,
                 std::vector<HighStmt> Else) {
    HighStmt S;
    S.Kind = StmtKind::IfElse;
    S.Addr = Address;
    S.Cond = std::move(Cond);
    S.Body = std::move(Body);
    S.ElseBody = std::move(Else);
    return S;
  };
  auto IsSeven = [](int Id) {
    return HighExpr::makeBinop(NdOp::INT_EQUAL, local(Id),
                               HighExpr::makeConst(7, 8));
  };
  HighStmt Deeper = Test(0x1020, IsSeven(3),
                         {Store(0x1024, 0x300, HighExpr::makeConst(1, 8)),
                          Store(0x1026, 0x308, local(4)), jump(0x1028, 0x1050)},
                         {Define(0x1020, 5, local(1), true)});
  HighStmt First =
      Test(0x1018, IsSeven(2), {Define(0x101c, 3, Field(12)), Deeper},
           {Define(0x1018, 5, local(1), true)});
  HighStmt Outer =
      Test(0x100c,
           HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                               HighExpr::makeConst(0, 8)),
           {Define(0x1010, 1, Load(HighExpr::makeConst(0x100, 8))),
            Define(0x1010, 4, local(1)), Define(0x1014, 2, Field(4)), First},
           {Define(0x100c, 5, local(6), true)});
  HighFunc F;
  F.Body = {Store(0x1000, 0x100, HighExpr::makeConst(0x200, 8)),
            Store(0x1004, 0x204, HighExpr::makeConst(7, 8)),
            Store(0x1008, 0x20c, local(0)),
            Define(0x100a, 6, HighExpr::makeConst(0, 8)),
            Outer,
            Store(0x1040, 0x300,
                  HighExpr::makeBinop(NdOp::INT_ADD, local(5),
                                      HighExpr::makeConst(2, 8))),
            result(0x1050, Load(HighExpr::makeConst(0x300, 8)))};
  auto Expected = [](uint64_t Input) -> uint64_t {
    return Input == 0 ? 2 : Input == 7 ? 1 : 0x202;
  };
  for (uint64_t Input : {uint64_t{0}, uint64_t{5}, uint64_t{7}})
    ASSERT_EQ(execute(F, Input), Expected(Input));
  structureIfElse(F, 10);
  for (uint64_t Input : {uint64_t{0}, uint64_t{5}, uint64_t{7}})
    EXPECT_EQ(execute(F, Input), Expected(Input));
}

TEST(HighControlFlowSemantics, CopyOnlyThenArmReadAfterItsListStays) {
  // `if (c) { if (x == 5) r = x; else work; } out = r + 2;` - the then-arm is
  // only a copy, but the statement after the enclosing if reads it.
  auto Store = [](va_t Address, uint64_t Slot, ExprPtr Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Address;
    S.StoreAddr = HighExpr::makeConst(Slot, 8);
    S.StoreVal = std::move(Value);
    return S;
  };
  auto Copy = assign(0x1010, 5, 0);
  Copy.Val = local(0);
  Copy.IsPhiCopy = true;
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1008;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(5, 8));
  Inner.Body = {Copy};
  Inner.ElseBody = {Store(0x1014, 0x310, HighExpr::makeConst(1, 8))};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1004;
  Outer.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                                   HighExpr::makeConst(0, 8));
  Outer.Body = {Inner};
  HighFunc F;
  F.Body = {assign(0x1000, 5, 0), Outer,
            Store(0x1020, 0x300,
                  HighExpr::makeBinop(NdOp::INT_ADD, local(5),
                                      HighExpr::makeConst(2, 8))),
            result(0x1024, HighExpr::makeLoad(HighExpr::makeConst(0x300, 8),
                                              NdType::makeInt(8)))};
  for (uint64_t Input : {uint64_t{0}, uint64_t{3}, uint64_t{5}})
    ASSERT_EQ(execute(F, Input), Input == 5 ? 7u : 2u);
  structureIfElse(F, 10);
  for (uint64_t Input : {uint64_t{0}, uint64_t{3}, uint64_t{5}})
    EXPECT_EQ(execute(F, Input), Input == 5 ? 7u : 2u);
}

TEST(HighControlFlowSemantics, SkipInvertKeepsAnElseArmThatDoesWork) {
  // In a try body, `if (c) goto L; else { work; goto J; } fall; L: ...` is not
  // a skip over `fall`: the else arm never reaches it. Rewriting the pair as
  // `if (!c) { fall }` would drop the else arm's work.
  auto Store = [](va_t Address, uint64_t Slot, uint64_t Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Address;
    S.StoreAddr = HighExpr::makeConst(Slot, 8);
    S.StoreVal = HighExpr::makeConst(Value, 8);
    return S;
  };
  HighStmt Check;
  Check.Kind = StmtKind::IfElse;
  Check.Addr = 0x100c;
  Check.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                                   HighExpr::makeConst(0, 8));
  Check.Body = {jump(0x100c, 0x1020)};
  Check.ElseBody = {Store(0x1010, 0x300, 7), jump(0x1014, 0x1030)};
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = 0x100c;
  Try.Body = {Check, Store(0x1018, 0x308, 1), Store(0x1020, 0x310, 2),
              jump(0x1028, 0x1030)};
  HighFunc F;
  F.Body = {Try, result(0x1030, HighExpr::makeConst(0, 8))};
  invertSkipGotos(F);
  bool ElseWork = false;
  walkStmts(F.Body, [&](const HighStmt &S) {
    ElseWork |= S.Kind == StmtKind::Store && S.StoreAddr &&
                S.StoreAddr->Kind == ExprKind::Const &&
                S.StoreAddr->ConstVal == 0x300;
  });
  EXPECT_TRUE(ElseWork);
}

TEST(HighControlFlowSemantics, SkipInvertKeepsTheSkipPathCopies) {
  // In a try body, `if (c) { r = x; goto L; } work; L: out = r;` sets r only
  // on the skip path. `if (!c) { work }` alone would lose that copy.
  auto Store = [](va_t Address, uint64_t Slot, ExprPtr Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Address;
    S.StoreAddr = HighExpr::makeConst(Slot, 8);
    S.StoreVal = std::move(Value);
    return S;
  };
  auto Copy = assign(0x100c, 5, 0);
  Copy.Val = local(1);
  Copy.IsPhiCopy = true;
  HighStmt Skip;
  Skip.Kind = StmtKind::If;
  Skip.Addr = 0x100c;
  Skip.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                                  HighExpr::makeConst(0, 8));
  Skip.Body = {Copy, jump(0x100c, 0x1020)};
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = 0x1008;
  Try.Body = {assign(0x1008, 5, 3), Skip,
              Store(0x1018, 0x308, HighExpr::makeConst(1, 8)),
              Store(0x1020, 0x300, local(5))};
  HighFunc F;
  F.Body = {assign(0x1000, 1, 9), Try,
            result(0x1030, HighExpr::makeConst(0, 8))};
  invertSkipGotos(F);
  bool SkipCopy = false;
  walkStmts(F.Body, [&](const HighStmt &S) {
    SkipCopy |= S.Kind == StmtKind::Assign && S.Dst && S.Val &&
                S.Dst->Kind == ExprKind::Var && S.Dst->Var.Id == 5 &&
                S.Val->Kind == ExprKind::Var && S.Val->Var.Id == 1;
  });
  EXPECT_TRUE(SkipCopy);
}

TEST(HighControlFlowSemantics, DefinitionLaidOutAfterItsUseIsStillBuilt) {
  // The entry jumps to the block that defines t1, which jumps back to an
  // earlier block that uses t1 once: block order is not dominance order, as
  // in a loop entered at its bottom test.
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    MedFunc F;
    F.Entry = 0x1000;
    F.Name = "late_definition";
    F.ReturnType = NdType::makeInt(8, false);
    auto Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
    F.Params = {Input};
    auto T1 = machineValue(1, Architecture), T2 = machineValue(2, Architecture);
    auto T3 = machineValue(3, Architecture);
    auto Value = machineValue(4, Architecture);
    auto Return = machineValue(5, Architecture);
    Return.Kind = MedVar::Reg;
    Return.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    F.Blocks.resize(3);
    for (int I = 0; I < 3; ++I) {
      F.Blocks[I].Id = I;
      F.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + 0x40;
    }
    F.Blocks[0].Succs = {2};
    F.Blocks[0].Ops = {
        operation(NdOp::BRANCH, 0x1000, {}, {MedVar::makeConst(0x1200, 8)})};
    auto &Use = F.Blocks[1];
    Use.Preds = {2};
    Use.Ops = {
        operation(NdOp::INT_ADD, 0x1100, T2, {T1, MedVar::makeConst(2, 8)}),
        operation(NdOp::STORE, 0x1104, {}, {T2, MedVar::makeConst(7, 8)}),
        operation(NdOp::INT_ADD, 0x1108, T3, {Input, MedVar::makeConst(7, 8)}),
        operation(NdOp::LOAD, 0x110c, Value, {T3}),
        operation(NdOp::COPY, 0x1110, Return, {Value}),
        operation(NdOp::RETURN, 0x1114, {}, {Return})};
    auto &Def = F.Blocks[2];
    Def.Preds = {0};
    Def.Succs = {1};
    Def.Ops = {
        operation(NdOp::INT_ADD, 0x1200, T1, {Input, MedVar::makeConst(5, 8)}),
        operation(NdOp::BRANCH, 0x1204, {}, {MedVar::makeConst(0x1100, 8)})};
    const auto High = MedToHighConverter().convert(F, Architecture);
    // Every temporary the body reads has an assignment, or was inlined.
    std::set<VarKey> Defined, Read;
    walkStmts(High.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
        Defined.insert(varKey(S.Dst->Var));
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
          if (N.Kind == ExprKind::Var && N.Var.Kind == MedVar::Temp)
            Read.insert(varKey(N.Var));
          N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
        };
        if (E)
          Walk(*E);
      });
    });
    for (const VarKey &Key : Read)
      EXPECT_TRUE(Defined.count(Key)) << "t" << Key.first << "_" << Key.second;
  }
}

TEST(HighControlFlowSemantics, SummarizedCallKeepsArgumentsAfterAnUnknownSlot) {
  // The callee reads RCX, RDX, R8 and R9. An earlier call clobbered RCX, so
  // its value is unknown, but the other three are known: the call keeps all
  // four positions instead of stopping at the first.
  const auto &TRI = getTargetRegInfo(Arch::X64);
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "unknown_first_slot";
  F.ReturnType = NdType::makeInt(8, false);
  F.CC = CallingConv::Win64;
  auto Reg = [](int Id, int Version, uint64_t Offset) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.SSAVer = Version;
    V.Size = 8;
    V.RegOff = Offset;
    return V;
  };
  const MedVar Clobbered = Reg(11, 1, TRI.IntParamRegs[0]);
  const MedVar First = Reg(20, 1, TRI.IntReturnReg);
  const MedVar Second = Reg(20, 2, TRI.IntReturnReg);
  F.CallClobbers.push_back({Clobbered, 1, {}, 0});
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  F.Blocks[0].StartAddr = 0x1000;
  F.Blocks[0].EndAddr = 0x1040;
  MedOp Earlier =
      operation(NdOp::CALL, 0x1000, First, {MedVar::makeConst(0x2000, 8)});
  Earlier.CallSiteId = 1;
  MedOp Summarized = operation(
      NdOp::CALL, 0x1010, Second,
      {MedVar::makeConst(0x3000, 8), Clobbered, MedVar::makeConst(1, 8),
       MedVar::makeConst(2, 8), MedVar::makeConst(3, 8)});
  Summarized.CalleeRegisterArgs = 4;
  F.Blocks[0].Ops = {Earlier, Summarized,
                     operation(NdOp::RETURN, 0x1020, {}, {Second})};
  const auto High = MedToHighConverter().convert(F, Arch::X64);
  const HighExpr *Call = nullptr;
  walkStmts(High.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
        if (N.Kind == ExprKind::Call && N.CallAddr == 0x3000)
          Call = &N;
        N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
      };
      if (E)
        Walk(*E);
    });
  });
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->Operands.size(), 4u);
  EXPECT_EQ(Call->Operands[0]->Kind, ExprKind::Undef);
  for (unsigned I = 1; I < 4; ++I) {
    ASSERT_EQ(Call->Operands[I]->Kind, ExprKind::Const);
    EXPECT_EQ(Call->Operands[I]->ConstVal, I);
  }
}

TEST(HighControlFlowSemantics,
     JoinDefaultCallStillRunsAfterAnArmWritingItsDest) {
  // `if (p) r = release(p); r = base_dtor(this); return r;` The arm falls
  // through, so the second call runs on both paths. Sinking it into an else
  // arm would drop it where the first call already wrote r.
  auto Call = [](const char *Target, ExprPtr Arg) {
    auto E = std::make_shared<HighExpr>();
    E->Kind = ExprKind::Call;
    E->CallTarget = Target;
    E->Operands = {std::move(Arg)};
    return E;
  };
  auto Release = assign(0x1008, 5, 0);
  Release.Val = Call("release", local(1));
  HighStmt Guard;
  Guard.Kind = StmtKind::If;
  Guard.Addr = 0x1004;
  Guard.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(1),
                                   HighExpr::makeConst(0, 8));
  Guard.Body = {Release};
  auto Base = assign(0x1010, 5, 0);
  Base.Val = Call("base_dtor", local(2));
  HighFunc F;
  F.Body = {assign(0x1000, 1, 9), Guard, Base, result(0x1018, local(5))};
  structureIfElse(F, 10);
  std::function<bool(const std::vector<HighStmt> &)> OnEveryPath =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          if (S.Kind == StmtKind::Assign && S.Val &&
              S.Val->Kind == ExprKind::Call && S.Val->CallTarget == "base_dtor")
            return true;
          if (S.Kind == StmtKind::IfElse && OnEveryPath(S.Body) &&
              OnEveryPath(S.ElseBody))
            return true;
        }
        return false;
      };
  EXPECT_TRUE(OnEveryPath(F.Body));
}

TEST(HighControlFlowSemantics, JoinDefaultValueStaysAfterAnArmWritingItsDest) {
  // `if (c) r = x + 2; r = x + 1; return r;` returns x + 1 on both paths.
  auto Plus = [](uint64_t N) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                               HighExpr::makeConst(N, 8));
  };
  auto Arm = assign(0x1008, 5, 0);
  Arm.Val = Plus(2);
  HighStmt Guard;
  Guard.Kind = StmtKind::If;
  Guard.Addr = 0x1004;
  Guard.Cond = local(0);
  Guard.Body = {Arm};
  auto Default = assign(0x1010, 5, 0);
  Default.Val = Plus(1);
  HighFunc F;
  F.Body = {assign(0x1000, 1, 9), Guard, Default, result(0x1018, local(5))};
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(10));
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(10));
}

namespace {
ExprPtr enteredLoopArgument() {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = 0;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, false));
}

HighFunc enteredLoopFunction(const char *Name) {
  HighFunc F;
  F.Name = Name;
  F.ReturnType = NdType::makeInt(8, false);
  F.Params = {{"arg0", NdType::makeInt(8, false)}};
  return F;
}

// Resolve exact nested entries using the source-flow authority, then execute
// the emitted C. The oracle values are independent of the rewritten layout.
void checkEnteredLoopExecution(const HighFunc &F,
                               llvm::ArrayRef<uint64_t> Expected) {
  const auto Flow = buildHighSourceFlowGraph(F);
  std::string Diagnostics;
  for (const auto &Item : Flow.Diagnostics.Items)
    Diagnostics += Item.Reason + "\n";
  ASSERT_TRUE(Flow.Diagnostics.Complete) << Diagnostics;

  std::string Source;
  llvm::raw_string_ostream SourceOS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, SourceOS, Options));
  SourceOS << "\nint main(void) {\n";
  for (size_t I = 0; I < Expected.size(); ++I)
    SourceOS << "  if (" << F.Name << "(" << I << ") != " << Expected[I]
             << ") return " << I + 1 << ";\n";
  SourceOS << "  return 0;\n}\n";
  SourceOS.flush();

#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName("clang");
  if (!Found)
    GTEST_SKIP() << "clang is required for the generated-C entry check";
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<128> SourcePath, ExecutablePath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-loop-entry", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-loop-entry", "exe",
                                                  ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-loop-entry", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC) << EC.message();
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization.str());
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-D__fastcall=",
        "-Werror=return-type",
        SourcePath,
        "-o",
        ExecutablePath};
    const int CompileResult = llvm::sys::ExecuteAndWait(
        Compiler, Arguments, std::nullopt, Redirects, 30);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(CompileResult, 0)
        << (Errors ? (*Errors)->getBuffer().str() : "no compiler diagnostic")
        << "\n"
        << Source;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, {ExecutablePath},
                                        std::nullopt, {}, 10),
              0)
        << Source;
  }
}
} // namespace

TEST(HighControlFlowSemantics, HoistedExitKeepsEnteredChildGoto) {
  for (bool TestFirst : {true, false}) {
    SCOPED_TRACE(TestFirst ? "loop head test" : "loop tail test");
    auto F = enteredLoopFunction(TestFirst ? "entered_head_exit"
                                           : "entered_tail_exit");
    HighStmt Direct;
    Direct.Kind = StmtKind::If;
    Direct.Addr = 0x18;
    Direct.Cond = enteredLoopArgument();
    Direct.Body = {jump(0, 0x34)};
    HighStmt Exit;
    Exit.Kind = StmtKind::If;
    Exit.Addr = 0x30;
    Exit.Cond = local(2);
    Exit.Body = {jump(0x34, 0x60)};
    HighStmt Loop;
    Loop.Kind = StmtKind::While;
    Loop.Addr = 0x20;
    Loop.Body = {assign(0x40, 1, 7), assign(0x44, 2, 1)};
    Loop.Body.insert(TestFirst ? Loop.Body.begin() : Loop.Body.end(), Exit);
    F.Body = {assign(0x10, 1, 3), assign(0x14, 2, 0), Direct, Loop,
              result(0x60, local(1))};

    // A direct jump to the conditional's child bypasses its false condition
    // and returns 3. Ordinary entry runs the body and returns 7.
    checkEnteredLoopExecution(F, {7, 3});
    ASSERT_TRUE(hoistLoopExitTests(F.Body));
    checkEnteredLoopExecution(F, {7, 3});
  }
}

TEST(HighControlFlowSemantics, MovedLoopTailKeepsEnteredBreak) {
  auto F = enteredLoopFunction("entered_break");
  HighStmt Direct;
  Direct.Kind = StmtKind::If;
  Direct.Addr = 0x1c;
  Direct.Cond = enteredLoopArgument();
  Direct.Body = {jump(0, 0x34)};
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  Break.Addr = 0x34;
  HighStmt Leave;
  Leave.Kind = StmtKind::If;
  Leave.Addr = 0x30;
  Leave.Cond = local(2);
  Leave.Body = {Break};
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Addr = 0x20;
  Loop.Body = {Leave, assign(0x40, 1, 7), assign(0x44, 2, 1)};
  auto Repeat = assign(0x18, 3, 0);
  Repeat.Val = HighExpr::makeBinop(NdOp::INT_EQUAL, enteredLoopArgument(),
                                   HighExpr::makeConst(2, 8));
  HighStmt Tail;
  Tail.Kind = StmtKind::If;
  Tail.Addr = 0x50;
  Tail.Cond = local(3);
  Tail.Body = {assign(0x54, 3, 0), jump(0x58, 0x40)};
  F.Body = {
      assign(0x10, 1, 3),    assign(0x14, 2, 0), Repeat, Direct, Loop, Tail,
      result(0x60, local(1))};

  // Inputs cover ordinary loop entry, a direct break, and a direct break
  // followed by one jump from the tail back into the loop body.
  checkEnteredLoopExecution(F, {7, 3, 7});
  ASSERT_TRUE(moveLoopTailsToTheirBreak(F.Body));
  checkEnteredLoopExecution(F, {7, 3, 7});
}

TEST(HighControlFlowSemantics, ConstantIncomingSurvivesTheJoinDefaultSink) {
  // `if (c) { r = 5; goto J; } r = x + 1; J: return r;` The arm's constant
  // is its join value; sinking the default must not overwrite it.
  auto Arm = assign(0x1008, 5, 5);
  HighStmt Guard;
  Guard.Kind = StmtKind::If;
  Guard.Addr = 0x1004;
  Guard.Cond = local(0);
  Guard.Body = {Arm, jump(0x100c, 0x1018)};
  auto Default = assign(0x1010, 5, 0);
  Default.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  HighFunc F;
  F.Body = {assign(0x1000, 1, 9), Guard, Default, result(0x1018, local(5))};
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(10));
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(5));
}

TEST(HighControlFlowSemantics, ConstantJoinDefaultSinksIntoEveryFallthrough) {
  // `if (c) { t = x + 1; if (t) { v = t + 7; goto J; } } v = 0; J: return v;`
  // Both fall-through paths take the constant; the jump carries its own.
  auto Plus = [](int Id, uint64_t N) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(Id),
                               HighExpr::makeConst(N, 8));
  };
  auto T = assign(0x1008, 2, 0);
  T.Val = Plus(1, 1);
  auto V = assign(0x1010, 5, 0);
  V.Val = Plus(2, 7);
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x100c;
  Inner.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(2),
                                   HighExpr::makeConst(0, 8));
  Inner.Body = {V, jump(0x1014, 0x1020)};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1004;
  Outer.Cond = local(0);
  Outer.Body = {T, Inner};
  HighFunc F;
  F.Body = {assign(0x1000, 1, 9), Outer, assign(0x1018, 5, 0),
            result(0x1020, local(5))};
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(0));
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(17));
  unsigned Gotos = 0;
  walkStmts(F.Body,
            [&](const HighStmt &S) { Gotos += S.Kind == StmtKind::Goto; });
  EXPECT_EQ(Gotos, 0u);
}

TEST(HighControlFlowSemantics, JoinDefaultCoversACopyOnlyInnerArm) {
  // `if (c) { t = c - 1; if (t) { v = t; goto J; } } v = 0; J: return v;`
  // The inner arm only copies, yet its else path still takes the default.
  auto T = assign(0x1008, 2, 0);
  T.Val =
      HighExpr::makeBinop(NdOp::INT_SUB, local(0), HighExpr::makeConst(1, 8));
  auto V = assign(0x1010, 5, 0);
  V.Val = local(2);
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x100c;
  Inner.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(2),
                                   HighExpr::makeConst(0, 8));
  Inner.Body = {V, jump(0x1014, 0x1020)};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1004;
  Outer.Cond = local(0);
  Outer.Body = {T, Inner};
  HighFunc F;
  F.Body = {Outer, assign(0x1018, 5, 0), result(0x1020, local(5))};
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(0));
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(0));
  EXPECT_EQ(execute(F, 2), std::optional<uint64_t>(1));
}

TEST(HighControlFlowSemantics, ConstantIncomingBesideARealOneKeepsItsValue) {
  // `if (c == 1) { v = x + 1; goto J; } else if (c == 2) { v = 5; goto J; }
  //  v = x + 7; J: return v;` Each jump carries its own value.
  auto Real = assign(0x1008, 5, 0);
  Real.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  HighStmt Second;
  Second.Kind = StmtKind::If;
  Second.Addr = 0x1010;
  Second.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Second.Body = {assign(0x1014, 5, 5), jump(0x1018, 0x1030)};
  HighStmt First;
  First.Kind = StmtKind::IfElse;
  First.Addr = 0x1004;
  First.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  First.Body = {Real, jump(0x100c, 0x1030)};
  First.ElseBody = {Second};
  auto Default = assign(0x1020, 5, 0);
  Default.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(7, 8));
  HighFunc F;
  F.Body = {assign(0x1000, 1, 9), First, Default, result(0x1030, local(5))};
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(16));
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(10));
  EXPECT_EQ(execute(F, 2), std::optional<uint64_t>(5));
}

/// `w = 3; if (x) { v = w + 7; goto Join; } v = 5; return v + 1;
///  0x1040: return 99;`
HighFunc joinValueChain(va_t Join) {
  auto Arm = assign(0x1008, 5, 0);
  Arm.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(7, 8));
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Addr = 0x1004;
  If.Cond = local(0);
  If.Body = {Arm, jump(0x100c, Join)};
  HighFunc F;
  F.Body = {assign(0x1000, 1, 3), If, assign(0x1030, 5, 5),
            result(0x1034, HighExpr::makeBinop(NdOp::INT_ADD, local(5),
                                               HighExpr::makeConst(1, 8))),
            result(0x1040, HighExpr::makeConst(99, 8))};
  return F;
}

TEST(HighControlFlowSemantics, JoinValueChainKeepsAJumpPastTheUse) {
  // The arm jumps past the use (MiResetAccessBitPteWorker skipped a
  // MiSetVaAgeList call this way); falling into the use would run it.
  for (bool Late : {false, true}) {
    SCOPED_TRACE(Late);
    HighFunc F = joinValueChain(0x1040);
    ASSERT_EQ(execute(F, 1), std::optional<uint64_t>(99));
    ASSERT_EQ(execute(F, 0), std::optional<uint64_t>(6));
    if (Late)
      invertSkipGotos(F);
    else
      structureIfElse(F, 10);
    EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(99));
    EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(6));
  }
}

TEST(HighControlFlowSemantics, JoinValueChainFoldsAJumpToTheUse) {
  for (bool Late : {false, true}) {
    SCOPED_TRACE(Late);
    HighFunc F = joinValueChain(0x1034);
    ASSERT_EQ(execute(F, 1), std::optional<uint64_t>(11));
    ASSERT_EQ(execute(F, 0), std::optional<uint64_t>(6));
    if (Late)
      invertSkipGotos(F);
    else
      structureIfElse(F, 10);
    EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(11));
    EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(6));
    if (Late)
      continue;
    bool HasGoto = false;
    walkStmts(F.Body,
              [&](const HighStmt &S) { HasGoto |= S.Kind == StmtKind::Goto; });
    EXPECT_FALSE(HasGoto) << "the jump to the use becomes fallthrough";
  }
}

TEST(HighControlFlowSemantics, ElseJumpPastTheNextStatementStays) {
  // `y = 7; z = 0; if (x != 0) { if (x == 1) y = 1; else if (x == 2) y = 2;
  // else goto T; z = 5; } T: return y + z;` The inner `else goto T` skips
  // `z = 5`; T is where the outer if falls through, not where this else arm
  // does.
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1010;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Inner.Body = {assign(0x1014, 1, 2)};
  Inner.ElseBody = {jump(0x1018, 0x1100)};
  HighStmt Middle;
  Middle.Kind = StmtKind::IfElse;
  Middle.Addr = 0x1008;
  Middle.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  Middle.Body = {assign(0x100c, 1, 1)};
  Middle.ElseBody = {Inner};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1004;
  Outer.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                                   HighExpr::makeConst(0, 8));
  Outer.Body = {Middle, assign(0x1020, 3, 5)};
  HighFunc F;
  F.Body = {
      assign(0x1000, 1, 7), assign(0x1002, 3, 0), Outer,
      result(0x1100, HighExpr::makeBinop(NdOp::INT_ADD, local(1), local(3)))};
  auto Expected = [](uint64_t X) -> uint64_t {
    return X == 0 ? 7 : X == 1 ? 6 : X == 2 ? 7 : 7;
  };
  for (uint64_t X : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, X), Expected(X));
  HighFunc Late = F;
  structureIfElse(F, 10);
  invertSkipGotos(Late);
  for (uint64_t X : {0, 1, 2, 3}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), Expected(X));
    EXPECT_EQ(execute(Late, X), Expected(X));
  }
}

/// A store to the one memory slot the skip-over tests observe.
HighStmt storeSlot(va_t Address, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Store;
  S.Addr = Address;
  S.StoreAddr = HighExpr::makeConst(0x100, 8);
  S.StoreVal = std::move(Value);
  return S;
}

ExprPtr loadSlot() {
  return HighExpr::makeLoad(HighExpr::makeConst(0x100, 8), NdType::makeInt(8));
}

TEST(HighControlFlowSemantics, StoreThroughAnAliasKeepsAFieldRetest) {
  // p = q = 0x100; p->f = 1; t = p->f; v = 0;
  // if (t) { q->f = x; if (p->f) v = 7; }  return v;
  // The store through q may change p->f, so `t` does not imply the second
  // test; a call that may write memory is the same case.
  auto PointerTo = [](int Id) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(Id),
                               HighExpr::makeConst(8, 8));
  };
  auto LoadP = [&] {
    return HighExpr::makeLoad(PointerTo(5), NdType::makeInt(8));
  };
  auto Store = [](va_t Address, ExprPtr Pointer, ExprPtr Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Address;
    S.StoreAddr = std::move(Pointer);
    S.StoreVal = std::move(Value);
    return S;
  };
  auto Field = assign(0x100c, 2, 0);
  Field.Val = LoadP();
  HighStmt Retest;
  Retest.Kind = StmtKind::If;
  Retest.Addr = 0x101c;
  Retest.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, LoadP(),
                                    HighExpr::makeConst(0, 8));
  Retest.Body = {assign(0x1020, 1, 7)};
  HighStmt Guard;
  Guard.Kind = StmtKind::If;
  Guard.Addr = 0x1014;
  Guard.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(2),
                                   HighExpr::makeConst(0, 8));
  Guard.Body = {Store(0x1018, PointerTo(6), local(0)), Retest};
  HighFunc F;
  F.Entry = 0x1000;
  F.Body = {assign(0x1000, 5, 0x100),
            assign(0x1004, 6, 0x100),
            Store(0x1008, PointerTo(5), HighExpr::makeConst(1, 8)),
            Field,
            assign(0x1010, 1, 0),
            Guard,
            result(0x1030, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t { return X ? 7 : 0; };
  for (uint64_t X : {0, 1})
    ASSERT_EQ(execute(F, X), Expected(X));
  invertSkipGotos(F);
  for (uint64_t X : {0, 1})
    EXPECT_EQ(execute(F, X), Expected(X)) << X;
}

TEST(HighControlFlowSemantics, SkipOverWorkStaysOnTheOuterElsePath) {
  // `*m = 0; w = 0; if (x != 0) { if (x == 2) { w = 1; goto J; } } else
  // { w = 3; } *m = 7; J: return *m + w;` The outer else runs into the store
  // too; moving it under the inner test would skip it there.
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x100c;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Inner.Body = {assign(0x1010, 1, 1), jump(0x1014, 0x1100)};
  HighStmt Outer;
  Outer.Kind = StmtKind::IfElse;
  Outer.Addr = 0x1004;
  Outer.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                                   HighExpr::makeConst(0, 8));
  Outer.Body = {Inner};
  Outer.ElseBody = {assign(0x1008, 1, 3)};
  HighFunc F;
  F.Body = {
      storeSlot(0x0ff0, HighExpr::makeConst(0, 8)), assign(0x1000, 1, 0), Outer,
      storeSlot(0x1020, HighExpr::makeConst(7, 8)),
      result(0x1100, HighExpr::makeBinop(NdOp::INT_ADD, loadSlot(), local(1)))};
  auto Expected = [](uint64_t X) -> uint64_t {
    return X == 0 ? 10 : X == 2 ? 1 : 7;
  };
  for (uint64_t X : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, X), Expected(X));
  HighFunc Late = F;
  structureIfElse(F, 10);
  invertSkipGotos(Late);
  for (uint64_t X : {0, 1, 2, 3}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), Expected(X));
    EXPECT_EQ(execute(Late, X), Expected(X));
  }
}

TEST(HighControlFlowSemantics, PredicateChainKeepsItsPrefixPath) {
  // `*m = 0; w = 0; if (x != 0) { w = 5; if (x == 2) { *m = w + 1; goto J; } }
  // *m = w + 100; J: return *m;` When x != 0 fails the inner test, `w = 5`
  // still ran before the else work; `if (x != 0 && x == 2)` would skip it.
  auto Plus = [](uint64_t N) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                               HighExpr::makeConst(N, 8));
  };
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x100c;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Inner.Body = {storeSlot(0x1010, Plus(1)), jump(0x1014, 0x1100)};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1004;
  Outer.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(0),
                                   HighExpr::makeConst(0, 8));
  Outer.Body = {assign(0x1008, 1, 5), Inner};
  HighFunc F;
  F.Body = {storeSlot(0x0ff0, HighExpr::makeConst(0, 8)), assign(0x1000, 1, 0),
            Outer, storeSlot(0x1020, Plus(100)), result(0x1100, loadSlot())};
  auto Expected = [](uint64_t X) -> uint64_t {
    return X == 0 ? 100 : X == 2 ? 6 : 105;
  };
  for (uint64_t X : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, X), Expected(X));
  HighFunc Late = F;
  structureIfElse(F, 10);
  invertSkipGotos(Late);
  for (uint64_t X : {0, 1, 2, 3}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(F, X), Expected(X));
    EXPECT_EQ(execute(Late, X), Expected(X));
  }
}

TEST(HighControlFlowSemantics, JoinJumpStillSkipsWorkBeforeTheDefault) {
  // `if (c) { v = x + 1; goto J; } w = 7; v = x + 3; J: return v + w;`
  // The jump skips `w = 7`; sinking the default must not make it fall into
  // that work (KsepShimDatabaseTime's error log ran on its success path).
  auto Plus = [](uint64_t N) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                               HighExpr::makeConst(N, 8));
  };
  auto Incoming = assign(0x1008, 5, 0);
  Incoming.Val = Plus(1);
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Addr = 0x1004;
  If.Cond = local(0);
  If.Body = {Incoming, jump(0x100c, 0x1040)};
  auto Work = assign(0x1010, 3, 0);
  Work.Val = HighExpr::makeConst(7, 8);
  auto Default = assign(0x1030, 5, 0);
  Default.Val = Plus(3);
  HighFunc F;
  F.Body = {
      assign(0x1000, 1, 9),
      assign(0x1002, 3, 1),
      If,
      Work,
      Default,
      result(0x1040, HighExpr::makeBinop(NdOp::INT_ADD, local(5), local(3)))};
  HighFunc Late = F;
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(11));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(19));
  sinkJoinDefaultsLate(Late);
  EXPECT_EQ(execute(Late, 1), std::optional<uint64_t>(11));
  EXPECT_EQ(execute(Late, 0), std::optional<uint64_t>(19));
}

TEST(HighControlFlowSemantics, JoinDefaultKeepsValuesOfAnArmWithTwoJumps) {
  // `if (c) { if (d) { v = a; goto J; } x = 1; v = b; goto J; }
  //  v = def; J: return v;` Either the sink leaves this alone or every
  // path keeps its value.
  auto Plus = [](uint64_t N) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                               HighExpr::makeConst(N, 8));
  };
  auto A = assign(0x1010, 5, 0);
  A.Val = Plus(1);
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x100c;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Inner.Body = {A, jump(0x1014, 0x1040)};
  auto B = assign(0x101c, 5, 0);
  B.Val = Plus(2);
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1004;
  Outer.Cond = local(0);
  Outer.Body = {Inner, assign(0x1018, 3, 1), B, jump(0x1020, 0x1040)};
  auto Default = assign(0x1030, 5, 0);
  Default.Val = Plus(3);
  HighFunc F;
  F.Body = {assign(0x1000, 1, 9), Outer, Default, result(0x1040, local(5))};
  HighFunc Late = F;
  structureIfElse(F, 10);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(12));
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(11));
  EXPECT_EQ(execute(F, 2), std::optional<uint64_t>(10));
  // The late entry point runs the same sink on the same shape.
  sinkJoinDefaultsLate(Late);
  EXPECT_EQ(execute(Late, 0), std::optional<uint64_t>(12));
  EXPECT_EQ(execute(Late, 1), std::optional<uint64_t>(11));
  EXPECT_EQ(execute(Late, 2), std::optional<uint64_t>(10));
}

TEST(HighControlFlowSemantics, IndirectCallTargetKeepsItsConditionTemporary) {
  // t1 = *slot; if (t1) return t1(); return 0;
  // The call target reads t1 as well, so t1 cannot fold into the condition.
  auto Load = assign(0x1000, 1, 0);
  Load.Val =
      HighExpr::makeLoad(HighExpr::makeConst(0x100, 8), NdType::makeInt(8));
  auto Call = HighExpr::makeCall("", 0, {});
  Call->IndirectTarget = local(1);
  Call->Type = NdType::makeInt(8);
  HighStmt Test;
  Test.Kind = StmtKind::If;
  Test.Addr = 0x1004;
  Test.Cond = local(1);
  Test.Body = {result(0x1008, Call)};
  std::vector<HighStmt> Body = {Load, Test,
                                result(0x100c, HighExpr::makeConst(0, 8))};
  reduceSingleUseGotos(Body);
  bool Assigned = false;
  walkStmts(Body, [&](const HighStmt &S) {
    Assigned |= S.Kind == StmtKind::Assign && S.Dst && S.Dst->Var.Id == 1;
  });
  EXPECT_TRUE(Assigned);
}

TEST(HighControlFlowSemantics, NestedIfMergeKeepsAnEnteredPrefix) {
  // if (t0) { t1 = *slot; if (t1) { *out = 7; if (t3) goto reload; } }
  // The back edge re-enters the load, so folding it into `t0 && *slot`
  // would drop the label and skip the inner test on re-entry.
  const auto Slot = HighExpr::makeConst(0x100, 8);
  auto Reload = assign(0x1008, 1, 0);
  Reload.Val = HighExpr::makeLoad(Slot, NdType::makeInt(8));
  HighStmt Keep;
  Keep.Kind = StmtKind::Store;
  Keep.Addr = 0x1010;
  Keep.StoreAddr = HighExpr::makeConst(0x200, 8);
  Keep.StoreVal = HighExpr::makeConst(7, 8);
  HighStmt Again;
  Again.Kind = StmtKind::If;
  Again.Addr = 0x1014;
  Again.Cond = local(3);
  Again.Body = {jump(0, 0x1008)};
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x100c;
  Inner.Cond = local(1);
  Inner.Body = {Keep, Again};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1004;
  Outer.Cond = local(0);
  Outer.Body = {Reload, Inner};
  HighFunc F;
  F.Body = {Outer, result(0x1020, HighExpr::makeConst(0, 8))};
  structureIfElse(F, 10);
  size_t Entries = 0;
  walkStmts(F.Body, [&](const HighStmt &S) { Entries += S.Addr == 0x1008; });
  EXPECT_EQ(Entries, 1u);
}

TEST(HighControlFlowSemantics, JoinDefaultStaysWhereAnEarlierJumpEntersIt) {
  // if (c & 1) goto pad; if (c & 2) { v = c + 5; goto join; } pad: ; v = 7;
  // join: return v. The first jump lands on the empty statement before the
  // default and runs into it, so the default cannot move into an else arm.
  auto Low = assign(0x0ff8, 3, 0);
  Low.Val =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  HighStmt ToPad;
  ToPad.Kind = StmtKind::If;
  ToPad.Addr = 0x1000;
  ToPad.Cond = local(3);
  ToPad.Body = {jump(0, 0x1010)};
  auto High = assign(0x1002, 4, 0);
  High.Val =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(2, 8));
  auto Early = assign(0x1004, 2, 0);
  Early.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(0), HighExpr::makeConst(5, 8));
  Early.IsPhiCopy = true;
  HighStmt ToJoin;
  ToJoin.Kind = StmtKind::If;
  ToJoin.Addr = 0x1004;
  ToJoin.Cond = local(4);
  ToJoin.Body = {Early, jump(0, 0x1018)};
  HighStmt Pad;
  Pad.Kind = StmtKind::Block;
  Pad.Addr = 0x1010;
  HighStmt Join;
  Join.Kind = StmtKind::Block;
  Join.Addr = 0x1018;
  auto Default = assign(0x1014, 2, 7);
  Default.IsPhiCopy = true;
  HighFunc F;
  F.Body = {Low, ToPad,   High, ToJoin,
            Pad, Default, Join, result(0x101c, local(2))};
  sinkJoinDefaultsLate(F);
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(7));
  EXPECT_EQ(execute(F, 2), std::optional<uint64_t>(7));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(7));
  EXPECT_EQ(execute(F, 6), std::optional<uint64_t>(11));
}

namespace {
HighStmt guardedBackedge(va_t Address, uint64_t Bit) {
  // if (v & Bit) { v = v - Bit; goto 0x1010; }
  auto Step = assign(Address + 4, 1, 0);
  Step.Val =
      HighExpr::makeBinop(NdOp::INT_SUB, local(1), HighExpr::makeConst(Bit, 8));
  HighStmt Test;
  Test.Kind = StmtKind::If;
  Test.Addr = Address;
  Test.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(1), HighExpr::makeConst(Bit, 8));
  Test.Body = {Step, jump(Address + 8, 0x1010)};
  return Test;
}

// v = c; goto X; X: ; if (v != 0) { <odd: v -= 1, goto X> <v & 2: v -= 2,
// goto X> return v; } return 0;
HighFunc entryJumpBeforeItsLoopLabel() {
  auto Copy = assign(0x1000, 1, 0);
  Copy.Val = local(0);
  HighStmt Anchor;
  Anchor.Kind = StmtKind::Block;
  Anchor.Addr = 0x1010;
  HighStmt Header;
  Header.Kind = StmtKind::If;
  Header.Addr = 0x1014;
  Header.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, local(1),
                                    HighExpr::makeConst(0, 8));
  Header.Body = {guardedBackedge(0x1018, 1), guardedBackedge(0x1024, 2),
                 result(0x1030, local(1))};
  // A region moved away leaves removed statements behind.
  HighStmt Removed;
  Removed.Kind = StmtKind::Nop;
  HighFunc F;
  F.Body = {Copy,    jump(0x1004, 0x1010),
            Removed, Anchor,
            Header,  result(0x1040, HighExpr::makeConst(0, 8))};
  return F;
}

size_t countKind(const HighFunc &F, StmtKind Kind) {
  size_t N = 0;
  walkStmts(F.Body, [&](const HighStmt &S) { N += S.Kind == Kind; });
  return N;
}
} // namespace

TEST(HighControlFlowSemantics, JumpOntoTheNextStatementLeavesTheLoopVisible) {
  // Earlier rewrites can leave the entry jump right before its label. It
  // is a fall-through, so the only real entries of the label are the two
  // backedges and the region becomes one loop.
  HighFunc F = entryJumpBeforeItsLoopLabel();
  const uint64_t Inputs[] = {0, 3, 5, 6, 8};
  const uint64_t Results[] = {0, 0, 4, 4, 8};
  for (size_t I = 0; I < 5; ++I)
    ASSERT_EQ(execute(F, Inputs[I]), Results[I]);
  EXPECT_TRUE(dropJumpsToTheNextStatement(F.Body));
  EXPECT_TRUE(loopifyBackwardGotos(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  EXPECT_EQ(countKind(F, StmtKind::While), 1u);
  for (size_t I = 0; I < 5; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
}

TEST(HighControlFlowSemantics, JumpOntoARepeatedAddressIsNotAFallthrough) {
  // C labels the first statement printed at an address. When the address
  // also starts an earlier statement, the jump goes there, not to the
  // statement after it.
  HighFunc F = entryJumpBeforeItsLoopLabel();
  HighStmt Earlier;
  Earlier.Kind = StmtKind::Block;
  Earlier.Addr = 0x1010;
  F.Body.insert(F.Body.begin(), Earlier);
  EXPECT_FALSE(dropJumpsToTheNextStatement(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 3u);

  // A jump that is itself entered keeps its address as an empty anchor.
  F = entryJumpBeforeItsLoopLabel();
  F.Body[1].Addr = 0x1008;
  F.Body.push_back(jump(0x1044, 0x1008));
  EXPECT_TRUE(dropJumpsToTheNextStatement(F.Body));
  ASSERT_EQ(F.Body[1].Kind, StmtKind::Block);
  EXPECT_EQ(F.Body[1].Addr, 0x1008u);
  EXPECT_TRUE(F.Body[1].Body.empty());
}

TEST(HighControlFlowSemantics, HandlerReceivesNoBlockFromOutside) {
  // __try { if (x) goto M; v = 1; } __except (1) { goto L; }  return v;
  // L: if (x) { M: v = 3; }  v = v + 4;  return v;
  // L is entered once, from the handler, but its block carries M, which the
  // protected body enters: moved into the handler, that jump would enter the
  // __except block, which C forbids.
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = 0x1000;
  Try.EHRange = {0x1000, 0x1008};
  Try.Body = {conditional(0x1000, 0x1028), assign(0x1004, 1, 1)};
  HighEHClause Clause;
  Clause.Kind = HighEHClauseKind::SEHExcept;
  Clause.HandlerVA = 0x1010;
  Try.EHClauses = {Clause};
  Try.EHClauseBodies = {{jump(0x1010, 0x1020)}};
  HighStmt Nested;
  Nested.Kind = StmtKind::If;
  Nested.Addr = 0x1020;
  Nested.Cond = local(0);
  Nested.Body = {assign(0x1028, 1, 3)};
  auto Plus4 = assign(0x1030, 1, 0);
  Plus4.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(4, 8));
  HighFunc F;
  F.Body = {Try, result(0x1014, local(1)), Nested, Plus4,
            result(0x1034, local(1))};
  reduceSingleUseGotos(F.Body, /*SpliceRegions=*/true);
  std::set<va_t> InHandler;
  std::size_t JumpsIn = 0;
  for (const auto &Handler : F.Body.front().EHClauseBodies)
    walkStmts(Handler, [&](const HighStmt &S) { InHandler.insert(S.Addr); });
  walkStmts(F.Body.front().Body, [&](const HighStmt &S) {
    JumpsIn += S.Kind == StmtKind::Goto && InHandler.count(S.GotoTarget);
  });
  EXPECT_EQ(JumpsIn, 0u);
}

TEST(HighControlFlowSemantics, ElseJumpPastAStatementWithoutAddressStays) {
  // if (x & 1) { v = 1; }
  // else {
  //   if (x & 2) { v = 2; }
  //   else { if (x & 4) { v = 4; } else { v = 3; goto J; } }
  //   v = v + 10;
  // }
  // J: return v;
  // The innermost jump skips `v = v + 10`, which has no address; J follows
  // the outer if, not the middle one the jump's if/else ends.
  auto Bit = [](uint64_t Mask) {
    return HighExpr::makeBinop(NdOp::INT_AND, local(0),
                               HighExpr::makeConst(Mask, 8));
  };
  auto Copy = assign(0x1028, 1, 3);
  Copy.IsPhiCopy = true;
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1020;
  Inner.Cond = Bit(4);
  Inner.Body = {assign(0x1024, 1, 4)};
  Inner.ElseBody = {Copy, jump(0, 0x1040)};
  HighStmt Middle;
  Middle.Kind = StmtKind::IfElse;
  Middle.Addr = 0x1010;
  Middle.Cond = Bit(2);
  Middle.Body = {assign(0x1014, 1, 2)};
  Middle.ElseBody = {Inner};
  auto Plus10 = assign(0, 1, 0);
  Plus10.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(10, 8));
  HighStmt Outer;
  Outer.Kind = StmtKind::IfElse;
  Outer.Addr = 0x1000;
  Outer.Cond = Bit(1);
  Outer.Body = {assign(0x1004, 1, 1)};
  Outer.ElseBody = {Middle, Plus10};
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x1040;
  HighFunc F;
  F.Entry = 0x1000;
  F.Body = {Outer, Label, result(0x1044, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t {
    return X & 1 ? 1 : X & 2 ? 12 : X & 4 ? 14 : 3;
  };
  for (uint64_t X : {0, 1, 2, 4})
    ASSERT_EQ(execute(F, X), Expected(X));
  invertSkipGotos(F);
  for (uint64_t X : {0, 1, 2, 4})
    EXPECT_EQ(execute(F, X), Expected(X)) << X;
}

TEST(HighControlFlowSemantics, TailThatCannotFaultEntersExceptProtection) {
  // __try { if (x & 1) { v = 1; goto out; } } __except (1) { v = 0;
  // return v; }  v = 2;  out: return v;
  // Returning v cannot fault, so a copy inside the __try runs the same; a
  // __finally would run around the copy instead of after the jump, and a
  // load could fault into the handler, so neither gets the copy.
  enum class Variant { ExceptOnly, Finally, Load };
  auto Build = [](Variant Kind) {
    HighStmt Leave;
    Leave.Kind = StmtKind::If;
    Leave.Addr = 0x1000;
    Leave.Cond =
        HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
    Leave.Body = {assign(0x1004, 1, 1), jump(0x1008, 0x1020)};
    HighStmt Try;
    Try.Kind = StmtKind::SEHTry;
    Try.Addr = 0x1000;
    Try.EHRange = {0x1000, 0x100c};
    Try.Body = {Leave};
    HighEHClause Clause;
    Clause.Kind = Kind == Variant::Finally ? HighEHClauseKind::SEHFinally
                                           : HighEHClauseKind::SEHExcept;
    Clause.HandlerVA = 0x1010;
    Try.EHClauses = {Clause};
    Try.EHClauseBodies = {{assign(0x1010, 1, 0), result(0x1014, local(1))}};
    HighStmt Tail = result(0x1020, local(1));
    if (Kind == Variant::Load)
      Tail.RetVal =
          HighExpr::makeLoad(HighExpr::makeConst(0x100, 8), NdType::makeInt(8));
    HighFunc F;
    F.Body = {Try, assign(0x1018, 1, 2), Tail};
    return F;
  };
  for (Variant Kind : {Variant::ExceptOnly, Variant::Finally, Variant::Load}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    HighFunc F = Build(Kind);
    duplicateSmallReturnTails(F.Body);
    const std::vector<HighStmt> &Protected = F.Body.front().Body;
    size_t Jumps = 0;
    walkStmts(Protected,
              [&](const HighStmt &S) { Jumps += S.Kind == StmtKind::Goto; });
    EXPECT_EQ(Jumps, Kind == Variant::ExceptOnly ? 0u : 1u);
    if (Kind == Variant::ExceptOnly)
      for (uint64_t X : {0, 1})
        EXPECT_EQ(execute(F, X), X & 1 ? 1u : 2u) << X;
  }
}

TEST(HighControlFlowSemantics, TailRunningOutOfAnExceptTryIsCopiedInside) {
  // __try { if (x & 1) { v = 1; goto out; } v = 2; out: w = 5; }
  // __except (1) { v = 9; }  return v;
  // The tail at out runs off the protected body into `return v`, which
  // cannot fault, so a copy of both runs the same inside the __try.  A
  // load after the try could fault into the handler there, and a __finally
  // would run around the copy, so neither gets one.
  enum class Variant { ExceptOnly, Finally, Load };
  auto Build = [](Variant Kind) {
    HighStmt Leave;
    Leave.Kind = StmtKind::If;
    Leave.Addr = 0x1000;
    Leave.Cond =
        HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
    Leave.Body = {assign(0x1004, 1, 1), jump(0x1008, 0x1010)};
    HighStmt Try;
    Try.Kind = StmtKind::SEHTry;
    Try.Addr = 0x1000;
    Try.EHRange = {0x1000, 0x1014};
    Try.Body = {Leave, assign(0x100c, 1, 2), assign(0x1010, 2, 5)};
    HighEHClause Clause;
    Clause.Kind = Kind == Variant::Finally ? HighEHClauseKind::SEHFinally
                                           : HighEHClauseKind::SEHExcept;
    Clause.HandlerVA = 0x1018;
    Try.EHClauses = {Clause};
    Try.EHClauseBodies = {{assign(0x1018, 1, 9)}};
    HighStmt Tail = result(0x1020, local(1));
    if (Kind == Variant::Load)
      Tail.RetVal =
          HighExpr::makeLoad(HighExpr::makeConst(0x100, 8), NdType::makeInt(8));
    HighFunc F;
    F.Body = {Try, Tail};
    return F;
  };
  for (Variant Kind : {Variant::ExceptOnly, Variant::Finally, Variant::Load}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    HighFunc F = Build(Kind);
    duplicateSmallReturnTails(F.Body);
    size_t Jumps = 0;
    walkStmts(F.Body.front().Body,
              [&](const HighStmt &S) { Jumps += S.Kind == StmtKind::Goto; });
    EXPECT_EQ(Jumps, Kind == Variant::ExceptOnly ? 0u : 1u);
    if (Kind == Variant::ExceptOnly)
      for (uint64_t X : {0, 1})
        EXPECT_EQ(execute(F, X), X & 1 ? 1u : 2u) << X;
  }
}

TEST(HighControlFlowSemantics, JumpIntoTheNextIfArmMergesTheTests) {
  // if (x & 1) goto L; if (x & 2) { L: v = 1; } else { v = 2; } return v;
  // is `if ((x & 1) || (x & 2))`; with L opening the else arm it is
  // `if (!(x & 1) && (x & 2))`.  The second test still runs only when the
  // first fails.  A jump to the second if would skip the first test, so a
  // label there keeps the code as it is.
  enum class Variant { Then, Else, Labeled };
  auto Bit = [](uint64_t B) {
    return HighExpr::makeBinop(NdOp::INT_AND, local(0),
                               HighExpr::makeConst(B, 8));
  };
  auto Build = [&](Variant Kind) {
    const va_t Target = Kind == Variant::Else ? 0x1018 : 0x1010;
    HighStmt Test;
    Test.Kind = StmtKind::If;
    Test.Addr = 0x1000;
    Test.Cond = Bit(1);
    Test.Body = {jump(0x1004, Target)};
    HighStmt Next;
    Next.Kind = StmtKind::IfElse;
    Next.Addr = 0x1008;
    Next.Cond = Bit(2);
    Next.Body = {assign(0x1010, 1, 1)};
    Next.ElseBody = {assign(0x1018, 1, 2)};
    HighStmt Skip;
    Skip.Kind = StmtKind::If;
    Skip.Addr = 0x1020;
    Skip.Cond = Bit(4);
    Skip.Body = {jump(0x1024, 0x1008)};
    HighFunc F;
    F.Body = {Test, Next, result(0x1030, local(1))};
    if (Kind == Variant::Labeled)
      F.Body.insert(F.Body.begin(), Skip);
    return F;
  };
  for (Variant Kind : {Variant::Then, Variant::Else, Variant::Labeled}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    HighFunc F = Build(Kind);
    EXPECT_EQ(mergeJumpsIntoNextIfArms(F), Kind != Variant::Labeled);
    size_t Jumps = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Jumps += S.Kind == StmtKind::Goto && S.GotoTarget != 0x1008;
    });
    EXPECT_EQ(Jumps, Kind == Variant::Labeled ? 1u : 0u);
    if (Kind == Variant::Labeled)
      continue;
    // The jump takes the arm it opens; otherwise the second test decides.
    for (uint64_t X = 0; X < 8; ++X) {
      const bool ThenArm = (X & 1) ? Kind == Variant::Then : (X & 2) != 0;
      EXPECT_EQ(execute(F, X), ThenArm ? 1u : 2u) << X;
    }
  }
}

TEST(HighControlFlowSemantics, JumpBackFromNestedArmsBecomesALoop) {
  // v = x; X: v = v + 1; if (v & 1) { if (v & 2) goto X; w = 3; } return v;
  // becomes `while (1) { v = v + 1; if (v & 1) { if (v & 2) continue; w = 3;
  // } break; }`.  A loose break in the region would bind to the new loop,
  // so it keeps the jump.
  auto Build = [](bool LooseBreak) {
    auto Bump = [](va_t Address) {
      HighStmt S;
      S.Kind = StmtKind::Assign;
      S.Addr = Address;
      S.Dst = local(1);
      S.Val = HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                  HighExpr::makeConst(1, 8));
      return S;
    };
    auto Bit = [](uint64_t B) {
      return HighExpr::makeBinop(NdOp::INT_AND, local(1),
                                 HighExpr::makeConst(B, 8));
    };
    HighStmt Init;
    Init.Kind = StmtKind::Assign;
    Init.Addr = 0x0ff0;
    Init.Dst = local(1);
    Init.Val = local(0);
    HighStmt Inner;
    Inner.Kind = StmtKind::If;
    Inner.Addr = 0x1008;
    Inner.Cond = Bit(2);
    Inner.Body = {jump(0x100c, 0x1000)};
    HighStmt Outer;
    Outer.Kind = StmtKind::If;
    Outer.Addr = 0x1004;
    Outer.Cond = Bit(1);
    Outer.Body = {Inner, assign(0x1010, 2, 3)};
    if (LooseBreak) {
      HighStmt Leave;
      Leave.Kind = StmtKind::Break;
      Outer.Body.push_back(Leave);
    }
    HighFunc F;
    F.Body = {Init, Bump(0x1000), Outer, result(0x1020, local(1))};
    return F;
  };
  for (bool LooseBreak : {false, true}) {
    SCOPED_TRACE(LooseBreak);
    HighFunc F = Build(LooseBreak);
    EXPECT_EQ(loopsForNestedJumpsBack(F.Body), !LooseBreak);
    size_t Jumps = 0, Loops = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Jumps += S.Kind == StmtKind::Goto;
      Loops += S.Kind == StmtKind::While;
    });
    EXPECT_EQ(Jumps, LooseBreak ? 1u : 0u);
    EXPECT_EQ(Loops, LooseBreak ? 0u : 1u);
    if (LooseBreak)
      continue;
    for (uint64_t X = 0; X < 8; ++X) {
      uint64_t V = X + 1;
      while ((V & 3) == 3)
        ++V;
      EXPECT_EQ(execute(F, X), V) << X;
    }
  }
}

TEST(HighControlFlowSemantics, LoopJumpsBecomeBreakAndContinue) {
  // v = x; while (1) { X: v = v + 1; if (v & 1) goto X; if (v & 4) goto Out;
  // v = v + 2; } Out: return v;
  // The jump to the top of the always-true loop is `continue` and the jump
  // to what follows it is `break`.  Inside a switch, `break` would only
  // leave the switch, so a jump out of the loop from a case stays.
  auto Build = [](bool InSwitch) {
    auto Add = [](va_t Address, uint64_t N) {
      HighStmt S;
      S.Kind = StmtKind::Assign;
      S.Addr = Address;
      S.Dst = local(1);
      S.Val = HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                  HighExpr::makeConst(N, 8));
      return S;
    };
    auto Test = [](va_t Address, uint64_t Bit, va_t Target) {
      HighStmt S;
      S.Kind = StmtKind::If;
      S.Addr = Address;
      S.Cond = HighExpr::makeBinop(NdOp::INT_AND, local(1),
                                   HighExpr::makeConst(Bit, 8));
      S.Body = {jump(Address + 4, Target)};
      return S;
    };
    HighStmt Init;
    Init.Kind = StmtKind::Assign;
    Init.Addr = 0x0ff0;
    Init.Dst = local(1);
    Init.Val = local(0);
    HighStmt Leave = Test(0x1010, 4, 0x1030);
    if (InSwitch) {
      HighStmt Dispatch;
      Dispatch.Kind = StmtKind::Switch;
      Dispatch.Addr = 0x1010;
      Dispatch.SwitchExpr = HighExpr::makeBinop(NdOp::INT_AND, local(1),
                                                HighExpr::makeConst(4, 8));
      Dispatch.Cases = {{4, {jump(0x1014, 0x1030)}}};
      Leave = Dispatch;
    }
    HighStmt Loop;
    Loop.Kind = StmtKind::While;
    Loop.Cond = HighExpr::makeConst(1, 1);
    Loop.Body = {Add(0x1000, 1), Test(0x1008, 1, 0x1000), Leave,
                 Add(0x1020, 2)};
    HighFunc F;
    F.Body = {Init, Loop, result(0x1030, local(1))};
    return F;
  };
  for (bool InSwitch : {false, true}) {
    SCOPED_TRACE(InSwitch);
    HighFunc F = Build(InSwitch);
    EXPECT_TRUE(loopJumpsAsBreakAndContinue(F.Body));
    size_t Jumps = 0;
    walkStmts(F.Body,
              [&](const HighStmt &S) { Jumps += S.Kind == StmtKind::Goto; });
    EXPECT_EQ(Jumps, InSwitch ? 1u : 0u);
    for (uint64_t X = 0; X < 8; ++X) {
      uint64_t V = X;
      while (true) {
        ++V;
        if (V & 1)
          continue;
        if (V & 4)
          break;
        V += 2;
      }
      EXPECT_EQ(execute(F, X), V) << X;
    }
  }
}

TEST(HighControlFlowSemantics, TrimmedJoinJumpKeepsTheLabelOthersEnter) {
  // v = 0; if (x & 4) goto J; if (x & 1) { v = 1; J: goto Out; } else {
  // v = 2; } Out: return v;
  // The arm's trailing jump to Out is redundant once Out follows the if,
  // but `goto J` still lands on that jump, so its label must stay.
  HighStmt Enter;
  Enter.Kind = StmtKind::If;
  Enter.Addr = 0x1000;
  Enter.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(4, 8));
  Enter.Body = {jump(0x1002, 0x1018)};
  HighStmt Split;
  Split.Kind = StmtKind::IfElse;
  Split.Addr = 0x1004;
  Split.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Split.Body = {assign(0x1008, 1, 1), jump(0x1018, 0x1030)};
  Split.ElseBody = {assign(0x1020, 1, 2)};
  HighFunc F;
  F.Body = {assign(0x0ff0, 1, 0), Enter, Split, result(0x1030, local(1))};
  structureIfElse(F, 10);
  std::set<va_t> Starts, Targets;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Starts.insert(S.Addr);
    if (S.Kind == StmtKind::Goto)
      Targets.insert(S.GotoTarget);
  });
  for (va_t Target : Targets)
    EXPECT_TRUE(Starts.count(Target)) << "no statement at " << Target;
}

TEST(HighControlFlowSemantics, CopiedReturnTailsFoldTheirTemporaries) {
  // if (x & 1) { t = v; return t + 1; } t = v; return t + 1;
  // Both copies of the tail assign t, so neither is a single assignment,
  // yet each run ends in a return: its read takes v directly.  A temporary
  // also read outside its own run, and an update of a variable, stay.
  auto Temp = [](int Id, int Ver) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.SSAVer = Ver;
    V.Size = 8;
    V.TheArch = Arch::X64;
    return HighExpr::makeVar(V, NdType::makeInt(8));
  };
  auto Copy = [&](va_t Address, ExprPtr Dst, ExprPtr Val) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.Dst = std::move(Dst);
    S.Val = std::move(Val);
    return S;
  };
  auto Plus = [](ExprPtr A, uint64_t N) {
    return HighExpr::makeBinop(NdOp::INT_ADD, std::move(A),
                               HighExpr::makeConst(N, 8));
  };
  enum class Variant { Copies, ReadElsewhere, Update };
  auto Build = [&](Variant Kind) {
    HighStmt Arm;
    Arm.Kind = StmtKind::If;
    Arm.Addr = 0x1000;
    Arm.Cond =
        HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
    Arm.Body = {Copy(0x1004, Temp(7, 1), local(1)),
                result(0x1008, Plus(Temp(7, 1), 1))};
    HighFunc F;
    F.Body = {assign(0x0ff0, 1, 5), Arm};
    if (Kind == Variant::ReadElsewhere)
      F.Body.push_back(Copy(0x1010, local(2), Temp(7, 1)));
    if (Kind == Variant::Update) {
      F.Body.front() = Copy(0x0ff0, Temp(7, 1), HighExpr::makeConst(5, 8));
      F.Body[1].Body.front() = Copy(0x1004, Temp(7, 2), Plus(Temp(7, 1), 20));
      F.Body[1].Body.back() = result(0x1008, Temp(7, 2));
    }
    F.Body.push_back(
        Copy(0x1014, Temp(7, Kind == Variant::Update ? 2 : 1),
             Kind == Variant::Update ? Plus(Temp(7, 1), 20) : local(1)));
    F.Body.push_back(result(
        0x1018, Kind == Variant::Update ? Temp(7, 2) : Plus(Temp(7, 1), 1)));
    return F;
  };
  for (Variant Kind :
       {Variant::Copies, Variant::ReadElsewhere, Variant::Update}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    HighFunc F = Build(Kind);
    EXPECT_EQ(foldTempsInReturnTails(F.Body), Kind == Variant::Copies);
    if (Kind != Variant::Copies)
      continue;
    size_t Assigns = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Assigns += S.Kind == StmtKind::Assign && S.Dst &&
                 S.Dst->Kind == ExprKind::Var && S.Dst->Var.Id == 7;
    });
    EXPECT_EQ(Assigns, 0u);
    for (uint64_t X = 0; X < 2; ++X)
      EXPECT_EQ(execute(F, X), 6u) << X;
  }
}

TEST(HighControlFlowSemantics, JumpToWhatFollowsATryBecomesAnElseArm) {
  // __try { if (x & 1) { v = 1; goto out; } v = 2; } __except (1) { v = 9; }
  // out: return v;
  // Falling off the protected body reaches out as the jump does, so the
  // rest of the body becomes the else arm.  An __except body continues
  // after the try the same way; a __finally body does not when it runs for
  // an exception, so its jump stays.
  enum class Where { Body, Except, Finally };
  auto Build = [](Where Site) {
    HighStmt Leave;
    Leave.Kind = StmtKind::If;
    Leave.Addr = 0x1000;
    Leave.Cond =
        HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
    Leave.Body = {assign(0x1004, 1, 1), jump(0x1008, 0x1030)};
    std::vector<HighStmt> Jumping = {Leave, assign(0x100c, 1, 2)};
    HighStmt Try;
    Try.Kind = StmtKind::SEHTry;
    Try.Addr = 0x1000;
    Try.EHRange = {0x1000, 0x1010};
    HighEHClause Clause;
    Clause.Kind = Site == Where::Finally ? HighEHClauseKind::SEHFinally
                                         : HighEHClauseKind::SEHExcept;
    Clause.HandlerVA = 0x1020;
    Try.EHClauses = {Clause};
    if (Site == Where::Body) {
      Try.Body = Jumping;
      Try.EHClauseBodies = {{assign(0x1020, 1, 9)}};
    } else {
      Try.Body = {assign(0x0ff8, 1, 5)};
      for (HighStmt &S : Jumping)
        S.Addr += 0x20;
      Jumping.front().Body[0].Addr += 0x20;
      Jumping.front().Body[1].Addr += 0x20;
      Try.EHClauseBodies = {Jumping};
    }
    HighStmt Label;
    Label.Kind = StmtKind::Block;
    Label.Addr = 0x1030;
    HighFunc F;
    F.Entry = 0x0ff8;
    F.Body = {Try, Label, result(0x1034, local(1))};
    return F;
  };
  for (Where Site : {Where::Body, Where::Except, Where::Finally}) {
    SCOPED_TRACE(static_cast<int>(Site));
    HighFunc F = Build(Site);
    std::vector<uint64_t> Before;
    for (uint64_t X : {0, 1})
      Before.push_back(*execute(F, X));
    elseArmsForFallthroughJumps(F);
    for (uint64_t X : {0, 1})
      EXPECT_EQ(execute(F, X), Before[X]) << X;
    const HighStmt &Try = F.Body.front();
    ASSERT_EQ(Try.Kind, StmtKind::SEHTry);
    const std::vector<HighStmt> &Holder =
        Site == Where::Body ? Try.Body : Try.EHClauseBodies.front();
    size_t Jumps = 0;
    walkStmts(Holder,
              [&](const HighStmt &S) { Jumps += S.Kind == StmtKind::Goto; });
    EXPECT_EQ(Jumps, Site == Where::Finally ? 1u : 0u);
  }
}

TEST(HighControlFlowSemantics, TailCopiesStayInTheirTryProtection) {
  // __try { v = 5; goto out; } __except (1) { goto out; } return 0;
  // out: observe(); return v;
  // The handler runs in the protection around the try, so its jump may
  // become a copy of `out`.  The protected body's may not: observe() would
  // then be guarded, and a fault in it would reach the handler.
  auto Build = [] {
    HighStmt Try;
    Try.Kind = StmtKind::SEHTry;
    Try.Addr = 0x1000;
    Try.EHRange = {0x1000, 0x1008};
    Try.Body = {assign(0x1000, 1, 5), jump(0x1004, 0x1020)};
    HighEHClause Clause;
    Clause.Kind = HighEHClauseKind::SEHExcept;
    Clause.HandlerVA = 0x1010;
    Try.EHClauses = {Clause};
    Try.EHClauseBodies = {{jump(0x1010, 0x1020)}};
    HighStmt Observe;
    Observe.Kind = StmtKind::Call;
    Observe.Addr = 0x1020;
    Observe.CallExpr = HighExpr::makeCall("observe", 0x5000, {});
    HighFunc F;
    F.Body = {Try, result(0x1008, HighExpr::makeConst(0, 8)), Observe,
              result(0x1024, local(1))};
    return F;
  };
  HighFunc F = Build();
  EXPECT_TRUE(duplicateSmallReturnTails(F.Body));
  const HighStmt &Try = F.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::SEHTry);
  ASSERT_FALSE(Try.Body.empty());
  EXPECT_EQ(Try.Body.back().Kind, StmtKind::Goto);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  ASSERT_FALSE(Try.EHClauseBodies[0].empty());
  EXPECT_EQ(Try.EHClauseBodies[0].back().Kind, StmtKind::Return);

  // With the handler ending in its own return, the protected body's jump
  // moves after the statement, where the copy may replace it.
  EXPECT_TRUE(hoistTryExitJumps(F.Body));
  ASSERT_EQ(F.Body.front().Body.size(), 1u);
  ASSERT_GE(F.Body.size(), 2u);
  EXPECT_EQ(F.Body[1].Kind, StmtKind::Goto);
  EXPECT_TRUE(duplicateSmallReturnTails(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);

  // A handler that falls through would run the moved jump too.
  F = Build();
  F.Body.front().EHClauseBodies = {{assign(0x1010, 1, 7)}};
  EXPECT_FALSE(hoistTryExitJumps(F.Body));
  EXPECT_EQ(F.Body.front().Body.back().Kind, StmtKind::Goto);
}

TEST(HighControlFlowSemantics, TemporariesReadOnceStayOutOfTheTailLimit) {
  // v = 7; if (c) goto tail; v = 5;
  // tail: t5 = v + 1; t6 = t5 * 3; m[0] = t6; m[1] = 1; m[2] = 2;
  //       m[3] = 3; m[4] = 4; return v;
  // Each temporary prints inside its one read, so by printed size the tail
  // counts as its five stores and the jump becomes a copy of it.  A
  // temporary read twice prints on its own and counts, which makes the tail
  // too long.
  auto Put = [](va_t Address, uint64_t Slot, ExprPtr Value) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = Address;
    S.StoreAddr = HighExpr::makeConst(0x9000 + 8 * Slot, 8);
    S.StoreVal = std::move(Value);
    return S;
  };
  auto Compute = [](va_t Address, int Id, NdOp Op, ExprPtr From, uint64_t N) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.Dst = local(Id);
    S.Val = HighExpr::makeBinop(Op, std::move(From), HighExpr::makeConst(N, 8));
    return S;
  };
  for (bool ReadTwice : {false, true}) {
    SCOPED_TRACE(ReadTwice);
    ExprPtr Stored = local(6);
    if (ReadTwice)
      Stored = HighExpr::makeBinop(NdOp::INT_ADD, local(6), local(6));
    HighFunc F;
    F.Body = {assign(0x1000, 1, 7),
              conditional(0x1004, 0x1020),
              assign(0x1008, 1, 5),
              Compute(0x1020, 5, NdOp::INT_ADD, local(1), 1),
              Compute(0x1024, 6, NdOp::INT_MULT, local(5), 3),
              Put(0x1028, 0, std::move(Stored)),
              Put(0x102c, 1, HighExpr::makeConst(1, 8)),
              Put(0x1030, 2, HighExpr::makeConst(2, 8)),
              Put(0x1034, 3, HighExpr::makeConst(3, 8)),
              Put(0x1038, 4, HighExpr::makeConst(4, 8)),
              result(0x103c, local(1))};
    // By statement count the tail is too long either way.
    EXPECT_FALSE(duplicateSmallReturnTails(F.Body));
    EXPECT_EQ(duplicateSmallReturnTails(F.Body, /*PrintedSize=*/true),
              !ReadTwice);
    EXPECT_EQ(countKind(F, StmtKind::Goto), ReadTwice ? 1u : 0u);
    for (uint64_t X = 0; X < 2; ++X)
      EXPECT_EQ(execute(F, X), std::optional<uint64_t>(X ? 7 : 5)) << X;
  }
}

TEST(HighControlFlowSemantics, AValueReadManyTimesIsNamedOnce) {
  // e0 = x; e(k+1) = e(k) * 3 + (e(k) & 0xff): each level reads the one
  // below twice, so the returned expression prints 2^12 copies of x.
  // Naming the repeated levels keeps every printed expression small, and
  // the result is unchanged.
  auto Typed = [](ExprPtr E) {
    E->Type = NdType::makeInt(8);
    return E;
  };
  ExprPtr Level = local(0);
  Level->Type = NdType::makeInt(8);
  for (unsigned K = 0; K < 12; ++K)
    Level = Typed(HighExpr::makeBinop(
        NdOp::INT_ADD,
        Typed(HighExpr::makeBinop(NdOp::INT_MULT, Level,
                                  HighExpr::makeConst(3, 8))),
        Typed(HighExpr::makeBinop(NdOp::INT_AND, Level,
                                  HighExpr::makeConst(0xff, 8)))));
  HighFunc F;
  F.Body = {result(0x10, Level)};
  const std::vector<uint64_t> Inputs{0, 1, 0xff, 0x1234567, ~uint64_t{0}};
  std::vector<std::optional<uint64_t>> Expected;
  for (uint64_t Input : Inputs)
    Expected.push_back(execute(F, Input));
  EXPECT_TRUE(nameRepeatedValues(F));
  // Printed in full, each statement's expressions stay small.
  std::function<uint64_t(const ExprPtr &)> Printed = [&](const ExprPtr &E) {
    uint64_t N = 1;
    if (E)
      for (const ExprPtr &Operand : E->Operands)
        N = std::min<uint64_t>(N + Printed(Operand), 1u << 20);
    return N;
  };
  unsigned Names = 0;
  for (const HighStmt &S : F.Body) {
    EXPECT_LE(Printed(S.Kind == StmtKind::Return ? S.RetVal : S.Val), 512u);
    Names += S.Kind == StmtKind::Assign && S.KeepsName;
  }
  EXPECT_GT(Names, 0u);
  for (size_t I = 0; I != Inputs.size(); ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Expected[I]) << Inputs[I];
}

TEST(HighControlFlowSemantics, ALaneOfAJoinIsThatLane) {
  // A 16-byte value joined from four 4-byte lanes, read back at bytes 8..11,
  // is the third lane; a comparison of two constants and a selection on a
  // constant fold to what they select.
  auto Int = [](uint16_t Bytes) { return NdType::makeInt(Bytes); };
  auto Lane = [&](uint64_t Factor) {
    auto Times = HighExpr::makeBinop(NdOp::INT_MULT, local(0),
                                     HighExpr::makeConst(Factor, 8));
    Times->Type = Int(8);
    auto Low =
        HighExpr::makeBinop(NdOp::SUBBYTES, Times, HighExpr::makeConst(0, 4));
    Low->Type = Int(4);
    return Low;
  };
  auto Join = [&](ExprPtr High, ExprPtr Low, uint16_t Bytes) {
    auto J = HighExpr::makeBinop(NdOp::CONCAT, std::move(High), std::move(Low));
    J->Type = Int(Bytes);
    return J;
  };
  ExprPtr Vector =
      Join(Join(Lane(7), Lane(5), 8), Join(Lane(3), Lane(2), 8), 16);
  auto Read =
      HighExpr::makeBinop(NdOp::SUBBYTES, Vector, HighExpr::makeConst(8, 4));
  Read->Type = Int(4);
  auto Less = HighExpr::makeBinop(NdOp::INT_LESS, HighExpr::makeConst(30, 4),
                                  HighExpr::makeConst(32, 4));
  Less->Type = Int(1);
  auto Select = std::make_shared<HighExpr>();
  Select->Kind = ExprKind::BinOp;
  Select->Op = NdOp::SELECT;
  Select->Type = Int(4);
  Select->Operands = {Less, Read, HighExpr::makeConst(0, 4)};
  HighFunc F;
  F.Body = {result(0x10, Select)};
  simplifyAllExprs(F.Body);
  const ExprPtr &Root = F.Body[0].RetVal;
  ASSERT_TRUE(Root);
  ASSERT_EQ(Root->Kind, ExprKind::BinOp);
  EXPECT_EQ(Root->Op, NdOp::SUBBYTES);
  ASSERT_EQ(Root->Operands.size(), 2u);
  ASSERT_EQ(Root->Operands[0]->Kind, ExprKind::BinOp);
  EXPECT_EQ(Root->Operands[0]->Op, NdOp::INT_MULT);
  EXPECT_EQ(Root->Operands[0]->Operands[1]->ConstVal, 5u);
}

TEST(HighControlFlowSemantics, PhiCopiesOfOneRegisterShareItsName) {
  // v1 = x; v2 = v1; L: v3 = v2 + 1; v2 = v3; if (v3 < x + 5) goto L;
  // return v3;  -- the PHI copies of one register never overlap the values
  // they copy, so all three versions become one local and the copies go.
  // A temporary copied into the register joins it too, and so does a
  // value of another register.  A version read after the other is
  // redefined, a copy of an unknown value, of a register nothing assigns, or
  // of a frame address stays.
  auto Reg = [](int Tag, unsigned Offset = 0x10) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.RegOff = Offset;
    V.Size = 8;
    V.TheArch = Arch::X64;
    V.RenameTag = static_cast<int16_t>(Tag);
    V.Id = 5000 + Tag;
    return HighExpr::makeVar(V, NdType::makeInt(8));
  };
  auto Input = [] {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Size = 8;
    V.TheArch = Arch::X64;
    return HighExpr::makeVar(V, NdType::makeInt(8));
  };
  auto Set = [](va_t Address, ExprPtr Dst, ExprPtr Val, bool Phi = false) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.Dst = std::move(Dst);
    S.Val = std::move(Val);
    S.IsPhiCopy = Phi;
    return S;
  };
  auto Plus = [](ExprPtr A, uint64_t N) {
    return HighExpr::makeBinop(NdOp::INT_ADD, std::move(A),
                               HighExpr::makeConst(N, 8));
  };
  auto Names = [](const HighFunc &F) {
    std::set<int> Tags;
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &X) {
          if (!X)
            return;
          if (X->Kind == ExprKind::Var && X->Var.Kind == MedVar::Reg &&
              X->Var.RenameTag >= 0)
            Tags.insert(X->Var.RenameTag);
          for (const auto &Operand : X->Operands)
            Visit(Operand);
        };
        Visit(E);
      });
    });
    return Tags;
  };
  {
    HighStmt Test;
    Test.Kind = StmtKind::If;
    Test.Addr = 0x1010;
    Test.Cond = HighExpr::makeBinop(NdOp::INT_LESS, Reg(3), Plus(Input(), 5));
    Test.Body = {jump(0x1014, 0x1008)};
    HighFunc F;
    F.Body = {Set(0x1000, Reg(1), Input()),
              Set(0x1004, Reg(2), Reg(1), true),
              Set(0x1008, Reg(3), Plus(Reg(2), 1)),
              Set(0x100c, Reg(2), Reg(3), true),
              Test,
              result(0x1018, Reg(3))};
    EXPECT_TRUE(coalesceHighPhiCopies(F));
    EXPECT_EQ(Names(F).size(), 1u);
    EXPECT_EQ(countKind(F, StmtKind::Nop), 2u);
    for (uint64_t X = 0; X < 4; ++X)
      EXPECT_EQ(execute(F, X), std::optional<uint64_t>(X + 5)) << X;
  }
  {
    // if (x < 3) { t7 = x + 2; v1 = t7; } else v1 = x; return v1;  -- copy
    // propagation left a temporary in the register's PHI copy; it joins the
    // register's local and takes its name.
    MedVar T;
    T.Kind = MedVar::Temp;
    T.Id = 7;
    T.Size = 8;
    T.TheArch = Arch::X64;
    auto Temp = [&] { return HighExpr::makeVar(T, NdType::makeInt(8)); };
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Addr = 0x1000;
    Branch.Cond =
        HighExpr::makeBinop(NdOp::INT_LESS, Input(), HighExpr::makeConst(3, 8));
    Branch.Body = {Set(0x1004, Temp(), Plus(Input(), 2)),
                   Set(0x1008, Reg(1), Temp(), true)};
    Branch.ElseBody = {Set(0x100c, Reg(1), Input(), true)};
    HighFunc F;
    F.Body = {Branch, result(0x1010, Reg(1))};
    EXPECT_TRUE(coalesceHighPhiCopies(F));
    EXPECT_EQ(countKind(F, StmtKind::Nop), 1u);
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E && E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Temp)
          EXPECT_EQ(E->Var.RenameTag, 1);
      });
    });
    for (uint64_t X = 0; X < 6; ++X)
      EXPECT_EQ(execute(F, X), std::optional<uint64_t>(X < 3 ? X + 2 : X)) << X;
  }
  {
    // v1 = x; v2 = v1; v2 = v2 + 1; return v2;  -- v2 is another register.
    HighFunc F;
    F.Body = {Set(0x1000, Reg(1), Input()),
              Set(0x1004, Reg(2, 0x18), Reg(1), true),
              Set(0x1008, Reg(2, 0x18), Plus(Reg(2, 0x18), 1)),
              result(0x100c, Reg(2, 0x18))};
    EXPECT_TRUE(coalesceHighPhiCopies(F));
    EXPECT_EQ(Names(F).size(), 1u);
    for (uint64_t X = 0; X < 4; ++X)
      EXPECT_EQ(execute(F, X), std::optional<uint64_t>(X + 1)) << X;
  }
  enum class Kept { ReadAfter, Unknown, Unassigned, Frame };
  for (Kept Kind :
       {Kept::ReadAfter, Kept::Unknown, Kept::Unassigned, Kept::Frame}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    // v1 = x; v2 = v1; v2 = v2 + 1; return v1 + v2;
    ExprPtr First = Input();
    if (Kind == Kept::Unknown) {
      First = HighExpr::makeConst(0, 8);
      First->Kind = ExprKind::Undef;
    }
    if (Kind == Kept::Unassigned)
      First = Reg(9);
    HighFunc F;
    if (Kind == Kept::Frame) {
      MedVar Frame;
      Frame.Kind = MedVar::Reg;
      Frame.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
      Frame.Size = 8;
      Frame.TheArch = Arch::X64;
      F.FrameSize = 64;
      First = HighExpr::makeBinop(NdOp::INT_SUB,
                                  HighExpr::makeVar(Frame, NdType::makeInt(8)),
                                  HighExpr::makeConst(16, 8));
    }
    const unsigned Second = 0x10;
    ExprPtr Returned =
        Kind == Kept::ReadAfter
            ? HighExpr::makeBinop(NdOp::INT_ADD, Reg(1), Reg(2, Second))
            : Reg(2, Second);
    F.Body = {Set(0x1000, Reg(1), First),
              Set(0x1004, Reg(2, Second), Reg(1), true),
              Set(0x1008, Reg(2, Second), Plus(Reg(2, Second), 1)),
              result(0x100c, Returned)};
    EXPECT_FALSE(coalesceHighPhiCopies(F));
    EXPECT_EQ(Names(F).size(), Kind == Kept::Unassigned ? 3u : 2u);
    if (Kind == Kept::ReadAfter)
      for (uint64_t X = 0; X < 4; ++X)
        EXPECT_EQ(execute(F, X), std::optional<uint64_t>(2 * X + 1)) << X;
  }
}

TEST(HighControlFlowSemantics, JumpToANoReturnCallBecomesItsCopy) {
  // if (c) goto fail; v = 5; return v; fail: abort(); -- the failing path
  // ends in the call, as a return tail ends in its return.
  HighStmt Fail;
  Fail.Kind = StmtKind::Call;
  Fail.Addr = 0x1010;
  Fail.CallExpr = HighExpr::makeCall("abort", 0x5000, {});
  HighFunc F;
  F.Body = {conditional(0x1000, 0x1010), assign(0x1004, 1, 5),
            result(0x1008, local(1)), Fail};
  EXPECT_TRUE(duplicateSmallReturnTails(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  ASSERT_EQ(F.Body.front().Kind, StmtKind::If);
  ASSERT_EQ(F.Body.front().Body.size(), 1u);
  EXPECT_EQ(F.Body.front().Body[0].Kind, StmtKind::Call);
  EXPECT_EQ(F.Body.front().Body[0].CallExpr->CallTarget, "abort");
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(5));

  // A call that returns keeps the jump: its tail runs into what follows.
  F.Body.back().CallExpr = HighExpr::makeCall("observe", 0x5000, {});
  F.Body.front() = conditional(0x1000, 0x1010);
  EXPECT_FALSE(duplicateSmallReturnTails(F.Body));
}

namespace {
// v = c; while (1) { v = v - 1; if (v & 1) goto out; if (v & 2) break; }
// out: return v; -- optionally with the exit jump inside an inner loop.
HighFunc loopWithExitJump(bool InInnerLoop) {
  auto Copy = assign(0x1000, 1, 0);
  Copy.Val = local(0);
  auto Step = assign(0x1010, 1, 0);
  Step.Val =
      HighExpr::makeBinop(NdOp::INT_SUB, local(1), HighExpr::makeConst(1, 8));
  HighStmt Odd;
  Odd.Kind = StmtKind::If;
  Odd.Addr = 0x1014;
  Odd.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(1), HighExpr::makeConst(1, 8));
  Odd.Body = {jump(0x1018, 0x1030)};
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  HighStmt Two;
  Two.Kind = StmtKind::If;
  Two.Addr = 0x101c;
  Two.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(1), HighExpr::makeConst(2, 8));
  Two.Body = {Break};
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Body = {Step, Odd, Two};
  if (InInnerLoop) {
    HighStmt Inner;
    Inner.Kind = StmtKind::While;
    Inner.Body = {Odd, Break};
    Loop.Body[1] = std::move(Inner);
  }
  HighFunc F;
  F.Body = {Copy, Loop, result(0x1030, local(1))};
  return F;
}
} // namespace

TEST(HighControlFlowSemantics, JumpToTheLoopFollowBecomesABreak) {
  HighFunc F = loopWithExitJump(false);
  const uint64_t Inputs[] = {3, 4, 8};
  const uint64_t Results[] = {2, 3, 7};
  EXPECT_TRUE(breakToTheLoopFollow(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (size_t I = 0; I < 3; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];

  // A break inside an inner loop would leave only that loop.
  F = loopWithExitJump(true);
  EXPECT_FALSE(breakToTheLoopFollow(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 1u);
  for (size_t I = 0; I < 3; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
}

TEST(HighControlFlowSemantics, CodeAfterALoopMovesToItsOnlyBreak) {
  // ... if (v & 2) break; } v = v + 100; out: return v; -- the addition runs
  // only after the break, so it can run there and let `goto out` break.
  HighFunc F = loopWithExitJump(false);
  auto Follow = assign(0x1020, 1, 0);
  Follow.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(100, 8));
  F.Body.insert(F.Body.begin() + 2, Follow);
  const uint64_t Inputs[] = {3, 4, 8};
  const uint64_t Results[] = {102, 3, 7};
  for (size_t I = 0; I < 3; ++I)
    ASSERT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
  EXPECT_TRUE(breakToTheLoopFollow(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  EXPECT_EQ(F.Body.size(), 3u);
  for (size_t I = 0; I < 3; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];

  // A label inside the code after the loop is another way in: it stays.
  F = loopWithExitJump(false);
  Follow.Addr = 0x1024;
  F.Body.insert(F.Body.begin() + 2, Follow);
  F.Body.push_back(jump(0x1040, 0x1024));
  EXPECT_FALSE(breakToTheLoopFollow(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 2u);
}

namespace {
class CalleeSignatures : public DebugContext {
public:
  std::vector<FunctionSym> Functions;

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    for (const FunctionSym &F : Functions)
      if (F.Addr == Addr)
        return F;
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return Functions; }
  bool hasInfo() const override { return true; }
};
} // namespace

TEST(HighControlFlowSemantics, DebugDeclaredNoReturnCalleeKeepsNoreturn) {
  // if (c) KeBugCheckEx(1, 2, 3, 4, 5); return 0; -- the statement writer
  // ends the path at the call, so the declaration from the debug signature
  // must not let C fall through it.
  FunctionSym Callee;
  Callee.Name = "KeBugCheckEx";
  Callee.Addr = 0x5000;
  Callee.ReturnType = NdType::makeInt(4);
  CalleeSignatures Dbg;
  Dbg.Functions = {Callee};
  std::vector<ExprPtr> Arguments;
  for (uint64_t I = 1; I <= 5; ++I)
    Arguments.push_back(HighExpr::makeConst(I, 8));
  HighStmt Check;
  Check.Kind = StmtKind::Call;
  Check.Addr = 0x1004;
  Check.CallExpr = HighExpr::makeCall("KeBugCheckEx", 0x5000, Arguments);
  HighStmt Test;
  Test.Kind = StmtKind::If;
  Test.Addr = 0x1000;
  Test.Cond = local(0);
  Test.Body = {Check};
  HighFunc F;
  F.Name = "checked";
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8);
  F.Body = {Test, result(0x1008, HighExpr::makeConst(0, 8))};
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({F}, Stream, {}, &Dbg));
  Stream.flush();
  const size_t Declaration = Source.find("KeBugCheckEx(");
  ASSERT_NE(Declaration, std::string::npos) << Source;
  const size_t End = Source.find(";\n", Declaration);
  ASSERT_NE(End, std::string::npos) << Source;
  EXPECT_NE(Source.substr(Declaration, End - Declaration)
                .find("__attribute__((noreturn))"),
            std::string::npos)
      << Source;
}

TEST(HighControlFlowSemantics, ProvedNoReturnCalleeIsDeclaredNoreturn) {
  // if (c) bug_check_wrapper(1); return 0; -- no name list knows the callee,
  // but the call carries a proof that it never returns, so the statement
  // writer ends the path there and the declaration must say so too.
  for (bool NoReturn : {false, true}) {
    SCOPED_TRACE(NoReturn);
    HighStmt Check;
    Check.Kind = StmtKind::Call;
    Check.Addr = 0x1004;
    Check.CallExpr = HighExpr::makeCall("bug_check_wrapper", 0x5000,
                                        {HighExpr::makeConst(1, 8)});
    Check.CallExpr->DoesNotReturn = NoReturn;
    HighStmt Test;
    Test.Kind = StmtKind::If;
    Test.Addr = 0x1000;
    Test.Cond = local(0);
    Test.Body = {Check};
    HighFunc F;
    F.Name = "checked";
    F.Entry = 0x1000;
    F.ReturnType = NdType::makeInt(8);
    F.Body = {Test, result(0x1008, HighExpr::makeConst(0, 8))};
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    ASSERT_TRUE(HighCEmitter().emit({F}, Stream, {}));
    Stream.flush();
    const size_t Declaration = Source.find("bug_check_wrapper(");
    ASSERT_NE(Declaration, std::string::npos) << Source;
    const size_t End = Source.find(";\n", Declaration);
    ASSERT_NE(End, std::string::npos) << Source;
    EXPECT_EQ(Source.substr(Declaration, End - Declaration)
                      .find("__attribute__((noreturn))") != std::string::npos,
              NoReturn)
        << Source;
  }
}

TEST(HighControlFlowSemantics, UnreachableCleanupDropsCodeAfterAJump) {
  // v = 1; goto X; v = 2; X: return v; -- nothing enters `v = 2`.
  HighFunc F;
  F.Body = {assign(0x1000, 1, 1), jump(0x1004, 0x1010), assign(0x1008, 1, 2),
            result(0x1010, local(1))};
  removeUnreachableCode(F.Body);
  ASSERT_EQ(F.Body.size(), 3u);
  EXPECT_EQ(F.Body[2].Kind, StmtKind::Return);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(1));

  // A branch into the statement after the jump keeps it.
  auto Enter = conditional(0xffc, 0x1008);
  F.Body = {Enter, assign(0x1000, 1, 1), jump(0x1004, 0x1010),
            assign(0x1008, 1, 2), result(0x1010, local(1))};
  removeUnreachableCode(F.Body);
  EXPECT_EQ(F.Body.size(), 5u);
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(2));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(1));
}

TEST(HighControlFlowSemantics, GuardWithAnEarlyExitStillJoinsTheElseArm) {
  // { if (c & 1) { if (c & 2) { v = 1; goto F; } return 7; } v = 2; }
  // F: return v; -- the early return turns inside out, so the jump ends the
  // outer arm and the join becomes an else arm.
  auto Bit = [](uint64_t Mask) {
    return HighExpr::makeBinop(NdOp::INT_AND, local(0),
                               HighExpr::makeConst(Mask, 8));
  };
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x1004;
  Inner.Cond = Bit(2);
  Inner.Body = {assign(0x1008, 1, 1), jump(0x100c, 0x1020)};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1000;
  Outer.Cond = Bit(1);
  Outer.Body = {Inner, result(0x1010, HighExpr::makeConst(7, 8))};
  HighStmt Region;
  Region.Kind = StmtKind::Block;
  Region.Body = {Outer, assign(0x1018, 1, 2)};
  HighFunc F;
  F.Body = {Region, result(0x1020, local(1))};
  const uint64_t Inputs[] = {0, 1, 2, 3};
  const uint64_t Results[] = {2, 7, 2, 1};
  for (size_t I = 0; I < 4; ++I)
    ASSERT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
  EXPECT_TRUE(reduceSingleUseGotos(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (size_t I = 0; I < 4; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];

  // Another way into the early exit keeps its position.
  F.Body = {Region, result(0x1020, local(1))};
  F.Body.push_back(jump(0x1030, 0x1010));
  reduceSingleUseGotos(F.Body);
  EXPECT_EQ(countKind(F, StmtKind::Goto), 2u);
}

TEST(HighControlFlowSemantics,
     SummarizedCallTakesNoStackSlotsPastItsRegisters) {
  // The callee reads only RCX and no incoming stack slot. A store to the
  // first outgoing stack slot before the call belongs to something else, so
  // it neither becomes a fifth argument nor turns RDX, R8 and R9 into
  // arguments.
  const auto &TRI = getTargetRegInfo(Arch::X64);
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "one_register_callee";
  F.ReturnType = NdType::makeInt(8, false);
  F.CC = CallingConv::Win64;
  MedVar Sp;
  Sp.Kind = MedVar::Reg;
  Sp.TheArch = Arch::X64;
  Sp.Id = 30;
  Sp.SSAVer = 1;
  Sp.Size = 8;
  Sp.RegOff = TRI.StackPointer;
  MedVar Slot;
  Slot.Kind = MedVar::Temp;
  Slot.Id = 40;
  Slot.SSAVer = 1;
  Slot.Size = 8;
  MedVar Result;
  Result.Kind = MedVar::Reg;
  Result.TheArch = Arch::X64;
  Result.Id = 20;
  Result.SSAVer = 1;
  Result.Size = 8;
  Result.RegOff = TRI.IntReturnReg;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  F.Blocks[0].StartAddr = 0x1000;
  F.Blocks[0].EndAddr = 0x1040;
  MedOp Address =
      operation(NdOp::INT_ADD, 0x1000, Slot, {Sp, MedVar::makeConst(0x20, 8)});
  MedOp Store =
      operation(NdOp::STORE, 0x1004, {}, {Slot, MedVar::makeConst(5, 8)});
  MedOp Call =
      operation(NdOp::CALL, 0x1010, Result,
                {MedVar::makeConst(0x3000, 8), MedVar::makeConst(7, 8)});
  Call.CalleeRegisterArgs = 1;
  Call.CalleeStackArgs = 0;
  F.Blocks[0].Ops = {Address, Store, Call,
                     operation(NdOp::RETURN, 0x1020, {}, {Result})};
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  const auto High = Converter.convert(F, Arch::X64);
  const HighExpr *Found = nullptr;
  walkStmts(High.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
        if (N.Kind == ExprKind::Call && N.CallAddr == 0x3000)
          Found = &N;
        N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
      };
      if (E)
        Walk(*E);
    });
  });
  ASSERT_NE(Found, nullptr);
  ASSERT_EQ(Found->Operands.size(), 1u);
  ASSERT_EQ(Found->Operands[0]->Kind, ExprKind::Const);
  EXPECT_EQ(Found->Operands[0]->ConstVal, 7u);
}

TEST(HighControlFlowSemantics,
     SummarizedCallPassesZeroInRegistersItNeverReads) {
  // The callee reads RCX and its sixth argument, so RDX, R8, R9 and the
  // fifth slot keep their positions.  It reads none of those registers:
  // the RDX an earlier block set for something else is not passed as one.
  // Through a dispatcher the count says only which registers the caller
  // set, so the RDX reaching the call is still passed.
  const auto &TRI = getTargetRegInfo(Arch::X64);
  auto Arguments = [&](NdOp Opcode) {
    MedFunc F;
    F.Entry = 0x1000;
    F.Name = "sixth_argument_callee";
    F.ReturnType = NdType::makeInt(8, false);
    F.CC = CallingConv::Win64;
    auto Reg = [&](int Id, uint64_t RegOff) {
      MedVar V;
      V.Kind = MedVar::Reg;
      V.TheArch = Arch::X64;
      V.Id = Id;
      V.SSAVer = 1;
      V.Size = 8;
      V.RegOff = RegOff;
      return V;
    };
    auto Temp = [](int Id) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = Id;
      V.SSAVer = 1;
      V.Size = 8;
      return V;
    };
    const MedVar Sp = Reg(30, TRI.StackPointer);
    const MedVar Rdx = Reg(31, TRI.integerParamRegs(BinaryFormat::COFF)[1]);
    const MedVar Result = Reg(20, TRI.IntReturnReg);
    F.Blocks.resize(2);
    F.Blocks[0].Id = 0;
    F.Blocks[0].StartAddr = 0x1000;
    F.Blocks[0].EndAddr = 0x1004;
    F.Blocks[0].Succs = {1};
    F.Blocks[0].Ops = {
        operation(NdOp::COPY, 0x1000, Rdx, {MedVar::makeConst(9, 8)})};
    F.Blocks[1].Id = 1;
    F.Blocks[1].StartAddr = 0x1004;
    F.Blocks[1].EndAddr = 0x1040;
    F.Blocks[1].Preds = {0};
    MedOp Call = operation(Opcode, 0x1010, Result,
                           {Opcode == NdOp::CALL ? MedVar::makeConst(0x3000, 8)
                                                 : Reg(32, TRI.IntReturnReg),
                            MedVar::makeConst(7, 8)});
    Call.CalleeRegisterArgs = 1;
    Call.CalleeStackArgs = 6;
    F.Blocks[1].Ops = {
        operation(NdOp::INT_ADD, 0x1004, Temp(40),
                  {Sp, MedVar::makeConst(0x20, 8)}),
        operation(NdOp::STORE, 0x1006, {}, {Temp(40), MedVar::makeConst(5, 8)}),
        operation(NdOp::INT_ADD, 0x1008, Temp(41),
                  {Sp, MedVar::makeConst(0x28, 8)}),
        operation(NdOp::STORE, 0x100a, {}, {Temp(41), MedVar::makeConst(6, 8)}),
        Call,
        operation(NdOp::STORE, 0x1014, {}, {Temp(41), Rdx}),
        operation(NdOp::RETURN, 0x1020, {}, {Result})};
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Format = BinaryFormat::COFF;
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Img);
    const auto High = Converter.convert(F, Arch::X64);
    const HighExpr *Found = nullptr;
    walkStmts(High.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
          if (N.Kind == ExprKind::Call)
            Found = &N;
          N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
        };
        if (E)
          Walk(*E);
      });
    });
    EXPECT_NE(Found, nullptr);
    return Found ? std::vector<ExprPtr>(Found->Operands.begin(),
                                        Found->Operands.end())
                 : std::vector<ExprPtr>{};
  };
  const auto Direct = Arguments(NdOp::CALL);
  ASSERT_GE(Direct.size(), 4u);
  for (size_t K = 1; K < 4; ++K) {
    ASSERT_EQ(Direct[K]->Kind, ExprKind::Const) << K;
    EXPECT_EQ(Direct[K]->ConstVal, 0u) << K;
  }
  const auto Dispatched = Arguments(NdOp::INDIR_CALL);
  ASSERT_GE(Dispatched.size(), 2u);
  EXPECT_FALSE(Dispatched[1]->Kind == ExprKind::Const &&
               Dispatched[1]->ConstVal == 0);
}

TEST(HighControlFlowSemantics, RenameCleanupKeepsLabelsOfRemovedStatements) {
  // v = 7; if (c) goto X; v = 3; X: v = v; return v; -- the self copy goes,
  // but the jump still needs its label.
  auto Self = assign(0x1010, 1, 0);
  Self.Val = local(1);
  HighFunc F;
  F.Body = {assign(0x0ffc, 1, 7), conditional(0x1000, 0x1010),
            assign(0x1004, 1, 3), Self, result(0x1014, local(1))};
  postRenameCleanup(F.Body);
  auto Labeled = [&](va_t Addr) {
    bool Found = false;
    walkStmts(F.Body, [&](const HighStmt &S) { Found |= S.Addr == Addr; });
    return Found;
  };
  EXPECT_TRUE(Labeled(0x1010));
  EXPECT_EQ(execute(F, 1, /*RequireExactTargets=*/true),
            std::optional<uint64_t>(7));
  EXPECT_EQ(execute(F, 0, /*RequireExactTargets=*/true),
            std::optional<uint64_t>(3));

  // X: v = 1; v = 2; -- the overwritten assignment goes, its label stays.
  F.Body = {conditional(0x1000, 0x1010), assign(0x1004, 1, 3),
            assign(0x1010, 1, 1), assign(0x1012, 1, 2),
            result(0x1014, local(1))};
  postRenameCleanup(F.Body);
  EXPECT_TRUE(Labeled(0x1010));
  EXPECT_EQ(execute(F, 1, /*RequireExactTargets=*/true),
            std::optional<uint64_t>(2));
}

TEST(HighControlFlowSemantics, ByteWriteIntoUnsetRegisterDropsUnreadBytes) {
  // v = CONCAT(unset upper seven bytes, 1); return (uint8_t)v; -- `mov al,
  // 1` in a function that never set RAX. Only the low byte is read, so the
  // unset bytes are dropped instead of printing as an unknown register.
  auto ByteWrite = [] {
    auto Upper = HighExpr::makeBinop(NdOp::SUBBYTES, HighExpr::makeUndef(8),
                                     HighExpr::makeConst(1, 4));
    Upper->Type = NdType::makeInt(7, false);
    auto Low = HighExpr::makeConst(1, 1);
    auto Joined = HighExpr::makeBinop(NdOp::CONCAT, Upper, Low);
    Joined->Type = NdType::makeInt(8, false);
    auto Write = assign(0x1000, 1, 0);
    Write.Val = Joined;
    return Write;
  };
  auto LowByte =
      HighExpr::makeBinop(NdOp::SUBBYTES, local(1), HighExpr::makeConst(0, 4));
  LowByte->Type = NdType::makeInt(1, false);
  auto HasUndef = [](const HighFunc &F) {
    bool Found = false;
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
          Found |= N.Kind == ExprKind::Undef;
          N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
        };
        if (E)
          Walk(*E);
      });
    });
    return Found;
  };
  HighFunc F;
  F.Body = {ByteWrite(), result(0x1004, LowByte)};
  narrowUnreadRegisterBytes(F);
  EXPECT_FALSE(HasUndef(F));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(1));

  // A full-width read observes the unset bytes, so they stay.
  HighStmt Keep;
  Keep.Kind = StmtKind::Store;
  Keep.Addr = 0x1002;
  Keep.StoreAddr = HighExpr::makeConst(0x9000, 8);
  Keep.StoreVal = local(1);
  F.Body = {ByteWrite(), Keep, result(0x1004, LowByte)};
  narrowUnreadRegisterBytes(F);
  EXPECT_TRUE(HasUndef(F));
}

TEST(HighControlFlowSemantics, ByteReadOfArithmeticDropsUnreadBytes) {
  // v = CONCAT(unset upper bytes, 7); return (uint8_t)(v + 0xFE); -- `and
  // al, 7; lea ecx, [rax-2]; cmp cl, ..`: the low byte of a sum reads only
  // the operands' low bytes, and the low byte of CONCAT(h, l) never reads h.
  auto ByteWrite = [] {
    auto Upper = HighExpr::makeBinop(NdOp::SUBBYTES, HighExpr::makeUndef(8),
                                     HighExpr::makeConst(1, 4));
    Upper->Type = NdType::makeInt(7, false);
    auto Joined =
        HighExpr::makeBinop(NdOp::CONCAT, Upper, HighExpr::makeConst(7, 1));
    Joined->Type = NdType::makeInt(8, false);
    auto Write = assign(0x1000, 1, 0);
    Write.Val = Joined;
    return Write;
  };
  auto LowByteOf = [](ExprPtr Value) {
    auto Low =
        HighExpr::makeBinop(NdOp::SUBBYTES, Value, HighExpr::makeConst(0, 4));
    Low->Type = NdType::makeInt(1, false);
    return Low;
  };
  auto HasUndef = [](const HighFunc &F) {
    bool Found = false;
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
          Found |= N.Kind == ExprKind::Undef;
          N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
        };
        if (E)
          Walk(*E);
      });
    });
    return Found;
  };
  auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, local(1),
                                 HighExpr::makeConst(0xFE, 8));
  Sum->Type = NdType::makeInt(8, false);
  HighFunc F;
  F.Body = {ByteWrite(), result(0x1004, LowByteOf(Sum))};
  narrowUnreadRegisterBytes(F);
  EXPECT_FALSE(HasUndef(F));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(5));

  // The upper bytes of v reach the result only through a high part that
  // the low byte never reads.
  auto High =
      HighExpr::makeBinop(NdOp::SUBBYTES, local(1), HighExpr::makeConst(1, 4));
  High->Type = NdType::makeInt(7, false);
  auto Rejoined = HighExpr::makeBinop(NdOp::CONCAT, High, LowByteOf(local(1)));
  Rejoined->Type = NdType::makeInt(8, false);
  auto Plus =
      HighExpr::makeBinop(NdOp::INT_ADD, Rejoined, HighExpr::makeConst(74, 8));
  Plus->Type = NdType::makeInt(8, false);
  F.Body = {ByteWrite(), result(0x1004, LowByteOf(Plus))};
  narrowUnreadRegisterBytes(F);
  EXPECT_FALSE(HasUndef(F));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(81));

  // So are the upper bytes of a register the function never sets: they
  // would print as an unknown register.
  MedVar Unset;
  Unset.Kind = MedVar::Reg;
  Unset.Id = 9;
  Unset.Size = 8;
  auto UnsetUpper = HighExpr::makeBinop(
      NdOp::SUBBYTES, HighExpr::makeVar(Unset), HighExpr::makeConst(1, 4));
  UnsetUpper->Type = NdType::makeInt(7, false);
  auto FromUnset =
      HighExpr::makeBinop(NdOp::CONCAT, UnsetUpper, HighExpr::makeConst(7, 1));
  FromUnset->Type = NdType::makeInt(8, false);
  auto UnsetWrite = assign(0x1000, 1, 0);
  UnsetWrite.Val = FromUnset;
  F.Body = {UnsetWrite, result(0x1004, LowByteOf(Sum))};
  narrowUnreadRegisterBytes(F);
  bool ReadsUnset = false;
  walkStmts(F.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
        ReadsUnset |= N.Kind == ExprKind::Var && N.Var.Kind == MedVar::Reg;
        N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
      };
      if (E)
        Walk(*E);
    });
  });
  EXPECT_FALSE(ReadsUnset);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(5));

  // The low byte of a right shift reads the bytes above it.
  auto Shifted =
      HighExpr::makeBinop(NdOp::INT_RIGHT, local(1), HighExpr::makeConst(8, 8));
  Shifted->Type = NdType::makeInt(8, false);
  F.Body = {ByteWrite(), result(0x1004, LowByteOf(Shifted))};
  narrowUnreadRegisterBytes(F);
  EXPECT_TRUE(HasUndef(F));
}

TEST(HighControlFlowSemantics, ReadUsedByTheNextStatementMovesIntoIt) {
  // t = *p; S;  -- S reads *p in t's place when S reads all its operands
  // before any effect of its own.
  auto Load = [] {
    auto L = std::make_shared<HighExpr>();
    L->Kind = ExprKind::Load;
    L->Type = NdType::makeInt(8);
    L->Operands = {local(9)};
    return L;
  };
  auto Read = [&] {
    HighStmt S = assign(0x1000, 2, 0);
    S.Val = Load();
    return S;
  };
  auto Call = [](std::vector<ExprPtr> Args) {
    auto C = std::make_shared<HighExpr>();
    C->Kind = ExprKind::Call;
    C->Type = NdType::makeInt(8);
    C->CallAddr = 0x5000;
    C->Operands = std::move(Args);
    return C;
  };
  auto TakesLoad = [](const ExprPtr &E) {
    bool Found = false;
    std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
      Found |= N.Kind == ExprKind::Load;
      N.forEachChildExpr([&](const ExprPtr &C) { Walk(*C); });
    };
    if (E)
      Walk(*E);
    return Found;
  };
  // A store of the value: `*q = *p;`.
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.Addr = 0x1004;
  Store.StoreAddr = local(8);
  Store.StoreVal = local(2);
  HighFunc F;
  F.Body = {Read(), Store, result(0x1008, HighExpr::makeConst(0, 8))};
  EXPECT_TRUE(inlineAdjacentLoads(F));
  ASSERT_EQ(F.Body.size(), 2U);
  EXPECT_TRUE(TakesLoad(F.Body[0].StoreVal));

  // A direct argument of a call whose arguments are pure.
  HighStmt Use = assign(0x1004, 3, 0);
  Use.Val = Call({local(2), HighExpr::makeConst(1, 8)});
  F.Body = {Read(), Use, result(0x1008, local(3))};
  EXPECT_TRUE(inlineAdjacentLoads(F));
  ASSERT_EQ(F.Body.size(), 2U);
  EXPECT_TRUE(TakesLoad(F.Body[0].Val));

  // Beside another call, which may write *p first, the read stays.
  HighStmt Beside = assign(0x1004, 3, 0);
  Beside.Val = HighExpr::makeBinop(NdOp::INT_ADD, Call({}), local(2));
  Beside.Val->Type = NdType::makeInt(8);
  F.Body = {Read(), Beside, result(0x1008, local(3))};
  EXPECT_FALSE(inlineAdjacentLoads(F));

  // A loop condition runs again; the read ran once.
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Addr = 0x1004;
  Loop.Cond = local(2);
  F.Body = {Read(), Loop, result(0x1008, HighExpr::makeConst(0, 8))};
  EXPECT_FALSE(inlineAdjacentLoads(F));
}

TEST(HighControlFlowSemantics, CopyBackJoinsItsDefinition) {
  // t = x + 1; y = 5; x = t; return x + y;  -- x takes x + 1 at once.
  auto Plus = [](int Id, uint64_t Value) {
    auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, local(Id),
                                   HighExpr::makeConst(Value, 8));
    Sum->Type = NdType::makeInt(8);
    return Sum;
  };
  auto Program = [&](HighStmt Between) {
    HighStmt Def = assign(0x1004, 2, 0);
    Def.Val = Plus(1, 1);
    HighStmt Copy = assign(0x100c, 1, 0);
    Copy.Val = local(2);
    HighFunc F;
    F.Body = {
        assign(0x1000, 1, 41), Def, Between, Copy,
        result(0x1010, HighExpr::makeBinop(NdOp::INT_ADD, local(1), local(3)))};
    F.Body.back().RetVal->Type = NdType::makeInt(8);
    return F;
  };
  HighFunc F = Program(assign(0x1008, 3, 5));
  const auto Expected = execute(F, 0);
  EXPECT_TRUE(foldCopiesIntoDefinitions(F));
  EXPECT_EQ(F.Body.size(), 4U);
  EXPECT_EQ(execute(F, 0), Expected);
  EXPECT_EQ(Expected, std::optional<uint64_t>(47));

  // A statement between that reads x keeps the temporary.
  HighStmt ReadsX = assign(0x1008, 3, 0);
  ReadsX.Val = local(1);
  HighFunc Reads = Program(ReadsX);
  EXPECT_FALSE(foldCopiesIntoDefinitions(Reads));

  // So does a label between, which a jump may enter past the definition.
  HighFunc Entered = Program(assign(0x1008, 3, 5));
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x1008;
  Entered.Body.push_back(Jump);
  EXPECT_FALSE(foldCopiesIntoDefinitions(Entered));
}

TEST(HighControlFlowSemantics, ExceptHandlerJumpToAReturnTailBecomesItsCopy) {
  // __try { v = 1; } __except (...) { v = 2; goto R; } v = 3; R: return v;
  // The handler's jump reaches a return tail, which it now returns through.
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = 0x1000;
  Try.Body = {assign(0x1000, 1, 1)};
  HighEHClause Except;
  Except.Kind = HighEHClauseKind::SEHExcept;
  Except.HandlerVA = 0x1040;
  Try.EHClauses = {Except};
  Try.EHClauseBodies = {{assign(0x1040, 1, 2), jump(0x1044, 0x1020)}};
  HighFunc F;
  F.Body = {Try, assign(0x1010, 1, 3), result(0x1020, local(1))};
  EXPECT_TRUE(duplicateSmallReturnTails(F.Body));
  ASSERT_EQ(F.Body.front().EHClauseBodies.size(), 1u);
  const auto &Handler = F.Body.front().EHClauseBodies.front();
  ASSERT_FALSE(Handler.empty());
  EXPECT_EQ(Handler.back().Kind, StmtKind::Return);
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
}

TEST(HighControlFlowSemantics, CodeAfterALoopInAnArmMovesToItsOnlyBreak) {
  // v = c; if (c) { while (1) { ... goto out; ... break; } v = v + 100; }
  // out: return v; -- the jump's target is what follows the arm, which the
  // arm reaches by falling off its end after the addition.
  HighFunc F = loopWithExitJump(false);
  auto Follow = assign(0x1020, 1, 0);
  Follow.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(100, 8));
  HighStmt Arm;
  Arm.Kind = StmtKind::If;
  Arm.Addr = 0x1008;
  Arm.Cond = local(0);
  Arm.Body = {F.Body[1], Follow};
  F.Body = {F.Body[0], Arm, F.Body[2]};
  const uint64_t Inputs[] = {0, 3, 4, 8};
  const uint64_t Results[] = {0, 102, 3, 7};
  for (size_t I = 0; I < 4; ++I)
    ASSERT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
  EXPECT_TRUE(breakToTheLoopFollow(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (size_t I = 0; I < 4; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
}

TEST(HighControlFlowSemantics, BackwardJumpsAroundATryBecomeALoop) {
  // X: __try { v = v - 1; if (v & 1) goto X; } __except (...) { v = 9; }
  // if (v) goto X; return v; -- the jump out of the try body back to X is
  // the loop's continue, exactly as the jump after the try is.
  auto Step = assign(0x1004, 1, 0);
  Step.Val =
      HighExpr::makeBinop(NdOp::INT_SUB, local(1), HighExpr::makeConst(1, 8));
  HighStmt Odd;
  Odd.Kind = StmtKind::If;
  Odd.Addr = 0x1008;
  Odd.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(1), HighExpr::makeConst(1, 8));
  Odd.Body = {jump(0x1008, 0x1000)};
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = 0x1000;
  Try.Body = {Step, Odd};
  HighEHClause Except;
  Except.Kind = HighEHClauseKind::SEHExcept;
  Except.HandlerVA = 0x1040;
  Try.EHClauses = {Except};
  Try.EHClauseBodies = {{assign(0x1040, 1, 9)}};
  HighStmt Again;
  Again.Kind = StmtKind::If;
  Again.Addr = 0x1010;
  Again.Cond = local(1);
  Again.Body = {jump(0x1010, 0x1000)};
  HighFunc F;
  F.Body = {Try, Again, result(0x1014, local(1))};
  EXPECT_TRUE(loopifyBackwardGotos(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  ASSERT_EQ(F.Body.front().Kind, StmtKind::While);
  ASSERT_FALSE(F.Body.front().Body.empty());
  EXPECT_EQ(F.Body.front().Body.front().Kind, StmtKind::SEHTry);

  // A jump back to X out of a __finally body is not a `continue`: leaving a
  // termination handler that way is not the same transfer.
  Try.EHClauses.front().Kind = HighEHClauseKind::SEHFinally;
  Try.Body = {Step};
  Try.EHClauseBodies = {{Odd}};
  HighFunc G;
  G.Body = {Try, Again, result(0x1014, local(1))};
  EXPECT_FALSE(loopifyBackwardGotos(G.Body));
  EXPECT_EQ(countKind(G, StmtKind::Goto), 2u);
}

namespace {
/// A MedIR function from (start address, operations, successors) blocks; the
/// predecessors follow from the successors.
MedFunc treeFunction(
    const std::vector<std::tuple<va_t, std::vector<MedOp>, std::vector<int>>>
        &Blocks) {
  MedFunc M;
  M.Entry = std::get<0>(Blocks.front());
  M.Name = "dispatch_tree";
  M.ReturnType = NdType::makeInt(8, false);
  M.Blocks.resize(Blocks.size());
  for (size_t I = 0; I < Blocks.size(); ++I) {
    MedBlock &B = M.Blocks[I];
    B.Id = static_cast<int>(I);
    B.StartAddr = std::get<0>(Blocks[I]);
    B.EndAddr = B.StartAddr + 0x10;
    B.Ops = std::get<1>(Blocks[I]);
    B.Succs = std::get<2>(Blocks[I]);
  }
  for (const MedBlock &B : M.Blocks)
    for (int Succ : B.Succs)
      M.Blocks[Succ].Preds.push_back(B.Id);
  return M;
}

/// Case values of the switches in \p F.
std::set<uint64_t> switchCaseValues(const HighFunc &F) {
  std::set<uint64_t> Values;
  walkStmts(F.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Switch)
      for (const SwitchCase &C : S.Cases)
        Values.insert(C.Value);
  });
  return Values;
}

/// Parts of a compare-tree test over one 64-bit parameter on x64.
struct TreeParts {
  const Arch Architecture = Arch::X64;
  MedVar Input;
  int Next = 1;
  TreeParts() {
    Input = machineValue(0, Architecture);
    Input.Kind = MedVar::Param;
    Input.RegOff = getTargetRegInfo(Architecture).IntParamRegs[0];
  }
  static MedVar constant(uint64_t V) { return MedVar::makeConst(V, 8); }
  MedVar value() { return machineValue(Next++, Architecture); }
  MedVar flag() {
    MedVar V = value();
    V.Size = 1;
    return V;
  }
  MedVar returnRegister() {
    MedVar R = value();
    R.Kind = MedVar::Reg;
    R.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    R.SSAVer = Next;
    return R;
  }
  /// `return V` at \p At.
  std::vector<MedOp> leaf(va_t At, uint64_t V) {
    const MedVar R = returnRegister();
    return {operation(NdOp::COPY, At, R, {constant(V)}),
            operation(NdOp::RETURN, At + 4, {}, {R})};
  }
  std::vector<MedOp> branch(va_t At, NdOp Compare, MedVar Left, MedVar Right,
                            va_t Target) {
    const MedVar F = flag();
    return {operation(Compare, At, F, {Left, Right}),
            operation(NdOp::COND_BR, At + 4, {}, {constant(Target), F})};
  }
};
} // namespace

TEST(HighControlFlowSemantics, CompareChainLowersToOneSwitch) {
  // sub x, 1; je A; sub x, 1; je B; cmp x, 2; jne D; C: -- the sparse switch
  // `case 1: case 2: case 4: default:` as compilers emit it.
  TreeParts P;
  const MedVar X1 = P.value(), X2 = P.value();
  auto First = P.branch(0x1004, NdOp::INT_EQUAL, X1, P.constant(0), 0x1100);
  First.insert(First.begin(),
               operation(NdOp::INT_SUB, 0x1000, X1, {P.Input, P.constant(1)}));
  auto Second = P.branch(0x1014, NdOp::INT_EQUAL, X2, P.constant(0), 0x1200);
  Second.insert(Second.begin(),
                operation(NdOp::INT_SUB, 0x1010, X2, {X1, P.constant(1)}));
  MedFunc M = treeFunction(
      {{0x1000, First, {1, 4}},
       {0x1010, Second, {2, 5}},
       {0x1020,
        P.branch(0x1020, NdOp::INT_NOTEQUAL, X2, P.constant(2), 0x1400),
        {3, 6}},
       {0x1030, P.leaf(0x1030, 40), {}},
       {0x1100, P.leaf(0x1100, 10), {}},
       {0x1200, P.leaf(0x1200, 20), {}},
       {0x1400, P.leaf(0x1400, 7), {}}});
  M.Params = {P.Input};
  MedToHighConverter Converter;
  const HighFunc High = Converter.convert(M, P.Architecture);
  EXPECT_EQ(countKind(High, StmtKind::Switch), 1u);
  EXPECT_EQ(switchCaseValues(High), (std::set<uint64_t>{1, 2, 4}));
  for (uint64_t X : {0ull, 1ull, 2ull, 3ull, 4ull, 5ull, ~0ull}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(High, X), X == 1   ? 10u
                                : X == 2 ? 20u
                                : X == 4 ? 40u
                                         : 7u);
  }
}

TEST(HighControlFlowSemantics, SignedBinarySearchLowersToOneSwitch) {
  // if (x > 5) { if (x == 9) C; } else { if (x == 5) B; if (x == 2) A; } D:
  // a signed spine; every value outside {2, 5, 9} reaches D, -1 included.
  TreeParts P;
  MedFunc M = treeFunction(
      {{0x1000,
        P.branch(0x1000, NdOp::INT_SLESS, P.constant(5), P.Input, 0x1030),
        {1, 3}},
       {0x1010,
        P.branch(0x1010, NdOp::INT_EQUAL, P.Input, P.constant(5), 0x1100),
        {2, 4}},
       {0x1020,
        P.branch(0x1020, NdOp::INT_EQUAL, P.Input, P.constant(2), 0x1200),
        {7, 5}},
       {0x1030,
        P.branch(0x1030, NdOp::INT_EQUAL, P.Input, P.constant(9), 0x1300),
        {7, 6}},
       {0x1100, P.leaf(0x1100, 2), {}},
       {0x1200, P.leaf(0x1200, 1), {}},
       {0x1300, P.leaf(0x1300, 3), {}},
       {0x1400, P.leaf(0x1400, 0), {}}});
  M.Params = {P.Input};
  MedToHighConverter Converter;
  const HighFunc High = Converter.convert(M, P.Architecture);
  EXPECT_EQ(countKind(High, StmtKind::Switch), 1u);
  EXPECT_EQ(switchCaseValues(High), (std::set<uint64_t>{2, 5, 9}));
  for (uint64_t X :
       {0ull, 2ull, 4ull, 5ull, 6ull, 9ull, 10ull, ~0ull, 1ull << 63}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(High, X), X == 2 ? 1u : X == 5 ? 2u : X == 9 ? 3u : 0u);
  }
}

TEST(HighControlFlowSemantics, CompareTreeEdgesWithDifferentValuesStayApart) {
  // The two edges into D give its PHI different values, so D cannot be one
  // default; no tree is left with three case targets, and nothing changes.
  TreeParts P;
  const MedVar Joined = P.returnRegister();
  MedFunc M = treeFunction(
      {{0x1000,
        P.branch(0x1000, NdOp::INT_SLESS, P.constant(5), P.Input, 0x1030),
        {1, 3}},
       {0x1010,
        P.branch(0x1010, NdOp::INT_EQUAL, P.Input, P.constant(5), 0x1100),
        {2, 4}},
       {0x1020,
        P.branch(0x1020, NdOp::INT_EQUAL, P.Input, P.constant(2), 0x1200),
        {7, 5}},
       {0x1030,
        P.branch(0x1030, NdOp::INT_EQUAL, P.Input, P.constant(9), 0x1300),
        {7, 6}},
       {0x1100, P.leaf(0x1100, 2), {}},
       {0x1200, P.leaf(0x1200, 1), {}},
       {0x1300, P.leaf(0x1300, 3), {}},
       {0x1400, {operation(NdOp::RETURN, 0x1400, {}, {Joined})}, {}}});
  M.Blocks[7].Phis = {{Joined, {{2, P.constant(100)}, {3, P.constant(200)}}}};
  M.Params = {P.Input};
  MedToHighConverter Converter;
  const HighFunc High = Converter.convert(M, P.Architecture);
  EXPECT_EQ(countKind(High, StmtKind::Switch), 0u);
  for (uint64_t X : {0ull, 2ull, 5ull, 6ull, 9ull, ~0ull}) {
    SCOPED_TRACE(X);
    EXPECT_EQ(execute(High, X), X == 2   ? 1u
                                : X == 5 ? 2u
                                : X == 9 ? 3u
                                : X == 6 ? 200u
                                         : 100u);
  }
}

TEST(HighControlFlowSemantics, CodeAfterASwitchMovesToItsOnlyFallOut) {
  // switch (x) { case 1: case 2: case 3: v = 10x; goto J; } v = 7; goto K;
  // J: return v; K: return 99; -- only the missing default falls out of the
  // switch, so `v = 7; goto K;` becomes the default and every goto J breaks.
  for (bool SecondFallOut : {false, true}) {
    SCOPED_TRACE(SecondFallOut);
    HighStmt Dispatch;
    Dispatch.Kind = StmtKind::Switch;
    Dispatch.Addr = 0x1000;
    Dispatch.SwitchExpr = local(0);
    for (uint64_t V : {1, 2, 3})
      Dispatch.Cases.push_back(
          {V,
           {assign(0x1000 + V * 4, 1, V * 10), jump(0x1000 + V * 4, 0x1100)}});
    // A case that falls out too keeps the code where it is.
    if (SecondFallOut)
      Dispatch.Cases.back().Body.pop_back();
    HighFunc F;
    F.Body = {Dispatch, assign(0x1040, 1, 7), jump(0x1044, 0x1200),
              result(0x1100, local(1)),
              result(0x1200, HighExpr::makeConst(99, 8))};
    auto Expected = [&](uint64_t X) -> uint64_t {
      if (X == 3 && SecondFallOut)
        return 99;
      return X >= 1 && X <= 3 ? X * 10 : 99;
    };
    for (uint64_t X : {0, 1, 2, 3, 4})
      ASSERT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
    const size_t Gotos = countKind(F, StmtKind::Goto);
    breakToTheLoopFollow(F.Body);
    EXPECT_EQ(countKind(F, StmtKind::Goto), SecondFallOut ? Gotos : 1u);
    for (uint64_t X : {0, 1, 2, 3, 4})
      EXPECT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
  }
}

TEST(HighControlFlowSemantics, CompareTreeCasesOwningTheirCodeBreakToTheJoin) {
  // switch (x) { case 1: v = 10; case 2: v = 20; case 3: v = 30; default:
  // v = 0; } return v; -- each case block jumps to the join, so the cases
  // end in break.  When the default path may also enter case 2's block, that
  // block is not the case's own and the tree stays branches.
  for (bool Shared : {false, true}) {
    SCOPED_TRACE(Shared);
    TreeParts P;
    const MedVar Reload = P.value();
    const MedVar V[4] = {P.returnRegister(), P.returnRegister(),
                         P.returnRegister(), P.returnRegister()};
    const MedVar Joined = P.returnRegister();
    auto Set = [&](va_t At, int K, uint64_t Value) {
      return std::vector<MedOp>{
          operation(NdOp::COPY, At, V[K], {P.constant(Value)}),
          operation(NdOp::BRANCH, At + 4, {}, {P.constant(0x1400)})};
    };
    auto Root =
        P.branch(0x1004, NdOp::INT_EQUAL, P.Input, P.constant(1), 0x1100);
    Root.insert(Root.begin(), operation(NdOp::STORE, 0x1000, {},
                                        {P.constant(0x9000), P.Input}));
    std::vector<MedOp> Default = Set(0x1030, 0, 0);
    std::vector<int> DefaultSuccs{7};
    if (Shared) {
      // v = 0; if (*p == 7) goto case2; -- a second way into case 2.
      Default =
          P.branch(0x1034, NdOp::INT_EQUAL, Reload, P.constant(7), 0x1200);
      Default.insert(Default.begin(), operation(NdOp::LOAD, 0x1030, Reload,
                                                {P.constant(0x9000)}));
      Default.insert(Default.begin(),
                     operation(NdOp::COPY, 0x1030, V[0], {P.constant(0)}));
      DefaultSuccs = {7, 5};
    }
    MedFunc M = treeFunction(
        {{0x1000, Root, {1, 4}},
         {0x1010,
          P.branch(0x1010, NdOp::INT_EQUAL, P.Input, P.constant(2), 0x1200),
          {2, 5}},
         {0x1020,
          P.branch(0x1020, NdOp::INT_EQUAL, P.Input, P.constant(3), 0x1300),
          {3, 6}},
         {0x1030, Default, DefaultSuccs},
         {0x1100, Set(0x1100, 1, 10), {7}},
         {0x1200, Set(0x1200, 2, 20), {7}},
         {0x1300, Set(0x1300, 3, 30), {7}},
         {0x1400, {operation(NdOp::RETURN, 0x1400, {}, {Joined})}, {}}});
    M.Blocks[7].Phis = {{Joined, {{3, V[0]}, {4, V[1]}, {5, V[2]}, {6, V[3]}}}};
    M.Params = {P.Input};
    EXPECT_EQ(findCompareTreeSwitches(M).size(), Shared ? 0u : 1u);
    MedToHighConverter Converter;
    const HighFunc High = Converter.convert(M, P.Architecture);
    if (!Shared) {
      EXPECT_EQ(countKind(High, StmtKind::Switch), 1u);
      EXPECT_EQ(countKind(High, StmtKind::Goto), 0u);
    }
    for (uint64_t X : {0ull, 1ull, 2ull, 3ull, 4ull, 7ull}) {
      SCOPED_TRACE(X);
      const uint64_t Expected = X >= 1 && X <= 3   ? X * 10
                                : X == 7 && Shared ? 20
                                                   : 0;
      EXPECT_EQ(execute(High, X), Expected);
    }
  }
}

TEST(HighControlFlowSemantics, FrameRegisterReadInACaseKeepsItsDefinition) {
  // rbp = x + 100 before a compare-tree switch whose cases return rbp + k.
  // MSVC uses rbp as an ordinary register: a read inside a case body keeps
  // its definition, which prologue cleanup must not take for a restore.
  TreeParts P;
  MedVar Frame = P.value();
  Frame.Kind = MedVar::Reg;
  Frame.RegOff = getTargetRegInfo(P.Architecture).FramePointer;
  Frame.SSAVer = 1;
  auto Root = P.branch(0x1004, NdOp::INT_EQUAL, P.Input, P.constant(1), 0x1100);
  Root.insert(Root.begin(), operation(NdOp::INT_ADD, 0x1000, Frame,
                                      {P.Input, P.constant(100)}));
  auto Plus = [&](va_t At, uint64_t K) {
    const MedVar R = P.returnRegister();
    return std::vector<MedOp>{
        operation(NdOp::INT_ADD, At, R, {Frame, P.constant(K)}),
        operation(NdOp::RETURN, At + 4, {}, {R})};
  };
  MedFunc M = treeFunction(
      {{0x1000, Root, {1, 4}},
       {0x1010,
        P.branch(0x1010, NdOp::INT_EQUAL, P.Input, P.constant(2), 0x1200),
        {2, 5}},
       {0x1020,
        P.branch(0x1020, NdOp::INT_EQUAL, P.Input, P.constant(3), 0x1300),
        {3, 6}},
       {0x1030, P.leaf(0x1030, 0), {}},
       {0x1100, Plus(0x1100, 1), {}},
       {0x1200, Plus(0x1200, 2), {}},
       {0x1300, Plus(0x1300, 3), {}}});
  M.Params = {P.Input};
  MedToHighConverter Converter;
  const HighFunc High = Converter.convert(M, P.Architecture);
  EXPECT_EQ(countKind(High, StmtKind::Switch), 1u);
  for (uint64_t X : {0ull, 1ull, 2ull, 3ull, 4ull}) {
    SCOPED_TRACE(X);
    EXPECT_NO_THROW(
        EXPECT_EQ(execute(High, X), X >= 1 && X <= 3 ? X + 100 + X : 0u));
  }
}

TEST(HighControlFlowSemantics, LoopEnteredFromOutsideStillBecomesALoop) {
  // v = 10; if (x) goto X; v = 0; X: v = v + 1; if (v < 5) goto X; return v;
  // The jump over `v = 0` lands on X, the first statement of the loop body,
  // which enters the loop as falling into it does; the back edge continues.
  auto Step = assign(0x1010, 1, 0);
  Step.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(1, 8));
  HighStmt Again;
  Again.Kind = StmtKind::If;
  Again.Addr = 0x1014;
  Again.Cond =
      HighExpr::makeBinop(NdOp::INT_LESS, local(1), HighExpr::makeConst(5, 8));
  Again.Body = {jump(0x1014, 0x1010)};
  HighStmt Skip;
  Skip.Kind = StmtKind::If;
  Skip.Addr = 0x1000;
  Skip.Cond = local(0);
  Skip.Body = {jump(0x1000, 0x1010)};
  HighFunc F;
  F.Body = {assign(0x0ffc, 1, 10),   Skip, assign(0x1004, 1, 0), Step, Again,
            result(0x1018, local(1))};
  ASSERT_EQ(execute(F, 0), std::optional<uint64_t>(5));
  ASSERT_EQ(execute(F, 1), std::optional<uint64_t>(11));
  EXPECT_TRUE(loopifyBackwardGotos(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::While), 1u);
  EXPECT_EQ(countKind(F, StmtKind::Goto), 1u);
  // Entered only from outside now, X moves onto the loop statement.
  EXPECT_TRUE(hoistLoopEntryLabels(F.Body));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(5));
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(11));
}

TEST(HighControlFlowSemantics, SmallJumpTailIsCopiedToItsJumps) {
  // if (x == 1) goto Z; if (x == 2) goto Z; v = 5; return v;
  // Z: v = 7; goto Y; Y: return v;  -- nothing falls into Z, so each jump
  // takes a copy of `v = 7; goto Y;` and Z is left unreferenced.
  auto Equals = [](uint64_t K) {
    return HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                               HighExpr::makeConst(K, 8));
  };
  HighStmt First = conditional(0x1000, 0x1020);
  First.Cond = Equals(1);
  HighStmt Second = conditional(0x1004, 0x1020);
  Second.Cond = Equals(2);
  HighFunc F;
  F.Body = {First,
            Second,
            assign(0x1008, 1, 5),
            result(0x100c, local(1)),
            assign(0x1020, 1, 7),
            jump(0x1024, 0x1030),
            result(0x1030, local(1))};
  auto Expected = [](uint64_t X) { return X == 1 || X == 2 ? 7u : 5u; };
  for (uint64_t X : {0, 1, 2, 3})
    ASSERT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
  EXPECT_TRUE(duplicateSmallJumpTails(F.Body));
  unsigned ToZ = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    ToZ += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1020;
  });
  EXPECT_EQ(ToZ, 0u);
  for (uint64_t X : {0, 1, 2, 3})
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
}

TEST(HighControlFlowSemantics, FiveAssignmentReturnTailIsCopied) {
  // if (x) goto T; v1 = 1; return v1; T: v1..v5 = ...; return v1; -- a tail
  // of limits::kMaxReturnTailStatements pure assignments still takes a copy.
  HighFunc F;
  F.Body = {conditional(0x1000, 0x1010), assign(0x1004, 1, 1),
            result(0x1008, local(1))};
  for (int K = 0; K < static_cast<int>(limits::kMaxReturnTailStatements); ++K)
    F.Body.push_back(assign(0x1010 + 4 * K, 1 + K, 10 + K));
  F.Body.push_back(result(0x1040, local(1)));
  ASSERT_EQ(execute(F, 1), std::optional<uint64_t>(10));
  EXPECT_TRUE(duplicateSmallReturnTails(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(10));
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(1));
}

TEST(HighControlFlowSemantics, BlockEndingAnArmMovesToItsOnlyJump) {
  // if (x) { if (x == 1) v = 1; else goto L; }
  // else { v = 5; return v; L: v = 7; }  return v;
  // L's block, reached only by the jump, runs off the end of the else arm
  // into `return v`: it moves to the jump with a jump to that follow.
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x1004;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(1, 8));
  Inner.Body = {assign(0x1008, 1, 1)};
  Inner.ElseBody = {jump(0x100c, 0x1020)};
  HighStmt Outer;
  Outer.Kind = StmtKind::IfElse;
  Outer.Addr = 0x1000;
  Outer.Cond = local(0);
  Outer.Body = {Inner};
  Outer.ElseBody = {assign(0x1010, 1, 5), result(0x1014, local(1)),
                    assign(0x1020, 1, 7)};
  HighFunc F;
  F.Body = {Outer, result(0x1100, local(1))};
  auto Expected = [](uint64_t X) { return X == 0 ? 5u : X == 1 ? 1u : 7u; };
  // The interpreter resolves only top-level targets, so the jump into the
  // else arm runs only once the block has moved.
  for (uint64_t X : {0, 1})
    ASSERT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
  for (int Round = 0; Round < 4 && reduceSingleUseGotos(F.Body, true); ++Round)
    ;
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (uint64_t X : {0, 1, 2})
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
}

TEST(HighControlFlowSemantics, CaseRegionHoldingATryMovesIntoItsCase) {
  // switch (x) { case 1: goto L; } return 8;
  // L: __try { v = 3; } __except (...) { v = 4; } return v;
  // Only the case enters L, so the region moves into it whole: its __try is
  // a complete statement, and the same handler guards it wherever it stands.
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = 0x1020;
  Try.Body = {assign(0x1020, 1, 3)};
  HighEHClause Except;
  Except.Kind = HighEHClauseKind::SEHExcept;
  Except.HandlerVA = 0x1040;
  Try.EHClauses = {Except};
  Try.EHClauseBodies = {{assign(0x1040, 1, 4)}};
  HighStmt Dispatch;
  Dispatch.Kind = StmtKind::Switch;
  Dispatch.Addr = 0x1000;
  Dispatch.SwitchExpr = local(0);
  SwitchCase One;
  One.Value = 1;
  One.Body = {jump(0x1004, 0x1020)};
  Dispatch.Cases = {One};
  HighFunc F;
  F.Body = {Dispatch, result(0x1010, HighExpr::makeConst(8, 8)), Try,
            result(0x1030, local(1))};
  for (uint64_t X : {0, 1})
    ASSERT_EQ(execute(F, X), std::optional<uint64_t>(X == 1 ? 3 : 8));
  for (int Round = 0; Round < 4 && reduceSingleUseGotos(F.Body, true); ++Round)
    ;
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  ASSERT_EQ(F.Body.front().Kind, StmtKind::Switch);
  ASSERT_EQ(F.Body.front().Cases.size(), 1u);
  const auto &Case = F.Body.front().Cases.front().Body;
  EXPECT_TRUE(std::any_of(Case.begin(), Case.end(), [](const HighStmt &S) {
    return S.Kind == StmtKind::SEHTry;
  }));
  for (uint64_t X : {0, 1})
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(X == 1 ? 3 : 8));
}

TEST(HighControlFlowSemantics, ArmThatAlwaysLeavesForTheFollowTakesTheRest) {
  // v = 9; if (x) { v = 0;
  //   if (x & 2) { switch (x) { case 3: v = 1; goto L; default: goto L; } }
  //   v = 2; }
  // L: return v;  -- the switch leaves only for L, so `v = 2` runs only when
  // `x & 2` fails: it becomes the else, and the jumps to L become breaks.
  HighStmt Dispatch;
  Dispatch.Kind = StmtKind::Switch;
  Dispatch.Addr = 0x1010;
  Dispatch.SwitchExpr = local(0);
  SwitchCase Three;
  Three.Value = 3;
  Three.Body = {assign(0x1014, 1, 1), jump(0x1018, 0x1040)};
  Dispatch.Cases = {Three};
  Dispatch.DefaultBody = {jump(0x101c, 0x1040)};
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x1008;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(2, 8));
  Inner.Body = {Dispatch};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1000;
  Outer.Cond = local(0);
  Outer.Body = {assign(0x1004, 1, 0), Inner, assign(0x1020, 1, 2)};
  HighFunc F;
  F.Body = {assign(0x0ff8, 1, 9), Outer, result(0x1040, local(1))};
  const uint64_t Inputs[] = {0, 1, 2, 3, 6};
  const uint64_t Results[] = {9, 2, 0, 1, 0};
  for (size_t I = 0; I < 5; ++I)
    ASSERT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
  for (int Round = 0; Round < 4 && breakToTheLoopFollow(F.Body); ++Round)
    ;
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  ASSERT_EQ(F.Body[1].Body.size(), 2u);
  EXPECT_EQ(F.Body[1].Body[1].Kind, StmtKind::IfElse);
  for (size_t I = 0; I < 5; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
}

TEST(HighControlFlowSemantics, ReturnTailWithAStoreIsCopiedToEachJump) {
  // if (x == 1) goto T; if (x == 2) goto T; return 5;
  // T: *(0x5000) = x + 10; return *(0x5000);  -- each path runs the store
  // once, so each jump can run its own copy of the tail.
  auto Equals = [](va_t Address, uint64_t Value) {
    HighStmt S;
    S.Kind = StmtKind::If;
    S.Addr = Address;
    S.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, local(0),
                                 HighExpr::makeConst(Value, 8));
    S.Body = {jump(Address, 0x1020)};
    return S;
  };
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.Addr = 0x1020;
  Store.StoreAddr = HighExpr::makeConst(0x5000, 8);
  Store.StoreVal =
      HighExpr::makeBinop(NdOp::INT_ADD, local(0), HighExpr::makeConst(10, 8));
  HighFunc F;
  F.Body = {Equals(0x1000, 1), Equals(0x1008, 2),
            result(0x1010, HighExpr::makeConst(5, 8)), Store,
            result(0x1024, HighExpr::makeLoad(HighExpr::makeConst(0x5000, 8),
                                              NdType::makeInt(8)))};
  const uint64_t Inputs[] = {0, 1, 2, 3};
  const uint64_t Results[] = {5, 11, 12, 5};
  for (size_t I = 0; I < 4; ++I)
    ASSERT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
  EXPECT_TRUE(duplicateSmallReturnTails(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (size_t I = 0; I < 4; ++I)
    EXPECT_EQ(execute(F, Inputs[I]), Results[I]) << Inputs[I];
}

TEST(HighControlFlowSemantics, ArmJumpingIntoTheOtherArmsTailSharesIt) {
  // v = 0; if (x & 1) { v = 1; X: v = v + 10; } else { v = 2; goto X; }
  // return v;  -- both arms finish with `v = v + 10`, which then runs after
  // the if/else.
  auto Add = assign(0x1008, 1, 0);
  Add.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(10, 8));
  HighStmt Split;
  Split.Kind = StmtKind::IfElse;
  Split.Addr = 0x1000;
  Split.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Split.Body = {assign(0x1004, 1, 1), Add};
  Split.ElseBody = {assign(0x100c, 1, 2), jump(0x1010, 0x1008)};
  HighFunc F;
  F.Body = {assign(0x0ff8, 1, 0), Split, result(0x1020, local(1))};
  // The interpreter resolves only top-level targets, so the jump into the
  // then arm runs only once the tail has moved.
  ASSERT_EQ(execute(F, 1), std::optional<uint64_t>(11));
  EXPECT_TRUE(hoistSharedArmTails(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  ASSERT_EQ(F.Body.size(), 4u);
  EXPECT_EQ(F.Body[2].Addr, 0x1008u);
  for (uint64_t X : {1, 2, 3, 4})
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(X & 1 ? 11 : 12)) << X;
}

TEST(HighControlFlowSemantics, HoistedTailKeepsTheLabelItsJumpCarried) {
  // v = 0; if (x & 1) { if (x & 2) goto Y; v = 1; X: v = v + 10; }
  // else { Y: goto X; }  return v;  -- the else arm's jump is itself the
  // target of a jump in the then arm, so its label stays behind as an
  // empty anchor.
  auto Add = assign(0x1008, 1, 0);
  Add.Val =
      HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(10, 8));
  HighStmt ToY;
  ToY.Kind = StmtKind::If;
  ToY.Addr = 0x1002;
  ToY.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(2, 8));
  ToY.Body = {jump(0x1002, 0x1010)};
  HighStmt Split;
  Split.Kind = StmtKind::IfElse;
  Split.Addr = 0x1000;
  Split.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Split.Body = {ToY, assign(0x1004, 1, 1), Add};
  Split.ElseBody = {jump(0x1010, 0x1008)};
  HighFunc F;
  F.Body = {assign(0x0ff8, 1, 0), Split, result(0x1020, local(1))};
  EXPECT_TRUE(hoistSharedArmTails(F.Body));
  ASSERT_EQ(F.Body.size(), 4u);
  EXPECT_EQ(F.Body[2].Addr, 0x1008u);
  ASSERT_EQ(F.Body[1].ElseBody.size(), 1u);
  EXPECT_EQ(F.Body[1].ElseBody[0].Kind, StmtKind::Block);
  EXPECT_EQ(F.Body[1].ElseBody[0].Addr, 0x1010u);
  EXPECT_EQ(countKind(F, StmtKind::Goto), 1u);
  // The interpreter resolves only top-level targets: run the paths that do
  // not take the jump into the else arm.
  EXPECT_EQ(execute(F, 1), std::optional<uint64_t>(11));
  EXPECT_EQ(execute(F, 2), std::optional<uint64_t>(10));
}

TEST(HighControlFlowSemantics, LoopTakesEveryBackedgeToItsHeader) {
  // 0x1100 is entered from 0x1000 and closed by two latches: the conditional
  // one at 0x1200 and the jump at 0x1300, which 0x1104 branches to.  Both
  // belong to one loop, so neither stays a jump to its header.
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
  detectAndConvertLoops(F, {}, Med, false);
  size_t Loops = 0, HeaderJumps = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Loops += S.Kind == StmtKind::While;
    HeaderJumps += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1100;
  });
  EXPECT_EQ(Loops, 1u);
  EXPECT_EQ(HeaderJumps, 0u);
  const auto Graph = buildHighSourceFlowGraph(F);
  for (const auto &Item : Graph.Diagnostics.Items)
    ADD_FAILURE() << Item.Reason << " at " << Item.RelatedAddress;
  EXPECT_TRUE(Graph.Diagnostics.Complete);
}

TEST(HighControlFlowSemantics, ElseJumpToALabelInAnotherListStays) {
  // if (x & 1) { } else { if (x & 2) v = 1; else { v = 0; goto L; } w = 2;
  // return w; }  L: return 0;  -- L (0x10c7) lies 9 bytes before `w = 2`
  // (0x10d0) but in the outer list, so the else jump never runs into w.
  auto Bit = [](uint64_t Mask) {
    return HighExpr::makeBinop(NdOp::INT_AND, local(0),
                               HighExpr::makeConst(Mask, 8));
  };
  auto Copy = assign(0x10c5, 1, 0);
  Copy.IsPhiCopy = true;
  HighStmt Inner;
  Inner.Kind = StmtKind::IfElse;
  Inner.Addr = 0x10c5;
  Inner.Cond = Bit(2);
  Inner.Body = {assign(0x10c5, 1, 1)};
  Inner.ElseBody = {Copy, jump(0, 0x10c7)};
  HighStmt Outer;
  Outer.Kind = StmtKind::IfElse;
  Outer.Addr = 0x1080;
  Outer.Cond = Bit(1);
  Outer.Body = {assign(0x1082, 2, 0)};
  Outer.ElseBody = {Inner, assign(0x10d0, 2, 2), result(0x10d4, local(2))};
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x10c7;
  HighFunc F;
  F.Body = {Outer, Label, result(0x10c9, HighExpr::makeConst(0, 8))};
  const uint64_t Expected[] = {0, 0, 2, 0};
  for (uint64_t X : {0u, 1u, 2u, 3u})
    ASSERT_EQ(execute(F, X, true), Expected[X]) << X;
  invertSkipGotos(F);
  for (uint64_t X : {0u, 1u, 2u, 3u})
    EXPECT_EQ(execute(F, X, true), Expected[X]) << X;
}

TEST(HighControlFlowSemantics,
     JoinDefaultStaysWithTheRestOfAnEnteredInstruction) {
  // if (x & 1) { if (x & 2) goto D; v = x + 7; goto J; }
  // D: v = x + 5; w = v;  J: return v;  -- the copy shares the default's
  // instruction, so a default moved into an else arm would leave the jump to
  // D landing on the copy without running the default.
  auto Bit = [](uint64_t Mask) {
    return HighExpr::makeBinop(NdOp::INT_AND, local(0),
                               HighExpr::makeConst(Mask, 8));
  };
  auto Plus = [](uint64_t Addend) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(0),
                               HighExpr::makeConst(Addend, 8));
  };
  HighStmt Enter;
  Enter.Kind = StmtKind::If;
  Enter.Addr = 0x1004;
  Enter.Cond = Bit(2);
  Enter.Body = {jump(0x1004, 0x1020)};
  auto Write = assign(0x1008, 1, 0);
  Write.Val = Plus(7);
  HighStmt Prev;
  Prev.Kind = StmtKind::If;
  Prev.Addr = 0x1000;
  Prev.Cond = Bit(1);
  Prev.Body = {Enter, Write, jump(0x100c, 0x1030)};
  auto Default = assign(0x1020, 1, 0);
  Default.Val = Plus(5);
  auto Copy = assign(0x1020, 2, 0);
  Copy.Val = local(1);
  Copy.IsPhiCopy = true;
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x1030;
  HighFunc F;
  F.Body = {Prev, Default, Copy, Label, result(0x1034, local(1))};
  const uint64_t Expected[] = {5, 8, 7, 8};
  for (uint64_t X : {0u, 1u, 2u, 3u})
    ASSERT_EQ(execute(F, X, true), Expected[X]) << X;
  sinkJoinDefaultsLate(F);
  for (uint64_t X : {0u, 1u, 2u, 3u})
    EXPECT_EQ(execute(F, X, true), Expected[X]) << X;
}

namespace {
/// The statement lists holding a statement at \p Address.
std::set<const std::vector<HighStmt> *>
listsHolding(const std::vector<HighStmt> &Body, va_t Address) {
  std::set<const std::vector<HighStmt> *> Lists;
  std::function<void(const std::vector<HighStmt> &)> Visit =
      [&](const std::vector<HighStmt> &L) {
        for (const HighStmt &S : L) {
          if (S.Addr == Address)
            Lists.insert(&L);
          Visit(S.Body);
          Visit(S.ElseBody);
        }
      };
  Visit(Body);
  return Lists;
}
} // namespace

TEST(HighControlFlowSemantics, SharedArmSuffixKeepsAnEnteredInstructionWhole) {
  // if (x & 4) goto L;  if (x & 1) { w = 1; v = 9; } else { L: u = 2; v = 9; }
  // -- both arms end in v = 9, but the else copy is the second statement of
  // the instruction at L.  Moving it out alone would leave a jump to L able
  // to land after the if/else without running u = 2.
  auto Bit = [](uint64_t Mask) {
    return HighExpr::makeBinop(NdOp::INT_AND, local(0),
                               HighExpr::makeConst(Mask, 8));
  };
  HighStmt Enter;
  Enter.Kind = StmtKind::If;
  Enter.Addr = 0x1000;
  Enter.Cond = Bit(4);
  Enter.Body = {jump(0x1000, 0x1020)};
  auto ThenCopy = assign(0x100c, 1, 9);
  ThenCopy.IsPhiCopy = true;
  auto ElseCopy = assign(0x1020, 1, 9);
  ElseCopy.IsPhiCopy = true;
  HighStmt Split;
  Split.Kind = StmtKind::IfElse;
  Split.Addr = 0x1004;
  Split.Cond = Bit(1);
  Split.Body = {assign(0x1008, 2, 1), ThenCopy};
  Split.ElseBody = {assign(0x1020, 3, 2), ElseCopy};
  HighFunc Structured;
  Structured.Body = {Enter, Split, result(0x1030, local(1))};
  HighFunc Inverted = Structured;
  structureIfElse(Structured, 10);
  EXPECT_EQ(listsHolding(Structured.Body, 0x1020).size(), 1u);
  invertSkipGotos(Inverted);
  EXPECT_EQ(listsHolding(Inverted.Body, 0x1020).size(), 1u);
}

TEST(HighControlFlowSemantics, ArmsWithoutASharedSuffixKeepTheirUndefCopies) {
  // if (x & 1) { a = 1; v = undef; } else { b = 2; }  -- nothing is shared,
  // so the arms stay as they are: the undef copy still gives v its join
  // value on that path.
  HighStmt Pad;
  Pad.Kind = StmtKind::Assign;
  Pad.Addr = 0x1008;
  Pad.IsPhiCopy = true;
  Pad.Dst = local(3);
  Pad.Val = HighExpr::makeUndef(8);
  HighStmt Arms;
  Arms.Kind = StmtKind::IfElse;
  Arms.Addr = 0x1000;
  Arms.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Arms.Body = {assign(0x1004, 1, 1), Pad};
  Arms.ElseBody = {assign(0x1010, 2, 2)};
  HighFunc F;
  F.Body = {Arms, result(0x1020, local(3))};
  structureIfElse(F, 10);
  size_t UndefCopies = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    UndefCopies +=
        S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Undef;
  });
  EXPECT_EQ(UndefCopies, 1u);
}

TEST(HighControlFlowSemantics, MatchingUndefCopiesLeaveTheArmsTogether) {
  // if (x & 1) { a = 1; v = x + 1; w = undef; }
  // else { b = 2; v = x + 1; w = undef; }  -- both copies of the shared
  // suffix move out, the undef one included: every path still gives w its
  // join value.
  auto Pad = [](va_t Address) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.IsPhiCopy = true;
    S.Dst = local(3);
    S.Val = HighExpr::makeUndef(8);
    return S;
  };
  auto Join = [](va_t Address) {
    auto S = assign(Address, 2, 0);
    S.IsPhiCopy = true;
    S.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(0), HighExpr::makeConst(1, 8));
    return S;
  };
  HighStmt Arms;
  Arms.Kind = StmtKind::IfElse;
  Arms.Addr = 0x1000;
  Arms.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Arms.Body = {assign(0x1004, 1, 1), Join(0x1008), Pad(0x1008)};
  Arms.ElseBody = {assign(0x1010, 4, 2), Join(0x1014), Pad(0x1014)};
  HighFunc F;
  F.Body = {Arms, result(0x1020, local(3))};
  structureIfElse(F, 10);
  size_t UndefCopies = 0, JoinCopies = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    UndefCopies +=
        S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Undef;
    JoinCopies +=
        S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::BinOp;
  });
  EXPECT_EQ(UndefCopies, 1u);
  EXPECT_EQ(JoinCopies, 1u);
}

TEST(HighControlFlowSemantics, MatchingUndefCopiesAloneStayInTheirArms) {
  // if (x & 1) { a = 1; w = undef; } else { b = 2; w = undef; }  -- with
  // no value to move along, the undef copies stay where they are.
  auto Pad = [](va_t Address) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Address;
    S.IsPhiCopy = true;
    S.Dst = local(3);
    S.Val = HighExpr::makeUndef(8);
    return S;
  };
  HighStmt Arms;
  Arms.Kind = StmtKind::IfElse;
  Arms.Addr = 0x1000;
  Arms.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Arms.Body = {assign(0x1004, 1, 1), Pad(0x1008)};
  Arms.ElseBody = {assign(0x1010, 4, 2), Pad(0x1014)};
  HighFunc F;
  F.Body = {Arms, result(0x1020, local(3))};
  structureIfElse(F, 10);
  size_t UndefCopies = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    UndefCopies +=
        S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Undef;
  });
  EXPECT_EQ(UndefCopies, 2u);
}

TEST(HighControlFlowSemantics, UndefCopyAheadOfASharedSuffixStays) {
  // if (x & 1) { a = 1; w = undef; v = x + 1; } else { b = 2; v = x + 1; }
  // -- v = x + 1 moves out; the then arm's w = undef runs before it either
  // way, so it stays and w keeps its join value on that path.
  HighStmt Pad;
  Pad.Kind = StmtKind::Assign;
  Pad.Addr = 0x1006;
  Pad.IsPhiCopy = true;
  Pad.Dst = local(3);
  Pad.Val = HighExpr::makeUndef(8);
  auto Join = [](va_t Address) {
    auto S = assign(Address, 2, 0);
    S.IsPhiCopy = true;
    S.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(0), HighExpr::makeConst(1, 8));
    return S;
  };
  HighStmt Arms;
  Arms.Kind = StmtKind::IfElse;
  Arms.Addr = 0x1000;
  Arms.Cond =
      HighExpr::makeBinop(NdOp::INT_AND, local(0), HighExpr::makeConst(1, 8));
  Arms.Body = {assign(0x1004, 1, 1), Pad, Join(0x1008)};
  Arms.ElseBody = {assign(0x1010, 4, 2), Join(0x1014)};
  HighFunc F;
  F.Body = {Arms, result(0x1020, local(2))};
  structureIfElse(F, 10);
  size_t UndefCopies = 0, JoinCopies = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    UndefCopies +=
        S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Undef;
    JoinCopies +=
        S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::BinOp;
  });
  EXPECT_EQ(UndefCopies, 1u);
  EXPECT_EQ(JoinCopies, 1u);
}

TEST(HighControlFlowSemantics, JoinDefaultLeavesAnArmThatJumpsWithinItself) {
  // H: n = n + 1;  if (x & 1) { if (x & 2) goto W; goto L; W: v = n + 7;
  // return v; L: if (n < 3) goto H; }  v = x + 100;  return v;
  // -- the arm's `goto L` lands later in the same arm, so the path through
  // L runs on into the default, and `goto H` is the loop's back edge, not a
  // jump past the default.
  auto Plus = [](int Id, uint64_t Addend) {
    return HighExpr::makeBinop(NdOp::INT_ADD, local(Id),
                               HighExpr::makeConst(Addend, 8));
  };
  auto Bit = [](uint64_t Mask) {
    return HighExpr::makeBinop(NdOp::INT_AND, local(0),
                               HighExpr::makeConst(Mask, 8));
  };
  auto Count = assign(0x1000, 4, 0);
  Count.Val = Plus(4, 1);
  HighStmt Skip;
  Skip.Kind = StmtKind::If;
  Skip.Addr = 0x1008;
  Skip.Cond = Bit(2);
  Skip.Body = {jump(0x1008, 0x1010)};
  auto Write = assign(0x1010, 1, 0);
  Write.Val = Plus(4, 7);
  HighStmt Latch;
  Latch.Kind = StmtKind::Block;
  Latch.Addr = 0x1018;
  HighStmt Back;
  Back.Kind = StmtKind::If;
  Back.Addr = 0x101c;
  Back.Cond =
      HighExpr::makeBinop(NdOp::INT_LESS, local(4), HighExpr::makeConst(3, 8));
  Back.Body = {jump(0x101c, 0x1000)};
  HighStmt Arm;
  Arm.Kind = StmtKind::If;
  Arm.Addr = 0x1004;
  Arm.Cond = Bit(1);
  Arm.Body = {
      Skip, jump(0x100c, 0x1018), Write, result(0x1014, local(1)), Latch, Back};
  auto Default = assign(0x1020, 1, 0);
  Default.Val = Plus(0, 100);
  HighFunc F;
  F.Body = {assign(0x0ff0, 4, 0), Count, Arm, Default,
            result(0x1024, local(1))};
  auto BackEdges = [&] {
    size_t Edges = 0;
    walkStmts(F.Body, [&](const HighStmt &S) {
      Edges += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1000;
    });
    return Edges;
  };
  EXPECT_FALSE(sinkJoinDefaultsLate(F));
  EXPECT_EQ(BackEdges(), 1u);
  EXPECT_EQ(F.Body.size(), 5u);
}

/// Two tests branch to one cold block placed far past the function, which
/// jumps back to the join: `r = 5; if (x & 1 || x & 2) { r = 7; ... }`.  The
/// join and the cold block each do more than a tail copy may repeat.
MedFunc sharedColdBlock() {
  const Arch Architecture = Arch::X64;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "shared_cold_block";
  M.ReturnType = NdType::makeInt(8, false);
  auto Input = machineValue(0, Architecture);
  Input.Kind = MedVar::Param;
  Input.RegOff = TRI.IntParamRegs[0];
  M.Params = {Input};
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  auto Flag = [&](int Id) {
    auto V = machineValue(Id, Architecture);
    V.Size = 1;
    return V;
  };
  auto Result = [&](int Version) {
    auto V = machineValue(9, Architecture);
    V.Kind = MedVar::Reg;
    V.RegOff = TRI.IntReturnReg;
    V.SSAVer = Version;
    return V;
  };
  auto Stores = [&](va_t At, uint64_t Base) {
    std::vector<MedOp> Ops;
    for (unsigned K = 0; K < 6; ++K)
      Ops.push_back(
          operation(NdOp::STORE, At + K * 4, {}, {C(Base + K * 8), C(K)}));
    return Ops;
  };
  const va_t Starts[] = {0x1000, 0x1100, 0x1200, 0x9000};
  M.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    M.Blocks[I].Id = I;
    M.Blocks[I].StartAddr = Starts[I];
    M.Blocks[I].EndAddr = Starts[I] + 0x40;
  }
  auto Low = machineValue(1, Architecture),
       High = machineValue(3, Architecture);
  M.Blocks[0].Succs = {3, 1};
  M.Blocks[0].Ops = {
      operation(NdOp::COPY, 0x1000, Result(1), {C(5)}),
      operation(NdOp::INT_AND, 0x1004, Low, {Input, C(1)}),
      operation(NdOp::INT_NOTEQUAL, 0x1008, Flag(2), {Low, C(0)}),
      operation(NdOp::COND_BR, 0x100c, {}, {C(0x9000), Flag(2)})};
  M.Blocks[1].Preds = {0};
  M.Blocks[1].Succs = {3, 2};
  M.Blocks[1].Ops = {
      operation(NdOp::INT_AND, 0x1100, High, {Input, C(2)}),
      operation(NdOp::INT_NOTEQUAL, 0x1104, Flag(4), {High, C(0)}),
      operation(NdOp::COND_BR, 0x1108, {}, {C(0x9000), Flag(4)})};
  M.Blocks[2].Preds = {1, 3};
  M.Blocks[2].Phis = {{Result(3), {{1, Result(1)}, {3, Result(2)}}}};
  M.Blocks[2].Ops = Stores(0x1200, 0x8000);
  M.Blocks[2].Ops.push_back(operation(NdOp::RETURN, 0x1230, {}, {Result(3)}));
  M.Blocks[3].Preds = {0, 1};
  M.Blocks[3].Succs = {2};
  M.Blocks[3].Ops = {operation(NdOp::COPY, 0x9000, Result(2), {C(7)})};
  for (MedOp &Op : Stores(0x9004, 0x8100))
    M.Blocks[3].Ops.push_back(std::move(Op));
  M.Blocks[3].Ops.push_back(operation(NdOp::BRANCH, 0x9030, {}, {C(0x1200)}));
  return M;
}

TEST(HighControlFlowSemantics, ColdBlockSharedByTwoBranchesRunsBeforeItsJoin) {
  const auto F = MedToHighConverter().convert(sharedColdBlock(), Arch::X64);
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (uint64_t X : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(X);
    EXPECT_NO_THROW(EXPECT_EQ(execute(F, X, true), X ? 7u : 5u));
  }
}

TEST(HighControlFlowSemantics, BlockLayoutFollowsForwardEdgesUnlessUnsure) {
  MedFunc M = sharedColdBlock();
  EXPECT_EQ(highBlockLayout(M, {}), (std::vector<int>{0, 1, 3, 2}));
  // An exception handler's regions follow addresses.
  MedFunc Handled = M;
  Handled.ExceptionMetadata.emplace();
  Handled.ExceptionMetadata->PersonalityVA = 0x5000;
  EXPECT_EQ(highBlockLayout(Handled, {}), (std::vector<int>{0, 1, 2, 3}));
  Handled.ExceptionMetadata->Personality =
      ExceptionPersonality::CSpecificHandler;
  Handled.ExceptionMetadata->SEH.emplace();
  Handled.ExceptionMetadata->SEH->Scopes.push_back({{0x1100, 0x1140}});
  // A frame with only SEH scopes keeps reverse postorder, which leaves the
  // guarded block in one piece; address order when asked for, or when
  // reverse postorder would split a guarded range (the cold block 3 between
  // blocks 1 and 2).
  EXPECT_EQ(highBlockLayout(Handled, {}), (std::vector<int>{0, 1, 3, 2}));
  EXPECT_EQ(highBlockLayout(Handled, {}, /*SEHReversePostorder=*/false),
            (std::vector<int>{0, 1, 2, 3}));
  Handled.ExceptionMetadata->SEH->Scopes.front().GuardedRange = {0x1100,
                                                                 0x1300};
  EXPECT_EQ(highBlockLayout(Handled, {}), (std::vector<int>{0, 1, 2, 3}));
  // A frame that only unwinds has no region to keep: the stack-cookie check,
  // or a personality with no table NeverD reads.
  for (ExceptionPersonality Kind :
       {ExceptionPersonality::GSHandlerCheck, ExceptionPersonality::Unknown}) {
    MedFunc Unwinds = M;
    Unwinds.ExceptionMetadata.emplace();
    Unwinds.ExceptionMetadata->PersonalityVA = 0x5000;
    Unwinds.ExceptionMetadata->Personality = Kind;
    EXPECT_EQ(highBlockLayout(Unwinds, {}), (std::vector<int>{0, 1, 3, 2}));
  }
  // So does a block whose fall-through successor is unknown.
  MedFunc Unknown = M;
  Unknown.Blocks[1].Ops.back().Inputs[0] = MedVar::makeConst(0x7000, 8);
  EXPECT_EQ(highBlockLayout(Unknown, {}), (std::vector<int>{0, 1, 2, 3}));
}

/// An ARM conditional branch lifts as a guard and an effect at one address;
/// MedPredicatedEffects splits the effect into a block numbered last whose
/// entry is the guard's instruction:
/// `r = 5; if (x == 10) goto L; goto J; L: r = 7; J: return r;`.
MedFunc splitPredicatedBranch() {
  const Arch Architecture = Arch::ARM;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc M;
  M.Entry = 0x1000;
  M.Name = "split_predicated_branch";
  M.ReturnType = NdType::makeInt(4, false);
  auto Input = machineValue(0, Architecture);
  Input.Kind = MedVar::Param;
  Input.Size = 4;
  Input.RegOff = TRI.IntParamRegs[0];
  M.Params = {Input};
  auto C = [](uint64_t V) { return MedVar::makeConst(V, 4); };
  auto Flag = machineValue(2, Architecture);
  Flag.Size = 1;
  auto Result = [&](int Version) {
    auto V = machineValue(9, Architecture);
    V.Kind = MedVar::Reg;
    V.RegOff = TRI.IntReturnReg;
    V.Size = 4;
    V.SSAVer = Version;
    return V;
  };
  const va_t Starts[] = {0x1000, 0x1008, 0x1010, 0x1006};
  const va_t Ends[] = {0x1008, 0x1010, 0x1020, 0x1008};
  M.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    M.Blocks[I].Id = I;
    M.Blocks[I].StartAddr = Starts[I];
    M.Blocks[I].EndAddr = Ends[I];
  }
  // The guard instruction tests its predicate and skips the branch effect
  // when the predicate fails.
  M.Blocks[0].Succs = {1, 3};
  M.Blocks[0].Ops = {operation(NdOp::COPY, 0x1000, Result(1), {C(5)}),
                     operation(NdOp::INT_EQUAL, 0x1006, Flag, {Input, C(10)}),
                     operation(NdOp::COND_BR, 0x1006, {}, {C(0x1008), Flag})};
  M.Blocks[1].Preds = {0};
  M.Blocks[1].Succs = {2};
  M.Blocks[1].Ops = {operation(NdOp::COPY, 0x1008, Result(2), {C(7)})};
  M.Blocks[2].Preds = {1, 3};
  M.Blocks[2].Phis = {{Result(3), {{1, Result(2)}, {3, Result(1)}}}};
  M.Blocks[2].Ops = {operation(NdOp::RETURN, 0x1010, {}, {Result(3)})};
  M.Blocks[3].Preds = {0};
  M.Blocks[3].Succs = {2};
  M.Blocks[3].Ops = {operation(NdOp::BRANCH, 0x1006, {}, {C(0x1010)})};
  return M;
}

TEST(HighControlFlowSemantics, SplitInstructionBlockFollowsItsGuard) {
  MedFunc M = splitPredicatedBranch();
  // A table-driven handler keeps address order, so the effect block must
  // still follow its guard: a jump to it would name the guard instruction
  // and run the test again.
  M.ExceptionMetadata.emplace();
  M.ExceptionMetadata->PersonalityVA = 0x5000;
  EXPECT_EQ(highBlockLayout(M, {}, /*SEHReversePostorder=*/false),
            (std::vector<int>{0, 3, 1, 2}));
  const auto F = MedToHighConverter().convert(M, Arch::ARM);
  EXPECT_EQ(countKind(F, StmtKind::DoWhile), 0u);
  EXPECT_EQ(countKind(F, StmtKind::While), 0u);
  for (uint64_t X : {0u, 10u, 11u}) {
    SCOPED_TRACE(X);
    EXPECT_NO_THROW(EXPECT_EQ(execute(F, X, true), X == 10 ? 7u : 5u));
  }
}

TEST(HighControlFlowSemantics, UnreachableCleanupDropsCodeAfterAnEndlessLoop) {
  // while (1) { if (x) return 1; v = 2; }  return v;  -- the loop has no
  // break of its own, so nothing reaches the trailing return.
  HighStmt Exit = conditional(0x1000, 0);
  Exit.Body = {result(0x1000, HighExpr::makeConst(1, 8))};
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Body = {Exit, assign(0x1004, 1, 2)};
  HighFunc F;
  F.Body = {Loop, result(0x1010, local(1))};
  removeUnreachableCode(F.Body);
  ASSERT_EQ(F.Body.size(), 1u);
  EXPECT_EQ(F.Body.front().Kind, StmtKind::While);
  // A break of its own lets the loop fall through: the return stays.
  F.Body.front().Body.push_back(HighStmt());
  F.Body.front().Body.back().Kind = StmtKind::Break;
  F.Body.push_back(result(0x1010, local(1)));
  removeUnreachableCode(F.Body);
  EXPECT_EQ(F.Body.size(), 2u);
}

TEST(HighControlFlowSemantics, NestedExitCopiesTheSmallTailItSkips) {
  // v = 0; if (x) { v = 1; if (x == 2) { v = 5; goto X; } v += 10; }
  // v += 100; X: return v;  -- the exit skips `v += 10` and `v += 100`:
  // `v += 10` moves under `else`, and a copy of `v += 100` goes on each
  // path that does not exit.
  auto Add = [](va_t At, uint64_t K) {
    HighStmt S = assign(At, 1, 0);
    S.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(K, 8));
    return S;
  };
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x1008;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Inner.Body = {assign(0x100c, 1, 5), jump(0x1010, 0x1030)};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1000;
  Outer.Cond = local(0);
  Outer.Body = {assign(0x1004, 1, 1), Inner, Add(0x1014, 10)};
  HighFunc F;
  F.Body = {assign(0x0ffc, 1, 0), Outer, Add(0x1020, 100),
            result(0x1030, local(1))};
  auto Expected = [](uint64_t X) { return X == 0 ? 100u : X == 2 ? 5u : 111u; };
  for (uint64_t X : {0, 1, 2})
    ASSERT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
  for (int Round = 0; Round < 4 && reduceSingleUseGotos(F.Body, true); ++Round)
    ;
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (uint64_t X : {0, 1, 2})
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
}

TEST(HighControlFlowSemantics, ThreeLevelExitCopiesEachSkippedTail) {
  // v = 0; if (x) { v += 1; if (x != 1) { v += 2;
  //   if (x == 3) { v = 50; goto X; } v += 4; } v += 8; }
  // v += 100; X: return v;
  // Each level's other arm keeps running the tails above it and the code
  // before X; only the exit skips them.
  auto Add = [](va_t At, uint64_t K) {
    HighStmt S = assign(At, 1, 0);
    S.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(K, 8));
    return S;
  };
  auto Test = [](va_t At, NdOp Op, uint64_t K, std::vector<HighStmt> Body) {
    HighStmt S;
    S.Kind = StmtKind::If;
    S.Addr = At;
    S.Cond = HighExpr::makeBinop(Op, local(0), HighExpr::makeConst(K, 8));
    S.Body = std::move(Body);
    return S;
  };
  HighStmt Third = Test(0x1010, NdOp::INT_EQUAL, 3,
                        {assign(0x1014, 1, 50), jump(0x1018, 0x1040)});
  HighStmt Second = Test(0x1008, NdOp::INT_NOTEQUAL, 1,
                         {Add(0x100c, 2), Third, Add(0x101c, 4)});
  HighStmt First = Test(0x1000, NdOp::INT_NOTEQUAL, 0,
                        {Add(0x1004, 1), Second, Add(0x1020, 8)});
  HighFunc F;
  F.Body = {assign(0x0ffc, 1, 0), First, Add(0x1030, 100),
            result(0x1040, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t {
    switch (X) {
    case 0:
      return 100;
    case 1:
      return 109;
    case 3:
      return 50;
    default:
      return 115;
    }
  };
  for (uint64_t X : {0, 1, 2, 3, 4})
    ASSERT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
  for (int Round = 0; Round < 4 && reduceSingleUseGotos(F.Body, true); ++Round)
    ;
  EXPECT_EQ(countKind(F, StmtKind::Goto), 0u);
  for (uint64_t X : {0, 1, 2, 3, 4})
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
}

TEST(HighControlFlowSemantics, NestedExitToASharedLabelCopiesItsTail) {
  // v = 0; if (x == 9) goto X; if (x) { if (x == 2) { v = 5; goto X; }
  // v += 10; } v += 100; X: return v;  -- X has another jump; it lands after
  // the copied `v += 100` as before, and only the nested exit goes away.
  auto Add = [](va_t At, uint64_t K) {
    HighStmt S = assign(At, 1, 0);
    S.Val =
        HighExpr::makeBinop(NdOp::INT_ADD, local(1), HighExpr::makeConst(K, 8));
    return S;
  };
  HighStmt Early = conditional(0x0ff8, 0x1030);
  Early.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(9, 8));
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x1008;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(2, 8));
  Inner.Body = {assign(0x100c, 1, 5), jump(0x1010, 0x1030)};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = 0x1000;
  Outer.Cond = local(0);
  Outer.Body = {Inner, Add(0x1014, 10)};
  HighFunc F;
  F.Body = {assign(0x0ff0, 1, 0), Early, Outer, Add(0x1020, 100),
            result(0x1030, local(1))};
  auto Expected = [](uint64_t X) -> uint64_t {
    return X == 9 ? 0 : X == 2 ? 5 : X == 0 ? 100 : 110;
  };
  for (uint64_t X : {0, 1, 2, 9})
    ASSERT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
  for (int Round = 0; Round < 4 && reduceSingleUseGotos(F.Body, true); ++Round)
    ;
  unsigned Exits = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Exits += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1030;
  });
  EXPECT_LE(Exits, 1u);
  for (uint64_t X : {0, 1, 2, 9})
    EXPECT_EQ(execute(F, X), std::optional<uint64_t>(Expected(X)));
}

TEST(HighControlFlowSemantics, PlainBlocksSpliceIntoTheirList) {
  // goto L; { v = 1; L: v = 2; } return v;  -- the block only groups its
  // statements, so they join the enclosing list and the jump stays there.
  HighStmt Group;
  Group.Kind = StmtKind::Block;
  Group.Addr = 0x1008;
  Group.Body = {assign(0x1008, 1, 1), assign(0x1010, 1, 2)};
  HighFunc F;
  F.Body = {jump(0x1000, 0x1010), Group, result(0x1020, local(1))};
  EXPECT_TRUE(flattenBlocks(F.Body));
  EXPECT_EQ(countKind(F, StmtKind::Block), 0u);
  ASSERT_EQ(F.Body.size(), 4u);
  EXPECT_EQ(F.Body[2].Addr, 0x1010u);
  EXPECT_EQ(execute(F, 0), std::optional<uint64_t>(2));
}

TEST(HighControlFlowSemantics, MachineCallReceiptsKeepOneSharedTailOccurrence) {
  // Source evidence authenticates one native occurrence. Mutually exclusive
  // source copies still need a new proof; a tail pass cannot clone a receipt.
  for (unsigned Receipt = 0; Receipt < 11; ++Receipt)
    for (bool Assigned : {false, true})
      for (bool Nested : {false, true})
        for (bool JumpTail : {false, true}) {
          SCOPED_TRACE(Receipt);
          SCOPED_TRACE(Assigned);
          SCOPED_TRACE(Nested);
          SCOPED_TRACE(JumpTail);
          auto Hint = std::make_shared<SourceCallTypeHint>();
          switch (Receipt) {
          case 0:
            Hint->BooleanResult = SourceCallTypeHint::BooleanResultProjection{};
            break;
          case 1:
            Hint->FunctionParameterCall =
                SourceCallTypeHint::FunctionParameterCallEvidence{};
            break;
          case 2:
            Hint->ImmutableNativeCall =
                decltype(Hint->ImmutableNativeCall)::value_type{};
            break;
          case 3:
            Hint->SwiftWitnessFrame =
                decltype(Hint->SwiftWitnessFrame)::value_type{};
            break;
          case 4:
            Hint->Virtual = decltype(Hint->Virtual)::value_type{};
            break;
          case 5:
            Hint->NativeSwiftReceiver =
                decltype(Hint->NativeSwiftReceiver)::value_type{};
            break;
          case 6:
            Hint->SwiftConsumedInput =
                decltype(Hint->SwiftConsumedInput)::value_type{};
            break;
          case 7:
            Hint->SwiftOpaqueValue =
                decltype(Hint->SwiftOpaqueValue)::value_type{};
            break;
          case 10:
            Hint->SwiftValueConstructor =
                decltype(Hint->SwiftValueConstructor)::value_type{};
            break;
          case 9:
            Hint->Signature.Parameters.push_back(
                {"error",
                 NdType::makePtr(NdType::makePtr(NdType::makeVoid()))});
            Hint->Signature.Parameters.back().TheRole =
                SourceParameterTypeHint::Role::SwiftErrorResult;
            break;
          case 8:
            break; // A declaration alone may be copied.
          }
          auto Call = HighExpr::makeCall("callback", 0x2000, {local(0)});
          Call->SourceCallHint = Hint;
          HighStmt Invocation;
          Invocation.Addr = 0x1024;
          if (Assigned) {
            Invocation.Kind = StmtKind::Assign;
            Invocation.Dst = local(1);
            Invocation.Val = Call;
          } else {
            Invocation.Kind = StmtKind::Call;
            Invocation.CallExpr = Call;
          }
          HighStmt Store;
          Store.Kind = StmtKind::Store;
          Store.Addr = 0x1020;
          Store.StoreAddr = HighExpr::makeConst(0x5000, 8);
          Store.StoreVal = local(0);
          std::vector<HighStmt> Tail{Store, Invocation};
          if (Nested) {
            HighStmt Block;
            Block.Kind = StmtKind::Block;
            Block.Addr = 0x1020;
            Block.Body = std::move(Tail);
            Tail = {Block};
          }
          HighFunc F;
          F.Body = {conditional(0x1000, 0x1020), conditional(0x1004, 0x1020),
                    result(0x1008, HighExpr::makeConst(5, 8))};
          F.Body.insert(F.Body.end(), Tail.begin(), Tail.end());
          F.Body.push_back(JumpTail ? jump(0x1028, 0x1040)
                                    : result(0x1028, local(0)));
          if (JumpTail)
            F.Body.push_back(result(0x1040, local(0)));
          if (JumpTail)
            duplicateSmallJumpTails(F.Body);
          else
            duplicateSmallReturnTails(F.Body);
          size_t Evaluations = 0;
          walkStmts(F.Body, [&](const HighStmt &S) {
            forEachExpr(S, [&](const ExprPtr &E) {
              if (E && E->SourceCallHint == Hint)
                ++Evaluations;
            });
          });
          if (Receipt == 8)
            EXPECT_GT(Evaluations, 1U);
          else {
            EXPECT_EQ(Evaluations, 1U);
            EXPECT_GT(countKind(F, StmtKind::Goto), 0U);
          }
        }
}

TEST(HighControlFlowSemantics, NestedExitKeepsOneCallReceiptInSkippedTail) {
  for (bool Bound : {false, true})
    for (bool NestedExpression : {false, true}) {
      SCOPED_TRACE(Bound);
      SCOPED_TRACE(NestedExpression);
      auto Hint = std::make_shared<SourceCallTypeHint>();
      if (Bound)
        Hint->FunctionParameterCall =
            SourceCallTypeHint::FunctionParameterCallEvidence{};
      auto Call = HighExpr::makeCall("callback", 0x2000, {local(0)});
      Call->SourceCallHint = Hint;
      HighStmt Invoke = assign(0x1020, 1, 0);
      Invoke.Val = NestedExpression
                       ? HighExpr::makeBinop(NdOp::INT_ADD, Call,
                                             HighExpr::makeConst(7, 8))
                       : Call;
      HighStmt Early = conditional(0x0ff8, 0x1030);
      HighStmt Inner = conditional(0x1008, 0x1030);
      HighStmt Outer;
      Outer.Kind = StmtKind::If;
      Outer.Addr = 0x1000;
      Outer.Cond = local(0);
      Outer.Body = {Inner, assign(0x1014, 1, 10)};
      HighFunc F;
      F.Body = {assign(0x0ff0, 1, 0), Early, Outer, Invoke,
                result(0x1030, local(1))};
      for (int Round = 0; Round < 4 && reduceSingleUseGotos(F.Body, true);
           ++Round)
        ;
      size_t Evaluations = 0;
      std::function<void(const ExprPtr &)> Count = [&](const ExprPtr &E) {
        if (!E)
          return;
        Evaluations += E->SourceCallHint == Hint;
        E->forEachChildExpr(Count);
      };
      walkStmts(F.Body, [&](const HighStmt &S) { forEachExpr(S, Count); });
      if (Bound)
        EXPECT_EQ(Evaluations, 1U);
      else
        EXPECT_GT(Evaluations, 1U);
    }
}

TEST(HighControlFlowSemantics, ReturnTailCopyKeepsTheOuterLabelOwner) {
  HighFunc F;
  F.Entry = 0x1000;
  F.Name = "nested_label_tail";
  F.ReturnType = NdType::makeInt(8);
  HighStmt Outer;
  Outer.Kind = StmtKind::Block;
  Outer.Addr = 0x1198;
  HighStmt Inner = Outer;
  Inner.Body = {HighStmt{}};
  Inner.Body.front().Kind = StmtKind::Block;
  HighStmt Test;
  Test.Kind = StmtKind::If;
  Test.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(0, 8));
  Test.Body = {assign(0, 1, 5), Inner, jump(0, 0x1300)};
  Outer.Body = {Test, assign(0x1200, 1, 7), jump(0x1204, 0x1300)};
  HighStmt End;
  End.Kind = StmtKind::Block;
  End.Addr = 0x1300;
  F.Body = {conditional(0x1000, 0x1198), Outer, End, result(0x1304, local(1))};
  for (uint64_t Input : {0U, 1U, 7U})
    ASSERT_EQ(execute(F, Input, true), Input ? 7U : 5U);
  duplicateSmallReturnTails(F.Body);
  size_t Entries = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Entries += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1198;
  });
  EXPECT_EQ(Entries, 1U);
  for (uint64_t Input : {0U, 1U, 7U})
    EXPECT_EQ(execute(F, Input, true), Input ? 7U : 5U);
}

TEST(HighControlFlowSemantics, ReturnTailCopyIncludesTheFirstChildOfItsLabel) {
  HighFunc F;
  F.Entry = 0x1000;
  F.Name = "first_child_tail";
  F.ReturnType = NdType::makeInt(8);
  HighStmt Label;
  Label.Kind = StmtKind::Block;
  Label.Addr = 0x1100;
  Label.Body = {assign(0x1100, 1, 23)};
  F.Body = {jump(0x1000, 0x1100), result(0x1004, HighExpr::makeConst(0, 8)),
            Label, result(0x1104, local(1))};
  ASSERT_EQ(execute(F, 0, true), 23U);
  EXPECT_TRUE(duplicateSmallReturnTails(F.Body));
  EXPECT_EQ(F.Body.front().Kind, StmtKind::Block);
  EXPECT_EQ(execute(F, 0, true), 23U);
}

TEST(HighControlFlowSemantics, JumpTailCopyKeepsTheOuterLabelOwner) {
  HighFunc F;
  F.Entry = 0x1000;
  F.Name = "nested_label_jump_tail";
  F.ReturnType = NdType::makeInt(8);
  HighStmt Outer;
  Outer.Kind = StmtKind::Block;
  Outer.Addr = 0x1198;
  HighStmt Inner = Outer;
  Inner.Body = {assign(0, 1, 41)};
  HighStmt Test;
  Test.Kind = StmtKind::If;
  Test.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, local(0), HighExpr::makeConst(0, 8));
  Test.Body = {assign(0, 1, 5), jump(0, 0x1300), Inner, jump(0, 0x1300)};
  Outer.Body = {Test, assign(0x1200, 1, 7), jump(0x1204, 0x1300)};
  HighStmt End;
  End.Kind = StmtKind::Block;
  End.Addr = 0x1300;
  F.Body = {conditional(0x1000, 0x1198), conditional(0x1004, 0x1198), Outer,
            End, result(0x1304, local(1))};
  for (uint64_t Input : {0U, 1U, 7U})
    ASSERT_EQ(execute(F, Input, true), Input ? 7U : 5U);
  duplicateSmallJumpTails(F.Body);
  size_t Entries = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    Entries += S.Kind == StmtKind::Goto && S.GotoTarget == 0x1198;
  });
  EXPECT_EQ(Entries, 2U);
  for (uint64_t Input : {0U, 1U, 7U})
    EXPECT_EQ(execute(F, Input, true), Input ? 7U : 5U);
}
