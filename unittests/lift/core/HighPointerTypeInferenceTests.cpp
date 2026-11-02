//===- HighPointerTypeInferenceTests.cpp - Source pointer types ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source type propagation with frame reuse, clobbers, joins and loop edges.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/CDisplayFieldPath.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighPointerTypeInference.h"

#include "llvm/Support/raw_ostream.h"

namespace {
using namespace neverd;

struct Fixture {
  Arch Architecture;
  uint16_t Bytes;
  TypeRef Integer;
  TypeRef Record;
  TypeRef Pointer;
  HighFunc Function;
  HighPointerTypeFacts Facts;

  explicit Fixture(Arch Architecture = Arch::X64)
      : Architecture(Architecture),
        Bytes(getTargetRegInfo(Architecture).PointerSize),
        Integer(NdType::makeInt(Bytes, false)),
        Record(NdType::makeNamedRecord("Device", 16)),
        Pointer(NdType::makePtr(Record)) {
    Pointer->Size = Bytes;
    Record->FieldDisplayNames = {"flags", "value"};
    Record->FieldDisplayOffsets = {0, 8};
    Record->FieldDisplayTypes = {NdType::makeInt(8, false),
                                 NdType::makeInt(8, false)};
    Function.Name = "transport";
    Function.FrameSize = 64;
    Function.ReturnType = NdType::makeInt(8, false);
    Function.Params = {{"arg0", Pointer}};
  }

  ExprPtr variable(int Id, MedVar::VarKind Kind = MedVar::Temp,
                   TypeRef Type = nullptr) const {
    MedVar Variable;
    Variable.Kind = Kind;
    Variable.Id = Id;
    Variable.Size = Type ? Type->Size : Bytes;
    Variable.TheArch = Architecture;
    return HighExpr::makeVar(Variable, Type ? Type : Integer);
  }

  ExprPtr parameter() const { return variable(0, MedVar::Param, Pointer); }

  ExprPtr slot(unsigned Distance = 16) const {
    auto SP = variable(900, MedVar::Reg);
    SP->Var.RegOff = getTargetRegInfo(Architecture).StackPointer;
    return HighExpr::makeBinop(NdOp::INT_SUB, SP,
                               HighExpr::makeConst(Distance, Bytes));
  }

  HighStmt store(ExprPtr Address, ExprPtr Value) const {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.StoreAddr = std::move(Address);
    S.StoreVal = std::move(Value);
    return S;
  }

  HighStmt assign(int Id, ExprPtr Value) const {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Dst = variable(Id);
    S.Val = std::move(Value);
    return S;
  }

  HighStmt load(int Id = 1) const {
    return assign(Id, HighExpr::makeLoad(slot(), Integer));
  }

  HighStmt call() const {
    HighStmt S;
    S.Kind = StmtKind::Call;
    S.CallExpr = HighExpr::makeCall("touch", 0x1000, {});
    S.CallExpr->Type = NdType::makeVoid();
    return S;
  }

  HighStmt returning(ExprPtr Value = nullptr) const {
    HighStmt S;
    S.Kind = StmtKind::Return;
    S.RetVal = Value ? Value : HighExpr::makeConst(0, 8);
    return S;
  }

  bool run() {
    Facts = inferHighPointerTypes(Function, Architecture);
    return !Facts.empty();
  }

  TypeRef inferred(const ExprPtr &Expression) const {
    const auto Found = Facts.find(highSourceLocalIdentity(Expression->Var));
    return Found == Facts.end() ? Expression->Type : Found->second;
  }
};

TEST(HighPointerTypeInference, CapturedValueSurvivesCallAndFrameSlotReuse) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    Fixture F(Architecture);
    SCOPED_TRACE(int(Architecture));
    F.Function.Body = {F.store(F.slot(), F.parameter()), F.load(), F.call(),
                       F.store(F.slot(), HighExpr::makeConst(17, F.Bytes)),
                       F.returning(F.variable(1))};
    EXPECT_TRUE(F.run());
    EXPECT_EQ(F.inferred(F.Function.Body[1].Dst), F.Pointer);
    EXPECT_EQ(F.Function.Body[1].Dst->Type, F.Integer);
    EXPECT_EQ(F.Function.Body[1].Val->Type, F.Integer);
    EXPECT_EQ(F.inferred(F.Function.Body.back().RetVal), F.Pointer);
    EXPECT_EQ(F.Function.Body[3].StoreVal->Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(F.Function.Body.size(), 5u);
  }
}

TEST(HighPointerTypeInference, MemoryClobbersRevokeStoredType) {
  for (unsigned Kind = 0; Kind < 5; ++Kind) {
    Fixture F;
    SCOPED_TRACE(Kind);
    auto Clobber = F.call();
    if (Kind == 1)
      Clobber = F.store(F.slot(), HighExpr::makeConst(12, F.Bytes));
    if (Kind == 2)
      Clobber = F.store(F.slot(12), HighExpr::makeConst(1, 1));
    if (Kind == 3)
      Clobber = F.store(F.parameter(), HighExpr::makeConst(2, F.Bytes));
    if (Kind == 4) {
      Clobber = F.store(F.slot(), F.parameter());
      Clobber.MemoryOrdering = NdMemoryOrdering::Release;
    }
    F.Function.Body = {F.store(F.slot(), F.parameter()), Clobber, F.load(),
                       F.returning(F.variable(1))};
    EXPECT_FALSE(F.run());
    EXPECT_EQ(F.Function.Body[2].Dst->Type, F.Integer);
  }
}

TEST(HighPointerTypeInference, KnownNonprivateWritesKeepDisjointPrivateFacts) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool AboveEntry : {false, true}) {
      Fixture F(Architecture);
      auto Address = AboveEntry
                         ? HighExpr::makeBinop(NdOp::INT_ADD, F.slot(0),
                                               HighExpr::makeConst(8, F.Bytes))
                         : F.slot(72);
      F.Function.Body = {F.assign(2, F.slot(0)),
                         F.store(F.slot(), F.parameter()),
                         F.store(Address, HighExpr::makeConst(19, F.Bytes)),
                         F.load(), F.returning(F.variable(1))};
      EXPECT_TRUE(F.run());
      EXPECT_EQ(F.inferred(F.Function.Body[3].Dst), F.Pointer);
    }
}

TEST(HighPointerTypeInference, JoinNeedsAgreementFromEveryPredecessor) {
  for (bool Agree : {false, true}) {
    Fixture F;
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = F.variable(0, MedVar::Param, F.Pointer);
    Branch.Body = {F.store(F.slot(), F.parameter())};
    Branch.ElseBody = {F.store(
        F.slot(), Agree ? F.parameter() : HighExpr::makeConst(1, F.Bytes))};
    F.Function.Body = {Branch, F.load(), F.returning(F.variable(1))};
    EXPECT_EQ(F.run(), Agree);
    EXPECT_EQ(F.inferred(F.Function.Body[1].Dst)->Kind,
              Agree ? NdTypeKind::Ptr : NdTypeKind::Int);
  }
}

TEST(HighPointerTypeInference, LoopBackEdgeRevokesFirstIterationType) {
  for (bool Overwrite : {false, true}) {
    Fixture F;
    HighStmt Loop;
    Loop.Kind = StmtKind::While;
    Loop.Cond = F.parameter();
    Loop.Body = {F.load(),
                 F.store(F.slot(), Overwrite ? HighExpr::makeConst(3, F.Bytes)
                                             : F.parameter())};
    F.Function.Body = {F.store(F.slot(), F.parameter()), Loop, F.returning()};
    EXPECT_EQ(F.run(), !Overwrite);
    EXPECT_EQ(F.inferred(F.Function.Body[1].Body[0].Dst)->Kind,
              Overwrite ? NdTypeKind::Int : NdTypeKind::Ptr);
  }
}

TEST(HighPointerTypeInference, ReassignedScalarCannotAcquireOnePointerType) {
  Fixture F;
  F.Function.Body = {F.assign(1, F.parameter()),
                     F.assign(1, HighExpr::makeConst(7, F.Bytes)),
                     F.returning(F.variable(1))};
  EXPECT_FALSE(F.run());
  EXPECT_EQ(F.Function.Body[0].Dst->Type, F.Integer);
}

TEST(HighPointerTypeInference, LateSourceBindingDoesNotMutateMachineTypes) {
  Fixture F;
  F.Function.Params.front().MedIndex = 46;
  F.Function.Params.front().Type = F.Integer;
  F.Function.Body = {F.store(F.slot(), F.variable(0, MedVar::Param)), F.load(),
                     F.returning(F.variable(1))};
  EXPECT_FALSE(F.run());
  F.Facts = inferHighPointerTypes(F.Function, F.Architecture, {F.Pointer});
  EXPECT_EQ(F.inferred(F.Function.Body[1].Dst), F.Pointer);
  EXPECT_EQ(F.Function.Params.front().Type, F.Integer);
  EXPECT_EQ(F.Function.Body[1].Dst->Type, F.Integer);
  EXPECT_EQ(F.Function.Body[1].Val->Type, F.Integer);
}

TEST(HighPointerTypeInference,
     IntegerViewsKeepBitsButOffsetsAndTruncationsDoNot) {
  for (unsigned Kind = 0; Kind < 3; ++Kind) {
    Fixture F;
    auto View = HighExpr::makeBitCast(F.parameter(), F.Integer);
    ExprPtr Value = View;
    if (Kind == 1)
      Value = HighExpr::makeBinop(NdOp::INT_ADD, View,
                                  HighExpr::makeConst(8, F.Bytes));
    if (Kind == 2) {
      Value = HighExpr::makeUnary(
          NdOp::INT_ZEXT,
          HighExpr::makeBitCast(F.parameter(), NdType::makeInt(4)));
      Value->Type = F.Integer;
    }
    F.Function.Body = {F.assign(1, Value), F.returning(F.variable(1))};
    EXPECT_EQ(F.run(), Kind == 0);
  }
}

TEST(HighPointerTypeInference, EscapedLocalAndIncompleteGraphStayUntyped) {
  for (bool Escape : {false, true}) {
    Fixture F;
    HighStmt Other;
    if (Escape) {
      Other = F.call();
      auto Address = std::make_shared<HighExpr>();
      Address->Kind = ExprKind::Addr;
      Address->Type = NdType::makePtr(F.Integer);
      Address->Operands = {F.variable(1)};
      Other.CallExpr->Operands = {Address};
    } else {
      Other.Kind = StmtKind::Goto;
      Other.GotoTarget = 0xBAD;
    }
    F.Function.Body = {F.assign(1, F.parameter()), Other,
                       F.returning(F.variable(1))};
    EXPECT_FALSE(F.run());
    EXPECT_EQ(F.Function.Body.front().Dst->Type, F.Integer);
  }
}

TEST(HighPointerTypeInference, DifferentRecordTypesDoNotMergeAtJoin) {
  Fixture F;
  auto Other = NdType::makePtr(NdType::makeNamedRecord("Other", 16));
  F.Function.Params.push_back({"arg1", Other});
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = F.parameter();
  Branch.Body = {F.assign(1, F.parameter())};
  Branch.ElseBody = {F.assign(1, F.variable(1, MedVar::Param, Other))};
  F.Function.Body = {Branch, F.returning(F.variable(1))};
  EXPECT_FALSE(F.run());
}

TEST(HighPointerTypeInference, DisplayPathsShareValidatedConstantIndices) {
  EXPECT_EQ(canonicalizeDisplayPath("slots[1]"), "slots[1]");
  EXPECT_EQ(canonicalizeDisplayPath("rows[2][0].member"), "rows[2][0].member");
  for (const char *Path : {"slots[i]", "slots[-1]", "slots[1] = 2", "a->b",
                           "slots[18446744073709551616]"})
    EXPECT_FALSE(canonicalizeDisplayPath(Path)) << Path;
}

TEST(HighPointerTypeInference, PartialValueAndOutsidePrivateFrameAreUnknown) {
  for (unsigned Kind = 0; Kind < 3; ++Kind) {
    Fixture F;
    auto Save = F.store(F.slot(), F.parameter());
    auto Load = F.load();
    if (Kind == 0)
      Load.Val->Type = NdType::makeInt(4, false);
    if (Kind == 1)
      Save.StoreAddr = Load.Val->Operands[0] = F.slot(72);
    if (Kind == 2)
      Save.StoreAddr = Load.Val->Operands[0] = F.slot(0);
    F.Function.Body = {Save, Load, F.returning(F.variable(1))};
    EXPECT_FALSE(F.run());
  }
}

TEST(HighPointerTypeInference, FollowsFrameAliasesButNotChangedAliases) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool Change : {false, true}) {
      Fixture F(Architecture);
      F.Function.Body = {F.assign(2, F.slot()),
                         F.store(F.variable(2), F.parameter())};
      if (Change)
        F.Function.Body.push_back(F.assign(2, F.slot(32)));
      F.Function.Body.push_back(
          F.assign(1, HighExpr::makeLoad(F.variable(2), F.Integer)));
      F.Function.Body.push_back(F.returning(F.variable(1)));
      EXPECT_EQ(F.run(), !Change);
    }
}

TEST(HighPointerTypeInference, TypedMemberRenderingPreservesByteAddressing) {
  Fixture F;
  F.Function.Body = {
      F.store(F.slot(), F.parameter()),
      F.load(),
      F.call(),
      F.store(F.slot(), HighExpr::makeConst(19, F.Bytes)),
      F.store(HighExpr::makeBinop(NdOp::INT_ADD, F.variable(1),
                                  HighExpr::makeConst(8, F.Bytes)),
              HighExpr::makeConst(42, 8)),
      F.returning(HighExpr::makeLoad(
          HighExpr::makeBinop(NdOp::INT_ADD, F.variable(1),
                              HighExpr::makeConst(8, F.Bytes)),
          NdType::makeInt(8, false)))};
  ASSERT_TRUE(F.run());
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.EmitComments = false;
  Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
    CRecordDefinitions Definitions;
    Definitions.Text = "struct Device { uint64_t flags; uint64_t value; };\n";
    Definitions.Records["Device"] = false;
    return Definitions;
  };
  ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options));
  EXPECT_NE(Text.find("->value"), std::string::npos) << Text;
  source_call_execution_test::compileAndRun(
      Text +
          "\nint touch(void) { return 0; }\n"
          "int main(void) { struct Device d = {11, 12}; "
          "return transport(&d) != 42 || d.flags != 11 || d.value != 42; }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(HighPointerTypeInference, DeclarationAndAssignmentsSpellOnePointer) {
  // A call's pointer parameter, or the assignment's own machine pointer type,
  // names another pointee than the transported Device. The local's
  // declaration, its assignments and the call must then agree in C.
  class Callee : public NullDebugContext {
  public:
    TypeRef Other = NdType::makeNamedRecord("Other", 16);
    Callee() {
      Other->FieldDisplayNames = {"tag", "payload"};
      Other->FieldDisplayOffsets = {0, 8};
      Other->FieldDisplayTypes = {NdType::makeInt(8, false),
                                  NdType::makeInt(8, false)};
    }
    bool hasInfo() const override { return true; }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x3000)
        return std::nullopt;
      FunctionSym F;
      F.Name = "consume";
      F.Addr = Address;
      F.ReturnType = NdType::makeVoid();
      F.Params = {{"other", NdType::makePtr(Other)}};
      return F;
    }
  } Debug;
  for (bool MachinePointer : {false, true}) {
    SCOPED_TRACE(MachinePointer);
    Fixture F;
    auto Load = F.load();
    auto Consume = F.call();
    if (MachinePointer) {
      Load.Dst->Type = NdType::makePtr(NdType::makeInt(4));
    } else {
      Consume.CallExpr = HighExpr::makeCall("consume", 0x3000, {F.variable(1)});
      Consume.CallExpr->Type = NdType::makeVoid();
    }
    const auto Member = HighExpr::makeBinop(NdOp::INT_ADD, F.variable(1),
                                            HighExpr::makeConst(8, F.Bytes));
    F.Function.Body = {
        F.store(F.slot(), F.parameter()),
        Load,
        F.call(),
        F.store(F.slot(), HighExpr::makeConst(19, F.Bytes)),
        Consume,
        F.store(Member, HighExpr::makeConst(42, 8)),
        F.returning(HighExpr::makeLoad(Member, NdType::makeInt(8, false)))};
    ASSERT_TRUE(F.run());
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    CEmitterOptions Options;
    Options.EmitComments = false;
    Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
      CRecordDefinitions Definitions;
      Definitions.Text = "struct Device { uint64_t flags; uint64_t value; };\n"
                         "struct Other { uint64_t tag; uint64_t payload; };\n";
      Definitions.Records["Device"] = false;
      Definitions.Records["Other"] = false;
      return Definitions;
    };
    ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options, &Debug));
    source_call_execution_test::compileAndRun(
        Text + "\nint touch(void) { return 0; }\n" +
            (MachinePointer ? ""
                            : "void consume(struct Other *other) "
                              "{ (void)other; }\n") +
            "int main(void) { struct Device d = {11, 12}; "
            "return transport(&d) != 42 || d.flags != 11 || "
            "d.value != 42; }\n",
        {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
  }
}

TEST(HighPointerTypeInference, NonmemberOffsetsRemainByteOffsets) {
  Fixture F;
  auto Address = HighExpr::makeBinop(NdOp::INT_ADD, F.variable(1),
                                     HighExpr::makeConst(24, F.Bytes));
  F.Function.Body = {
      F.assign(1, F.parameter()), F.store(Address, HighExpr::makeConst(42, 8)),
      F.returning(HighExpr::makeLoad(Address, NdType::makeInt(8, false)))};
  ASSERT_TRUE(F.run());
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.EmitComments = false;
  Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
    CRecordDefinitions Definitions;
    Definitions.Text = "struct Device { uint64_t flags; uint64_t value; };\n";
    Definitions.Records["Device"] = false;
    return Definitions;
  };
  ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options));
  source_call_execution_test::compileAndRun(
      Text + "\nint main(void) { struct Device d[2] = {{11, 12}, {21, 22}}; "
             "return transport(d) != 42 || d[0].flags != 11 || "
             "d[0].value != 12 || d[1].flags != 21 || d[1].value != 42; }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}
} // namespace
