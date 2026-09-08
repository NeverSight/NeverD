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
