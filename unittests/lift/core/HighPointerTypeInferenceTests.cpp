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
#include "neverd/ir/high/AArch64AddressMemory.h"
#include "neverd/ir/high/HighPointerTypeInference.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/raw_ostream.h"

namespace {
using namespace neverd;

constexpr Arch PointerArchitectures[] = {
    Arch::X86,       Arch::X64,        Arch::ARM,    Arch::AArch64,
    Arch::MIPS32,    Arch::MIPS32EL,   Arch::MIPS64, Arch::MIPS64EL,
    Arch::MIPS64N32, Arch::MIPS64N32EL};

struct Fixture {
  Arch Architecture;
  uint16_t Bytes;
  TypeRef Integer;
  TypeRef AddressInteger;
  TypeRef Record;
  TypeRef Pointer;
  HighFunc Function;
  HighPointerTypeFacts Facts;

  explicit Fixture(Arch Architecture = Arch::X64)
      : Architecture(Architecture),
        Bytes(getTargetRegInfo(Architecture).PointerSize),
        Integer(NdType::makeInt(Bytes, false)),
        AddressInteger(NdType::makeInt(
            getTargetRegInfo(Architecture).addressRegisterBytes(), false)),
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
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto AddressBytes = TRI.addressRegisterBytes();
    auto SP = variable(900, MedVar::Reg, NdType::makeInt(AddressBytes, false));
    SP->Var.RegOff = TRI.StackPointer;
    return HighExpr::makeBinop(NdOp::INT_SUB, SP,
                               HighExpr::makeConst(Distance, AddressBytes));
  }

  HighStmt store(ExprPtr Address, ExprPtr Value) const {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.StoreAddr = std::move(Address);
    S.StoreVal = std::move(Value);
    return S;
  }

  HighStmt assign(int Id, ExprPtr Value, TypeRef Carrier = nullptr) const {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Dst = variable(Id, MedVar::Temp, Carrier);
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

TEST(HighPointerTypeInference,
     ReadOnlyIndexedMemoryDoesNotEscapePrivateSpills) {
  Fixture F(Arch::AArch64);
  auto Read = HighExpr::makeCall("indexed_read", 0,
                                 {F.slot(32), HighExpr::makeConst(0, 8),
                                  HighExpr::makeConst(0xf87f6800, 4)});
  Read->IntrinsicId = Intrinsic::A64_Load64RegisterOffset;
  Read->Type = F.Integer;
  ASSERT_TRUE(aarch64AddressMemoryHighShapeIsValid(*Read));
  F.Function.Body = {F.store(F.slot(), F.parameter()), F.assign(2, Read),
                     F.call(), F.load(), F.returning(F.variable(1))};
  ASSERT_TRUE(F.run());
  EXPECT_EQ(F.inferred(F.Function.Body[3].Dst), F.Pointer);

  // The exception is a checked read contract, not an intrinsic-name heuristic.
  Read->Operands.back() = HighExpr::makeConst(0, 4);
  EXPECT_FALSE(F.run());
  Read->Operands.back() = HighExpr::makeConst(0xf87f6800, 4);
  Read->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  EXPECT_FALSE(F.run());
}

TEST(HighPointerTypeInference, PointerUsesAndMembersSurviveLaterCarrierReuse) {
  Fixture F;
  auto Child = NdType::makeNamedRecord("Child", 16);
  auto ChildPointer = NdType::makePtr(Child);
  F.Record->FieldDisplayTypes[1] = ChildPointer;
  auto Call = HighExpr::makeCall("produce", 0x2000, {});
  Call->Type = F.Integer;
  const auto Use = F.variable(1);
  const auto Member = HighExpr::makeLoad(
      HighExpr::makeBinop(NdOp::INT_ADD, Use, HighExpr::makeConst(8, F.Bytes)),
      F.Integer);
  F.Function.Body = {F.assign(1, Call), F.assign(2, Member),
                     F.assign(1, HighExpr::makeConst(42, F.Bytes)),
                     F.returning(F.variable(2))};
  HighPointerUseLocations Uses;
  const HighPointerTypeOrigins Origins{{Call.get(), F.Pointer}};
  const auto Whole =
      inferHighPointerLocations(F.Function, F.Architecture, {}, Origins, &Uses);
  EXPECT_FALSE(Whole.count(highSourceLocalIdentity(Use->Var)));
  ASSERT_TRUE(Uses.count(Use.get()));
  EXPECT_EQ(Uses.at(Use.get()).Type, F.Pointer);
  ASSERT_TRUE(Whole.count(highSourceLocalIdentity(F.variable(2)->Var)));
  EXPECT_EQ(Whole.at(highSourceLocalIdentity(F.variable(2)->Var)).Type,
            ChildPointer);
  // An expression DAG shared between two program points must satisfy both.
  F.Function.Body.back().RetVal = Use;
  inferHighPointerLocations(F.Function, F.Architecture, {}, Origins, &Uses);
  EXPECT_FALSE(Uses.count(Use.get()));
}

TEST(HighPointerTypeInference, ScopedDerivedMemberChainKeepsTheOtherBranch) {
  Fixture F;
  auto Leaf = NdType::makeNamedRecord("Leaf", 16);
  Leaf->FieldDisplayNames = {"tag", "payload"};
  Leaf->FieldDisplayOffsets = {0, 8};
  Leaf->FieldDisplayTypes = {F.Integer, F.Integer};
  auto Derived = NdType::makeNamedRecord("Extended", 24);
  Derived->FieldDisplayNames = {"base", "child"};
  Derived->FieldDisplayOffsets = {0, 16};
  Derived->FieldDisplayTypes = {F.Record, NdType::makePtr(Leaf)};
  auto Use = F.variable(1);
  Use->SourcePointerType = NdType::makePtr(Derived);
  EXPECT_FALSE(Use->structuralEq(*F.variable(1)));
  F.Function.Params.push_back({"choose", F.Integer});
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = F.variable(1, MedVar::Param);
  Branch.Body = {
      F.assign(2, HighExpr::makeLoad(
                      HighExpr::makeBinop(NdOp::INT_ADD, Use,
                                          HighExpr::makeConst(16, F.Bytes)),
                      F.Integer)),
      F.returning(HighExpr::makeLoad(
          HighExpr::makeBinop(NdOp::INT_ADD, F.variable(2),
                              HighExpr::makeConst(8, F.Bytes)),
          F.Integer))};
  F.Function.Body = {F.assign(1, F.parameter()), Branch,
                     F.returning(HighExpr::makeLoad(
                         HighExpr::makeBinop(NdOp::INT_ADD, F.variable(1),
                                             HighExpr::makeConst(8, F.Bytes)),
                         F.Integer))};
  CEmitterOptions Options;
  Options.TheArch = F.Architecture;
  Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
    CRecordDefinitions D;
    D.Text = "struct Device { uint64_t flags, value; };\n"
             "struct Leaf { uint64_t tag, payload; };\n"
             "struct Extended { struct Device base; struct Leaf *child; };\n";
    D.Records = {{"Device", false}, {"Leaf", false}, {"Extended", false}};
    D.Tags = D.Records;
    return D;
  };
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options));
  EXPECT_NE(Text.find("->child"), std::string::npos) << Text;
  EXPECT_NE(Text.find("->payload"), std::string::npos) << Text;
  EXPECT_NE(Text.find("->value"), std::string::npos) << Text;
  for (llvm::StringRef Opt : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(
        Text + "\nint main(void) { struct Leaf leaf = {11, 91};"
               "struct Extended d = {{7, 43}, &leaf};"
               "return transport(&d.base, 1) != 91 ||"
               " transport(&d.base, 0) != 43; }\n",
        {Opt, "-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(HighPointerTypeInference, CapturedValueSurvivesCallAndFrameSlotReuse) {
  for (Arch Architecture : PointerArchitectures) {
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

TEST(HighPointerTypeInference, DeclaredGlobalLoadsSurvivePrivateSpills) {
  for (Arch Architecture : PointerArchitectures) {
    Fixture F(Architecture);
    SCOPED_TRACE(int(Architecture));
    F.Function.Params.clear();
    const auto Global =
        HighExpr::makeLoad(HighExpr::makeConst(0x2000, F.Bytes), F.Integer);
    F.Function.Body = {F.assign(2, Global), F.store(F.slot(), F.variable(2)),
                       F.call(), F.load(), F.returning(F.variable(1))};
    const HighPointerTypeOrigins Origins{{Global.get(), F.Pointer}};
    F.Facts = inferHighPointerTypes(F.Function, Architecture, {}, Origins);
    EXPECT_EQ(F.inferred(F.Function.Body[0].Dst), F.Pointer);
    EXPECT_EQ(F.inferred(F.Function.Body[3].Dst), F.Pointer);
    EXPECT_EQ(F.Function.Body[0].Dst->Type, F.Integer);
    EXPECT_TRUE(inferHighPointerTypes(F.Function, Architecture).empty());

    // A partial read is not the declared pointer, even if an origin was
    // supplied for it. Nor may a declaration promote a floating carrier.
    for (TypeRef Narrow : {NdType::makeInt(2), NdType::makeFloat(F.Bytes)}) {
      Global->Type = Narrow;
      EXPECT_TRUE(
          inferHighPointerTypes(F.Function, Architecture, {}, Origins).empty());
    }
  }
}

TEST(HighPointerTypeInference, ConflictingGlobalPointersDoNotMeetAtAJoin) {
  Fixture F;
  F.Function.Params = {{"condition", F.Integer}};
  auto Left =
      HighExpr::makeLoad(HighExpr::makeConst(0x2000, F.Bytes), F.Integer);
  auto Right =
      HighExpr::makeLoad(HighExpr::makeConst(0x2010, F.Bytes), F.Integer);
  auto Other = NdType::makePtr(NdType::makeNamedRecord("Other", 16));
  HighStmt Choice;
  Choice.Kind = StmtKind::IfElse;
  Choice.Cond = F.variable(0, MedVar::Param);
  Choice.Body = {F.store(F.slot(), Left)};
  Choice.ElseBody = {F.store(F.slot(), Right)};
  F.Function.Body = {Choice, F.load(), F.returning(F.variable(1))};
  HighPointerTypeOrigins Origins{{Left.get(), F.Pointer}, {Right.get(), Other}};
  auto Facts = inferHighPointerTypes(F.Function, F.Architecture, {}, Origins);
  EXPECT_FALSE(Facts.count(highSourceLocalIdentity(F.variable(1)->Var)));
  Origins[Right.get()] = F.Pointer;
  Facts = inferHighPointerTypes(F.Function, F.Architecture, {}, Origins);
  EXPECT_EQ(Facts.at(highSourceLocalIdentity(F.variable(1)->Var)), F.Pointer);
}

TEST(HighPointerTypeInference, AssertedLocalTypesReachCopiesAndCanBeWithdrawn) {
  for (Arch Architecture : PointerArchitectures) {
    Fixture F(Architecture);
    F.Function.Params.clear();
    F.Function.Body = {F.assign(2, HighExpr::makeCall("obtain", 0x2000, {})),
                       F.store(F.slot(), F.variable(2)), F.load(),
                       F.returning(F.variable(1))};
    F.Function.Body[0].Val->Type = F.Integer;
    auto Definition = F.Function.Body[0].Dst;
    auto Use = F.Function.Body[1].StoreVal;
    HighPointerTypeOrigins Origins{{Definition.get(), F.Pointer},
                                   {Use.get(), F.Pointer}};
    F.Facts = inferHighPointerTypes(F.Function, Architecture, {}, Origins);
    EXPECT_EQ(F.inferred(F.Function.Body[2].Dst), F.Pointer);
    EXPECT_EQ(Definition->Type, F.Integer);
    for (const auto &Type : {F.Integer, NdType::makePtr(NdType::makeVoid())}) {
      Origins[Definition.get()] = Type;
      Origins[Use.get()] = Type;
      EXPECT_TRUE(
          inferHighPointerTypes(F.Function, Architecture, {}, Origins).empty());
    }
    EXPECT_TRUE(inferHighPointerTypes(F.Function, Architecture).empty());
  }
}

TEST(HighPointerTypeInference, MemoryClobbersRevokeStoredType) {
  for (unsigned Kind = 0; Kind < 5; ++Kind) {
    Fixture F;
    SCOPED_TRACE(Kind);
    auto Clobber = F.call();
    Clobber.CallExpr->Operands = {F.slot()};
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
    if (Kind == 3) {
      auto Escape = F.call();
      Escape.CallExpr->Operands = {F.slot()};
      F.Function.Body.insert(F.Function.Body.begin(), Escape);
    }
    EXPECT_FALSE(F.run());
    EXPECT_EQ(F.Function.Body[F.Function.Body.size() - 2].Dst->Type, F.Integer);
  }
}

TEST(HighPointerTypeInference, PrivatePointerSpillsSurviveCallsAndJoins) {
  for (Arch Architecture : PointerArchitectures) {
    Fixture F(Architecture);
    F.Function.Params.push_back({"arg1", F.Pointer});
    auto Call = F.call();
    Call.CallExpr->Operands = {F.parameter()};
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = F.parameter();
    Branch.Body = {F.store(F.slot(), F.parameter()), Call,
                   F.store(F.parameter(), HighExpr::makeConst(3, 8)), F.load()};
    Branch.ElseBody = {F.assign(1, F.variable(1, MedVar::Param, F.Pointer))};
    F.Function.Body = {Branch, F.returning(F.variable(1))};
    EXPECT_TRUE(F.run());
    EXPECT_EQ(F.inferred(F.Function.Body.back().RetVal), F.Pointer);
  }
}

TEST(HighPointerTypeInference, EscapesRemainVisibleThroughAliasesAndJoins) {
  for (bool BranchEscape : {false, true}) {
    Fixture F;
    auto Escape = F.call();
    Escape.CallExpr->Operands = {HighExpr::makeBinop(
        NdOp::INT_ADD, F.variable(2), F.variable(3, MedVar::Param))};
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = F.parameter();
    Branch.Body = {Escape};
    F.Function.Body = {F.assign(2, F.slot()),
                       BranchEscape ? Branch : Escape,
                       F.store(F.slot(), F.parameter()),
                       F.call(),
                       F.load(),
                       F.returning(F.variable(1))};
    EXPECT_FALSE(F.run());
    EXPECT_EQ(F.inferred(F.Function.Body.back().RetVal), F.Integer);
  }
}

TEST(HighPointerTypeInference, JoinedReceiverFieldsKeepBothObjectIdentities) {
  Fixture F(Arch::AArch64);
  F.Function.Params.push_back({"arg1", F.Pointer});
  F.Function.Params.push_back({"choose", F.Integer});
  F.Function.Params.push_back({"expected", F.Integer});
  auto Call = F.call();
  Call.CallExpr->Operands = {F.parameter()};
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = F.variable(2, MedVar::Param);
  Branch.Body = {F.store(F.slot(), F.parameter()), Call, F.load()};
  Branch.ElseBody = {F.assign(1, F.variable(1, MedVar::Param, F.Pointer))};
  const auto Field = HighExpr::makeBinop(NdOp::INT_ADD, F.variable(1),
                                         HighExpr::makeConst(8, F.Bytes));
  HighStmt Check;
  Check.Kind = StmtKind::If;
  Check.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, F.variable(1),
                                   F.variable(3, MedVar::Param));
  Check.Body = {F.returning(HighExpr::makeConst(1, 8))};
  F.Function.Body = {
      Branch, Check, F.store(Field, HighExpr::makeConst(42, 8)),
      F.returning(HighExpr::makeLoad(Field, NdType::makeInt(8)))};
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = F.Architecture;
  Options.EmitComments = false;
  Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
    CRecordDefinitions Definitions;
    Definitions.Text = "struct Device { uint64_t flags; uint64_t value; };\n";
    Definitions.Records["Device"] = false;
    return Definitions;
  };
  ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options));
  EXPECT_NE(Text.find("->value ="), std::string::npos) << Text;
  source_call_execution_test::compileAndRun(
      Text + "\nint touch(uint64_t p) { ((struct Device *)(uintptr_t)p)->flags"
             " += 1; return 0; }\n"
             "int main(void) { for (int choose = 0; choose < 2; ++choose) {"
             " struct Device a = {11, 12}, b = {21, 22};"
             " if (transport(&a, &b, choose, (uintptr_t)(choose ? &a : &b)) != "
             "42 ||"
             " a.flags != 11 + choose || b.flags != 21 ||"
             " a.value != (choose ? 42 : 12) ||"
             " b.value != (choose ? 22 : 42)) return 1; } return 0; }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types",
       "-Werror=pointer-integer-compare"});
}

TEST(HighPointerTypeInference, KnownNonprivateWritesKeepDisjointPrivateFacts) {
  for (Arch Architecture : PointerArchitectures)
    for (bool AboveEntry : {false, true}) {
      Fixture F(Architecture);
      SCOPED_TRACE(getArchName(Architecture));
      auto Address = AboveEntry
                         ? HighExpr::makeBinop(NdOp::INT_ADD, F.slot(0),
                                               HighExpr::makeConst(8, F.Bytes))
                         : F.slot(72);
      F.Function.Body = {F.assign(2, F.slot(0), F.AddressInteger),
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
  for (Arch Architecture : PointerArchitectures)
    for (bool Change : {false, true}) {
      Fixture F(Architecture);
      SCOPED_TRACE(getArchName(Architecture));
      F.Function.Body = {F.assign(2, F.slot(), F.AddressInteger),
                         F.store(F.variable(2, MedVar::Temp, F.AddressInteger),
                                 F.parameter())};
      if (Change)
        F.Function.Body.push_back(F.assign(2, F.slot(32), F.AddressInteger));
      F.Function.Body.push_back(F.assign(
          1, HighExpr::makeLoad(F.variable(2, MedVar::Temp, F.AddressInteger),
                                F.Integer)));
      F.Function.Body.push_back(F.returning(F.variable(1)));
      EXPECT_EQ(F.run(), !Change);
    }
}

TEST(HighPointerTypeInference, NarrowN32FrameAddressesCannotKeepPointerFacts) {
  for (Arch Architecture : {Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    SCOPED_TRACE(getArchName(Architecture));
    Fixture F(Architecture);
    auto Truncated = HighExpr::makeBinop(NdOp::SUBBYTES, F.slot(),
                                         HighExpr::makeConst(0, 4));
    Truncated->Type = F.Integer;
    F.Function.Body = {
        F.assign(2, Truncated), F.store(F.variable(2), F.parameter()),
        F.assign(1, HighExpr::makeLoad(F.variable(2), F.Integer)),
        F.returning(F.variable(1))};
    EXPECT_FALSE(F.run());
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

TEST(HighPointerTypeInference,
     ConstantInteriorPointersKeepTheirByteDisplacement) {
  for (Arch Architecture : PointerArchitectures) {
    Fixture F(Architecture);
    SCOPED_TRACE(getArchName(Architecture));
    auto Interior = HighExpr::makeBinop(NdOp::INT_ADD, F.parameter(),
                                        HighExpr::makeConst(5, F.Bytes));
    F.Function.Body = {F.assign(1, Interior), F.store(F.slot(), F.variable(1)),
                       F.call(), F.load(2), F.returning(F.variable(2))};
    const auto Locations = inferHighPointerLocations(F.Function, Architecture);
    for (int Id : {1, 2}) {
      const auto Found =
          Locations.find(highSourceLocalIdentity(F.variable(Id)->Var));
      ASSERT_NE(Found, Locations.end());
      EXPECT_EQ(Found->second.Type, F.Pointer);
      EXPECT_EQ(Found->second.ByteOffset, 5);
    }
    // A carrier pointing inside the object must not be declared as its base.
    EXPECT_TRUE(inferHighPointerTypes(F.Function, Architecture).empty());
    F.Function.Body.insert(
        F.Function.Body.end() - 1,
        F.assign(2, HighExpr::makeBinop(NdOp::INT_ADD, F.variable(2),
                                        HighExpr::makeConst(1, F.Bytes))));
    EXPECT_FALSE(inferHighPointerLocations(F.Function, Architecture)
                     .count(highSourceLocalIdentity(F.variable(2)->Var)));
  }
}

TEST(HighPointerTypeInference, InteriorMemberRenderingKeepsTheCapturedValue) {
  Fixture F;
  F.Function.Body = {
      F.assign(1, HighExpr::makeBinop(NdOp::INT_ADD, F.parameter(),
                                      HighExpr::makeConst(5, 4))),
      F.store(F.slot(), F.variable(1)),
      F.call(),
      F.load(2),
      F.store(HighExpr::makeBinop(NdOp::INT_ADD, F.variable(2),
                                  HighExpr::makeConst(3, F.Bytes)),
              HighExpr::makeConst(42, 8)),
      F.returning(HighExpr::makeLoad(
          HighExpr::makeBinop(NdOp::INT_ADD, F.variable(2),
                              HighExpr::makeConst(3, F.Bytes)),
          F.Integer))};
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
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

TEST(HighPointerTypeInference, DisplacementsUseTheConstantsMachineWidth) {
  for (Arch Architecture : PointerArchitectures) {
    Fixture F(Architecture);
    for (uint16_t Width : {uint16_t(1), uint16_t(2), uint16_t(4), F.Bytes}) {
      SCOPED_TRACE(Width);
      for (NdOp Op : {NdOp::INT_ADD, NdOp::INT_SUB}) {
        const uint64_t Bits = UINT64_MAX - 2;
        F.Function.Body = {
            F.assign(1, HighExpr::makeBinop(Op, F.parameter(),
                                            HighExpr::makeConst(Bits, Width))),
            F.returning(F.variable(1))};
        const auto Locations =
            inferHighPointerLocations(F.Function, Architecture);
        const auto Found =
            Locations.find(highSourceLocalIdentity(F.variable(1)->Var));
        ASSERT_NE(Found, Locations.end());
        const int64_t Delta =
            Width == F.Bytes
                ? -3
                : int64_t(Bits & ((UINT64_C(1) << (Width * 8)) - 1));
        EXPECT_EQ(Found->second.ByteOffset,
                  Op == NdOp::INT_ADD ? Delta : -Delta);
      }
    }
    // A truncated base no longer identifies the original object.
    auto Narrow = HighExpr::makeBitCast(F.parameter(), NdType::makeInt(2));
    auto Offset =
        HighExpr::makeBinop(NdOp::INT_ADD, Narrow, HighExpr::makeConst(3, 2));
    Offset->Type = F.Integer;
    F.Function.Body = {F.assign(1, Offset), F.returning(F.variable(1))};
    EXPECT_TRUE(inferHighPointerLocations(F.Function, Architecture).empty());
  }
}

TEST(HighPointerTypeInference, GlobalWrapperPointerKeepsItsCapturedObject) {
  Fixture F;
  F.Function.Params.clear();
  auto Wrapper = NdType::makeNamedRecord("Handle", 16);
  Wrapper->FieldDisplayNames = {"object", "count"};
  Wrapper->FieldDisplayOffsets = {0, 8};
  Wrapper->FieldDisplayTypes = {F.Pointer, F.Integer};
  class Globals : public NullDebugContext {
  public:
    TypeRef Wrapper;
    std::optional<DataObjectSym>
    resolveDataObject(va_t Address) const override {
      if (Address == 0x2000)
        return DataObjectSym{"GlobalRoot", Address, 16, false, Wrapper};
      return std::nullopt;
    }
  } Debug;
  Debug.Wrapper = Wrapper;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 16;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(16);
  Image.Segments.push_back(std::move(Data));
  Image.Symbols = {{"GlobalRoot", 0x2000, 16, false}};
  auto Pointer = HighExpr::makeLoad(HighExpr::makeConst(0x2000, 8), F.Integer);
  F.Function.Body = {F.assign(1, Pointer), F.call(),
                     F.returning(HighExpr::makeLoad(
                         HighExpr::makeBinop(NdOp::INT_ADD, F.variable(1),
                                             HighExpr::makeConst(8, 8)),
                         F.Integer))};
  CEmitterOptions Options;
  Options.Image = &Image;
  Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
    CRecordDefinitions Definitions;
    Definitions.Text = "struct Device { uint64_t flags; uint64_t value; };\n";
    Definitions.Records["Device"] = false;
    return Definitions;
  };
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options, &Debug));
  EXPECT_NE(Text.find("->value"), std::string::npos) << Text;
  source_call_execution_test::compileAndRun(
      Text + "\nint touch(void) { GlobalRoot = 0; return 0; }\n"
             "int main(void) { struct Device d = {11, 37}; GlobalRoot = &d;"
             "return transport() != 37 || GlobalRoot != 0; }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(HighPointerTypeInference, BitfieldUpdatesPreserveEveryOtherBit) {
  for (bool Set : {false, true})
    for (bool Assignment : {false, true}) {
      Fixture F;
      auto Flags = NdType::makeNamedRecord("FlagState", 2);
      Flags->FieldDisplayBits = {{"active", 13, 1, false}};
      F.Record->FieldDisplayNames = {"flags", "base"};
      F.Record->FieldDisplayTypes[1] = Flags;
      auto Address = HighExpr::makeBinop(NdOp::INT_ADD, F.parameter(),
                                         HighExpr::makeConst(8, F.Bytes));
      auto Read = HighExpr::makeLoad(Address, NdType::makeInt(2, false));
      auto Value =
          HighExpr::makeBinop(Set ? NdOp::INT_OR : NdOp::INT_AND, Read,
                              HighExpr::makeConst(Set ? 0x2000 : 0xdfff, 2));
      auto Write = F.store(Address, Value);
      if (Assignment) {
        Write.Kind = StmtKind::Assign;
        Write.Dst = Read;
        Write.Val = Value;
        Write.StoreAddr = Write.StoreVal = nullptr;
      }
      F.Function.Body = {Write, F.returning()};
      CEmitterOptions Options;
      Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
        CRecordDefinitions Definitions;
        Definitions.Text =
            "struct FlagState { uint16_t low:13, active:1, high:2; };\n"
            "struct Device { uint64_t flags; struct FlagState base; uint8_t "
            "tail[6]; };\n";
        Definitions.Records["Device"] = false;
        Definitions.Records["FlagState"] = false;
        return Definitions;
      };
      auto Emit = [&] {
        std::string Text;
        llvm::raw_string_ostream OS(Text);
        EXPECT_TRUE(HighCEmitter().emit({F.Function}, OS, Options));
        return Text;
      };
      auto Text = Emit();
      EXPECT_NE(
          Text.find(std::string("->base.active = ") + (Set ? "1;" : "0;")),
          std::string::npos)
          << Text;
      source_call_execution_test::compileAndRun(
          Text +
          "\nint main(void) { for (unsigned bits = 0; bits < 65536; ++bits) {"
          " struct Device d; __builtin_memset(&d, 0xa5, sizeof(d));"
          " uint16_t before = bits, after;"
          " __builtin_memcpy(&d.base, &before, 2); transport(&d);"
          " __builtin_memcpy(&after, &d.base, 2);"
          " if (after != (uint16_t)(before " +
          (Set ? "| 0x2000" : "& 0xdfff") +
          ")) return 1; const uint8_t *p = (const uint8_t *)&d;"
          " for (unsigned i = 0; i < sizeof(d); ++i)"
          " if ((i < 8 || i >= 10) && p[i] != 0xa5) return 2; } return 0; }\n");
      // A signed field, conflicting overlay, or an ordered access must not
      // borrow the unsigned ordinary-bitfield spelling.
      Flags->FieldDisplayBits[0].IsSigned = true;
      EXPECT_EQ(Emit().find("->base.active ="), std::string::npos);
      Flags->FieldDisplayBits[0].IsSigned = false;
      Flags->FieldDisplayBits.push_back({"other", 13, 1, false});
      EXPECT_EQ(Emit().find("->base.active ="), std::string::npos);
      Flags->FieldDisplayBits.pop_back();
      if (Assignment)
        F.Function.Body[0].Dst->MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      else
        F.Function.Body[0].MemoryOrdering = NdMemoryOrdering::Release;
      EXPECT_EQ(Emit().find("->base.active ="), std::string::npos);
    }
}

TEST(HighPointerTypeInference, CapturedBitfieldRestoresOnlyItsOwnBits) {
  for (bool AliasingWrite : {false, true}) {
    Fixture F;
    auto Flags = NdType::makeNamedRecord("FlagState", 3);
    Flags->FieldDisplayBits = {{"active", 13, 1, false}};
    F.Record->FieldDisplayNames[1] = "state";
    F.Record->FieldDisplayTypes[1] = Flags;
    auto U8 = NdType::makeInt(1, false), U16 = NdType::makeInt(2, false),
         U32 = NdType::makeInt(4, false);
    const auto Address = HighExpr::makeBinop(NdOp::INT_ADD, F.parameter(),
                                             HighExpr::makeConst(8, F.Bytes));
    const auto Next = HighExpr::makeBinop(NdOp::INT_ADD, F.parameter(),
                                          HighExpr::makeConst(10, F.Bytes));
    const auto Cast = [](ExprPtr Value, TypeRef Type) {
      auto Result = std::make_shared<HighExpr>();
      Result->Kind = ExprKind::Cast;
      Result->Type = Result->CastTo = std::move(Type);
      Result->Operands = {std::move(Value)};
      return Result;
    };
    const auto Assign = [&](int Id, ExprPtr Value) {
      auto S = F.assign(Id, Value);
      S.Dst = F.variable(Id, MedVar::Temp, Value->Type);
      return S;
    };
    const auto Old = F.variable(4, MedVar::Temp, U32);
    const auto High = F.variable(5, MedVar::Temp, U32);
    const auto Merged = F.variable(6, MedVar::Temp, U32);
    auto Set =
        HighExpr::makeBinop(NdOp::INT_OR, Old, HighExpr::makeConst(0x2000, 4));
    auto Reload = HighExpr::makeBinop(
        NdOp::INT_OR, Cast(HighExpr::makeLoad(Address, U16), U32),
        HighExpr::makeBinop(NdOp::INT_LEFT, High, HighExpr::makeConst(16, 4)));
    auto Clear = HighExpr::makeBinop(NdOp::INT_AND, Reload,
                                     HighExpr::makeConst(0xffdfff, 4));
    auto Restore = HighExpr::makeBinop(
        NdOp::INT_OR, Merged,
        HighExpr::makeBinop(NdOp::INT_AND, Old,
                            HighExpr::makeConst(0x2000, 4)));
    auto Call = F.call();
    Call.CallExpr->Operands = {F.parameter()};
    F.Function.Body = {Assign(4, Cast(HighExpr::makeLoad(Address, U16), U32))};
    if (AliasingWrite) {
      const auto OtherType = NdType::makePtr(U16);
      F.Function.Params.push_back({"other", OtherType});
      F.Function.Body.push_back(F.store(F.variable(1, MedVar::Param, OtherType),
                                        HighExpr::makeConst(0x5a5a, 2)));
    }
    F.Function.Body.push_back(F.store(Address, Cast(Set, U16)));
    F.Function.Body.push_back(Call);
    F.Function.Body.push_back(
        Assign(5, Cast(HighExpr::makeLoad(Next, U8), U32)));
    F.Function.Body.push_back(Assign(6, Clear));
    F.Function.Body.push_back(F.store(Address, Cast(Restore, U16)));
    F.Function.Body.push_back(
        F.store(Next, Cast(HighExpr::makeBinop(NdOp::INT_RIGHT, Merged,
                                               HighExpr::makeConst(16, 4)),
                           U8)));
    F.Function.Body.push_back(F.returning());
    class Calls : public NullDebugContext {
    public:
      FunctionSym Touch;
      std::optional<FunctionSym> resolveFunction(va_t Address) const override {
        return Address == Touch.Addr ? std::optional(Touch) : std::nullopt;
      }
    } Debug;
    Debug.Touch.Name = "touch";
    Debug.Touch.Addr = 0x1000;
    Debug.Touch.ReturnType = NdType::makeVoid();
    Debug.Touch.Params = {{"object", F.Pointer}};
    Debug.Touch.PlatformConvention = true;
    CEmitterOptions Options;
    Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
      CRecordDefinitions D;
      D.Text = "struct FlagState { uint8_t low; uint8_t pad:5, active:1, "
               "top:2; uint8_t high; };\n"
               "struct Device { uint64_t flags; struct FlagState state; "
               "uint8_t tail[5]; };\n";
      D.Records["Device"] = D.Records["FlagState"] = false;
      return D;
    };
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options, &Debug));
    EXPECT_NE(Text.find("->state.active ="), std::string::npos) << Text;
    if (!AliasingWrite)
      EXPECT_NE(Text.find("= arg0->state.active;"), std::string::npos) << Text;
    const std::string Main =
        std::string(R"c(
static uint16_t expected_before, changed;
static unsigned error;
void touch(Device *d) {
  uint16_t before;
  __builtin_memcpy(&before, &d->state, 2);
  if (before != expected_before) error = 1;
  __builtin_memcpy(&d->state, &changed, 2);
  d->state.high = (uint8_t)(changed ^ 0x7b);
}
int main(void) {
  for (unsigned bits = 0; bits < 65536; ++bits) {
    Device d; __builtin_memset(&d, 0xa5, sizeof(d));
    uint16_t before = bits, after;
    expected_before = before | 0x2000;
    changed = (uint16_t)(bits * 109 + 0x731);
    __builtin_memcpy(&d.state, &before, 2);
)c") +
        (AliasingWrite ? "transport(&d, (uint16_t *)(void *)&d.state);\n"
                       : "transport(&d);\n") +
        R"c(
    __builtin_memcpy(&after, &d.state, 2);
    if (error || after != (uint16_t)((changed & ~0x2000) | (before & 0x2000))) return 1;
    if (d.state.high != (uint8_t)(changed ^ 0x7b)) return 2;
    const uint8_t *p = (const uint8_t *)&d;
    for (unsigned i = 0; i < sizeof(d); ++i)
      if ((i < 8 || i >= 11) && p[i] != 0xa5) return 3;
  }
  return 0;
}
)c";
    for (const auto Optimization : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(Text + Main, {Optimization});
  }
}

TEST(HighPointerTypeInference, MaskedBitfieldReadKeepsSignExtendedSourceBits) {
  Fixture F;
  F.Record->FieldDisplayNames.clear();
  F.Record->FieldDisplayTypes.clear();
  F.Record->FieldDisplayOffsets.clear();
  F.Record->FieldDisplayBits = {{"active", 13, 1, false}};
  auto Address = HighExpr::makeBinop(NdOp::INT_ADD, F.parameter(),
                                     HighExpr::makeConst(1, F.Bytes));
  auto Read = HighExpr::makeLoad(Address, NdType::makeInt(1, true));
  auto Extended = std::make_shared<HighExpr>();
  Extended->Kind = ExprKind::Cast;
  Extended->Type = Extended->CastTo = NdType::makeInt(4, true);
  Extended->Operands = {Read};
  auto Shift =
      HighExpr::makeBinop(NdOp::INT_ASHR, Extended, HighExpr::makeConst(5, 4));
  auto Mask =
      HighExpr::makeBinop(NdOp::INT_AND, Shift, HighExpr::makeConst(1, 4));
  F.Function.Body = {F.returning(Mask)};
  CEmitterOptions Options;
  Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
    CRecordDefinitions D;
    D.Text = "struct Device { uint8_t low; uint8_t pad:5, active:1, top:2; "
             "uint8_t tail[14]; };\n";
    D.Records["Device"] = false;
    return D;
  };
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(HighCEmitter().emit({F.Function}, OS, Options));
  EXPECT_NE(Text.find("->active"), std::string::npos) << Text;
  source_call_execution_test::compileAndRun(Text + R"c(
int main(void) {
  for (unsigned value = 0; value < 256; ++value) {
    Device d = {0}; ((uint8_t *)&d)[1] = value;
    if (transport(&d) != ((value >> 5) & 1)) return 1;
  }
  return 0;
}
)c");
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
