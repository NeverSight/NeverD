#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceEvidence.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

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
    Map.LLVMSources = &Sources;
    CEmitterOptions Options;
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
  Map.LLVMSources = &Observations;
  CEmitterOptions Options;
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
