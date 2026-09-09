//===- ObjCSourceProjectionTests.cpp - Objective-C projection boundaries --===//

#include "../../../lib/sdk/capi/ObjCSourceProjection.h"
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
  Loop.Cond = flowCondition();
  Loop.Body = {Branch, flowAssignment()};
  P.Func.Body = {Loop, flowReturn()};
  EXPECT_FALSE(P.limitation().empty())
      << "continue reaches the loop test without defining the exit value";

  P.Func.Body.insert(P.Func.Body.begin(), flowAssignment(7));
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
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
  Call.CallExpr->IntrinsicId = Intrinsic::Ud2;
  P.Func.Body = {Call};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

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

} // namespace
