#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceEvidence.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace neverd;

TEST(CSourceAnchors, ArithmeticNavigationDoesNotSupplyLibraryCoverage) {
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.Size = 4;
  const auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeVar(Input),
                                       HighExpr::makeConst(13, 4));
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "computed_return";
  Function.ReturnType = NdType::makeInt(4, false);
  Function.Params = {{"arg0", Function.ReturnType}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = 0x1020;
  Return.RetVal = Sum;
  Function.Body = {Return};
  const auto Computation =
      observeHighComputation(Function.Entry, {0x1010, 2}, Sum);
  ASSERT_TRUE(Computation);
  HighSourceMap Sources = {
      *Computation,
      observeHighStatement(Function.Entry, {Return.Addr, 0}, Return)};
  std::vector<sigs::LibraryRecognition> Recognitions(1);
  Recognitions[0].Function = Function.Entry;
  Recognitions[0].Isolated = true;
  Recognitions[0].Occurrences = {{0x1010, 2}};
  CEmitterOptions Options;
  std::string Ordinary;
  llvm::raw_string_ostream Plain(Ordinary);
  ASSERT_TRUE(HighCEmitter().emit({Function}, Plain, Options));
  for (bool Library : {false, true}) {
    SCOPED_TRACE(Library);
    CSourceMap Map;
    Map.HighSources = &Sources;
    if (Library)
      Map.Recognitions = &Recognitions;
    Options.SourceMap = &Map;
    std::string Mapped;
    llvm::raw_string_ostream Out(Mapped);
    ASSERT_TRUE(HighCEmitter().emit({Function}, Out, Options));
    EXPECT_EQ(Mapped, Ordinary);
    ASSERT_EQ(Map.Anchors.size(), 1u);
    const auto &Anchor = Map.Anchors.front();
    EXPECT_EQ(Anchor.Function, Function.Entry);
    EXPECT_EQ(Anchor.Occurrences,
              (std::vector<sigs::LibraryOccurrence>{{0x1020, 0}, {0x1010, 2}}));
    EXPECT_TRUE(llvm::StringRef(Mapped)
                    .slice(Anchor.Span.Begin, Anchor.Span.End)
                    .contains(" + "));
    if (Library) {
      ASSERT_EQ(Map.Regions.size(), 1u);
      EXPECT_FALSE(Map.Regions[0].Mapped);
      EXPECT_TRUE(Map.Regions[0].Spans.empty());
    }
  }
}

TEST(CSourceAnchors, LLVMMissingValuesNameEachConsumerWithOrWithoutNavigation) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool Optimize : {false, true}) {
      SCOPED_TRACE(testing::Message() << int(A) << ':' << Optimize);
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      auto Module = llvm::parseAssemblyString(R"(
declare i64 @missing() "neverd.source.unknown-value"
declare i64 @consume(i64)
define i64 @observe() {
  %unavailable = call i64 @missing()
  %first = call i64 @consume(i64 %unavailable)
  %second = call i64 @consume(i64 %unavailable)
  %sum = add i64 %first, %second
  ret i64 %sum
})",
                                              Diagnostic, Context);
      ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
      auto *Function = Module->getFunction("observe");
      LLVMSourceMap Sources;
      Sources.Functions[0x1000] = Function;
      va_t Address = 0x1010;
      for (auto &Instruction : Function->getEntryBlock()) {
        Sources.Observations.push_back({0x1000, {Address, 0}, &Instruction});
        Address += 0x10;
      }
      if (Optimize)
        Pipeline::optimizeOrPromoteModule(*Module, &Sources);
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.LLVMSources = &Sources;
      std::string Ordinary, Mapped, Before, After;
      llvm::raw_string_ostream Plain(Ordinary), Out(Mapped), BeforeOS(Before),
          AfterOS(After);
      Module->print(BeforeOS, nullptr);
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Plain, Options));
      CSourceMap Map;
      Map.RecordCallExpressions = true;
      Options.SourceMap = &Map;
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      Module->print(AfterOS, nullptr);
      EXPECT_EQ(Before, After);
      EXPECT_EQ(Ordinary, Mapped);
      for (va_t Consumer : {0x1020, 0x1030}) {
        const std::string Text =
            "no recovered machine state at 0x" + llvm::utohexstr(Consumer);
        EXPECT_NE(Mapped.find(Text), std::string::npos) << Mapped;
        bool Anchored = false;
        for (const auto &Anchor : Map.Anchors)
          if (llvm::StringRef(Mapped)
                  .slice(Anchor.Span.Begin, Anchor.Span.End)
                  .contains(Text))
            for (const auto &Occurrence : Anchor.Occurrences)
              Anchored |= Occurrence.Address == Consumer;
        EXPECT_TRUE(Anchored) << Mapped;
      }
      EXPECT_EQ(Mapped.find("no recovered machine state at 0x1010"),
                std::string::npos);
      EXPECT_EQ(Mapped.find("source address unavailable"), std::string::npos);
    }
}

TEST(CSourceAnchors, LLVMMissingValueOriginsNeverGuessAnAddress) {
  for (unsigned Mode = 0; Mode != 8; ++Mode) {
    SCOPED_TRACE(Mode);
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(R"(
declare i64 @missing() "neverd.source.unknown-value"
define i64 @observe() {
  %unavailable = call i64 @missing()
  ret i64 %unavailable
})",
                                            Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    auto *Function = Module->getFunction("observe");
    auto *Return = Function->getEntryBlock().getTerminator();
    LLVMSourceMap Sources;
    Sources.Functions[0x1000] = Function;
    Sources.Observations.push_back({0x1000, {0x1234, 0}, Return});
    switch (Mode) {
    case 0: // No provenance input.
      break;
    case 1: // An observation from a different function is not this return.
      Sources.Observations[0].Function = 0x2000;
      break;
    case 2: // An ancestor's location is not the consumer's location.
      Sources.Observations[0].ExpressionDerived = true;
      break;
    case 3: // A merged origin has no single address, in either order.
    case 4:
      Sources.Observations.push_back({0x1000, {0x5678, 0}, Return});
      if (Mode == 4)
        std::reverse(Sources.Observations.begin(), Sources.Observations.end());
      break;
    case 5: // An expired observation stays unknown.
      Sources.Observations[0].Value = nullptr;
      break;
    case 6: // Several suboperations of the same instruction agree.
      Sources.Observations.push_back({0x1000, {0x1234, 1}, Return});
      break;
    case 7: // Zero is valid only when an actual observation supplies it.
      Sources.Observations[0].Occurrence.Address = 0;
      break;
    }
    CEmitterOptions Options;
    if (Mode)
      Options.LLVMSources = &Sources;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
    const std::string Expected =
        Mode == 6   ? "no recovered machine state at 0x1234"
        : Mode == 7 ? "no recovered machine state at 0x0"
                    : "no recovered machine state (source address unavailable)";
    EXPECT_NE(Source.find(Expected), std::string::npos) << Source;
    if (Mode < 6)
      EXPECT_EQ(Source.find("no recovered machine state at 0x"),
                std::string::npos);
  }
}

TEST(CSourceAnchors, LLVMDeferredValuesKeepTheStoreAndInlineConsumerOrigins) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
declare i64 @missing() "neverd.source.unknown-value"
declare i64 @consume(i64)
define i64 @observe(i1 %choice, ptr %out) {
entry:
  %unavailable = call i64 @missing()
  br i1 %choice, label %known, label %unknown
known:
  br label %join
unknown:
  br label %join
join:
  %value = phi i64 [42, %known], [%unavailable, %unknown]
  store i64 %value, ptr %out
  %incremented = add i64 %value, 7
  %first = call i64 @consume(i64 %incremented)
  %second = call i64 @consume(i64 %incremented)
  %sum = add i64 %first, %second
  ret i64 %sum
})",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  auto *Function = Module->getFunction("observe");
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = Function;
  for (auto &Block : *Function)
    for (auto &Instruction : Block)
      Sources.Observations.push_back(
          {0x1000,
           {llvm::isa<llvm::StoreInst>(Instruction)  ? 0x2010u
            : Instruction.getName() == "incremented" ? 0x2020u
                                                     : 0x1000u,
            0},
           &Instruction});
  CEmitterOptions Options;
  Options.LLVMSources = &Sources;
  std::string Plain, Mapped;
  llvm::raw_string_ostream PlainOS(Plain), MappedOS(Mapped);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, PlainOS, Options));
  CSourceMap Map;
  Options.SourceMap = &Map;
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, MappedOS, Options));
  EXPECT_EQ(Plain, Mapped);
  EXPECT_NE(Plain.find("_known"), std::string::npos) << Plain;
  EXPECT_NE(Plain.find("no recovered machine state at 0x2010"),
            std::string::npos)
      << Plain;
  EXPECT_NE(Plain.find("no recovered machine state at 0x2020"),
            std::string::npos)
      << Plain;
  EXPECT_EQ(Plain.find("no recovered machine state at 0x1000"),
            std::string::npos)
      << Plain;
  EXPECT_EQ(Plain.find("source address unavailable"), std::string::npos);
}

TEST(CSourceAnchors, UnrecoveredControlFlowKeepsItsMachineOccurrence) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool Optimize : {false, true}) {
      SCOPED_TRACE(testing::Message() << int(A) << ':' << Optimize);
      MedFunc Function;
      Function.Entry = 0x4000;
      Function.Name = "unrecovered_exit";
      Function.ReturnType = NdType::makeVoid();
      Function.ReturnsNoValue = true;
      auto &Block = Function.Blocks.emplace_back();
      Block.Id = 0;
      Block.StartAddr = Function.Entry;
      Block.EndAddr = 0x4014;
      MedOp Branch;
      Branch.Opcode = NdOp::INDIR_BR;
      Branch.Addr = 0x4010;
      Branch.OriginSeq = 4;
      MedVar Target;
      Target.Kind = MedVar::Temp;
      Target.Id = 1;
      Target.Size = A == Arch::X86 || A == Arch::ARM ? 4 : 8;
      Target.TheArch = A;
      Branch.addInput(Target);
      Block.Ops.push_back(Branch);
      llvm::LLVMContext Context;
      LLVMSourceMap Sources;
      MedLLVMEmitter Emitter;
      Emitter.setSourceMap(&Sources);
      Emitter.setPreserveUnknownSourceValues(true);
      auto Module = Emitter.emit({Function}, Context, "failure-map", A);
      ASSERT_TRUE(Module);
      if (Optimize)
        Pipeline::optimizeOrPromoteModule(*Module, &Sources);
      CSourceMap Map;
      CEmitterOptions Options;
      Options.LLVMSources = &Sources;
      Options.TheArch = A;
      std::string Ordinary, Mapped;
      llvm::raw_string_ostream Plain(Ordinary), Out(Mapped);
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Plain, Options));
      Options.SourceMap = &Map;
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      EXPECT_EQ(Mapped, Ordinary);
      bool Found = false;
      for (const auto &Anchor : Map.Anchors) {
        ASSERT_LE(Anchor.Span.End, Mapped.size());
        if (!llvm::StringRef(Mapped)
                 .slice(Anchor.Span.Begin, Anchor.Span.End)
                 .contains("__builtin_trap();"))
          continue;
        EXPECT_EQ(Anchor.Function, Function.Entry);
        for (const auto &Occurrence : Anchor.Occurrences)
          if (Occurrence.Address == Branch.Addr &&
              Occurrence.Sequence == Branch.OriginSeq)
            Found = true;
      }
      EXPECT_TRUE(Found) << Mapped;
    }
}

TEST(CSourceEvidence, RuntimeNamesAreOccurrenceBoundOnWindowsAndELF) {
  for (const char *Name : {"GetProcAddress", "dlsym"}) {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(
        std::string("@saved = global ptr null\ndeclare ptr @") + Name +
            R"((ptr, ptr)
define ptr @resolve(ptr %module, ptr %name) {
entry:
  %result = call ptr @)" +
            Name + R"((ptr %module, ptr %name)
  store ptr %result, ptr @saved
  ret ptr %result
})",
        Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    Module->setTargetTriple(llvm::Triple(Name[0] == 'G'
                                             ? "x86_64-pc-windows-msvc"
                                             : "aarch64-unknown-linux-gnu"));
    auto *F = Module->getFunction("resolve");
    auto *Call = llvm::cast<llvm::CallInst>(&F->getEntryBlock().front());
    LLVMSourceMap Sources;
    Sources.Functions[0x1000] = F;
    Sources.Observations.push_back({0x1000, {0x1010, 7}, Call});
    CSourceEvidence E{0x1000,
                      {0x1010, 7},
                      std::string(Name) + "(module, \"example_api\")",
                      "example_api_result"};
    auto *Type = Call->getFunctionType();
    auto *Callee = Call->getCalledOperand();
    nameLLVMSourceEvidence(Sources, {E});
    EXPECT_EQ(Call->getFunctionType(), Type);
    EXPECT_EQ(Call->getCalledOperand(), Callee);
    CSourceMap Map;
    Map.RecordCallExpressions = true;
    CEmitterOptions Options;
    Options.LLVMSources = &Sources;
    Options.TheArch = Name[0] == 'G' ? Arch::X64 : Arch::AArch64;
    Options.Format = Name[0] == 'G' ? BinaryFormat::COFF : BinaryFormat::ELF;
    Options.SourceMap = &Map;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
    auto Result = annotateCSourceEvidence(Source, Map, {E});
    EXPECT_EQ(Result.Mapped, 1u);
    EXPECT_EQ(Result.Unmapped, 0u);
    EXPECT_NE(Result.Source.find("observed_example_api_result"),
              std::string::npos);
    EXPECT_NE(Result.Source.find(E.Text), std::string::npos);
    E.Function = 0x2000;
    auto Wrong = annotateCSourceEvidence(Source, Map, {E});
    EXPECT_EQ(Wrong.Source, Source);
    EXPECT_EQ(Wrong.Unmapped, 1u);
  }
}

TEST(CSourceEvidence, AmbiguousSpansAndHostileTextNeverAcquireCodeAuthority) {
  std::string Source = "left();\nright();\n";
  CSourceMap Map;
  Map.Anchors = {{1, {0, 7}, {{2, 3}}}};
  CSourceEvidence E{1, {2, 3}, "name*/\n#error injected\\\n", ""};
  auto Result = annotateCSourceEvidence(Source, Map, {E});
  EXPECT_EQ(Result.Mapped, 1u);
  EXPECT_EQ(Result.Source.find("name*/"), std::string::npos);
  EXPECT_EQ(Result.Source.find("\n#error"), std::string::npos);
  EXPECT_EQ(Result.Source.find("\\\n"), std::string::npos);
  EXPECT_EQ(Result.Source.find(" \n"), std::string::npos);
  EXPECT_TRUE(llvm::StringRef(Result.Source).ends_with(Source));
  Map.Anchors.push_back({1, {8, 16}, {{2, 3}}});
  Result = annotateCSourceEvidence(Source, Map, {E});
  EXPECT_EQ(Result.Mapped, 0u);
  EXPECT_EQ(Result.Unmapped, 1u);
  EXPECT_EQ(Result.Source, Source);
}

TEST(CSourceEvidence, HighCallExpressionRequiresSurvivingIdentityAndKind) {
  HighFunc Function;
  Function.Name = "resolve";
  Function.Entry = 0x1000;
  Function.ReturnType = NdType::makeInt(8, false);
  auto Call = HighExpr::makeCall("resolver", 0x2000, {});
  Call->Type = Function.ReturnType;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = 0x1020;
  Return.RetVal = Call;
  Function.Body = {Return};
  HighSourceMap Sources = {
      {Function.Entry, {0x1010, 3}, Call, HighSourceKind::CallExpression, {}}};
  CSourceMap Map;
  Map.HighSources = &Sources;
  CEmitterOptions Options;
  std::string Ordinary;
  llvm::raw_string_ostream OrdinaryOS(Ordinary);
  ASSERT_TRUE(HighCEmitter().emit({Function}, OrdinaryOS, Options));
  Options.SourceMap = &Map;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
  EXPECT_EQ(Source, Ordinary);
  EXPECT_EQ(Source.find('\x1f'), std::string::npos);
  CSourceEvidence Evidence{
      Function.Entry, {0x1010, 3}, "resolver(\"example\")", ""};
  auto Result = annotateCSourceEvidence(Source, Map, {Evidence});
  EXPECT_EQ(Result.Mapped, 1u);
  Call->Kind = ExprKind::Const;
  Call->ConstVal = 7;
  Source.clear();
  ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
  EXPECT_TRUE(Map.Anchors.empty());
}

// A marked render must spell what the ordinary one does, or the map is refused.
TEST(CSourceAnchors, SignedZeroPairSpellsTheSameWhenMarked) {
  HighFunc Function;
  Function.Name = "mapped_pair";
  Function.Entry = 0x1000;
  Function.ReturnType = NdType::makeInt(4);
  auto Call = HighExpr::makeCall("resolver", 0x2000, {});
  Call->Type = NdType::makeInt(4, true);
  const auto Zero = HighExpr::makeConst(0, 4);
  HighStmt Taken;
  Taken.Kind = StmtKind::Return;
  Taken.Addr = 0x1020;
  Taken.RetVal = HighExpr::makeConst(1, 4);
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Addr = 0x1010;
  If.Cond = HighExpr::makeUnary(
      NdOp::BOOL_NOT,
      HighExpr::makeBinop(NdOp::BOOL_OR,
                          HighExpr::makeBinop(NdOp::INT_EQUAL, Call, Zero),
                          HighExpr::makeBinop(NdOp::INT_SLESS, Call, Zero)));
  If.Body = {Taken};
  HighStmt Otherwise = Taken;
  Otherwise.Addr = 0x1030;
  Otherwise.RetVal = HighExpr::makeConst(0, 4);
  Function.Body = {If, Otherwise};
  const HighSourceMap Sources = {
      {Function.Entry, {0x1010, 3}, Call, HighSourceKind::CallExpression, {}}};
  // Library evidence is what makes the emitter render with markers at all.
  const std::vector<sigs::LibraryRecognition> Recognitions(1);
  CEmitterOptions Options;
  std::string Ordinary;
  llvm::raw_string_ostream OrdinaryOS(Ordinary);
  ASSERT_TRUE(HighCEmitter().emit({Function}, OrdinaryOS, Options));
  OrdinaryOS.flush();
  EXPECT_NE(Ordinary.find("> 0"), std::string::npos) << Ordinary;
  CSourceMap Map;
  Map.HighSources = &Sources;
  Map.Recognitions = &Recognitions;
  Options.SourceMap = &Map;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
  OS.flush();
  EXPECT_EQ(Source, Ordinary);
  EXPECT_FALSE(Map.Anchors.empty()) << Source;
}

TEST(CSourceAnchors, HighStatementRequiresObservedAddressAndKind) {
  HighFunc Function;
  Function.Name = "mapped_return";
  Function.Entry = 0x1000;
  Function.ReturnType = NdType::makeInt(4);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = 0x1012;
  Return.RetVal = HighExpr::makeConst(7, 4);
  Function.Body = {Return};
  HighSourceMap Observations = {
      {0x1000, {0x1012, 3}, {}, HighSourceKind::Statement, StmtKind::Return}};
  CSourceMap Map;
  Map.HighSources = &Observations;
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Ordinary, Mapped;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
  ASSERT_TRUE(HighCEmitter().emit({Function}, OrdinaryOS, Options));
  Options.SourceMap = &Map;
  ASSERT_TRUE(HighCEmitter().emit({Function}, MappedOS, Options));
  EXPECT_EQ(Ordinary, Mapped);
  ASSERT_EQ(Map.Anchors.size(), 1u);
  EXPECT_EQ(Map.Anchors[0].Function, 0x1000u);
  EXPECT_EQ(Map.Anchors[0].Occurrences,
            (std::vector<sigs::LibraryOccurrence>{{0x1012, 3}}));
  EXPECT_NE(llvm::StringRef(Mapped)
                .slice(Map.Anchors[0].Span.Begin, Map.Anchors[0].Span.End)
                .find("return"),
            llvm::StringRef::npos);
  EXPECT_FALSE(Map.Anchors[0].Span.Begin == 0);

  auto emitAgain = [&] {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
    EXPECT_TRUE(Map.Anchors.empty());
    EXPECT_EQ(Text, Ordinary);
  };
  Observations[0].StatementKind = StmtKind::Assign;
  emitAgain();
  Observations[0].StatementKind = StmtKind::Return;
  Observations[0].Occurrence.Address = 0x1013;
  emitAgain();
  Observations[0].Occurrence.Address = 0x1012;
  Observations.push_back(
      {0x1000, {0x1012, 4}, {}, HighSourceKind::Statement, StmtKind::Return});
  emitAgain();
  Observations.pop_back();
  Function.Body[0].IsPhiCopy = true;
  emitAgain();
}

TEST(CSourceAnchors, LLVMObservationsSurviveOnlyWithTheirFunctionAndValue) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module =
      llvm::parseAssemblyString("define i32 @mapped_return(i32 %value) { %sum "
                                "= add i32 %value, 7 ret i32 %sum }",
                                Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  auto *Function = Module->getFunction("mapped_return");
  ASSERT_NE(Function, nullptr);
  auto *Return = Function->getEntryBlock().getTerminator();
  LLVMSourceMap Observations;
  Observations.Functions[0x2000] = Function;
  Observations.Observations.push_back({0x2000, {0x2010, 2}, Return});
  CSourceMap Map;
  CEmitterOptions Options;
  Options.LLVMSources = &Observations;
  Options.TheArch = Arch::X64;
  std::string Ordinary, Mapped;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OrdinaryOS, Options));
  Options.SourceMap = &Map;
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, MappedOS, Options));
  EXPECT_EQ(Ordinary, Mapped);
  ASSERT_FALSE(Map.Anchors.empty());
  EXPECT_EQ(Map.Anchors[0].Occurrences[0].Address, 0x2010u);
  EXPECT_TRUE(llvm::StringRef(Mapped)
                  .slice(Map.Anchors[0].Span.Begin, Map.Anchors[0].Span.End)
                  .contains("return"));
  Observations.Observations[0].Function = 0x2001;
  std::string Refused;
  llvm::raw_string_ostream OS(Refused);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  EXPECT_EQ(Refused, Ordinary);
  EXPECT_TRUE(Map.Anchors.empty());
}
