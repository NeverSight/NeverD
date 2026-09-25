//===- ObjCSourceProjectionTests.cpp - Objective-C projection boundaries --===//

#include "../../../lib/sdk/capi/ObjCSourceProjection.h"
#include "../../../lib/sdk/capi/SourceProjectionEvidenceJSON.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::sdk;

struct Projection {
  SourceFunctionTypeHint Hint;
  HighFunc Func;
  PipelineFunctionAudit Audit;

  Projection() {
    Hint.ReturnType = NdType::makeInt(4, true);
    Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                       {"objc_cmd", NdType::makePtr(NdType::makeVoid())},
                       {"arg0", NdType::makeInt(4, true)}};
    Func.Entry = 0x1000;
    Func.Name = "neverd_objc_imp_1000";
    Func.ReturnType = Hint.ReturnType;
    Func.SourceTypeHint = Hint;
    for (const auto &Parameter : Hint.Parameters)
      Func.Params.push_back({Parameter.Name, Parameter.Type});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(42, 4);
    Func.Body.push_back(Return);
    Audit.Entry = Func.Entry;
    Audit.Disposition = PipelineFunctionDisposition::Accepted;
    Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
    Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  }

  std::string limitation() const {
    return objcSourceBodyLimitation(Func, Hint, &Audit);
  }
};

std::string emit(const HighFunc &Func) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  return Source;
}

TEST(ObjCSourceProjection,
     CompleteConstantBodyAndUnusedParametersAreSupported) {
  Projection P;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  // C projection does not require an LLVM definition or invent one as proof.
  EXPECT_FALSE(P.Audit.HasLLVMDefinition);
  const std::string Source = emit(P.Func);
  EXPECT_NE(Source.find("return 42;"), std::string::npos) << Source;
  EXPECT_TRUE(objcSourceTextLimitation(Source).empty());
}

TEST(ObjCSourceProjection, SignatureChangesCannotInheritRecoveryStatus) {
  using Mutation = std::function<void(Projection &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"missing hint", [](Projection &P) { P.Func.SourceTypeHint.reset(); }},
      {"return width",
       [](Projection &P) { P.Func.ReturnType = NdType::makeInt(8); }},
      {"return signedness",
       [](Projection &P) { P.Func.ReturnType = NdType::makeInt(4, false); }},
      {"parameter type",
       [](Projection &P) {
         P.Func.Params[2].Type = NdType::makeInt(4, false);
       }},
      {"parameter pointee",
       [](Projection &P) {
         P.Func.Params[0].Type = NdType::makePtr(NdType::makeInt(4));
       }},
      {"parameter order",
       [](Projection &P) { std::swap(P.Func.Params[0], P.Func.Params[1]); }},
      {"missing parameter", [](Projection &P) { P.Func.Params.pop_back(); }},
      {"additional parameter",
       [](Projection &P) {
         P.Func.Params.push_back({"extra", NdType::makeInt(4)});
       }},
      {"bound hint drift",
       [](Projection &P) {
         P.Func.SourceTypeHint->Parameters[2].Name = "wrong";
       }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    Mutate(P);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, RequiresEveryInstructionAndCompleteMatchingAudit) {
  Projection Complete;
  EXPECT_FALSE(
      objcSourceBodyLimitation(Complete.Func, Complete.Hint, nullptr).empty());
  using Mutation = std::function<void(PipelineFunctionAudit &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"wrong function", [](auto &A) { ++A.Entry; }},
      {"empty decode",
       [](auto &A) { A.DecodedInstructions = A.LiftedInstructions = 0; }},
      {"incomplete lift", [](auto &A) { --A.LiftedInstructions; }},
      {"limited",
       [](auto &A) {
         A.Disposition = PipelineFunctionDisposition::SkippedLimit;
       }},
      {"missing low IR", [](auto &A) { A.HasLowIR = false; }},
      {"missing med IR", [](auto &A) { A.HasMedIR = false; }},
      {"unverified med IR", [](auto &A) { A.MedIRVerified = false; }},
      {"decode failure", [](auto &A) { A.DecodeFailures.push_back(0x1004); }},
      {"unsupported instruction",
       [](auto &A) { A.UnsupportedInstructions.push_back(0x1004); }},
      {"truncated path", [](auto &A) { A.TruncatedPaths.push_back(0x1008); }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    Mutate(P.Audit);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, UnboundCallDiagnosticIdentifiesAndClearsTheFailure) {
  Projection P;
  auto Call = HighExpr::makeCall("external", 0x2000, {});
  P.Func.Body[0].RetVal = Call;
  const HighExpr *Failure = nullptr;
  EXPECT_FALSE(
      objcSourceBodyLimitation(P.Func, P.Hint, &P.Audit, {}, &Failure).empty());
  EXPECT_EQ(Failure, Call.get());
  EXPECT_TRUE(objcSourceBodyLimitation(
                  P.Func, P.Hint, &P.Audit,
                  [](const HighExpr &) { return true; }, &Failure)
                  .empty());
  EXPECT_EQ(Failure, nullptr);
  Failure = Call.get();
  auto WrongOrigin = P.Hint;
  WrongOrigin.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  EXPECT_FALSE(
      objcSourceBodyLimitation(P.Func, WrongOrigin, &P.Audit, {}, &Failure)
          .empty());
  EXPECT_EQ(Failure, nullptr);
}

TEST(ObjCSourceProjection,
     InventoriesIndependentCallsAndTheirStatementAddresses) {
  Projection P;
  auto SharedCall = HighExpr::makeCall("first", 0x2000, {});
  HighStmt First;
  First.Kind = StmtKind::ExprStmt;
  First.Addr = 0x1000;
  First.Val = SharedCall;
  HighStmt Second = First;
  Second.Addr = 0x1004;
  P.Func.Body.insert(P.Func.Body.begin(), {First, Second});
  P.Func.Body.back().Addr = 0x1008;
  P.Func.Body.back().RetVal =
      HighExpr::makeCall("last", 0x3000, {HighExpr::makeUndef(8)});

  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_TRUE(Report.Complete);
  std::set<va_t> Calls;
  bool Unresolved = false;
  for (const auto &Item : Report.Items) {
    if (Item.Issue == SourceProjectionIssue::CallBinding) {
      ASSERT_NE(Item.Expression, nullptr);
      Calls.insert(Item.StatementAddress);
    }
    Unresolved |= Item.Issue == SourceProjectionIssue::UnresolvedValue;
  }
  EXPECT_EQ(Calls, (std::set<va_t>{0x1000, 0x1004, 0x1008}));
  EXPECT_TRUE(Unresolved);
  const HighExpr *FirstRejected = nullptr;
  EXPECT_EQ(sourceBodyLimitation(P.Func, P.Hint, &P.Audit, {}, &FirstRejected),
            Report.limitation());
  EXPECT_EQ(FirstRejected, SharedCall.get());
}

TEST(ObjCSourceProjection,
     UnknownEdgesLeaveFlowIncompleteButStillInventoryCalls) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.Addr = 0x1004;
  Jump.GotoTarget = 0x9000;
  P.Func.Body.insert(P.Func.Body.begin(), Jump);
  P.Func.Body.back().RetVal = HighExpr::makeCall("external", 0x2000, {});
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_FALSE(Report.Complete);
  ASSERT_EQ(Report.Items.size(), 2u);
  EXPECT_EQ(Report.Items[0].Issue, SourceProjectionIssue::ControlFlow);
  EXPECT_EQ(Report.Items[0].StatementAddress, 0x1004u);
  EXPECT_EQ(Report.Items[0].RelatedAddress, 0x9000u);
  EXPECT_EQ(Report.Items[1].Issue, SourceProjectionIssue::CallBinding);
  EXPECT_EQ(P.limitation(), Report.limitation());
}

TEST(ObjCSourceProjection, InventoriesFallthroughAndEachUninitializedLocal) {
  Projection P;
  MedVar Left;
  Left.Kind = MedVar::Reg;
  Left.Id = 11;
  Left.SSAVer = 2;
  MedVar Right = Left;
  Right.Id = 12;
  P.Func.Body[0].Kind = StmtKind::ExprStmt;
  P.Func.Body[0].Addr = 0x1000;
  P.Func.Body[0].RetVal.reset();
  P.Func.Body[0].Val = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeVar(Left), HighExpr::makeVar(Right));
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_TRUE(Report.Complete);
  EXPECT_EQ(P.limitation(), Report.limitation());
  std::set<int> MissingDefinitions;
  for (const auto &Item : Report.Items)
    if (Item.Issue == SourceProjectionIssue::DefiniteAssignment) {
      ASSERT_NE(Item.Expression, nullptr);
      EXPECT_EQ(Item.StatementAddress, 0x1000u);
      MissingDefinitions.insert(Item.Expression->Var.Id);
    }
  EXPECT_EQ(MissingDefinitions, (std::set<int>{11, 12}));
}

TEST(ObjCSourceProjection, InvalidSignatureDoesNotHideIndependentBodyEvidence) {
  Projection P;
  P.Hint.ReturnType.reset();
  P.Hint.Parameters[0].Type.reset();
  MedVar InvalidParameter;
  InvalidParameter.Kind = MedVar::Param;
  InvalidParameter.Id = 999;
  P.Func.Body[0].RetVal = HighExpr::makeCall(
      "external", 0x2000, {HighExpr::makeVar(InvalidParameter)});
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_FALSE(Report.Complete);
  std::set<SourceProjectionIssue> Issues;
  for (const auto &Item : Report.Items)
    Issues.insert(Item.Issue);
  EXPECT_TRUE(Issues.count(SourceProjectionIssue::Signature));
  EXPECT_TRUE(Issues.count(SourceProjectionIssue::CallBinding));
  EXPECT_TRUE(Issues.count(SourceProjectionIssue::ParameterBinding));
  EXPECT_EQ(P.limitation(), Report.limitation());
}

TEST(ObjCSourceProjection,
     EvidenceBudgetCannotTurnAnIncompleteGraphIntoSuccess) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x9000;
  P.Func.Body.insert(P.Func.Body.begin(), Jump);
  for (size_t Index = 0; Index < SourceProjectionDiagnostics::MaxDiagnostics;
       ++Index) {
    HighStmt Call;
    Call.Kind = StmtKind::ExprStmt;
    Call.Val = HighExpr::makeCall("external", 0x2000, {});
    P.Func.Body.push_back(Call);
  }
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_FALSE(Report.Complete);
  ASSERT_EQ(Report.Items.size(),
            SourceProjectionDiagnostics::MaxDiagnostics + 1);
  EXPECT_EQ(Report.Items.back().Issue, SourceProjectionIssue::Budget);
  EXPECT_EQ(P.limitation(), Report.limitation());
}

TEST(ObjCSourceProjection, RejectsUnresolvedBodiesCallsAndExceptionState) {
  using Mutation = std::function<void(Projection &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"empty body", [](Projection &P) { P.Func.Body.clear(); }},
      {"missing return",
       [](Projection &P) { P.Func.Body[0].Kind = StmtKind::Nop; }},
      {"missing return value",
       [](Projection &P) { P.Func.Body[0].RetVal.reset(); }},
      {"undef",
       [](Projection &P) { P.Func.Body[0].RetVal = HighExpr::makeUndef(4); }},
      {"native call",
       [](Projection &P) {
         P.Func.Body[0].RetVal = HighExpr::makeCall("external", 0x2000, {});
       }},
      {"exception metadata",
       [](Projection &P) { P.Func.ExceptionMetadata.emplace(); }},
      {"exception region",
       [](Projection &P) { P.Func.Body[0].Kind = StmtKind::ItaniumTry; }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    Mutate(P);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, OrdinaryDarwinUnwindDoesNotImplyExceptionCode) {
  for (const auto Encoding :
       {ExceptionEncoding::CompactUnwind, ExceptionEncoding::DwarfFDE}) {
    Projection P;
    auto &Metadata = P.Func.ExceptionMetadata.emplace();
    Metadata.Encoding = Encoding;
    if (Encoding == ExceptionEncoding::CompactUnwind)
      Metadata.Compact.emplace();
    else
      Metadata.Dwarf.emplace();
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  }
}

TEST(ObjCSourceProjection,
     UnwindWithLanguageDispatchOrPartialMetadataIsRejected) {
  using Mutation = std::function<void(ExceptionFunction &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"partial decode",
       [](auto &M) { M.ParseStatus = ExceptionParseStatus::Partial; }},
      {"personality kind",
       [](auto &M) {
         M.Personality = ExceptionPersonality::ObjCPersonalityV0;
       }},
      {"personality address", [](auto &M) { M.PersonalityVA = 0x2000; }},
      {"unresolved personality",
       [](auto &M) { M.PersonalityName = "__objc_personality_v0"; }},
      {"handler data", [](auto &M) { M.HandlerDataVA = 0x2000; }},
      {"language table", [](auto &M) { M.Itanium.emplace(); }},
      {"objc dispatch", [](auto &M) { M.ObjC.emplace(); }},
      {"compact personality",
       [](auto &M) { M.Compact->PersonalityVA = 0x2000; }},
      {"compact LSDA flag", [](auto &M) { M.Compact->HasLSDA = true; }},
      {"compact LSDA address", [](auto &M) { M.Compact->LSDAVA = 0x2000; }},
      {"partial compact frame",
       [](auto &M) {
         M.Compact->SemanticStatus = CompactUnwindSemanticStatus::Partial;
       }},
      {"dwarf LSDA", [](auto &M) { M.Dwarf.emplace().LSDAVA = 0x2000; }},
      {"missing compact record", [](auto &M) { M.Compact.reset(); }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    auto &Metadata = P.Func.ExceptionMetadata.emplace();
    Metadata.Encoding = ExceptionEncoding::CompactUnwind;
    Metadata.Compact.emplace();
    Mutate(Metadata);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, ParameterReferencesMustOccupyTheDeclaredABISlot) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    Projection P;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Architecture;
    Parameter.Id = 2;
    Parameter.Size = 4;
    Parameter.RegOff = getTargetRegInfo(Architecture).IntParamRegs[2];
    P.Func.Body[0].RetVal = HighExpr::makeVar(Parameter);
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    P.Func.Body[0].RetVal->Var.RegOff =
        getTargetRegInfo(Architecture).IntParamRegs[3];
    EXPECT_FALSE(P.limitation().empty());
    P.Func.Body[0].RetVal->Var = Parameter;
    P.Func.Body[0].RetVal->Var.Id = 3;
    EXPECT_FALSE(P.limitation().empty());
    P.Func.Body[0].RetVal->Var = Parameter;
    P.Func.Body[0].RetVal->Var.Kind = MedVar::Reg;
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection,
     SyntheticFrameBaseIsExplainedButIncomingFlagsAreNot) {
  Projection P;
  MedVar Register;
  Register.Kind = MedVar::Reg;
  Register.TheArch = Arch::X64;
  Register.Id = 9;
  Register.Size = 8;
  Register.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  P.Func.Body[0].RetVal = HighExpr::makeVar(Register);
  EXPECT_FALSE(P.limitation().empty());
  P.Func.FrameSize = 16;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  P.Func.Body[0].RetVal->Var.Kind = MedVar::Flag;
  EXPECT_FALSE(P.limitation().empty());
}

TEST(ObjCSourceProjection, MissingLocalDefinitionsCannotBeRecovered) {
  for (const auto Kind : {MedVar::Temp, MedVar::Reg, MedVar::Stack}) {
    for (bool Renamed : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      SCOPED_TRACE(Renamed);
      Projection P;
      MedVar Local;
      Local.Kind = Kind;
      Local.TheArch = Arch::X64;
      Local.Id = 99;
      Local.SSAVer = 1;
      Local.Size = 4;
      Local.StackOff = -8;
      Local.RenameTag = Renamed ? 7 : -1;
      P.Func.Body[0].RetVal = HighExpr::makeVar(Local);
      EXPECT_FALSE(P.limitation().empty());

      HighStmt Definition;
      Definition.Kind = StmtKind::Assign;
      Definition.Dst = HighExpr::makeVar(Local);
      Definition.Val = HighExpr::makeConst(42, 4);
      P.Func.Body.insert(P.Func.Body.begin(), Definition);
      EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    }
  }
}

MedVar flowLocal() {
  MedVar Local;
  Local.Kind = MedVar::Temp;
  Local.TheArch = Arch::X64;
  Local.Id = 99;
  Local.SSAVer = 1;
  Local.Size = 4;
  return Local;
}

ExprPtr flowCondition() {
  MedVar Parameter;
  Parameter.Kind = MedVar::Param;
  Parameter.TheArch = Arch::X64;
  Parameter.Id = 2;
  Parameter.Size = 4;
  Parameter.RegOff = getTargetRegInfo(Arch::X64).IntParamRegs[2];
  return HighExpr::makeVar(Parameter);
}

HighStmt flowAssignment(uint64_t Value = 42) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Assign;
  Statement.Dst = HighExpr::makeVar(flowLocal());
  Statement.Val = HighExpr::makeConst(Value, 4);
  return Statement;
}

HighStmt flowReturn(ExprPtr Value = HighExpr::makeVar(flowLocal())) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Return;
  Statement.RetVal = std::move(Value);
  return Statement;
}

TEST(ObjCSourceProjection, OneBranchDefinitionDoesNotCoverTheOtherPath) {
  Projection P;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {flowAssignment()};
  P.Func.Body = {Branch, flowReturn()};
  EXPECT_FALSE(P.limitation().empty())
      << "A reachable return reads a local undefined on the false branch";

  Branch.Kind = StmtKind::IfElse;
  Branch.ElseBody = {flowAssignment(7)};
  P.Func.Body = {Branch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, RepeatedStableGuardProvesConditionalDefinition) {
  Projection P;
  HighStmt Define;
  Define.Kind = StmtKind::If;
  Define.Cond = flowCondition();
  Define.Body = {flowAssignment()};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Cond = flowCondition();
  Use.Body = {flowReturn()};
  P.Func.Body = {Define, Use, flowReturn(HighExpr::makeConst(7, 4))};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

  HighStmt Change;
  Change.Kind = StmtKind::Assign;
  Change.Dst = flowCondition();
  Change.Val = HighExpr::makeConst(1, 4);
  P.Func.Body.insert(P.Func.Body.begin() + 1, Change);
  EXPECT_FALSE(P.limitation().empty())
      << "Overwriting the guard must invalidate the earlier branch fact";
}

TEST(ObjCSourceProjection, GuardedPhiCleanupRemovesTheUndefinedReadItself) {
  Projection P;
  HighStmt Define;
  Define.Kind = StmtKind::IfElse;
  Define.Cond = flowCondition();
  Define.Body = {flowAssignment()};
  auto Copy = flowAssignment();
  auto Incoming = flowLocal();
  Incoming.Id = 100;
  Copy.Val = HighExpr::makeVar(Incoming);
  Copy.IsPhiCopy = true;
  Copy.Addr = 0x2000;
  Define.ElseBody = {Copy};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, flowCondition(),
                                 HighExpr::makeConst(0, 4));
  Use.Body = {flowReturn()};
  P.Func.Body = {Define, Use, flowReturn(HighExpr::makeConst(7, 4))};
  EXPECT_FALSE(P.limitation().empty());
  ASSERT_TRUE(eliminateHighDeadPhiCopies(P.Func));
  EXPECT_EQ(P.Func.Body[0].ElseBody[0].Kind, StmtKind::Nop);
  EXPECT_EQ(P.Func.Body[0].ElseBody[0].Addr, 0x2000u);
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  EXPECT_FALSE(eliminateHighDeadPhiCopies(P.Func));
}

TEST(ObjCSourceProjection, GuardFactsDoNotCrossDifferentLocalsOrWidths) {
  for (bool DifferentWidth : {false, true}) {
    Projection P;
    HighStmt Define;
    Define.Kind = StmtKind::If;
    Define.Cond = flowCondition();
    Define.Body = {flowAssignment()};
    HighStmt Use = Define;
    Use.Cond = flowCondition();
    Use.Body = {flowReturn()};
    if (DifferentWidth) {
      Use.Cond->Var.Size = 1;
      Use.Cond->Type = NdType::makeInt(1);
    } else {
      Use.Cond->Var.Id = 3;
    }
    P.Func.Body = {Define, Use, flowReturn(HighExpr::makeConst(7, 4))};
    const auto Report = analyzeHighSourceFlow(P.Func, true);
    ASSERT_TRUE(Report.Complete);
    EXPECT_FALSE(Report.Items.empty());
  }
}

TEST(ObjCSourceProjection, EscapedGuardAndIntrinsicWritesInvalidateFacts) {
  for (bool AddressEscape : {false, true}) {
    Projection P;
    HighStmt Define;
    Define.Kind = StmtKind::If;
    Define.Cond = flowCondition();
    Define.Body = {flowAssignment()};
    HighStmt Use = Define;
    Use.Cond = flowCondition();
    Use.Body = {flowReturn()};
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall("may_change_guard", 0x3000, {});
    if (AddressEscape) {
      auto Address = std::make_shared<HighExpr>();
      Address->Kind = ExprKind::Addr;
      Address->Type = NdType::makePtr();
      Address->Operands = {flowCondition()};
      Call.CallExpr->Operands = {Address};
    } else {
      Call.CallExpr->IntrinsicOutputs = {flowCondition()->Var};
    }
    P.Func.Body = {Define, Call, Use, flowReturn(HighExpr::makeConst(7, 4))};
    const auto Report = analyzeHighSourceFlow(P.Func, true);
    ASSERT_TRUE(Report.Complete);
    EXPECT_FALSE(Report.Items.empty());
  }
}

TEST(ObjCSourceProjection, GotoIntoGuardedArmCannotBorrowItsCondition) {
  Projection P;
  HighStmt Define;
  Define.Kind = StmtKind::If;
  Define.Cond = flowCondition();
  Define.Body = {flowAssignment()};
  HighStmt Use = Define;
  Use.Body = {flowReturn()};
  Use.Body[0].Addr = 0x2000;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  P.Func.Body = {Define, Jump, Use, flowReturn(HighExpr::makeConst(7, 4))};
  EXPECT_FALSE(P.limitation().empty());
}

TEST(ObjCSourceProjection, PredicatePartitionBudgetFallsBackConservatively) {
  Projection P;
  P.Func.Body.clear();
  // Repeated independent tests produce more than 32 contexts at a join.
  // An unreachable return must never be inferred from a partial expansion.
  for (int Pass = 0; Pass < 2; ++Pass)
    for (int Id = 2; Id < 10; ++Id) {
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = flowCondition();
      Branch.Cond->Var.Id = Id;
      P.Func.Body.push_back(Branch);
    }
  P.Func.Body.push_back(flowReturn());
  auto Report = analyzeHighSourceFlow(P.Func, true);
  ASSERT_TRUE(Report.Complete);
  ASSERT_FALSE(Report.Items.empty());
  EXPECT_EQ(Report.Items[0].Issue, HighSourceFlowIssue::DefiniteAssignment);
}

TEST(ObjCSourceProjection, AReadCannotBorrowALaterOrUnreachableDefinition) {
  Projection P;
  P.Func.Body = {flowReturn(), flowAssignment()};
  EXPECT_FALSE(P.limitation().empty())
      << "A definition after return cannot initialize its operand";

  P.Func.Body = {flowAssignment(), flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, EveryReachableNonVoidExitNeedsAReturn) {
  Projection P;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {flowReturn(HighExpr::makeConst(42, 4))};
  P.Func.Body = {Branch};
  EXPECT_FALSE(P.limitation().empty())
      << "A return in one arm does not cover the fallthrough exit";

  P.Func.Body.push_back(flowReturn(HighExpr::makeConst(7, 4)));
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  Branch.Kind = StmtKind::IfElse;
  Branch.ElseBody = {flowReturn(HighExpr::makeConst(7, 4))};
  P.Func.Body = {Branch};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, EarlyReturnDoesNotPolluteTheContinuingPath) {
  Projection P;
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = flowCondition();
  Branch.Body = {flowReturn(HighExpr::makeConst(7, 4))};
  Branch.ElseBody = {flowAssignment()};
  P.Func.Body = {Branch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, LoopDefinitionsRespectWhetherTheBodyExecutes) {
  for (const auto Kind : {StmtKind::While, StmtKind::For}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    Projection P;
    HighStmt Loop;
    Loop.Kind = Kind;
    Loop.Cond = flowCondition();
    Loop.Body = {flowAssignment()};
    P.Func.Body = {Loop, flowReturn()};
    EXPECT_FALSE(P.limitation().empty())
        << "A possibly zero-iteration loop cannot initialize an exit value";

    P.Func.Body.insert(P.Func.Body.begin(), flowAssignment(7));
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    Loop.Kind = StmtKind::DoWhile;
    P.Func.Body = {Loop, flowReturn()};
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  }
}

TEST(ObjCSourceProjection, ContinueCannotSkipARequiredLoopDefinition) {
  Projection P;
  HighStmt Continue;
  Continue.Kind = StmtKind::Continue;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {Continue};
  HighStmt Loop;
  Loop.Kind = StmtKind::DoWhile;
  // continue reaches a false test and exits before the assignment.
  Loop.Cond = HighExpr::makeConst(0, 1);
  Loop.Body = {Branch, flowAssignment()};
  P.Func.Body = {Loop, flowReturn()};
  EXPECT_FALSE(P.limitation().empty())
      << "continue reaches the loop test without defining the exit value";

  P.Func.Body.insert(P.Func.Body.begin(), flowAssignment(7));
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, RepeatedLoopTestDoesNotInventAnExitingPath) {
  Projection P;
  HighStmt Continue;
  Continue.Kind = StmtKind::Continue;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {Continue};
  HighStmt Loop;
  Loop.Kind = StmtKind::DoWhile;
  Loop.Cond = flowCondition();
  Loop.Body = {Branch, flowAssignment()};
  P.Func.Body = {Loop, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

  HighStmt Change;
  Change.Kind = StmtKind::Assign;
  Change.Dst = flowCondition();
  Change.Val = HighExpr::makeConst(0, 4);
  P.Func.Body[0].Body[0].Body.insert(P.Func.Body[0].Body[0].Body.begin(),
                                     Change);
  EXPECT_FALSE(P.limitation().empty())
      << "Clearing the guard before continue creates an undefined exit";
}

TEST(ObjCSourceProjection, BreakPreservesDefinitionsFromAnEnteredLoop) {
  Projection P;
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = HighExpr::makeConst(1, 1);
  Loop.Body = {flowAssignment(), Break};
  P.Func.Body = {Loop, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, SwitchDefinitionsNeedToCoverTheDefaultPath) {
  Projection P;
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.SwitchExpr = flowCondition();
  Switch.Cases.push_back({0, {flowAssignment()}});
  P.Func.Body = {Switch, flowReturn()};
  EXPECT_FALSE(P.limitation().empty())
      << "An unmatched case reaches return without defining its value";

  Switch.DefaultBody = {flowAssignment(7)};
  P.Func.Body = {Switch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, GotoCannotSkipARequiredDefinition) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  auto Return = flowReturn();
  Return.Addr = Jump.GotoTarget;
  P.Func.Body = {Jump, flowAssignment(), Return};
  EXPECT_FALSE(P.limitation().empty())
      << "The only path jumps over the local definition";

  auto Definition = flowAssignment();
  Definition.Addr = Jump.GotoTarget;
  Return.Addr = 0;
  P.Func.Body = {Jump, Definition, Return};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, BackwardGotoRetainsAReachingDefinition) {
  Projection P;
  HighStmt Label;
  Label.Kind = StmtKind::Nop;
  Label.Addr = 0x2000;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = Label.Addr;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {Jump};
  P.Func.Body = {flowAssignment(), Label, Branch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, GotoRequiresOneEmittableTarget) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  P.Func.Body.insert(P.Func.Body.begin(), Jump);
  EXPECT_FALSE(P.limitation().empty())
      << "A goto with no emitted target cannot be recovered";

  P.Func.Body.back().Addr = Jump.GotoTarget;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  auto Duplicate = flowAssignment();
  Duplicate.Addr = Jump.GotoTarget;
  P.Func.Body.insert(P.Func.Body.begin(), Duplicate);
  EXPECT_FALSE(P.limitation().empty())
      << "Distinct statements cannot emit the same goto label twice";
}

TEST(ObjCSourceProjection, NoReturnMetadataCannotHideSourceFallthrough) {
  Projection P;
  P.Func.DoesNotReturn = true;
  P.Func.Body = {flowAssignment()};
  EXPECT_FALSE(P.limitation().empty())
      << "_Noreturn is a declaration, not an actual terminating operation";

  P.Func.Body = {flowReturn(HighExpr::makeConst(42, 4))};
  EXPECT_FALSE(P.limitation().empty())
      << "A reachable return contradicts the emitted _Noreturn declaration";
}

TEST(ObjCSourceProjection, ActualInfiniteControlFlowDoesNotNeedAReturnValue) {
  Projection P;
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = HighExpr::makeConst(1, 1);
  P.Func.Body = {Loop};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  P.Func.DoesNotReturn = true;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.Addr = Jump.GotoTarget = 0x2000;
  P.Func.Body = {Jump};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, OnlyAnActualUnconditionalTrapTerminatesThePath) {
  Projection P;
  P.Func.DoesNotReturn = true;
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall("trap", 0, {});
  for (auto Id : {Intrinsic::Ud2, Intrinsic::ArmHlt, Intrinsic::Brk,
                  Intrinsic::Hlt_A64}) {
    Call.CallExpr->IntrinsicId = Id;
    P.Func.Body = {Call};
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    EXPECT_TRUE(isUnconditionalTrapIntrinsic(Id));
  }
  EXPECT_STREQ(intrinsicCName(Intrinsic::Brk), "__builtin_trap");

  // A debugger can resume after a breakpoint. Its spelling is not proof of
  // a terminating path, even when the containing native function is flagged.
  Call.CallExpr->IntrinsicId = Intrinsic::Int3;
  P.Func.Body = {Call};
  EXPECT_FALSE(P.limitation().empty());

  Call.CallExpr->IntrinsicId = Intrinsic::Ud2;
  Call.CallExpr->SourceCallHint = std::make_shared<SourceCallTypeHint>();
  P.Func.Body = {Call};
  EXPECT_FALSE(P.limitation().empty())
      << "The emitter's explicit source binding takes precedence over the "
         "intrinsic tag; it is not proof that the emitted call traps";
}

TEST(ObjCSourceProjection, ReachingDefinitionsAlsoCoverCallAndStoreOperands) {
  for (const auto Kind :
       {StmtKind::ExprStmt, StmtKind::Store, StmtKind::Call}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    Projection P;
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = flowCondition();
    Branch.Body = {flowAssignment()};
    HighStmt Use;
    Use.Kind = Kind;
    if (Kind == StmtKind::ExprStmt)
      Use.Val = HighExpr::makeVar(flowLocal());
    else if (Kind == StmtKind::Store) {
      Use.StoreAddr = HighExpr::makeConst(0x3000, 8);
      Use.StoreVal = HighExpr::makeVar(flowLocal());
    } else {
      Use.CallExpr = HighExpr::makeCall("bound_helper", 0x4000,
                                        {HighExpr::makeVar(flowLocal())});
    }
    P.Func.Body = {Branch, Use, flowReturn(HighExpr::makeConst(42, 4))};
    auto Check = [&] {
      return sourceBodyLimitation(P.Func, P.Hint, &P.Audit,
                                  [](const HighExpr &) { return true; });
    };
    EXPECT_FALSE(Check().empty());
    HighStmt Block;
    Block.Kind = StmtKind::Block;
    Block.Body = {flowAssignment()};
    P.Func.Body.insert(P.Func.Body.begin(), Block);
    EXPECT_TRUE(Check().empty()) << Check();
  }
}

TEST(ObjCSourceProjection, SourceFlowGraphRejectsExcessiveStatements) {
  Projection P;
  P.Func.Body.assign(100001, HighStmt{});
  P.Func.Body.push_back(flowReturn(HighExpr::makeConst(42, 4)));
  EXPECT_NE(P.limitation().find("limit"), std::string::npos);
}

TEST(ObjCSourceProjection, EmptySwitchArmsStillHaveABoundedFanout) {
  Projection P;
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.SwitchExpr = flowCondition();
  for (unsigned Value = 0; Value < 4097; ++Value)
    Switch.Cases.push_back({Value, {}});
  P.Func.Body = {Switch, flowReturn(HighExpr::makeConst(42, 4))};
  EXPECT_NE(P.limitation().find("switch-case limit"), std::string::npos);
}

TEST(ObjCSourceProjection, LocalInventoryIsBoundedBeforeDataflowAllocation) {
  Projection P;
  P.Func.Body.clear();
  for (unsigned Index = 0; Index < 8193; ++Index) {
    auto Definition = flowAssignment();
    Definition.Dst->Var.Id = Index;
    P.Func.Body.push_back(std::move(Definition));
  }
  P.Func.Body.push_back(flowReturn(HighExpr::makeConst(42, 4)));
  EXPECT_NE(P.limitation().find("local-value limit"), std::string::npos);
}

TEST(ObjCSourceProjection, RenamedPhiUsesItsEmittedLocalIdentity) {
  Projection P;
  MedVar Destination;
  Destination.Kind = MedVar::Temp;
  Destination.TheArch = Arch::AArch64;
  Destination.Id = 17;
  Destination.SSAVer = 2;
  Destination.Size = 4;
  Destination.RenameTag = 3;
  HighStmt Definition;
  Definition.Kind = StmtKind::Assign;
  Definition.IsPhiCopy = true;
  Definition.Dst = HighExpr::makeVar(Destination);
  Definition.Val = HighExpr::makeConst(42, 4);
  P.Func.Body.insert(P.Func.Body.begin(), Definition);
  MedVar Use = Destination;
  Use.Kind = MedVar::Reg;
  Use.Id = 31;
  Use.SSAVer = 5;
  auto Phi = HighExpr::makeVar(Use);
  Phi->Kind = ExprKind::Phi;
  P.Func.Body.back().RetVal = Phi;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  P.Func.Body.back().RetVal->Var.RenameTag = 4;
  EXPECT_FALSE(P.limitation().empty());
}

TEST(ObjCSourceProjection, DetectsTheRealHighCExpressionDepthFallback) {
  Projection P;
  auto Expression = HighExpr::makeConst(42, 4);
  for (unsigned Index = 0; Index < 210; ++Index)
    Expression = HighExpr::makeBinop(NdOp::INT_ADD, Expression,
                                     HighExpr::makeConst(1, 4));
  P.Func.Body[0].RetVal = Expression;
  EXPECT_FALSE(P.limitation().empty());
  const std::string Source = emit(P.Func);
  ASSERT_NE(Source.find("/* truncated: expr too deep */"), std::string::npos)
      << Source;
  EXPECT_FALSE(objcSourceTextLimitation(Source).empty());
}

TEST(ObjCSourceProjection, TextGuardRejectsEmitterPlaceholdersButNotCLiterals) {
  for (const char *Comment :
       {"truncated: expr too deep", "bad unary", "bad binop", "bad load",
        "bad store", "bad cast", "bad addr", "bad field", "unknown_op(0)",
        "unknown expr", "unary 999",
        "caller-saved register clobbered by call: unknown"}) {
    SCOPED_TRACE(Comment);
    const std::string Body =
        std::string("int method(void) { return 0 /* ") + Comment + " */; }";
    EXPECT_FALSE(objcSourceTextLimitation(Body).empty());
    const std::string Literal =
        std::string("const char *value = \"/* ") + Comment + " */\";";
    EXPECT_TRUE(objcSourceTextLimitation(Literal).empty());
  }
  EXPECT_TRUE(objcSourceTextLimitation(
                  "// /* bad load */\nint method(void) { return 42; }")
                  .empty());
  EXPECT_FALSE(objcSourceTextLimitation("\n ").empty());
  EXPECT_FALSE(
      objcSourceTextLimitation("int method(void) { /* incomplete").empty());
}

TEST(ObjCSourceProjection, SerializesEvidenceWithoutBorrowedIdentities) {
  const std::string InvalidName("helper\xff", 7);
  auto Call = HighExpr::makeCall(InvalidName, 0x8000, {});
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->TargetName = InvalidName;
  Call->SourceCallHint = std::move(Hint);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 7;
  Variable.SSAVer = 3;
  Variable.RenameTag = 2;
  Variable.StackOff = -16;
  auto Value = HighExpr::makeVar(Variable);
  SourceProjectionDiagnostics Diagnostics;
  Diagnostics.add(SourceProjectionIssue::CallBinding, InvalidName, 0x1004,
                  Call.get());
  Diagnostics.add(SourceProjectionIssue::DefiniteAssignment, "missing write",
                  0x1008, Value.get());
  SourceProjectionDiagnostics Dependencies;
  Dependencies.Complete = false;
  Dependencies.add(SourceProjectionIssue::Dependency, "missing dependency", 0,
                   nullptr, 0x9000);
  Diagnostics.append(Dependencies);
  auto Object = sourceProjectionEvidenceJSON(Diagnostics);
  EXPECT_EQ(Object.getBoolean("checks_complete"), false);
  auto *Items = Object.getArray("items");
  ASSERT_TRUE(Items);
  ASSERT_EQ(Items->size(), 3U);
  auto *First = (*Items)[0].getAsObject();
  ASSERT_TRUE(First);
  EXPECT_EQ(First->getString("statement_address"), "0x1004");
  auto *CallObject = First->getObject("call");
  ASSERT_TRUE(CallObject);
  EXPECT_EQ(CallObject->getString("target_address"), "0x8000");
  EXPECT_EQ(CallObject->getString("binding_name"), jsonSafeText(InvalidName));
  auto *Local = (*Items)[1].getAsObject()->getObject("value");
  ASSERT_TRUE(Local);
  EXPECT_EQ(Local->getInteger("ssa_version"), 3);
  EXPECT_EQ(Local->getInteger("rename_tag"), 2);
  EXPECT_EQ(Local->getInteger("stack_offset"), -16);
  EXPECT_EQ((*Items)[2].getAsObject()->getString("related_address"), "0x9000");
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Object));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Call->CallTarget, InvalidName);
}

struct NativeDependencyFixture {
  BinaryImage Image;
  PipelineResult Result;
  NativeDependencyFixture() {
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x5000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    Image.Segments.push_back(std::move(Text));
    Result.SourceImage = &Image;
    for (va_t Entry : {0x1000, 0x2000}) {
      ObjCMethod Method;
      Method.Implementation = Entry;
      Method.Status = "supported";
      Method.TypeHint = SourceFunctionTypeHint{};
      Image.ObjCMethods.push_back(std::move(Method));
    }
    for (va_t Entry : {0x1000, 0x2000, 0x3000}) {
      LowFunc Function;
      Function.Entry = Entry;
      Function.Blocks.emplace_back();
      Function.Blocks[0].StartAddr = Entry;
      Result.LowFuncs.push_back(std::move(Function));
    }
  }
  void call(size_t Function, va_t Target, bool Indirect = false) {
    auto &Block = Result.LowFuncs[Function].Blocks[0];
    LowOp Op;
    Op.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
    Op.Addr = Block.StartAddr + Block.Ops.size() * 4;
    Op.addInput(NdVar::cst(Target, 8));
    Block.Ops.push_back(Op);
  }
};

TEST(ObjCSourceProjection, IntegerPairDemandOnlyFollowsExactDirectTail) {
  NativeDependencyFixture F;
  F.Image.Arch = Arch::AArch64;
  F.call(0, 0x3000);
  auto &Function = F.Result.LowFuncs[0];
  auto &Ops = Function.Blocks[0].Ops;
  const auto ReturnRegister = getTargetRegInfo(F.Image.Arch).IntReturnReg;
  Ops[0].Output = NdVar::reg(ReturnRegister, 8);
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Ops[0].Addr;
  Return.addInput(NdVar::reg(ReturnRegister, 8));
  Ops.push_back(Return);
  EXPECT_EQ(forwardedNativeIntegerPairTarget(F.Image, Function), 0x3000U);

  Ops.back().Addr += 4;
  EXPECT_FALSE(forwardedNativeIntegerPairTarget(F.Image, Function));
  Ops.back().Addr = Ops[0].Addr;
  Ops[0].Opcode = NdOp::INDIR_CALL;
  EXPECT_FALSE(forwardedNativeIntegerPairTarget(F.Image, Function));
  Ops[0].Opcode = NdOp::CALL;
  Function.Blocks.emplace_back();
  EXPECT_FALSE(forwardedNativeIntegerPairTarget(F.Image, Function));
}

TEST(ObjCSourceProjection, NativeDependencyGraphKeepsSharedCallsAndCycles) {
  NativeDependencyFixture F;
  F.call(0, 0x3000);
  F.call(1, 0x3000);
  F.call(2, 0x1000);
  // An indirect operand is not an authenticated edge, even if constant.
  F.call(2, 0x4000, true);
  NativeSourceDependencyEvidence Evidence;
  const auto Targets = walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_EQ(Targets, (std::set<va_t>{0x1000, 0x3000}));
  EXPECT_EQ(Evidence.Roots, (std::set<va_t>{0x1000, 0x2000}));
  EXPECT_TRUE(Evidence.InventoryComplete);
  EXPECT_FALSE(Evidence.TargetsComplete);
  ASSERT_EQ(Evidence.Calls.size(), 4U);
  size_t SharedCallers = 0;
  for (const auto &Call : Evidence.Calls) {
    if (Call.Target == 0x3000)
      ++SharedCallers;
    if (Call.Indirect)
      EXPECT_EQ(Call.Target, 0U);
  }
  EXPECT_EQ(SharedCallers, 2U);
  PipelineOptions Options;
  std::map<va_t, std::string> Diagnostics;
  EXPECT_EQ(
      inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics), 0U);
  EXPECT_TRUE(Options.SourceTypeHints.empty());
  ASSERT_EQ(Diagnostics.size(), Targets.size());
  for (const auto &[Entry, Reason] : Diagnostics) {
    EXPECT_TRUE(Targets.count(Entry));
    EXPECT_FALSE(Reason.empty());
  }
  auto JSON = nativeSourceDependencyEvidenceJSON(Evidence);
  EXPECT_EQ(JSON.getBoolean("inventory_complete"), true);
  EXPECT_EQ(JSON.getBoolean("targets_complete"), false);
  ASSERT_EQ(JSON.getArray("calls")->size(), 4U);
  for (const auto &Call : *JSON.getArray("calls"))
    if (*Call.getAsObject()->getBoolean("indirect"))
      EXPECT_EQ(*Call.getAsObject()->get("target_address"),
                llvm::json::Value(nullptr));
}

TEST(ObjCSourceProjection,
     AmbiguousSelectorBodiesRemainSeparateNativeDependencyRoots) {
  NativeDependencyFixture F;
  F.Image.ObjCMethods[0].Status = "ambiguous_dispatch";
  F.Image.ObjCMethods[1].Status = "conflicting_encoding";
  NativeSourceDependencyEvidence Evidence;
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_EQ(Evidence.Roots, (std::set<va_t>{0x1000}));
  F.Image.ObjCMethods[0].TypeHint.reset();
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_TRUE(Evidence.Roots.empty());
}

TEST(ObjCSourceProjection, NativeInferenceSkipsCallOnlyThunkTargets) {
  NativeDependencyFixture F;
  F.call(0, 0x3000);
  PipelineOptions Options;
  std::map<va_t, std::string> Diagnostics;
  const std::set<va_t> CallOnlyTargets{0x3000};
  EXPECT_EQ(inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics,
                                        {}, CallOnlyTargets),
            0U);
  EXPECT_TRUE(Options.SourceTypeHints.empty());
  EXPECT_FALSE(Diagnostics.count(0x3000));
}

TEST(ObjCSourceProjection, NativeInferenceUsesSourceBoundRefinementBodies) {
  NativeDependencyFixture F;
  F.Image.Arch = Arch::AArch64;
  F.call(0, 0x3000);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.ReturnType = NdType::makeInt(4, false);
  Hint.Parameters = {{"native_arg0", NdType::makeInt(8, false)},
                     {"native_arg1", NdType::makeInt(8, false)}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Arch::AArch64, Error)) << Error;
  HighFunc Function;
  Function.Entry = 0x3000;
  Function.ReturnType = Hint.ReturnType;
  Function.SourceTypeHint = Hint;
  for (const auto &Parameter : Hint.Parameters)
    Function.Params.push_back({Parameter.Name, Parameter.Type});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 4);
  Function.Body.push_back(Return);
  F.Result.HighFuncs.push_back(Function);
  PipelineFunctionAudit Audit;
  Audit.Entry = Function.Entry;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 1;
  F.Result.FunctionAudits.push_back(Audit);

  auto Bound = Function;
  MedVar ParameterValue;
  ParameterValue.Kind = MedVar::Param;
  ParameterValue.TheArch = Arch::AArch64;
  ParameterValue.Id = 1;
  ParameterValue.Size = 8;
  auto Parameter = HighExpr::makeVar(ParameterValue, NdType::makeInt(8, false));
  auto Prefix =
      HighExpr::makeBinop(NdOp::SUBBYTES, Parameter, HighExpr::makeConst(0, 4));
  Prefix->Type = NdType::makeInt(4, false);
  HighStmt Use;
  Use.Kind = StmtKind::Assign;
  MedVar Local;
  Local.Kind = MedVar::Temp;
  Local.Id = 9;
  Local.Size = 4;
  Local.TheArch = Arch::AArch64;
  Use.Dst = HighExpr::makeVar(Local, NdType::makeInt(4, false));
  Use.Val = Prefix;
  Bound.Body.insert(Bound.Body.begin(), std::move(Use));

  PipelineOptions Options;
  Options.SourceTypeHints.emplace(Function.Entry, Hint);
  std::map<va_t, HighFunc> Refinements{{Function.Entry, std::move(Bound)}};
  std::map<va_t, std::string> Diagnostics;
  EXPECT_EQ(inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics,
                                        {}, {}, &Refinements),
            1U);
  ASSERT_EQ(Options.SourceTypeHints.at(Function.Entry).Parameters.size(), 2U);
  EXPECT_EQ(Options.SourceTypeHints.at(Function.Entry).Parameters[1].Type->Size,
            4U);
  EXPECT_EQ(inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics,
                                        {}, {}, &Refinements),
            0U);
}

TEST(ObjCSourceProjection, NativeDependencyGraphTracksMissingAndFinalEvidence) {
  NativeDependencyFixture F;
  F.call(0, 0x3000);
  F.call(2, 0x4000);
  NativeSourceDependencyEvidence Evidence;
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_FALSE(Evidence.InventoryComplete);
  EXPECT_FALSE(Evidence.TargetsComplete);
  EXPECT_EQ(Evidence.MissingFunctions, (std::set<va_t>{0x4000}));
  LowFunc Added;
  Added.Entry = 0x4000;
  F.Result.LowFuncs.push_back(std::move(Added));
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_TRUE(Evidence.InventoryComplete);
  EXPECT_TRUE(Evidence.TargetsComplete);
  EXPECT_TRUE(Evidence.MissingFunctions.empty());
  EXPECT_EQ(Evidence.Calls.size(), 2U);
  BinaryImage Other;
  EXPECT_THROW(walkObjCNativeDependencies(Other, F.Result, &Evidence),
               std::invalid_argument);
}

} // namespace

TEST(ObjCSourceProjection,
     OrdinarySynchronizationAnnotationsDoNotInventHandlers) {
  for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
    Projection P;
    auto &Metadata = P.Func.ExceptionMetadata.emplace();
    Metadata.Encoding = ExceptionEncoding::CompactUnwind;
    Metadata.Compact.emplace();
    auto &ObjC = Metadata.ObjC.emplace();
    ObjC.RuntimeCalls = {
        {0x1000, 0x2000, "objc_sync_enter", ObjCRuntimeCallKind::SyncEnter},
        {0x1004, 0x2004, "objc_sync_exit", ObjCRuntimeCallKind::SyncExit},
        {0x1008, 0x2008, "objc_release", ObjCRuntimeCallKind::ARCCleanup}};
    if (Mutation == 1)
      ObjC.LandingPads.emplace_back();
    if (Mutation == 2)
      ObjC.UsesFragileSetjmp = true;
    if (Mutation == 3)
      ObjC.UsesMSVCTables = true;
    if (Mutation == 4)
      ObjC.Runtime = ObjCRuntimeKind::GNU;
    if (Mutation == 5)
      ObjC.RuntimeCalls[0].Kind = ObjCRuntimeCallKind::Throw;
    if (Mutation == 6)
      ObjC.RuntimeCalls[0].Kind = ObjCRuntimeCallKind::BeginCatch;
    if (Mutation == 7)
      ObjC.RuntimeCalls[0].Kind = ObjCRuntimeCallKind::EndCatch;
    if (Mutation == 8)
      Metadata.Compact->HasLSDA = true;
    if (Mutation == 9)
      Metadata.PersonalityVA = 0x2000;
    EXPECT_EQ(P.limitation().empty(), Mutation == 0)
        << Mutation << ':' << P.limitation();
  }
}
