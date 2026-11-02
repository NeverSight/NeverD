//===- WindowsSEHFallbackTests.cpp - Native SEH annotation contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Irreducible regions retain native dispatch without inventing catch-all code.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/raw_ostream.h"

#include <stdexcept>

namespace {
using namespace neverd;

HighFunc fallbackFunction() {
  HighFunc Function;
  Function.Name = "native_seh_fallback";
  Function.Entry = 0x1000;
  Function.ReturnType = NdType::makeInt(4, true);
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.EHRange = {0x1000, 0x1100};
  Try.EHIsReducible = false;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(42, 4);
  Try.Body.push_back(std::move(Return));
  HighEHClause Except;
  Except.FilterOrActionVA = 0x1020;
  Except.HandlerVA = 0x1060;
  Except.State = 0;
  Except.FilterValue = HighExpr::makeConst(0, 4);
  Try.EHClauses.push_back(Except);
  HighEHClause Finally;
  Finally.Kind = HighEHClauseKind::SEHFinally;
  Finally.FilterOrActionVA = Finally.HandlerVA = 0x1040;
  Finally.State = 1;
  Try.EHClauses.push_back(Finally);
  Except.FilterOrActionVA = 0x1050;
  Except.HandlerVA = 0x1070;
  Except.State = 2;
  Except.ParseStatus = ExceptionParseStatus::Partial;
  Except.ContinuationVAs = {0x1080};
  Except.FilterValue.reset();
  Try.EHClauses.push_back(Except);
  Try.EHClauseBodies.resize(3);
  Function.Body.push_back(std::move(Try));
  return Function;
}

std::string emitFallback(const HighFunc &Function, Arch Architecture,
                         bool Structured = true, bool Comments = true) {
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Architecture;
  Options.Format = BinaryFormat::COFF;
  Options.StructuredExceptionSyntax = Structured;
  Options.EmitComments = Comments;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  EXPECT_TRUE(HighCEmitter().emit({Function}, OS, Options));
  return Source;
}

TEST(WindowsSEHFallback, PreservesEveryClauseWithoutInventingCatchAllCode) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (bool Structured : {false, true}) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(Structured);
      const auto Source =
          emitFallback(fallbackFunction(), Architecture, Structured);
      EXPECT_EQ(Source.find("__try {"), std::string::npos) << Source;
      EXPECT_EQ(Source.find("__except ("), std::string::npos) << Source;
      EXPECT_EQ(Source.find("__finally {"), std::string::npos) << Source;
      EXPECT_NE(Source.find("unstructured SEH region [0x1000, 0x1100)"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("filter @ 0x1020, handler @ 0x1060"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("action @ 0x1040, handler @ 0x1040"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("filter @ 0x1050, handler @ 0x1070"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("state=2, status=partial"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("continuation @ 0x1080"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("filter_value=EXCEPTION_CONTINUE_SEARCH"),
                std::string::npos)
          << Source;
      EXPECT_EQ(Source.find("EXCEPTION_EXECUTE_HANDLER"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("return "), std::string::npos) << Source;
    }
}

TEST(WindowsSEHFallback,
     RegistrationTopologyAndNarrowStateStoresRemainVisible) {
  auto Function = fallbackFunction();
  ExceptionFunction EH;
  EH.CodeRange = {0x1000, 0x1100};
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  RegistrationChainInfo Chain;
  Chain.HandlerVA = 0x2000;
  Chain.ScopeTableVA = 0x3000;
  Chain.RegistrationOffset = -16;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = -2;
  Chain.ChainInstallVA = 0x1008;
  Chain.ChainRemoveVA = 0x1090;
  Chain.Scopes = {{-2, 0x1020, 0x1060, false},
                  {0, 0, 0x1040, true},
                  {1, 0x1050, 0x1070, false}};
  Chain.TryLevelStores = {{0x1010, 0x1017, 0, 4}, {0x1018, 0x101c, 255, 1}};
  EH.Registration = std::move(Chain);
  Function.ExceptionMetadata = std::move(EH);
  const auto Source = emitFallback(Function, Arch::X86);
  EXPECT_NE(Source.find("registration.handler=0x2000, scope_table=0x3000"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("registration.scope[0]: except, enclosing=-2"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("registration.scope[1]: finally, enclosing=0"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("registration.scope[2]: except, enclosing=1"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("store=0x1018, end=0x101C, level=255, width=1"),
            std::string::npos)
      << Source;
}

TEST(WindowsSEHFallback, DoesNotSilentlyDiscardAnUnboundHandlerBody) {
  auto Function = fallbackFunction();
  Function.Body.front().EHClauseBodies.front() = Function.Body.front().Body;
  EXPECT_THROW(emitFallback(Function, Arch::X86), std::invalid_argument);
}

// A unit that lists the definitions it leaves out names that one, with what
// it threw, and writes the others: few enough for one writer, or as many as
// worker threads write.
TEST(WindowsSEHFallback, AUnitLeavesOutOnlyTheDefinitionItCannotWrite) {
  for (size_t Count : {size_t(3), size_t(40)}) {
    SCOPED_TRACE(Count);
    std::vector<HighFunc> Functions;
    for (size_t I = 0; I < Count; ++I) {
      Functions.push_back(fallbackFunction());
      Functions.back().Name = "native_seh_" + std::to_string(I);
      Functions.back().Entry = 0x1000 + 0x1000 * I;
    }
    HighFunc &Broken = Functions[Count / 2];
    Broken.Body.front().EHClauseBodies.front() = Broken.Body.front().Body;
    CEmitterOptions Options;
    Options.EmitIncludes = false;
    Options.TheArch = Arch::X86;
    Options.Format = BinaryFormat::COFF;
    std::vector<CEmitterOptions::RefusedDefinition> Refused;
    Options.RefusedDefinitions = &Refused;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    ASSERT_EQ(Refused.size(), 1u) << Source;
    EXPECT_EQ(Refused.front().Entry, Broken.Entry);
    EXPECT_EQ(Refused.front().Name, Broken.Name);
    EXPECT_EQ(Refused.front().Reason,
              "unstructured SEH view requires the original native handler "
              "body");
    for (const HighFunc &Function : Functions)
      EXPECT_EQ(Source.find(Function.Name + "(void) {") != std::string::npos,
                &Function != &Broken)
          << Function.Name << "\n"
          << Source;
    // Without the list, the first definition that fails fails the unit.
    Options.RefusedDefinitions = nullptr;
    std::string Unit;
    llvm::raw_string_ostream UnitOS(Unit);
    EXPECT_THROW(HighCEmitter().emit(Functions, UnitOS, Options),
                 std::invalid_argument);
  }
}

TEST(WindowsSEHFallback, AUnitLeavesOutADefinitionWhoseRequirementsFail) {
  std::vector<HighFunc> Functions;
  for (size_t I = 0; I < 4; ++I) {
    Functions.push_back(fallbackFunction());
    Functions.back().Name = "native_seh_" + std::to_string(I);
    Functions.back().Entry = 0x1000 + 0x1000 * I;
  }
  // The second one's C cannot be written; x86 C cannot meet the third one's
  // requirements, which are checked before anything is rendered.
  Functions[1].Body.front().EHClauseBodies.front() =
      Functions[1].Body.front().Body;
  Functions[2].ReturnProtection.emplace();
  // As a whole unit is written without a C++ dialect.
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.StructuredExceptionSyntax = false;
  Options.TheArch = Arch::X86;
  Options.Format = BinaryFormat::COFF;
  std::vector<CEmitterOptions::RefusedDefinition> Refused;
  Options.RefusedDefinitions = &Refused;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  ASSERT_EQ(Refused.size(), 2u) << Source;
  EXPECT_EQ(Refused[0].Entry, Functions[2].Entry);
  EXPECT_EQ(Refused[0].Name, Functions[2].Name);
  EXPECT_EQ(Refused[0].Reason,
            "AArch64 return protection requires AArch64 source emission");
  EXPECT_EQ(Refused[1].Entry, Functions[1].Entry);
  EXPECT_EQ(Refused[1].Reason,
            "unstructured SEH view requires the original native handler "
            "body");
  for (size_t I = 0; I < Functions.size(); ++I)
    EXPECT_EQ(Source.find(Functions[I].Name + "(void) {") != std::string::npos,
              I == 0 || I == 3)
        << Functions[I].Name << "\n"
        << Source;
  // No rendering reads the function whose requirements failed.
  EXPECT_EQ(Source.find(Functions[2].Name), std::string::npos) << Source;
  // Without the list, with source names, or with structured exception
  // syntax, which writes funclets inside their parents, the requirements fail
  // the unit.
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    SCOPED_TRACE(Variant);
    std::vector<CSourceName> SourceNames;
    Options.RefusedDefinitions = Variant ? &Refused : nullptr;
    Options.SourceNames = Variant == 1 ? &SourceNames : nullptr;
    Options.StructuredExceptionSyntax = Variant == 2;
    std::string Unit;
    llvm::raw_string_ostream UnitOS(Unit);
    EXPECT_THROW(HighCEmitter().emit(Functions, UnitOS, Options),
                 std::invalid_argument);
  }
}

TEST(WindowsSEHFallback, NativeContinuationKeepsItsBodyAfterReturn) {
  for (StmtKind Kind : {StmtKind::SEHTry, StmtKind::CxxTry}) {
    SCOPED_TRACE(unsigned(Kind));
    auto Function = fallbackFunction();
    ExceptionFunction EH;
    EH.CodeRange = {0x1000, 0x1100};
    Function.ExceptionMetadata = std::move(EH);
    auto &Try = Function.Body.front();
    Try.Kind = Kind;
    if (Kind == StmtKind::CxxTry)
      for (auto &Clause : Try.EHClauses)
        Clause.Kind = HighEHClauseKind::CxxCatch;
    HighStmt Resume;
    Resume.Kind = StmtKind::Return;
    Resume.Addr = 0x1080;
    Resume.RetVal = HighExpr::makeConst(43, 4);
    Try.Body.push_back(std::move(Resume));
    const auto Source = emitFallback(Function, Arch::X86);
    const size_t Entry = Source.find("L_1080:");
    ASSERT_NE(Entry, std::string::npos) << Source;
    EXPECT_LT(Source.find("return 0x2A;"), Entry) << Source;
    EXPECT_NE(Source.find("return 0x2B;", Entry), std::string::npos) << Source;
  }
}

TEST(WindowsSEHFallback, IrreducibleCxxRegionDoesNotInventLexicalProtection) {
  auto Function = fallbackFunction();
  auto &Try = Function.Body.front();
  Try.Kind = StmtKind::CxxTry;
  Try.EHClauses.clear();
  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.HandlerVA = 0x1040;
  Catch.State = 3;
  Catch.CatchObjectOffset = -32;
  Catch.TypeDescriptorVA = 0x2000;
  Catch.ContinuationVAs = {0x1080};
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.CodeRange = {0x1000, 0x1100};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 3;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {1, 0}, {2, 0x1060}};
  CxxTryBlock NativeTry;
  NativeTry.TryLow = 0;
  NativeTry.TryHigh = 1;
  NativeTry.CatchHigh = 3;
  CxxCatchHandler NativeCatch;
  NativeCatch.HandlerVA = Catch.HandlerVA;
  NativeCatch.CatchObjectOffset = Catch.CatchObjectOffset;
  NativeCatch.TypeDescriptorVA = Catch.TypeDescriptorVA;
  NativeCatch.ContinuationVAs = Catch.ContinuationVAs;
  NativeTry.Handlers = {NativeCatch};
  Cxx.TryBlocks = {NativeTry};
  Cxx.IPMap = {{0x1000, 0}, {0x1020, -1}, {0x1040, 3}};
  Try.EHClauses = {Catch};
  Try.EHClauseBodies.resize(1);
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (bool Structured : {false, true})
      for (bool Comments : {false, true}) {
        const auto Source =
            emitFallback(Function, Architecture, Structured, Comments);
        EXPECT_EQ(Source.find("try {"), std::string::npos) << Source;
        EXPECT_EQ(Source.find(" catch ("), std::string::npos) << Source;
        EXPECT_NE(Source.find("C++ unwind-table region [0x1000, 0x1100)"),
                  std::string::npos)
            << Source;
        EXPECT_NE(Source.find("handler @ 0x1040"), std::string::npos) << Source;
        EXPECT_NE(Source.find("type descriptor @ 0x2000, state=3"),
                  std::string::npos)
            << Source;
        EXPECT_NE(Source.find("catch-object offset=-32, continuation @ 0x1080"),
                  std::string::npos)
            << Source;
        EXPECT_NE(Source.find("cxx.unwind[3]: to_state=2"), std::string::npos)
            << Source;
        EXPECT_NE(Source.find("cxx.try[0]: states=0..1, catch_high=3"),
                  std::string::npos)
            << Source;
        EXPECT_NE(Source.find("cxx.ip_state[2]: ip=0x1040, state=3"),
                  std::string::npos)
            << Source;
      }
}

MedFunc nativeFilterFunction(bool HasSideEffect) {
  MedFunc Function;
  Function.Name = "native_filter_proof";
  Function.Entry = 0x401000;
  Function.ReturnType = NdType::makeInt(4, true);
  Function.CC = CallingConv::CDECL;
  Function.Blocks.resize(4);
  for (int I = 0; I != 4; ++I) {
    auto &Block = Function.Blocks[I];
    Block.Id = I;
    Block.StartAddr = Function.Entry + 0x20 * I;
    Block.EndAddr = Block.StartAddr + 0x10;
  }
  auto AddCall = [&](int Block, va_t Callee) {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Function.Blocks[Block].StartAddr;
    Call.addInput(MedVar::makeConst(Callee, 4));
    Function.Blocks[Block].Ops.push_back(std::move(Call));
  };
  AddCall(0, 0x401090);
  AddCall(2, 0x4010B0);
  if (HasSideEffect)
    AddCall(1, 0x4010A0);
  for (int Block : {1, 3}) {
    MedVar Value;
    Value.Kind = MedVar::Reg;
    Value.TheArch = Arch::X86;
    Value.RegOff = getTargetRegInfo(Arch::X86).IntReturnReg;
    Value.Size = 4;
    Value.Id = 100 + Block;
    Value.SSAVer = 1;
    MedOp Define;
    Define.Opcode = NdOp::COPY;
    Define.Addr = Function.Blocks[Block].StartAddr + 1;
    Define.Output = Value;
    Define.addInput(MedVar::makeConst(Block == 1 ? 1 : 0, 4));
    Function.Blocks[Block].Ops.push_back(std::move(Define));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Function.Blocks[Block].StartAddr + 4;
    Return.addInput(Value);
    Function.Blocks[Block].Ops.push_back(std::move(Return));
  }
  Function.Blocks[0].Succs = {3};
  Function.Blocks[2].Succs = {3};
  Function.Blocks[3].Preds = {0, 2};
  for (int Block : {0, 2}) {
    MedOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = Function.Blocks[Block].StartAddr + 8;
    Branch.addInput(MedVar::makeConst(0x401060, 4));
    Function.Blocks[Block].Ops.push_back(std::move(Branch));
  }
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  EH.Personality = ExceptionPersonality::ExceptHandler3;
  EH.PersonalityVA = 0x401100;
  EH.CodeRange = {Function.Entry, Function.Entry + 0x70};
  EH.Registration.emplace().Scopes = {{-1, 0x401020, 0x401040, false}};
  auto &States = Function.RegistrationStates.emplace();
  States.Complete = true;
  States.Blocks = {{0, {0x401000, 0x401010}, {0}, false, false, true},
                   {1, {0x401020, 0x401030}, {-1}, false, true, true},
                   {2, {0x401040, 0x401050}, {-1}, false, false, true},
                   {3, {0x401060, 0x401070}, {-1}, false, false, true}};
  return Function;
}

TEST(WindowsSEHFallback, SideEffectingNativeFilterRetainsItsOriginalCode) {
  const auto Function =
      MedToHighConverter().convert(nativeFilterFunction(true), Arch::X86);
  EXPECT_EQ(Function.StructuredExceptionRegions, 0u);
  EXPECT_NE(Function.UnstructuredExceptionRegions, 0u);
  const auto Source = emitFallback(Function, Arch::X86);
  EXPECT_EQ(Source.find("__except ("), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_4010A0("), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_4010B0("), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_401020:"), std::string::npos) << Source;
  const size_t Filter = Source.find("L_401020:");
  EXPECT_NE(Source.find("sub_4010A0(", Filter), std::string::npos) << Source;
  EXPECT_NE(Source.find("filter @ 0x401020, handler @ 0x401040"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("filter_value="), std::string::npos) << Source;
}

TEST(WindowsSEHFallback, ProvenPureNativeFilterCanBeStructured) {
  const auto Function =
      MedToHighConverter().convert(nativeFilterFunction(false), Arch::X86);
  EXPECT_EQ(Function.StructuredExceptionRegions, 1u);
  const auto Source = emitFallback(Function, Arch::X86);
  EXPECT_NE(Source.find("__except (EXCEPTION_EXECUTE_HANDLER)"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("sub_401020("), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_4010B0("), std::string::npos) << Source;
}

} // namespace
