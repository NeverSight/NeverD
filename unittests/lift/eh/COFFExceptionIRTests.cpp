//===- COFFExceptionIRTests.cpp - Windows EH IR carriage tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <optional>

#ifndef NEVERD_RUNTIME_FIXTURE_COMPILER
#define NEVERD_RUNTIME_FIXTURE_COMPILER ""
#endif

namespace {

using namespace neverd;

bool hasHostFixtureCompiler() {
  return NEVERD_RUNTIME_FIXTURE_COMPILER[0] != '\0';
}

#define REQUIRE_HOST_FIXTURE_COMPILER()                                        \
  do {                                                                         \
    if (!hasHostFixtureCompiler())                                             \
      GTEST_SKIP() << "host C syntax fixtures require a GNU-style GCC/Clang "  \
                      "driver";                                                \
  } while (false)

struct CompilerResult {
  int ExitCode = -1;
  std::string Output;
  std::string Error;
};

CompilerResult runCCompiler(llvm::StringRef Source,
                            llvm::ArrayRef<llvm::StringRef> Options) {
  CompilerResult Result;
  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  llvm::SmallString<128> ErrorPath;
  std::error_code EC =
      llvm::sys::fs::createTemporaryFile("neverd-windows-eh-c", "c", InputPath);
  if (EC) {
    Result.Error = EC.message();
    return Result;
  }
  llvm::FileRemover RemoveInput(InputPath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-windows-eh-c", "stdout",
                                          OutputPath);
  if (EC) {
    Result.Error = EC.message();
    return Result;
  }
  llvm::FileRemover RemoveOutput(OutputPath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-windows-eh-c", "stderr",
                                          ErrorPath);
  if (EC) {
    Result.Error = EC.message();
    return Result;
  }
  llvm::FileRemover RemoveError(ErrorPath);

  {
    llvm::raw_fd_ostream Input(InputPath, EC);
    if (EC) {
      Result.Error = EC.message();
      return Result;
    }
    Input << Source;
  }

  llvm::SmallVector<llvm::StringRef, 12> Arguments;
  Arguments.push_back(NEVERD_RUNTIME_FIXTURE_COMPILER);
  Arguments.append(Options.begin(), Options.end());
  Arguments.push_back(InputPath);
  std::optional<llvm::StringRef> Redirects[] = {std::nullopt, OutputPath.str(),
                                                ErrorPath.str()};
  Result.ExitCode = llvm::sys::ExecuteAndWait(
      NEVERD_RUNTIME_FIXTURE_COMPILER, Arguments, std::nullopt, Redirects,
      /*SecondsToWait=*/30, /*MemoryLimit=*/0, &Result.Error);

  auto Read = [](llvm::StringRef Path) {
    auto Buffer = llvm::MemoryBuffer::getFile(Path);
    return Buffer ? (*Buffer)->getBuffer().str() : std::string{};
  };
  Result.Output = Read(OutputPath);
  Result.Error += Read(ErrorPath);
  return Result;
}

std::string emitHighC(llvm::ArrayRef<HighFunc> Funcs,
                      bool EmitComments = true) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.EmitComments = EmitComments;
  EXPECT_TRUE(HighCEmitter().emit(
      std::vector<HighFunc>(Funcs.begin(), Funcs.end()), OS, Options));
  OS.flush();
  return Source;
}

std::string emitLLVMC(llvm::Module &Module, bool EmitComments = true) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.EmitComments = EmitComments;
  EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  return Source;
}

MedFunc makeWindowsHandlerFixture(llvm::StringRef Name, va_t HandlerMarkerVA,
                                  unsigned HandlerBlockCopies = 1) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  constexpr va_t ContinuationVA = FunctionVA + 0x30;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = FunctionVA + 0x10;
  MedOp ProtectedMarker;
  ProtectedMarker.Opcode = NdOp::CALL;
  ProtectedMarker.Addr = FunctionVA + 4;
  ProtectedMarker.addInput(MedVar::makeConst(0x140008000, 8));
  Protected.Ops.push_back(std::move(ProtectedMarker));
  Func.Blocks.push_back(std::move(Protected));

  for (unsigned I = 0; I < HandlerBlockCopies; ++I) {
    MedBlock Handler;
    Handler.Id = static_cast<int>(I + 1);
    Handler.StartAddr = HandlerVA;
    Handler.EndAddr = ContinuationVA;
    MedOp Marker;
    Marker.Opcode = NdOp::CALL;
    Marker.Addr = HandlerVA;
    Marker.addInput(MedVar::makeConst(HandlerMarkerVA + I * 0x10, 8));
    Handler.Ops.push_back(std::move(Marker));
    Func.Blocks.push_back(std::move(Handler));
  }

  MedBlock Continuation;
  Continuation.Id = static_cast<int>(HandlerBlockCopies + 1);
  Continuation.StartAddr = ContinuationVA;
  Continuation.EndAddr = FunctionVA + 0x40;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ContinuationVA;
  Continuation.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Continuation));
  return Func;
}

ExceptionFunction makeFH3Metadata(llvm::ArrayRef<va_t> HandlerVAs) {
  constexpr va_t FunctionVA = 0x140001000;
  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;

  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}};
  Cxx.IPMap = {
      {FunctionVA, 0}, {FunctionVA + 0x10, -1}, {FunctionVA + 0x20, 1}};
  CxxTryBlock NativeTry;
  NativeTry.TryLow = 0;
  NativeTry.TryHigh = 0;
  NativeTry.CatchHigh = 1;
  for (va_t HandlerVA : HandlerVAs) {
    CxxCatchHandler Catch;
    Catch.HandlerVA = HandlerVA;
    Catch.ContinuationVAs.push_back(FunctionVA + 0x30);
    NativeTry.Handlers.push_back(std::move(Catch));
  }
  Cxx.TryBlocks.push_back(std::move(NativeTry));
  EH.Cxx = std::move(Cxx);
  return EH;
}

TEST(COFFExceptionIR,
     HighCSEHFH3AndFH4BodiesAreCommentsAndActiveDefinitionsTrap) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t HandlerVA = FunctionVA + 0x20;

  MedFunc SEH = makeWindowsHandlerFixture("analysis_seh", 0x140009000);
  ExceptionFunction SEHMetadata;
  SEHMetadata.CodeRange = {FunctionVA, FunctionVA + 0x40};
  SEHMetadata.ParseStatus = ExceptionParseStatus::Complete;
  SEHMetadata.Personality = ExceptionPersonality::CSpecificHandler;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = HandlerVA;
  SEHExceptionInfo ScopeTable;
  ScopeTable.Scopes.push_back(Scope);
  SEHMetadata.SEH = std::move(ScopeTable);
  SEH.ExceptionMetadata = std::move(SEHMetadata);

  MedFunc FH3 = makeWindowsHandlerFixture("analysis_fh3", 0x14000a000);
  FH3.ExceptionMetadata = makeFH3Metadata({HandlerVA});

  MedFunc FH4 = makeWindowsHandlerFixture("analysis_fh4", 0x14000b000);
  FH4.ExceptionMetadata = makeFH3Metadata({HandlerVA});
  FH4.ExceptionMetadata->Personality = ExceptionPersonality::CxxFrameHandler4;
  FH4.ExceptionMetadata->Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH4;

  std::vector<HighFunc> Functions;
  Functions.push_back(MedToHighConverter().convert(SEH, Arch::X64));
  Functions.push_back(MedToHighConverter().convert(FH3, Arch::X64));
  Functions.push_back(MedToHighConverter().convert(FH4, Arch::X64));
  std::string Source = emitHighC(Functions);

  EXPECT_NE(Source.find("neverd.analysis-only"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("#if 0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  EXPECT_NE(Source.find("__try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("__except (EXCEPTION_EXECUTE_HANDLER)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("catch ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("// __try"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__except (1)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_140009000();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000A000();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000B000();"), std::string::npos) << Source;
}

TEST(COFFExceptionIR,
     HighCSharedAmbiguousCrossingAndExternalHandlersStayAnalysisOnly) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  std::vector<MedFunc> MedFunctions;

  MedFunc Shared = makeWindowsHandlerFixture("analysis_shared", 0x14000c000);
  Shared.ExceptionMetadata = makeFH3Metadata({HandlerVA, HandlerVA});
  MedFunctions.push_back(std::move(Shared));

  MedFunc Ambiguous =
      makeWindowsHandlerFixture("analysis_ambiguous", 0x14000d000, 2);
  Ambiguous.ExceptionMetadata = makeFH3Metadata({HandlerVA});
  MedFunctions.push_back(std::move(Ambiguous));

  MedFunc Crossing =
      makeWindowsHandlerFixture("analysis_crossing", 0x14000e000);
  Crossing.Blocks[1].StartAddr = FunctionVA + 8;
  Crossing.Blocks[1].EndAddr = FunctionVA + 0x18;
  Crossing.Blocks[1].Ops.front().Addr = FunctionVA + 8;
  Crossing.ExceptionMetadata = makeFH3Metadata({FunctionVA + 8});
  MedFunctions.push_back(std::move(Crossing));

  MedFunc External =
      makeWindowsHandlerFixture("analysis_external", 0x14000f000, 0);
  External.ExceptionMetadata = makeFH3Metadata({0x180001000});
  MedFunctions.push_back(std::move(External));

  std::vector<HighFunc> Functions;
  for (const MedFunc &Med : MedFunctions)
    Functions.push_back(MedToHighConverter().convert(Med, Arch::X64));
  std::string Source = emitHighC(Functions);
  EXPECT_NE(Source.find("sub_14000C000();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000D000();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000E000();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("handler @ 0x180001000"), std::string::npos) << Source;
  EXPECT_NE(Source.find("catch ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("#if 0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, HighCCxxTryRendersCatchSyntaxInReadableListing) {
  HighFunc Function;
  Function.Name = "cxx_try_fn";
  Function.Entry = 0x140001000;
  Function.ReturnType = NdType::makeVoid();

  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHRange = {0x140001000, 0x140001040};
  HighStmt BodyRet;
  BodyRet.Kind = StmtKind::Return;
  Try.Body.push_back(BodyRet);

  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.TypeName = "int";
  Catch.HandlerVA = 0x140001020;
  Try.EHClauses.push_back(Catch);
  HighStmt CatchRet;
  CatchRet.Kind = StmtKind::Return;
  Try.EHClauseBodies.push_back({CatchRet});
  Function.Body.push_back(std::move(Try));
  Function.ExceptionMetadata = makeFH3Metadata({0x140001020});

  std::string Source = emitHighC({Function});
  const size_t Open = Source.find("cxx_try_fn(void) {");
  const size_t TryPos = Source.find("try {");
  const size_t CatchPos = Source.find("catch (int)");
  ASSERT_NE(Open, std::string::npos) << Source;
  ASSERT_NE(TryPos, std::string::npos) << Source;
  ASSERT_NE(CatchPos, std::string::npos) << Source;
  EXPECT_LT(Open, TryPos);
  EXPECT_LT(TryPos, CatchPos);
  EXPECT_EQ(Source.find("#if 0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("// try {"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, HighCCxxCatchRendersQualifiedTypeName) {
  HighFunc Function;
  Function.Name = "cxx_qualified_catch";
  Function.Entry = 0x140001000;
  Function.ReturnType = NdType::makeVoid();

  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHRange = {0x140001000, 0x140001040};
  HighStmt BodyRet;
  BodyRet.Kind = StmtKind::Return;
  Try.Body.push_back(BodyRet);

  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.TypeName = "Ns::Outer::Inner";
  Catch.Adjectives = 0x9; // const | reference
  Catch.HandlerVA = 0x140001020;
  Try.EHClauses.push_back(Catch);
  HighStmt CatchRet;
  CatchRet.Kind = StmtKind::Return;
  Try.EHClauseBodies.push_back({CatchRet});
  Function.Body.push_back(std::move(Try));
  Function.ExceptionMetadata = makeFH3Metadata({0x140001020});

  std::string Source = emitHighC({Function});
  EXPECT_NE(Source.find("catch (const Ns::Outer::Inner &)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("/* Ns::Outer::Inner"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, HighCCxxCatchRendersRecoveredTypeName) {
  HighFunc Function;
  Function.Name = "cxx_named_catch";
  Function.Entry = 0x140001000;
  Function.ReturnType = NdType::makeVoid();

  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHRange = {0x140001000, 0x140001040};
  HighStmt BodyRet;
  BodyRet.Kind = StmtKind::Return;
  Try.Body.push_back(BodyRet);

  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.TypeName = "ProbeError";
  Catch.Adjectives = 0x9; // const | reference
  Catch.HandlerVA = 0x140001020;
  Try.EHClauses.push_back(Catch);
  HighStmt CatchRet;
  CatchRet.Kind = StmtKind::Return;
  Try.EHClauseBodies.push_back({CatchRet});
  Function.Body.push_back(std::move(Try));
  Function.ExceptionMetadata = makeFH3Metadata({0x140001020});

  std::string Source = emitHighC({Function});
  EXPECT_NE(Source.find("catch (const ProbeError &)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("type @ 0x"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, HighCCxxCatchRendersConstReferenceTypeAndHandlerNote) {
  HighFunc Function;
  Function.Name = "cxx_typed_catch";
  Function.Entry = 0x140001000;
  Function.ReturnType = NdType::makeVoid();

  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHRange = {0x140001000, 0x140001040};
  HighStmt BodyRet;
  BodyRet.Kind = StmtKind::Return;
  Try.Body.push_back(BodyRet);

  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.TypeDescriptorVA = 0x140002000;
  Catch.Adjectives = 0x9; // const | reference
  Catch.HandlerVA = 0x140001020;
  Try.EHClauses.push_back(Catch);
  Try.EHClauseBodies.emplace_back();
  Function.Body.push_back(std::move(Try));

  std::string Source = emitHighC({Function});
  EXPECT_NE(Source.find("catch (const /* type @ 0x140002000 */ &)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("handler @ 0x140001020"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("#if 0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, HighCSEHFilterRendersNamedFilterCall) {
  HighFunc Function;
  Function.Name = "seh_filter_fn";
  Function.Entry = 0x140001000;
  Function.ReturnType = NdType::makeVoid();

  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.EHIsReducible = true;
  Try.EHRange = {0x140001000, 0x140001020};
  HighStmt BodyRet;
  BodyRet.Kind = StmtKind::Return;
  Try.Body.push_back(BodyRet);

  HighEHClause Except;
  Except.Kind = HighEHClauseKind::SEHExcept;
  Except.FilterOrActionVA = 0x140001100;
  Except.HandlerVA = 0x140001200;
  Try.EHClauses.push_back(Except);
  Try.EHClauseBodies.emplace_back();
  Function.Body.push_back(std::move(Try));

  std::string Source = emitHighC({Function});
  EXPECT_NE(Source.find("__try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_140001100(GetExceptionInformation())"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("nd_seh_filter_"), std::string::npos) << Source;
  EXPECT_NE(Source.find("handler @ 0x140001200"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("#if 0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, HighCCommentsDisabledStillEmitsOnlyTrap) {
  HighFunc Function;
  Function.Name = "analysis_no_comments";
  Function.ReturnType = NdType::makeVoid();
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Function.Body.push_back(std::move(Return));
  Function.ExceptionMetadata = makeFH3Metadata({0x140001020});

  std::string Source = emitHighC({Function}, /*EmitComments=*/false);
  EXPECT_EQ(Source.find("neverd.analysis-only"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("neverd.exception"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("#if 0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return"), std::string::npos) << Source;
}

TEST(COFFExceptionIR,
     LLVMCMetadataPersonalityAndFuncletsHaveTrapOnlyActiveDefinitions) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-windows-eh-analysis", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage, "__CxxFrameHandler4",
      Module);

  auto AddCallingFunction = [&](llvm::StringRef Name,
                                llvm::StringRef HelperName) {
    llvm::Function *Helper = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, HelperName, Module);
    llvm::Function *Function = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateCall(Helper);
    Builder.CreateRetVoid();
    return Function;
  };

  llvm::MDNode *Payload =
      llvm::MDNode::get(Context, llvm::MDString::get(Context, "fh4"));
  llvm::Function *Attached =
      AddCallingFunction("llvmc_attached", "unsafe_attached_helper");
  Attached->setMetadata(windows_eh_md::FunctionAttachment, Payload);

  llvm::Function *Native =
      AddCallingFunction("llvmc_native", "unsafe_native_helper");
  Native->setMetadata(windows_eh_md::NativeAttachment, Payload);

  llvm::Function *PersonalityOnly =
      AddCallingFunction("llvmc_personality", "unsafe_personality_helper");
  PersonalityOnly->setPersonalityFn(Personality);

  llvm::Function *TableOnly =
      AddCallingFunction("llvmc_table", "unsafe_table_helper");
  llvm::NamedMDNode *Table =
      Module.getOrInsertNamedMetadata(windows_eh_md::FunctionTable);
  Table->addOperand(llvm::MDNode::get(
      Context, {llvm::ValueAsMetadata::get(TableOnly), Payload}));

  llvm::Function *Funclet = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "llvmc_funclet", Module);
  Funclet->setPersonalityFn(Personality);
  llvm::IRBuilder<> FuncletBuilder(
      llvm::BasicBlock::Create(Context, "entry", Funclet));
  llvm::CleanupPadInst *Pad = FuncletBuilder.CreateCleanupPad(
      llvm::ConstantTokenNone::get(Context), {});
  FuncletBuilder.CreateCleanupRet(Pad, nullptr);

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("neverd.analysis-only"), std::string::npos) << Source;
  EXPECT_NE(Source.find("unsafe_attached_helper();"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("unsafe_native_helper();"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("unsafe_personality_helper();"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("unsafe_table_helper();"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("#if 0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCExceptionAnnotationReadsCanonicalStringsOnly) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-windows-eh-annotation", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto AddFunction = [&](llvm::StringRef Name) {
    llvm::Function *Function = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateRetVoid();
    return Function;
  };

  ExceptionFunction EH;
  EH.CodeRange = {0x140001000, 0x140001040};
  EH.Encoding = ExceptionEncoding::X64UnwindV2;
  EH.ParseStatus = ExceptionParseStatus::Partial;
  EH.UnwindInfoVA = 0x140003000;
  llvm::Function *Canonical = AddFunction("canonical_eh");
  Canonical->setMetadata(windows_eh_md::FunctionAttachment,
                         windows_eh_md::getCanonicalFunctionMetadata(
                             Context, EH, Arch::X64, BinaryFormat::COFF));

  llvm::Function *NativeOnly = AddFunction("native_marker_only");
  NativeOnly->setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(Context,
                        {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                             llvm::Type::getInt1Ty(Context), 1)),
                         llvm::MDString::get(Context, "seh-x64-native")}));

  std::string Source = emitLLVMC(Module);
  const auto Annotation = Source.find("neverd.exception:");
  ASSERT_NE(Annotation, std::string::npos) << Source;
  EXPECT_NE(Source.find("encoding=x64-unwind-v2, status=partial", Annotation),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("code=[0x140001000, 0x140001040), "
                        "unwind=0x140003000",
                        Annotation),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("neverd.exception:", Annotation + 1), std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCProjectsCanonicalWindowsEHAsCommentsOnly) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-windows-eh-details", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto AddFunction = [&](llvm::StringRef Name, const ExceptionFunction &EH) {
    llvm::Function *Function = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateRetVoid();
    Function->setMetadata(windows_eh_md::FunctionAttachment,
                          windows_eh_md::getCanonicalFunctionMetadata(
                              Context, EH, Arch::X64, BinaryFormat::COFF));
  };

  ExceptionFunction SEH;
  SEH.CodeRange = {0x140001000, 0x140001080};
  SEH.Encoding = ExceptionEncoding::X64UnwindV1;
  SEH.PersonalityName = "__GSHandlerCheck_SEH";
  SEH.SEH.emplace();
  SEHScopeRecord Filter;
  Filter.GuardedRange = {0x140001010, 0x140001030};
  Filter.Kind = SEHScopeKind::Filter;
  Filter.FilterOrFinallyVA = 0x140002000;
  Filter.HandlerVA = 0x140002020;
  Filter.ContinuationVA = 0x140001040;
  SEH.SEH->Scopes.push_back(Filter);
  SEHScopeRecord Finally;
  Finally.GuardedRange = {0x140001040, 0x140001060};
  Finally.Kind = SEHScopeKind::Finally;
  Finally.FilterOrFinallyVA = 0x140002040;
  SEH.SEH->Scopes.push_back(Finally);
  SEH.GSCookie.emplace();
  SEH.GSCookie->ParseStatus = ExceptionParseStatus::Complete;
  SEH.GSCookie->CookieOffset = 128;
  SEH.GSCookie->HasExceptionHandler = true;
  SEH.GSCookie->HasUnwindHandler = true;
  SEH.GSCookie->HasAlignment = true;
  SEH.GSCookie->AlignmentBaseOffset = -16;
  SEH.GSCookie->Alignment = 16;
  AddFunction("seh_details", SEH);

  ExceptionFunction Cxx;
  Cxx.CodeRange = {0x140003000, 0x140003100};
  Cxx.Encoding = ExceptionEncoding::X64UnwindV1;
  Cxx.PersonalityName = "__GSHandlerCheck_EH4";
  Cxx.Cxx.emplace();
  Cxx.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.Cxx->MaxState = 3;
  Cxx.Cxx->UnwindMap.push_back(
      {-1, 0x140004000, CxxUnwindAction::ActionKind::Direct, 0});
  Cxx.Cxx->UnwindMap.push_back(
      {0, 0x140004020, CxxUnwindAction::ActionKind::DestructorWithObject, -24});
  Cxx.Cxx->UnwindMap.push_back(
      {1, 0x140004040, CxxUnwindAction::ActionKind::Direct, 0});
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 1;
  Try.CatchHigh = 2;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.TypeDescriptorVA = 0x140005000;
  Catch.CatchObjectOffset = -32;
  Catch.HandlerVA = 0x140003080;
  Catch.ParentFrameOffset = -16;
  Catch.ContinuationVAs = {0x1400030a0, 0x1400030b0};
  Try.Handlers.push_back(Catch);
  Cxx.Cxx->TryBlocks.push_back(Try);
  Cxx.Cxx->IPMap = {{0x140003010, -1}, {0x140003040, 0}, {0x140003080, 2}};
  Cxx.GSCookie.emplace();
  Cxx.GSCookie->ParseStatus = ExceptionParseStatus::Complete;
  Cxx.GSCookie->CookieOffset = 128;
  AddFunction("cxx_details", Cxx);

  const std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("seh.scope[0]: filter [0x140001010, 0x140001030)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("seh.scope[1]: finally [0x140001040, 0x140001060)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("gs.cookie_offset=128, ehandler=1, uhandler=1, "
                        "alignment_base=-16, alignment=16"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("cxx.format=fh4, states=3, try_blocks=1, ip_states=3"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("cxx.unwind[0]: to_state=-1, kind=direct"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("cxx.unwind[1]: to_state=0, "
                        "kind=destructor-object, action=0x140004020, "
                        "object_offset=-24"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("cxx.try[0]: states=0..1, catch_high=2, handlers=1"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("catch[0]: type=0x140005000, handler=0x140003080, "
                        "adjectives=0x40, object_offset=-32, "
                        "parent_frame_offset=-16, "
                        "continuations=0x1400030A0,0x1400030B0"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("cxx.ip_state[2]: ip=0x140003080, state=2"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("__try"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__except"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("try {"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("catch ("), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCCorruptWindowsEHMetadataIsExplicitAndBounded) {
  auto EmitCorrupt = [](const ExceptionFunction &EH, auto Corrupt) {
    llvm::LLVMContext Context;
    llvm::Module Module("llvm-c-corrupt-windows-eh", Context);
    Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
    llvm::FunctionType *VoidType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    llvm::Function *Function = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, "corrupt_eh", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateRetVoid();
    llvm::MDNode *Canonical = windows_eh_md::getCanonicalFunctionMetadata(
        Context, EH, Arch::X64, BinaryFormat::COFF);
    llvm::SmallVector<llvm::Metadata *, windows_eh_md::OperandCount> Fields;
    for (const llvm::MDOperand &Operand : Canonical->operands())
      Fields.push_back(Operand.get());
    Corrupt(Context, Fields);
    Function->setMetadata(windows_eh_md::FunctionAttachment,
                          llvm::MDNode::get(Context, Fields));
    return emitLLVMC(Module);
  };

  ExceptionFunction EH;
  EH.CodeRange = {0x140001000, 0x140001080};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {0x140001010, 0x140001030};
  Scope.Kind = SEHScopeKind::Finally;
  EH.SEH->Scopes.push_back(Scope);

  const std::string BadVersion =
      EmitCorrupt(EH, [](llvm::LLVMContext &Context, auto &Fields) {
        Fields[windows_eh_md::Version] = llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 9));
      });
  EXPECT_NE(BadVersion.find("unsupported metadata schema version 9"),
            std::string::npos)
      << BadVersion;
  EXPECT_EQ(BadVersion.find("status=complete"), std::string::npos)
      << BadVersion;

  ExceptionFunction BadCodeRange = EH;
  BadCodeRange.CodeRange = {0x140001080, 0x140001000};
  const std::string InvertedCode =
      EmitCorrupt(BadCodeRange, [](llvm::LLVMContext &, auto &) {});
  EXPECT_NE(InvertedCode.find("metadata-invalid (function code range)"),
            std::string::npos)
      << InvertedCode;
  EXPECT_EQ(InvertedCode.find("status=complete"), std::string::npos)
      << InvertedCode;

  ExceptionFunction BadScopeRange = EH;
  BadScopeRange.SEH->Scopes[0].GuardedRange = {0x140001030, 0x140001010};
  const std::string InvertedScope =
      EmitCorrupt(BadScopeRange, [](llvm::LLVMContext &, auto &) {});
  EXPECT_NE(InvertedScope.find("metadata-invalid (seh.scope range)"),
            std::string::npos)
      << InvertedScope;
  EXPECT_EQ(InvertedScope.find("status=complete"), std::string::npos)
      << InvertedScope;

  const std::string BadScope =
      EmitCorrupt(EH, [](llvm::LLVMContext &Context, auto &Fields) {
        Fields[windows_eh_md::SEHScopes] = llvm::MDNode::get(
            Context, {llvm::MDNode::get(
                         Context, {llvm::MDString::get(Context, "bad row")})});
      });
  EXPECT_NE(BadScope.find("metadata-invalid (seh.scope row)"),
            std::string::npos)
      << BadScope;
  EXPECT_EQ(BadScope.find("status=complete"), std::string::npos) << BadScope;
  EXPECT_EQ(BadScope.find("seh.scope["), std::string::npos) << BadScope;

  const std::string NullStatus =
      EmitCorrupt(EH, [](llvm::LLVMContext &, auto &Fields) {
        Fields[windows_eh_md::ParseStatus] = nullptr;
      });
  EXPECT_NE(NullStatus.find("metadata-invalid (function string field)"),
            std::string::npos)
      << NullStatus;
  EXPECT_EQ(NullStatus.find("status=complete"), std::string::npos)
      << NullStatus;

  const std::string NullScopeRow =
      EmitCorrupt(EH, [](llvm::LLVMContext &Context, auto &Fields) {
        llvm::Metadata *Null = nullptr;
        Fields[windows_eh_md::SEHScopes] = llvm::MDNode::get(Context, {Null});
      });
  EXPECT_NE(NullScopeRow.find("metadata-invalid (seh.scope row)"),
            std::string::npos)
      << NullScopeRow;
  EXPECT_EQ(NullScopeRow.find("status=complete"), std::string::npos)
      << NullScopeRow;

  const std::string BadUnwindRow =
      EmitCorrupt(EH, [](llvm::LLVMContext &Context, auto &Fields) {
        Fields[windows_eh_md::UnwindOperations] = llvm::MDNode::get(
            Context, {llvm::MDString::get(Context, "bad unwind row")});
      });
  EXPECT_NE(BadUnwindRow.find("metadata-invalid (unwind operations)"),
            std::string::npos)
      << BadUnwindRow;
  EXPECT_EQ(BadUnwindRow.find("status=complete"), std::string::npos)
      << BadUnwindRow;

  ExceptionFunction Cxx = EH;
  Cxx.SEH.reset();
  Cxx.Cxx.emplace();
  Cxx.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.Cxx->TryBlocks.push_back({});
  const std::string BadCatch =
      EmitCorrupt(Cxx, [](llvm::LLVMContext &Context, auto &Fields) {
        llvm::MDNode *BadHandlers = llvm::MDNode::get(
            Context,
            {llvm::MDNode::get(Context,
                               {llvm::MDString::get(Context, "bad catch")})});
        llvm::MDNode *BadTry = llvm::MDNode::get(
            Context,
            {llvm::ConstantAsMetadata::get(
                 llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0)),
             llvm::ConstantAsMetadata::get(
                 llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0)),
             llvm::ConstantAsMetadata::get(
                 llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 1)),
             BadHandlers});
        Fields[windows_eh_md::CxxTryMap] = llvm::MDNode::get(Context, {BadTry});
      });
  EXPECT_NE(BadCatch.find("metadata-invalid (cxx.catch row)"),
            std::string::npos)
      << BadCatch;
  EXPECT_EQ(BadCatch.find("status=complete"), std::string::npos) << BadCatch;
  EXPECT_EQ(BadCatch.find("cxx.format="), std::string::npos) << BadCatch;

  ExceptionFunction Hostile = EH;
  Hostile.PersonalityName = "safe*/\nint injected;";
  Hostile.Diagnostics.push_back("payload */\nint injected;");
  const std::string Escaped =
      EmitCorrupt(Hostile, [](llvm::LLVMContext &, auto &) {});
  EXPECT_NE(Escaped.find("personality=safe* /?int injected;"),
            std::string::npos)
      << Escaped;
  EXPECT_NE(Escaped.find("diagnostic: payload * /?int injected;"),
            std::string::npos)
      << Escaped;
  EXPECT_EQ(Escaped.find("*/\nint injected;"), std::string::npos) << Escaped;

  ExceptionFunction Oversized = EH;
  Oversized.Diagnostics.push_back(std::string(4097, 'a'));
  const std::string Limited =
      EmitCorrupt(Oversized, [](llvm::LLVMContext &, auto &) {});
  EXPECT_NE(Limited.find("metadata-invalid (diagnostic string field)"),
            std::string::npos)
      << Limited;
  EXPECT_EQ(Limited.find("status=complete"), std::string::npos) << Limited;
  EXPECT_LT(Limited.size(), 10000u) << Limited;
}

TEST(COFFExceptionIR,
     HighCWindowsEvidenceAndHostileIdentifiersRemainFailClosed) {
  HighFunc Guarded;
  Guarded.Name = "guard(void) { pwned(); }\nvoid injected";
  Guarded.ReturnType = NdType::makeVoid();
  HighParam KeywordParam;
  KeywordParam.Name = "for";
  KeywordParam.Type = NdType::makePtr();
  Guarded.Params.push_back(std::move(KeywordParam));
  HighParam HostileParam;
  const char HostileBytes[] = "for\0) { pwned(); }\nint injected";
  HostileParam.Name.assign(HostileBytes, sizeof(HostileBytes) - 1);
  HostileParam.Type = NdType::makePtr();
  Guarded.Params.push_back(std::move(HostileParam));
  HighStmt GuardedReturn;
  GuardedReturn.Kind = StmtKind::Return;
  Guarded.Body.push_back(std::move(GuardedReturn));

  ExceptionFunction IncompleteWindows;
  IncompleteWindows.Encoding = ExceptionEncoding::X64UnwindV1;
  IncompleteWindows.ParseStatus = ExceptionParseStatus::Partial;
  IncompleteWindows.HandlerDataVA = 0x140004000;
  Guarded.ExceptionMetadata = std::move(IncompleteWindows);

  HighFunc UnwindOnly;
  UnwindOnly.Name = "highc_unwind_only";
  UnwindOnly.ReturnType = NdType::makeVoid();
  HighStmt UnwindReturn;
  UnwindReturn.Kind = StmtKind::Return;
  UnwindOnly.Body.push_back(std::move(UnwindReturn));
  ExceptionFunction CompleteUnwind;
  CompleteUnwind.Encoding = ExceptionEncoding::X64UnwindV1;
  CompleteUnwind.ParseStatus = ExceptionParseStatus::Complete;
  CompleteUnwind.Personality = ExceptionPersonality::None;
  UnwindOnly.ExceptionMetadata = std::move(CompleteUnwind);

  std::string Source = emitHighC({Guarded, UnwindOnly});
  EXPECT_NE(Source.find("neverd.analysis-only"), std::string::npos) << Source;
  EXPECT_NE(Source.find("nd_for"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("pwned();"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("void injected"), std::string::npos) << Source;

  REQUIRE_HOST_FIXTURE_COMPILER();
  CompilerResult Syntax = runCCompiler(Source, {"-std=c11", "-fsyntax-only"});
  ASSERT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
}

TEST(COFFExceptionIR, LLVMCAllocaLoadStoreUsesNamedLocalsAndAddressOf) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-alloca", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *Type = llvm::FunctionType::get(
      llvm::PointerType::getUnqual(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "slot_addr", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::AllocaInst *Slot = Builder.CreateAlloca(I32, nullptr, "local");
  Builder.CreateStore(llvm::ConstantInt::get(I32, 7), Slot);
  Builder.CreateLoad(I32, Slot);
  Builder.CreateRet(Slot);

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find(" = 7;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return &"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("*(uint32_t*)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unhandled:"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCCatchSwitchRendersExceptSyntax) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_try_fn", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  EntryBuilder.CreateInvoke(Helper, Handler, Dispatch);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch, {llvm::ConstantPointerNull::get(
                   llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("__try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("__except (EXCEPTION_EXECUTE_HANDLER)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("may_raise();"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("/* __except (EXCEPTION_EXECUTE_HANDLER) */"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("unhandled:"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCWrapUsesRecoveredSEHFilter) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-filter", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::FunctionType *FilterType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context),
      {llvm::PointerType::getUnqual(Context)}, false);
  llvm::Function *Filter = llvm::Function::Create(
      FilterType, llvm::GlobalValue::ExternalLinkage, "probe_filter", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_try_fn", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  EntryBuilder.CreateInvoke(Helper, Handler, Dispatch);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(Switch, {Filter});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("} __except (probe_filter(GetExceptionInformation())) {"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("} __except (EXCEPTION_EXECUTE_HANDLER)"),
            std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCNestsFinallyInsideExceptWrap) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-nested", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "nested_seh", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Finally =
      llvm::BasicBlock::Create(Context, "finally", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  EntryBuilder.CreateInvoke(Helper, Cont, Dispatch);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch, {llvm::ConstantPointerNull::get(
                   llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Cont);
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup = FinallyBuilder.CreateCleanupPad(
      llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCleanupRet(Cleanup, Cont);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("__try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} __finally {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {"),
            std::string::npos)
      << Source;
  const auto ExceptAt = Source.find("} __except");
  const auto FinallyAt = Source.find("} __finally");
  EXPECT_NE(FinallyAt, std::string::npos) << Source;
  EXPECT_NE(ExceptAt, std::string::npos) << Source;
  EXPECT_LT(FinallyAt, ExceptAt) << Source;
}

TEST(COFFExceptionIR, LLVMCExceptWrapContainsHandlerBody) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-handler-body", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType =
      llvm::FunctionType::get(I32, /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_try_fn", Module);
  Function->setPersonalityFn(Personality);
  auto *Sink = new llvm::GlobalVariable(
      Module, I32, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(I32, 0), "ProbeSink");

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  EntryBuilder.CreateInvoke(Helper, Cont, Dispatch);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch, {llvm::ConstantPointerNull::get(
                   llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateStore(llvm::ConstantInt::get(I32, 41), Sink);
  HandlerBuilder.CreateBr(Cont);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  const auto ExceptAt = Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {");
  const auto StoreAt = Source.find("ProbeSink = 41");
  EXPECT_NE(ExceptAt, std::string::npos) << Source;
  EXPECT_NE(StoreAt, std::string::npos) << Source;
  EXPECT_LT(ExceptAt, StoreAt) << Source;
  EXPECT_EQ(Source.find("/* recovered handler labels remain in the protected body */"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("/* __except"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCExceptContinuationFollowsHandler) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-except-cont", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_except_cont", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::Function *Raise = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  llvm::Function *Handle = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "handler_step", Module);
  llvm::Function *After = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateInvoke(Raise, Cont, Dispatch);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch, {llvm::ConstantPointerNull::get(
                   llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateCall(Handle);
  HandlerBuilder.CreateBr(Cont);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateCall(After);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  const auto BodyAt = Source.find("seh_except_cont(");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto TryAt = Source.find("__try {", BodyAt);
  const auto RaiseAt = Source.find("may_raise();", BodyAt);
  const auto ExceptAt =
      Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {", BodyAt);
  const auto HandleAt = Source.find("handler_step();", BodyAt);
  const auto AfterAt = Source.find("after_step();", BodyAt);
  ASSERT_NE(TryAt, std::string::npos) << Source;
  ASSERT_NE(RaiseAt, std::string::npos) << Source;
  ASSERT_NE(ExceptAt, std::string::npos) << Source;
  ASSERT_NE(HandleAt, std::string::npos) << Source;
  ASSERT_NE(AfterAt, std::string::npos) << Source;
  const auto ExceptOpen = Source.find('{', ExceptAt);
  ASSERT_NE(ExceptOpen, std::string::npos) << Source;
  size_t ExceptClose = std::string::npos;
  int Depth = 1;
  for (size_t I = ExceptOpen + 1; I < Source.size(); ++I) {
    if (Source[I] == '{')
      ++Depth;
    else if (Source[I] == '}') {
      --Depth;
      if (Depth == 0) {
        ExceptClose = I;
        break;
      }
    }
  }
  ASSERT_NE(ExceptClose, std::string::npos) << Source;
  EXPECT_LT(TryAt, RaiseAt) << Source;
  EXPECT_LT(RaiseAt, ExceptAt) << Source;
  EXPECT_LT(ExceptAt, HandleAt) << Source;
  EXPECT_LT(HandleAt, ExceptClose) << Source;
  EXPECT_LT(ExceptClose, AfterAt) << Source;
  EXPECT_EQ(Source.find("goto ", BodyAt), std::string::npos) << Source;
  EXPECT_EQ(Source.find("handler_step();", HandleAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("after_step();", AfterAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("may_raise();", RaiseAt + 1), std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCFinallyWrapContainsCleanupBody) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-finally-body", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_finally_fn", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Finally =
      llvm::BasicBlock::Create(Context, "finally", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  llvm::Function *CleanupFn = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "run_finally", Module);
  EntryBuilder.CreateInvoke(Helper, Cont, Finally);
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup = FinallyBuilder.CreateCleanupPad(
      llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn);
  FinallyBuilder.CreateCleanupRet(Cleanup, Cont);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  const auto FinallyAt = Source.find("} __finally {");
  const auto CallAt = Source.find("run_finally();");
  EXPECT_NE(FinallyAt, std::string::npos) << Source;
  EXPECT_NE(CallAt, std::string::npos) << Source;
  EXPECT_LT(FinallyAt, CallAt) << Source;
  EXPECT_EQ(Source.find("/* recovered handler labels remain in the protected body */"),
            std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCFinallyContinuationFollowsCleanup) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-finally-cont", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_finally_cont",
      Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Finally =
      llvm::BasicBlock::Create(Context, "finally", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::Function *Raise = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  llvm::Function *CleanupFn = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "run_finally", Module);
  llvm::Function *After = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateInvoke(Raise, Cont, Finally);
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup = FinallyBuilder.CreateCleanupPad(
      llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn);
  FinallyBuilder.CreateCleanupRet(Cleanup, Cont);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateCall(After);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  const auto BodyAt = Source.find("seh_finally_cont(");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto TryAt = Source.find("__try {", BodyAt);
  const auto RaiseAt = Source.find("may_raise();", BodyAt);
  const auto FinallyAt = Source.find("} __finally {", BodyAt);
  const auto RunAt = Source.find("run_finally();", BodyAt);
  const auto AfterAt = Source.find("after_step();", BodyAt);
  ASSERT_NE(TryAt, std::string::npos) << Source;
  ASSERT_NE(RaiseAt, std::string::npos) << Source;
  ASSERT_NE(FinallyAt, std::string::npos) << Source;
  ASSERT_NE(RunAt, std::string::npos) << Source;
  ASSERT_NE(AfterAt, std::string::npos) << Source;
  const auto FinallyOpen = Source.find('{', FinallyAt);
  ASSERT_NE(FinallyOpen, std::string::npos) << Source;
  size_t FinallyClose = std::string::npos;
  int Depth = 1;
  for (size_t I = FinallyOpen + 1; I < Source.size(); ++I) {
    if (Source[I] == '{')
      ++Depth;
    else if (Source[I] == '}') {
      --Depth;
      if (Depth == 0) {
        FinallyClose = I;
        break;
      }
    }
  }
  ASSERT_NE(FinallyClose, std::string::npos) << Source;
  EXPECT_LT(TryAt, RaiseAt) << Source;
  EXPECT_LT(RaiseAt, FinallyAt) << Source;
  EXPECT_LT(FinallyAt, RunAt) << Source;
  EXPECT_LT(RunAt, FinallyClose) << Source;
  EXPECT_LT(FinallyClose, AfterAt) << Source;
  EXPECT_EQ(Source.find("goto ", BodyAt), std::string::npos) << Source;
  EXPECT_EQ(Source.find("run_finally();", RunAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("after_step();", AfterAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("may_raise();", RaiseAt + 1), std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCNestedFinallyInsideExceptContainsBothBodies) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-nested-bodies", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType =
      llvm::FunctionType::get(I32, /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_begin);
  llvm::Function *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_end);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "nested_seh_bodies",
      Module);
  Function->setPersonalityFn(Personality);
  auto *Sink = new llvm::GlobalVariable(
      Module, I32, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(I32, 0), "ProbeSink");

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *BeginOuter =
      llvm::BasicBlock::Create(Context, "begin_outer", Function);
  llvm::BasicBlock *BeginInner =
      llvm::BasicBlock::Create(Context, "begin_inner", Function);
  llvm::BasicBlock *InnerBody =
      llvm::BasicBlock::Create(Context, "inner_body", Function);
  llvm::BasicBlock *EndInner =
      llvm::BasicBlock::Create(Context, "end_inner", Function);
  llvm::BasicBlock *AfterInner =
      llvm::BasicBlock::Create(Context, "after_inner", Function);
  llvm::BasicBlock *EndOuter =
      llvm::BasicBlock::Create(Context, "end_outer", Function);
  llvm::BasicBlock *AfterOuter =
      llvm::BasicBlock::Create(Context, "after_outer", Function);
  llvm::BasicBlock *CatchDispatch =
      llvm::BasicBlock::Create(Context, "catch_dispatch", Function);
  llvm::BasicBlock *CatchPad =
      llvm::BasicBlock::Create(Context, "catch_pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::BasicBlock *FinallyDispatch =
      llvm::BasicBlock::Create(Context, "finally_dispatch", Function);

  llvm::Function *Arm = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "arm_region", Module);
  llvm::Function *Raise = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  llvm::Function *OuterStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "outer_step", Module);
  llvm::Function *RunFinally = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "run_finally", Module);

  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateCall(Arm);
  EntryBuilder.CreateBr(BeginOuter);
  llvm::IRBuilder<> BeginOuterBuilder(BeginOuter);
  BeginOuterBuilder.CreateInvoke(TryBegin, BeginInner, CatchDispatch);
  llvm::IRBuilder<> BeginInnerBuilder(BeginInner);
  BeginInnerBuilder.CreateInvoke(TryBegin, InnerBody, FinallyDispatch);
  llvm::IRBuilder<> InnerBuilder(InnerBody);
  InnerBuilder.CreateInvoke(Raise, EndInner, FinallyDispatch);
  llvm::IRBuilder<> EndInnerBuilder(EndInner);
  EndInnerBuilder.CreateInvoke(TryEnd, AfterInner, FinallyDispatch);
  llvm::IRBuilder<> AfterInnerBuilder(AfterInner);
  AfterInnerBuilder.CreateCall(OuterStep);
  AfterInnerBuilder.CreateBr(EndOuter);
  llvm::IRBuilder<> EndOuterBuilder(EndOuter);
  EndOuterBuilder.CreateInvoke(TryEnd, AfterOuter, CatchDispatch);
  llvm::IRBuilder<> AfterOuterBuilder(AfterOuter);
  AfterOuterBuilder.CreateRetVoid();

  llvm::IRBuilder<> DispatchBuilder(CatchDispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(CatchPad);
  llvm::IRBuilder<> PadBuilder(CatchPad);
  llvm::CatchPadInst *Catch = PadBuilder.CreateCatchPad(
      Switch, {llvm::ConstantPointerNull::get(
                   llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(Catch, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateStore(llvm::ConstantInt::get(I32, 41), Sink);
  HandlerBuilder.CreateBr(AfterOuter);

  llvm::IRBuilder<> FinallyBuilder(FinallyDispatch);
  llvm::CleanupPadInst *Cleanup = FinallyBuilder.CreateCleanupPad(
      llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(RunFinally);
  FinallyBuilder.CreateCleanupRet(Cleanup, CatchDispatch);

  std::string Source = emitLLVMC(Module);
  const auto BodyAt = Source.find("nested_seh_bodies(");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto ArmAt = Source.find("arm_region();", BodyAt);
  const auto TryAt = Source.find("__try {", BodyAt);
  const auto RaiseAt = Source.find("may_raise();", BodyAt);
  const auto FinallyKw = Source.find("} __finally {", BodyAt);
  const auto RunAt = Source.find("run_finally();", BodyAt);
  const auto OuterAt = Source.find("outer_step();", BodyAt);
  const auto ExceptKw =
      Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {", BodyAt);
  const auto StoreAt = Source.find("ProbeSink = 41", BodyAt);
  EXPECT_NE(ArmAt, std::string::npos) << Source;
  EXPECT_NE(TryAt, std::string::npos) << Source;
  EXPECT_NE(RaiseAt, std::string::npos) << Source;
  EXPECT_NE(FinallyKw, std::string::npos) << Source;
  EXPECT_NE(RunAt, std::string::npos) << Source;
  EXPECT_NE(OuterAt, std::string::npos) << Source;
  EXPECT_NE(ExceptKw, std::string::npos) << Source;
  EXPECT_NE(StoreAt, std::string::npos) << Source;
  EXPECT_LT(ArmAt, TryAt) << Source;
  EXPECT_LT(TryAt, RaiseAt) << Source;
  EXPECT_LT(RaiseAt, FinallyKw) << Source;
  EXPECT_LT(FinallyKw, RunAt) << Source;
  EXPECT_LT(RunAt, OuterAt) << Source;
  EXPECT_LT(OuterAt, ExceptKw) << Source;
  EXPECT_LT(ExceptKw, StoreAt) << Source;
  EXPECT_EQ(Source.find("run_finally();", BodyAt),
            Source.rfind("run_finally();"))
      << Source;
  EXPECT_EQ(Source.find("outer_step();", BodyAt), Source.rfind("outer_step();"))
      << Source;
  EXPECT_EQ(Source.find("ProbeSink = 41", BodyAt),
            Source.rfind("ProbeSink = 41"))
      << Source;
  EXPECT_EQ(Source.find("may_raise();", BodyAt), Source.rfind("may_raise();"))
      << Source;
  EXPECT_EQ(Source.find(
                "/* recovered handler labels remain in the protected body */"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("llvm_x2E_seh"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCCxxCleanupWrapContainsDestructor) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-cxx-dtor", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage, "__CxxFrameHandler3",
      Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "cxx_dtor_fn", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *CleanupBB =
      llvm::BasicBlock::Create(Context, "cleanup", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  llvm::Function *Dtor = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "release_string", Module);
  EntryBuilder.CreateInvoke(Helper, Cont, CleanupBB);
  llvm::IRBuilder<> CleanupBuilder(CleanupBB);
  llvm::CleanupPadInst *Cleanup = CleanupBuilder.CreateCleanupPad(
      llvm::ConstantTokenNone::get(Context));
  CleanupBuilder.CreateCall(Dtor);
  CleanupBuilder.CreateCleanupRet(Cleanup, nullptr);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  const auto CleanupAt = Source.find("} /* unwind cleanup */");
  const auto CallAt = Source.find("release_string();");
  EXPECT_NE(CleanupAt, std::string::npos) << Source;
  EXPECT_NE(CallAt, std::string::npos) << Source;
  EXPECT_LT(CleanupAt, CallAt) << Source;
  EXPECT_EQ(Source.find("release_string();"), Source.rfind("release_string();"))
      << Source;
}

TEST(COFFExceptionIR, LLVMCUnwindOnlyAttachmentOmitsFakeTryWrap) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-unwind-only", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "unwind_only", Module);
  Function->setMetadata(windows_eh_md::FunctionAttachment,
                        llvm::MDNode::get(Context, llvm::MDString::get(
                                                       Context, "unwind")));
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "unwind_only_helper",
      Module);
  Builder.CreateCall(Helper);
  Builder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("unwind_only_helper();"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__try"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__except"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("try {"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("neverd.analysis-only"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCAnalysisOnlyCxxUsesUnwindCleanupWrap) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-cxx-cleanup", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage, "__CxxFrameHandler3",
      Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "cleanup_only", Module);
  Function->setPersonalityFn(Personality);
  Function->setMetadata(windows_eh_md::FunctionAttachment,
                        llvm::MDNode::get(Context, llvm::MDString::get(
                                                       Context, "fh3")));
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "use_string", Module);
  Builder.CreateCall(Helper);
  Builder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} /* unwind cleanup */"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__except"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("catch ("), std::string::npos) << Source;
}

TEST(COFFExceptionIR,
     LLVMCProvenanceAliasesForwardersAndHostileNamesRemainFailClosed) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-adversarial-windows-eh", Context);
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::MDNode *Payload =
      llvm::MDNode::get(Context, llvm::MDString::get(Context, "windows-eh"));

  auto AddCallingFunction = [&](llvm::StringRef Name,
                                llvm::StringRef HelperName) {
    llvm::Function *Helper = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, HelperName, Module);
    llvm::Function *Function = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateCall(Helper);
    Builder.CreateRetVoid();
    return Function;
  };

  llvm::Function *Handler = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage, "__CxxFrameHandler4",
      Module);
  llvm::GlobalAlias *Alias = llvm::GlobalAlias::create(
      llvm::GlobalValue::ExternalLinkage, "opaque_personality", Handler);
  llvm::Function *AliasFunction =
      AddCallingFunction("llvmc_alias", "unsafe_alias_helper");
  AliasFunction->setPersonalityFn(Alias);

  llvm::Function *Forwarder = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__gxx_personality_seh0", Module);
  llvm::Function *ForwardingFunction =
      AddCallingFunction("llvmc_forwarder", "unsafe_forwarder_helper");
  ForwardingFunction->setPersonalityFn(Forwarder);

  llvm::Function *ProvenanceFunction =
      AddCallingFunction("llvmc_provenance", "unsafe_provenance_helper");
  llvm::CallInst *ProvenanceCall = nullptr;
  for (llvm::Instruction &Inst : ProvenanceFunction->getEntryBlock())
    if ((ProvenanceCall = llvm::dyn_cast<llvm::CallInst>(&Inst)))
      break;
  ASSERT_NE(ProvenanceCall, nullptr);
  llvm::IRBuilder<> ProvenanceBuilder(ProvenanceCall);
  llvm::SmallVector<llvm::Value *, 1> ProvenanceInputs = {
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 1)};
  llvm::OperandBundleDef ProvenanceBundle(windows_eh_md::ProvenanceBundle.str(),
                                          ProvenanceInputs);
  llvm::CallInst *BundledCall = ProvenanceBuilder.CreateCall(
      ProvenanceCall->getFunctionType(), ProvenanceCall->getCalledOperand(), {},
      {ProvenanceBundle});
  ProvenanceCall->eraseFromParent();
  (void)BundledCall;

  const char HostileFunctionBytes[] = "guard(void) { pwned(); }\nvoid injected";
  llvm::Function *Hostile = AddCallingFunction(
      llvm::StringRef(HostileFunctionBytes, sizeof(HostileFunctionBytes) - 1),
      "unsafe_hostile_helper");
  Hostile->setMetadata(windows_eh_md::FunctionAttachment, Payload);
  const char HostileArgumentBytes[] = "for\0) { pwned(); }\nint injected";
  llvm::FunctionType *ArgumentsType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::PointerType::getUnqual(Context),
                               llvm::PointerType::getUnqual(Context)},
                              false);
  llvm::Function *Arguments = llvm::Function::Create(
      ArgumentsType, llvm::GlobalValue::ExternalLinkage, "for", Module);
  auto Argument = Arguments->arg_begin();
  Argument->setName("for");
  (++Argument)
      ->setName(llvm::StringRef(HostileArgumentBytes,
                                sizeof(HostileArgumentBytes) - 1));
  llvm::IRBuilder<> ArgumentsBuilder(
      llvm::BasicBlock::Create(Context, "entry", Arguments));
  ArgumentsBuilder.CreateRetVoid();
  Arguments->setMetadata(windows_eh_md::FunctionAttachment, Payload);

  auto AddAttachedEmptyFunction = [&](llvm::StringRef Name) {
    llvm::Function *Function = llvm::Function::Create(
        VoidType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateRetVoid();
    Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
  };
  AddAttachedEmptyFunction("collision-name");
  AddAttachedEmptyFunction("collision_x2D_name");

  llvm::FunctionType *VarArgType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context), /*isVarArg=*/true);
  llvm::Function *VarArg = llvm::Function::Create(
      VarArgType, llvm::GlobalValue::ExternalLinkage, "llvmc_varargs", Module);
  llvm::IRBuilder<> VarArgBuilder(
      llvm::BasicBlock::Create(Context, "entry", VarArg));
  VarArgBuilder.CreateRetVoid();
  VarArg->setMetadata(windows_eh_md::FunctionAttachment, Payload);

  std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("llvmc_varargs()"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("llvmc_varargs(...)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("nd_for"), std::string::npos) << Source;
  EXPECT_NE(Source.find("collision_x2D_name()"), std::string::npos) << Source;
  EXPECT_NE(Source.find("collision_x2D_name_2()"), std::string::npos)
      << Source;

  EXPECT_NE(Source.find("nd_for"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, EmitsLosslessNamedMetadata) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "eh_metadata_test";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Kind = RuntimeFunctionKind::Chained;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindInfoVA = 0x140003000;
  EH.PackedUnwindData = 0x12345678;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityName = "resolved!__C_specific_handler";
  EH.PersonalityVA = 0x140001100;
  EH.HandlerDataVA = 0x140003010;
  EH.NativeUnwindBytes = {0x09, 0x04, 0x00, 0x00};
  UnwindOperation UnwindOp;
  UnwindOp.Kind = UnwindOperationKind::AllocateLarge;
  UnwindOp.SlotCount = 2;
  EH.UnwindOperations.push_back(UnwindOp);
  EH.PrimaryFunctionIndex = 7;
  EH.ChainedPrimaryRange = ExceptionAddressRange{Func.Entry, Func.Entry + 0x20};
  EH.ChainedUnwindInfoRVA = 0x3010;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = EH.CodeRange;
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = 0x140001080;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-metadata", Arch::X64, {},
                                      nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  llvm::NamedMDNode *Table =
      Module->getNamedMetadata("neverd.windows.eh.functions");
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->getNumOperands(), 1u);
  llvm::Function *IRFunc = Module->getFunction("eh_metadata_test");
  ASSERT_NE(IRFunc, nullptr);
  llvm::MDNode *Payload = IRFunc->getMetadata("neverd.windows.eh");
  ASSERT_NE(Payload, nullptr);
  ASSERT_EQ(Payload->getNumOperands(), windows_eh_md::OperandCount);
  auto UIntAt = [](const llvm::MDNode &Node, unsigned Index) -> uint64_t {
    const auto *Constant =
        llvm::dyn_cast<llvm::ConstantAsMetadata>(Node.getOperand(Index).get());
    EXPECT_NE(Constant, nullptr);
    const auto *Integer =
        Constant ? llvm::dyn_cast<llvm::ConstantInt>(Constant->getValue())
                 : nullptr;
    EXPECT_NE(Integer, nullptr);
    return Integer ? Integer->getZExtValue() : 0;
  };
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::Version),
            windows_eh_md::SchemaVersion);
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::RuntimeKind),
            static_cast<uint8_t>(RuntimeFunctionKind::Chained));
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::PackedUnwindData), 0x12345678u);
  const auto *ResolvedName = llvm::dyn_cast<llvm::MDString>(
      Payload->getOperand(windows_eh_md::PersonalityName).get());
  ASSERT_NE(ResolvedName, nullptr);
  EXPECT_EQ(ResolvedName->getString(), "resolved!__C_specific_handler");
  const auto *Operations = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::UnwindOperations).get());
  ASSERT_NE(Operations, nullptr);
  ASSERT_EQ(Operations->getNumOperands(), 1u);
  const auto *FirstOperation =
      llvm::dyn_cast<llvm::MDNode>(Operations->getOperand(0).get());
  ASSERT_NE(FirstOperation, nullptr);
  EXPECT_EQ(UIntAt(*FirstOperation, 3), 2u);
  const auto *PrimaryIndex = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::PrimaryFunctionIndex).get());
  ASSERT_NE(PrimaryIndex, nullptr);
  ASSERT_EQ(PrimaryIndex->getNumOperands(), 1u);
  EXPECT_EQ(UIntAt(*PrimaryIndex, 0), 7u);
  const auto *PrimaryRange = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::ChainedPrimaryRange).get());
  ASSERT_NE(PrimaryRange, nullptr);
  ASSERT_EQ(PrimaryRange->getNumOperands(), 2u);
  EXPECT_EQ(UIntAt(*PrimaryRange, 0), Func.Entry);
  EXPECT_EQ(UIntAt(*PrimaryRange, 1), Func.Entry + 0x20);
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::ChainedUnwindInfoRVA), 0x3010u);

  // This fixture carries source metadata but deliberately contains no call in
  // its protected range.  Close that zero-call contract explicitly so the
  // optimization portion isolates metadata preservation rather than native-EH
  // call-site lowering, which has dedicated tests below.
  exception_rewrite::setContract(
      *IRFunc, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete,
      /*RequiredCalls=*/0, /*LoweredCalls=*/0, /*SkippedPads=*/0);
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O2;
  OptimizationResult Result = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));

  Table = Module->getNamedMetadata("neverd.windows.eh.functions");
  ASSERT_NE(Table, nullptr);
  EXPECT_EQ(Table->getNumOperands(), 1u);
  IRFunc = Module->getFunction("eh_metadata_test");
  ASSERT_NE(IRFunc, nullptr);
  Payload = IRFunc->getMetadata("neverd.windows.eh");
  ASSERT_NE(Payload, nullptr);
  EXPECT_EQ(Payload->getNumOperands(), windows_eh_md::OperandCount);
  EXPECT_NE(IRFunc->getMetadata(exception_rewrite::FunctionAttachment),
            nullptr);
}
TEST(COFFExceptionIR, StructuresReducibleSEHAndCxxRegionsInHighIR) {
  auto MakeFunction = [] {
    MedFunc Func;
    Func.Entry = 0x140001000;
    Func.Name = "structured_eh";
    Func.ReturnType = NdType::makeVoid();

    MedBlock Protected;
    Protected.Id = 0;
    Protected.StartAddr = Func.Entry;
    Protected.EndAddr = Func.Entry + 0x10;
    MedOp ProtectedReturn;
    ProtectedReturn.Opcode = NdOp::RETURN;
    ProtectedReturn.Addr = Func.Entry + 8;
    Protected.Ops.push_back(ProtectedReturn);

    MedBlock Handler;
    Handler.Id = 1;
    Handler.StartAddr = Func.Entry + 0x20;
    Handler.EndAddr = Func.Entry + 0x30;
    MedOp HandlerReturn;
    HandlerReturn.Opcode = NdOp::RETURN;
    HandlerReturn.Addr = Func.Entry + 0x28;
    Handler.Ops.push_back(HandlerReturn);

    Func.Blocks.push_back(std::move(Protected));
    Func.Blocks.push_back(std::move(Handler));
    return Func;
  };

  MedFunc SEHFunc = MakeFunction();
  ExceptionFunction SEHMetadata;
  SEHMetadata.CodeRange = {SEHFunc.Entry, SEHFunc.Entry + 0x30};
  SEHMetadata.ParseStatus = ExceptionParseStatus::Complete;
  SEHMetadata.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {SEHFunc.Entry, SEHFunc.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = SEHFunc.Entry + 0x20;
  SEH.Scopes.push_back(Scope);
  SEHMetadata.SEH = std::move(SEH);
  SEHFunc.ExceptionMetadata = std::move(SEHMetadata);

  HighFunc HighSEH = MedToHighConverter().convert(SEHFunc, Arch::X64);
  ASSERT_EQ(HighSEH.StructuredExceptionRegions, 1u);
  ASSERT_EQ(HighSEH.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(HighSEH.Body.empty());
  EXPECT_EQ(HighSEH.Body.front().Kind, StmtKind::SEHTry);
  ASSERT_EQ(HighSEH.Body.front().EHClauses.size(), 1u);
  EXPECT_EQ(HighSEH.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::SEHExcept);

  MedFunc CxxFunc = MakeFunction();
  CxxFunc.Name = "structured_cxx";
  ExceptionFunction CxxMetadata;
  CxxMetadata.CodeRange = {CxxFunc.Entry, CxxFunc.Entry + 0x30};
  CxxMetadata.ParseStatus = ExceptionParseStatus::Complete;
  CxxMetadata.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}};
  Cxx.UnwindMap[0].ActionVA = CxxFunc.Entry + 0x100;
  Cxx.UnwindMap[0].Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Cxx.UnwindMap[0].ObjectOffset = -0x20;
  Cxx.IPMap = {{CxxFunc.Entry, 0},
               {CxxFunc.Entry + 0x10, -1},
               {CxxFunc.Entry + 0x20, 1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = CxxFunc.Entry + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  CxxMetadata.Cxx = std::move(Cxx);
  CxxFunc.ExceptionMetadata = std::move(CxxMetadata);

  HighFunc HighCxx = MedToHighConverter().convert(CxxFunc, Arch::X64);
  ASSERT_EQ(HighCxx.StructuredExceptionRegions, 1u);
  ASSERT_EQ(HighCxx.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(HighCxx.Body.empty());
  EXPECT_EQ(HighCxx.Body.front().Kind, StmtKind::CxxTry);
  ASSERT_EQ(HighCxx.Body.front().EHClauses.size(), 2u);
  EXPECT_EQ(HighCxx.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::CxxCatch);
  const HighEHClause &Cleanup = HighCxx.Body.front().EHClauses.back();
  EXPECT_EQ(Cleanup.Kind, HighEHClauseKind::CxxCleanup);
  EXPECT_EQ(Cleanup.UnwindActionKind,
            CxxUnwindAction::ActionKind::DestructorWithObjectPointer);
  EXPECT_EQ(Cleanup.UnwindObjectOffset, -0x20);
}

TEST(COFFExceptionIR, CleanupOnlyCxxStatesBecomeCxxTryNotSEH) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ActionVA = FunctionVA + 0x20;
  MedFunc Func =
      makeWindowsHandlerFixture("cleanup_only_cxx", 0x14000f000);
  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 1;
  Cxx.UnwindMap = {{-1, 0}};
  Cxx.UnwindMap[0].ActionVA = ActionVA;
  Cxx.UnwindMap[0].Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Cxx.UnwindMap[0].ObjectOffset = -0x20;
  Cxx.IPMap = {{FunctionVA, 0}, {FunctionVA + 0x10, -1}};
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.StructuredExceptionRegions, 1u);
  ASSERT_EQ(High.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(High.Body.empty());
  EXPECT_EQ(High.Body.front().Kind, StmtKind::CxxTry);
  ASSERT_EQ(High.Body.front().EHClauses.size(), 1u);
  EXPECT_EQ(High.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::CxxCleanup);
  EXPECT_EQ(High.Body.front().EHClauses.front().FilterOrActionVA, ActionVA);
  ASSERT_EQ(High.Body.front().EHClauseBodies.size(), 1u);
  ASSERT_EQ(High.Body.front().EHClauseBodies.front().size(), 1u);
  EXPECT_EQ(High.Body.front().EHClauseBodies.front().front().Addr, ActionVA);

  std::string Source = emitHighC({High});
  EXPECT_NE(Source.find("__wind {"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("unwind cleanup"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000F000();"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unstructured SEH"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__try"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__except"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, CleanupOnlyCxxWithoutIpMapIsNotUnstructuredSEH) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ActionVA = FunctionVA + 0x100;
  MedFunc Func = makeWindowsHandlerFixture("cleanup_only_no_ip", 0x14000f000);
  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 1;
  Cxx.UnwindMap = {{-1, 0}};
  Cxx.UnwindMap[0].ActionVA = ActionVA;
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::Direct;
  Cxx.UnwindMap[0].ObjectOffset = 40;
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_GE(High.StructuredExceptionRegions, 1u);
  ASSERT_FALSE(High.Body.empty());
  EXPECT_EQ(High.Body.front().Kind, StmtKind::CxxTry);
  ASSERT_FALSE(High.Body.front().EHClauses.empty());
  EXPECT_EQ(High.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::CxxCleanup);

  std::string Source = emitHighC({High});
  EXPECT_NE(Source.find("__wind {"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("unwind cleanup"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unstructured SEH"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__try"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, CleanupOnlyCxxStateInsideIfBecomesCxxTry) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ThenVA = FunctionVA + 0x2C0;
  constexpr va_t ElseVA = FunctionVA + 0x400;
  constexpr va_t ActionVA = FunctionVA + 0x800;
  MedFunc Med;
  Med.Entry = FunctionVA;
  Med.Name = "cleanup_inside_if";
  Med.ReturnType = NdType::makeVoid();
  MedVar Arg0;
  Arg0.Kind = MedVar::Param;
  Arg0.Id = 0;
  Arg0.Size = 8;
  Arg0.TheArch = Arch::X64;
  Med.Params.push_back(Arg0);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = FunctionVA;
  Entry.EndAddr = FunctionVA + 0x20;
  Entry.Succs = {1, 2};
  MedOp Br;
  Br.Opcode = NdOp::COND_BR;
  Br.Addr = FunctionVA + 0x8;
  Br.addInput(MedVar::makeConst(ThenVA, 8));
  Br.addInput(MedVar::makeConst(1, 1));
  Entry.Ops.push_back(std::move(Br));

  MedBlock Then;
  Then.Id = 1;
  Then.StartAddr = ThenVA;
  Then.EndAddr = ThenVA + 0x70;
  Then.Preds = {0};
  Then.Succs = {2};
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = ThenVA + 8;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Then.Ops.push_back(std::move(Call));
  MedOp ThenBr;
  ThenBr.Opcode = NdOp::BRANCH;
  ThenBr.Addr = ThenVA + 0x10;
  ThenBr.addInput(MedVar::makeConst(ElseVA, 8));
  Then.Ops.push_back(std::move(ThenBr));

  MedBlock Else;
  Else.Id = 2;
  Else.StartAddr = ElseVA;
  Else.EndAddr = ElseVA + 0x10;
  Else.Preds = {0, 1};
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = ElseVA;
  Else.Ops.push_back(std::move(Ret));

  Med.Blocks.push_back(std::move(Entry));
  Med.Blocks.push_back(std::move(Then));
  Med.Blocks.push_back(std::move(Else));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x500};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 1;
  Cxx.UnwindMap = {{-1, 0}};
  Cxx.UnwindMap[0].ActionVA = ActionVA;
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::Direct;
  Cxx.IPMap = {{FunctionVA, -1}, {ThenVA, 0}, {ThenVA + 0x70, -1}};
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Med.ExceptionMetadata = std::move(EH);

  std::map<va_t, std::string> Names{{0x140002000, "use_temp"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);

  const HighStmt *Try = nullptr;
  walkStmts(High.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::CxxTry)
      Try = &S;
  });
  ASSERT_NE(Try, nullptr);
  EXPECT_EQ(Try->EHRange.Begin, ThenVA);
  EXPECT_EQ(Try->EHRange.End, ThenVA + 0x70);
  ASSERT_FALSE(Try->EHClauses.empty());
  EXPECT_EQ(Try->EHClauses.front().Kind, HighEHClauseKind::CxxCleanup);
  EXPECT_EQ(High.StructuredExceptionRegions, 1u);

  bool Nested = false;
  walkStmts(High.Body, [&](const HighStmt &S) {
    if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) &&
        !S.Body.empty() && S.Body.front().Kind == StmtKind::CxxTry)
      Nested = true;
  });
  EXPECT_TRUE(Nested);

  const std::string Source = emitHighC({High});
  EXPECT_NE(Source.find("__wind {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("use_temp("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("try {"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__try"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, CleanupOnlyParentLiveRangeWrapsChildFragments) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t OuterActionVA = FunctionVA + 0x60;
  constexpr va_t InnerActionVA = FunctionVA + 0x70;
  MedFunc Med;
  Med.Entry = FunctionVA;
  Med.Name = "cleanup_parent_live";
  Med.ReturnType = NdType::makeVoid();

  auto AddCall = [](MedBlock &Block, va_t Addr, va_t Target) {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Addr;
    Call.addInput(MedVar::makeConst(Target, 8));
    Block.Ops.push_back(std::move(Call));
  };

  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = FunctionVA;
  Body.EndAddr = FunctionVA + 0x50;
  AddCall(Body, FunctionVA + 4, 0x140002000);
  AddCall(Body, FunctionVA + 0x14, 0x140002010);
  AddCall(Body, FunctionVA + 0x24, 0x140002020);
  AddCall(Body, FunctionVA + 0x34, 0x140002030);
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = FunctionVA + 0x44;
  Body.Ops.push_back(std::move(Ret));
  Med.Blocks.push_back(std::move(Body));

  MedBlock OuterHandler;
  OuterHandler.Id = 1;
  OuterHandler.StartAddr = OuterActionVA;
  OuterHandler.EndAddr = OuterActionVA + 0x10;
  AddCall(OuterHandler, OuterActionVA, 0x14000F000);
  Med.Blocks.push_back(std::move(OuterHandler));

  MedBlock InnerHandler;
  InnerHandler.Id = 2;
  InnerHandler.StartAddr = InnerActionVA;
  InnerHandler.EndAddr = InnerActionVA + 0x10;
  AddCall(InnerHandler, InnerActionVA, 0x14000F100);
  Med.Blocks.push_back(std::move(InnerHandler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x80};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}};
  Cxx.UnwindMap[0].ActionVA = OuterActionVA;
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::Direct;
  Cxx.UnwindMap[1].ActionVA = InnerActionVA;
  Cxx.UnwindMap[1].Kind = CxxUnwindAction::ActionKind::Direct;
  Cxx.IPMap = {{FunctionVA, -1},
               {FunctionVA + 0x10, 0},
               {FunctionVA + 0x20, 1},
               {FunctionVA + 0x30, 0},
               {FunctionVA + 0x40, -1}};
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Med.ExceptionMetadata = std::move(EH);

  std::map<va_t, std::string> Names{{0x140002000, "before"},
                                    {0x140002010, "outer_a"},
                                    {0x140002020, "inner_work"},
                                    {0x140002030, "outer_b"},
                                    {0x14000F000, "dtor_outer"},
                                    {0x14000F100, "dtor_inner"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);

  const HighStmt *Outer = nullptr;
  const HighStmt *Inner = nullptr;
  walkStmts(High.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::CxxTry || S.EHClauses.empty())
      return;
    if (S.EHClauses.front().State == 0)
      Outer = &S;
    if (S.EHClauses.front().State == 1)
      Inner = &S;
  });
  ASSERT_NE(Outer, nullptr);
  ASSERT_NE(Inner, nullptr);
  EXPECT_EQ(Outer->EHRange.Begin, FunctionVA + 0x10);
  EXPECT_EQ(Outer->EHRange.End, FunctionVA + 0x40);
  EXPECT_EQ(Inner->EHRange.Begin, FunctionVA + 0x20);
  EXPECT_EQ(Inner->EHRange.End, FunctionVA + 0x30);
  bool Nested = false;
  walkStmts(Outer->Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::CxxTry && !S.EHClauses.empty() &&
        S.EHClauses.front().State == 1)
      Nested = true;
  });
  EXPECT_TRUE(Nested);
  EXPECT_GE(High.StructuredExceptionRegions, 2u);

  const std::string Source = emitHighC({High});
  EXPECT_NE(Source.find("before("), std::string::npos) << Source;
  EXPECT_NE(Source.find("outer_a("), std::string::npos) << Source;
  EXPECT_NE(Source.find("inner_work("), std::string::npos) << Source;
  EXPECT_NE(Source.find("outer_b("), std::string::npos) << Source;
  EXPECT_NE(Source.find("dtor_outer("), std::string::npos) << Source;
  EXPECT_NE(Source.find("dtor_inner("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("try {"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, CleanupOnlyLiveRangeWrapsIfWhoseCondIsPreviousIp) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ActionVA = FunctionVA + 0x60;
  MedFunc Med;
  Med.Entry = FunctionVA;
  Med.Name = "cleanup_if_cond_prior_ip";
  Med.ReturnType = NdType::makeVoid();
  MedVar Arg0;
  Arg0.Kind = MedVar::Param;
  Arg0.Id = 0;
  Arg0.Size = 8;
  Arg0.TheArch = Arch::X64;
  Med.Params.push_back(Arg0);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = FunctionVA;
  Entry.EndAddr = FunctionVA + 0x10;
  Entry.Succs = {1, 2};
  MedOp Br;
  Br.Opcode = NdOp::COND_BR;
  Br.Addr = FunctionVA + 8;
  Br.addInput(MedVar::makeConst(FunctionVA + 0x20, 8));
  Br.addInput(MedVar::makeConst(1, 1));
  Entry.Ops.push_back(std::move(Br));

  MedBlock Then;
  Then.Id = 1;
  Then.StartAddr = FunctionVA + 0x20;
  Then.EndAddr = FunctionVA + 0x30;
  Then.Preds = {0};
  Then.Succs = {3};
  MedOp ThenCall;
  ThenCall.Opcode = NdOp::CALL;
  ThenCall.Addr = FunctionVA + 0x24;
  ThenCall.addInput(MedVar::makeConst(0x140002000, 8));
  Then.Ops.push_back(std::move(ThenCall));
  MedOp ThenBr;
  ThenBr.Opcode = NdOp::BRANCH;
  ThenBr.Addr = FunctionVA + 0x28;
  ThenBr.addInput(MedVar::makeConst(FunctionVA + 0x50, 8));
  Then.Ops.push_back(std::move(ThenBr));

  MedBlock Else;
  Else.Id = 2;
  Else.StartAddr = FunctionVA + 0x30;
  Else.EndAddr = FunctionVA + 0x40;
  Else.Preds = {0};
  Else.Succs = {3};
  MedOp ElseCall;
  ElseCall.Opcode = NdOp::CALL;
  ElseCall.Addr = FunctionVA + 0x34;
  ElseCall.addInput(MedVar::makeConst(0x140002010, 8));
  Else.Ops.push_back(std::move(ElseCall));
  MedOp ElseBr;
  ElseBr.Opcode = NdOp::BRANCH;
  ElseBr.Addr = FunctionVA + 0x38;
  ElseBr.addInput(MedVar::makeConst(FunctionVA + 0x50, 8));
  Else.Ops.push_back(std::move(ElseBr));

  MedBlock Join;
  Join.Id = 3;
  Join.StartAddr = FunctionVA + 0x50;
  Join.EndAddr = FunctionVA + 0x58;
  Join.Preds = {1, 2};
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = FunctionVA + 0x50;
  Join.Ops.push_back(std::move(Ret));
  Med.Blocks.push_back(std::move(Entry));
  Med.Blocks.push_back(std::move(Then));
  Med.Blocks.push_back(std::move(Else));
  Med.Blocks.push_back(std::move(Join));

  MedBlock Handler;
  Handler.Id = 4;
  Handler.StartAddr = ActionVA;
  Handler.EndAddr = ActionVA + 0x10;
  MedOp Dtor;
  Dtor.Opcode = NdOp::CALL;
  Dtor.Addr = ActionVA;
  Dtor.addInput(MedVar::makeConst(0x14000F000, 8));
  Handler.Ops.push_back(std::move(Dtor));
  Med.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x70};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 1;
  Cxx.UnwindMap = {{-1, 0}};
  Cxx.UnwindMap[0].ActionVA = ActionVA;
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::Direct;
  Cxx.IPMap = {{FunctionVA, -1},
               {FunctionVA + 0x20, 0},
               {FunctionVA + 0x50, -1}};
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Med.ExceptionMetadata = std::move(EH);

  std::map<va_t, std::string> Names{{0x140002000, "then_work"},
                                    {0x140002010, "else_work"},
                                    {0x14000F000, "dtor_name"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);

  const HighStmt *Try = nullptr;
  walkStmts(High.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::CxxTry)
      Try = &S;
  });
  ASSERT_NE(Try, nullptr);
  bool WrappedIf = false;
  walkStmts(Try->Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse)
      WrappedIf = true;
  });
  EXPECT_TRUE(WrappedIf);
  EXPECT_EQ(Try->EHRange.Begin, FunctionVA + 0x20);
  EXPECT_EQ(Try->EHRange.End, FunctionVA + 0x50);

  const std::string Source = emitHighC({High});
  EXPECT_NE(Source.find("__wind {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("then_work("), std::string::npos) << Source;
  EXPECT_NE(Source.find("else_work("), std::string::npos) << Source;
  EXPECT_NE(Source.find("dtor_name("), std::string::npos) << Source;
}

TEST(COFFExceptionIR, CleanupOnlySplitFragmentsWrapsIfElseDiamond) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ActionVA = FunctionVA + 0x60;
  MedFunc Med;
  Med.Entry = FunctionVA;
  Med.Name = "cleanup_split_diamond";
  Med.ReturnType = NdType::makeVoid();
  MedVar Arg0;
  Arg0.Kind = MedVar::Param;
  Arg0.Id = 0;
  Arg0.Size = 8;
  Arg0.TheArch = Arch::X64;
  Med.Params.push_back(Arg0);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = FunctionVA;
  Entry.EndAddr = FunctionVA + 0x10;
  Entry.Succs = {1, 2};
  MedOp Br;
  Br.Opcode = NdOp::COND_BR;
  Br.Addr = FunctionVA + 8;
  Br.addInput(MedVar::makeConst(FunctionVA + 0x20, 8));
  Br.addInput(MedVar::makeConst(1, 1));
  Entry.Ops.push_back(std::move(Br));

  MedBlock Then;
  Then.Id = 1;
  Then.StartAddr = FunctionVA + 0x20;
  Then.EndAddr = FunctionVA + 0x30;
  Then.Preds = {0};
  Then.Succs = {3};
  MedOp ThenCall;
  ThenCall.Opcode = NdOp::CALL;
  ThenCall.Addr = FunctionVA + 0x24;
  ThenCall.addInput(MedVar::makeConst(0x140002000, 8));
  Then.Ops.push_back(std::move(ThenCall));
  MedOp ThenBr;
  ThenBr.Opcode = NdOp::BRANCH;
  ThenBr.Addr = FunctionVA + 0x28;
  ThenBr.addInput(MedVar::makeConst(FunctionVA + 0x50, 8));
  Then.Ops.push_back(std::move(ThenBr));

  MedBlock Else;
  Else.Id = 2;
  Else.StartAddr = FunctionVA + 0x40;
  Else.EndAddr = FunctionVA + 0x50;
  Else.Preds = {0};
  Else.Succs = {3};
  MedOp ElseCall;
  ElseCall.Opcode = NdOp::CALL;
  ElseCall.Addr = FunctionVA + 0x44;
  ElseCall.addInput(MedVar::makeConst(0x140002010, 8));
  Else.Ops.push_back(std::move(ElseCall));
  MedOp ElseBr;
  ElseBr.Opcode = NdOp::BRANCH;
  ElseBr.Addr = FunctionVA + 0x48;
  ElseBr.addInput(MedVar::makeConst(FunctionVA + 0x50, 8));
  Else.Ops.push_back(std::move(ElseBr));

  MedBlock Join;
  Join.Id = 3;
  Join.StartAddr = FunctionVA + 0x50;
  Join.EndAddr = FunctionVA + 0x58;
  Join.Preds = {1, 2};
  MedOp Hole;
  Hole.Opcode = NdOp::CALL;
  Hole.Addr = FunctionVA + 0x50;
  Hole.addInput(MedVar::makeConst(0x140002020, 8));
  Join.Ops.push_back(std::move(Hole));
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = FunctionVA + 0x54;
  Join.Ops.push_back(std::move(Ret));
  Med.Blocks.push_back(std::move(Entry));
  Med.Blocks.push_back(std::move(Then));
  Med.Blocks.push_back(std::move(Else));
  Med.Blocks.push_back(std::move(Join));

  MedBlock Handler;
  Handler.Id = 4;
  Handler.StartAddr = ActionVA;
  Handler.EndAddr = ActionVA + 0x10;
  MedOp Dtor;
  Dtor.Opcode = NdOp::CALL;
  Dtor.Addr = ActionVA;
  Dtor.addInput(MedVar::makeConst(0x14000F000, 8));
  Handler.Ops.push_back(std::move(Dtor));
  Med.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x70};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 1;
  Cxx.UnwindMap = {{-1, 0}};
  Cxx.UnwindMap[0].ActionVA = ActionVA;
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::Direct;
  Cxx.IPMap = {{FunctionVA, -1},
               {FunctionVA + 0x20, 0},
               {FunctionVA + 0x30, -1},
               {FunctionVA + 0x40, 0},
               {FunctionVA + 0x50, -1}};
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Med.ExceptionMetadata = std::move(EH);

  std::map<va_t, std::string> Names{{0x140002000, "then_work"},
                                    {0x140002010, "else_work"},
                                    {0x140002020, "hole_work"},
                                    {0x14000F000, "dtor_name"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  EXPECT_GE(High.StructuredExceptionRegions, 1u);

  const std::string Source = emitHighC({High});
  const size_t Wind = Source.find("__wind {");
  const size_t Unwind = Source.find("__unwind", Wind);
  ASSERT_NE(Wind, std::string::npos) << Source;
  ASSERT_NE(Unwind, std::string::npos) << Source;
  const size_t ThenPos = Source.find("then_work(", Wind);
  const size_t ElsePos = Source.find("else_work(", Wind);
  const size_t HolePos = Source.find("hole_work(", Unwind);
  ASSERT_NE(ThenPos, std::string::npos) << Source;
  ASSERT_NE(ElsePos, std::string::npos) << Source;
  ASSERT_NE(HolePos, std::string::npos) << Source;
  EXPECT_LT(ThenPos, Unwind) << Source;
  EXPECT_LT(ElsePos, Unwind) << Source;
  EXPECT_GT(HolePos, Unwind) << Source;
}

TEST(COFFExceptionIR, CleanupOnlySplitFragmentsDoNotWrapLoneIfGoto) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ActionVA = FunctionVA + 0x60;
  MedFunc Med;
  Med.Entry = FunctionVA;
  Med.Name = "cleanup_split_if_goto";
  Med.ReturnType = NdType::makeVoid();
  MedVar Arg0;
  Arg0.Kind = MedVar::Param;
  Arg0.Id = 0;
  Arg0.Size = 8;
  Arg0.TheArch = Arch::X64;
  Med.Params.push_back(Arg0);

  MedBlock Work;
  Work.Id = 0;
  Work.StartAddr = FunctionVA;
  Work.EndAddr = FunctionVA + 0x50;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = FunctionVA + 0x24;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Work.Ops.push_back(std::move(Call));
  MedOp Br;
  Br.Opcode = NdOp::COND_BR;
  Br.Addr = FunctionVA + 0x44;
  Br.addInput(MedVar::makeConst(FunctionVA + 0x50, 8));
  Br.addInput(MedVar::makeConst(1, 1));
  Work.Ops.push_back(std::move(Br));
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = FunctionVA + 0x48;
  Work.Ops.push_back(std::move(Ret));
  Med.Blocks.push_back(std::move(Work));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = ActionVA;
  Handler.EndAddr = ActionVA + 0x10;
  MedOp Dtor;
  Dtor.Opcode = NdOp::CALL;
  Dtor.Addr = ActionVA;
  Dtor.addInput(MedVar::makeConst(0x14000F000, 8));
  Handler.Ops.push_back(std::move(Dtor));
  Med.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x70};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 1;
  Cxx.UnwindMap = {{-1, 0}};
  Cxx.UnwindMap[0].ActionVA = ActionVA;
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::Direct;
  Cxx.IPMap = {{FunctionVA, -1},
               {FunctionVA + 0x20, 0},
               {FunctionVA + 0x30, -1},
               {FunctionVA + 0x40, 0},
               {FunctionVA + 0x50, -1}};
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Med.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Med, Arch::X64);
  EXPECT_EQ(High.StructuredExceptionRegions, 0u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 1u);
  bool LoneIfTry = false;
  walkStmts(High.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::CxxTry && S.EHIsReducible)
      LoneIfTry = true;
  });
  EXPECT_FALSE(LoneIfTry);
}

TEST(COFFExceptionIR, NestsCxxTriesThatShareIpInterval) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "nested_cxx";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x20;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Func.Entry + 8;
  Call.addInput(MedVar::makeConst(Func.Entry + 0x100, 8));
  Protected.Ops.push_back(Call);
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = Func.Entry + 0x10;
  Protected.Ops.push_back(Ret);
  Func.Blocks.push_back(std::move(Protected));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x20};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler4;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 4;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {0, 0}, {-1, 0}};
  Cxx.IPMap = {{Func.Entry, -1},
               {Func.Entry + 4, 1},
               {Func.Entry + 0x18, -1}};
  CxxTryBlock Inner;
  Inner.TryLow = 1;
  Inner.TryHigh = 1;
  Inner.CatchHigh = 2;
  CxxCatchHandler InnerCatch;
  InnerCatch.HandlerVA = Func.Entry + 0x40;
  InnerCatch.Adjectives = 0x9;
  Inner.Handlers.push_back(InnerCatch);
  CxxTryBlock Outer;
  Outer.TryLow = 0;
  Outer.TryHigh = 2;
  Outer.CatchHigh = 3;
  CxxCatchHandler OuterCatch;
  OuterCatch.HandlerVA = Func.Entry + 0x50;
  OuterCatch.Adjectives = 0x9;
  Outer.Handlers.push_back(OuterCatch);
  Cxx.TryBlocks.push_back(std::move(Inner));
  Cxx.TryBlocks.push_back(std::move(Outer));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_GE(High.StructuredExceptionRegions, 2u);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt *OuterTry = nullptr;
  for (const HighStmt &S : High.Body)
    if (S.Kind == StmtKind::CxxTry)
      OuterTry = &S;
  ASSERT_NE(OuterTry, nullptr);
  bool Nested = false;
  for (const HighStmt &S : OuterTry->Body)
    if (S.Kind == StmtKind::CxxTry)
      Nested = true;
  EXPECT_TRUE(Nested) << "inner try must stay nested, not a sibling catch";
  EXPECT_EQ(OuterTry->EHClauses.size(), 1u);
}

TEST(COFFExceptionIR, StructuresOuterCxxTrySplitByNegativeIpState) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "outer_split_ip";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x50;
  MedOp OuterCall;
  OuterCall.Opcode = NdOp::CALL;
  OuterCall.Addr = Func.Entry + 8;
  OuterCall.addInput(MedVar::makeConst(Func.Entry + 0x100, 8));
  Protected.Ops.push_back(OuterCall);
  MedOp InnerCall;
  InnerCall.Opcode = NdOp::CALL;
  InnerCall.Addr = Func.Entry + 0x18;
  InnerCall.addInput(MedVar::makeConst(Func.Entry + 0x110, 8));
  Protected.Ops.push_back(InnerCall);
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = Func.Entry + 0x48;
  Protected.Ops.push_back(Ret);
  Func.Blocks.push_back(std::move(Protected));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x50};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler4;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 4;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {0, 0}, {-1, 0}};
  Cxx.IPMap = {{Func.Entry, -1},
               {Func.Entry + 4, 0},
               {Func.Entry + 0x10, 2},
               {Func.Entry + 0x30, 0},
               {Func.Entry + 0x40, -1},
               {Func.Entry + 0x44, 0}};
  CxxTryBlock Inner;
  Inner.TryLow = 2;
  Inner.TryHigh = 2;
  Inner.CatchHigh = 3;
  CxxCatchHandler InnerCatch;
  InnerCatch.HandlerVA = Func.Entry + 0x80;
  InnerCatch.Adjectives = 0x9;
  Inner.Handlers.push_back(InnerCatch);
  CxxTryBlock Outer;
  Outer.TryLow = 0;
  Outer.TryHigh = 2;
  Outer.CatchHigh = 3;
  CxxCatchHandler OuterCatch;
  OuterCatch.HandlerVA = Func.Entry + 0x90;
  OuterCatch.TypeDescriptorVA = 0;
  OuterCatch.Adjectives = 0x40;
  Outer.Handlers.push_back(OuterCatch);
  Cxx.TryBlocks.push_back(std::move(Inner));
  Cxx.TryBlocks.push_back(std::move(Outer));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_GE(High.StructuredExceptionRegions, 2u);
  const HighStmt *OuterTry = nullptr;
  for (const HighStmt &S : High.Body)
    if (S.Kind == StmtKind::CxxTry)
      OuterTry = &S;
  ASSERT_NE(OuterTry, nullptr);
  bool Nested = false;
  for (const HighStmt &S : OuterTry->Body)
    if (S.Kind == StmtKind::CxxTry)
      Nested = true;
  EXPECT_TRUE(Nested);
  bool CatchAll = false;
  for (const HighEHClause &Clause : OuterTry->EHClauses)
    if (Clause.Kind == HighEHClauseKind::CxxCatch &&
        Clause.TypeDescriptorVA == 0 && Clause.Adjectives == 0x40)
      CatchAll = true;
  EXPECT_TRUE(CatchAll);
}

TEST(COFFExceptionIR, StructuresSingleBlockSEHHandlerBody) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  constexpr va_t ContinuationVA = FunctionVA + 0x30;
  constexpr va_t HandlerMarkerVA = 0x140009000;

  MedFunc Func =
      makeWindowsHandlerFixture("structured_seh_handler", HandlerMarkerVA);

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = HandlerVA;
  SEHExceptionInfo SEH;
  SEH.Scopes.push_back(std::move(Scope));
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.StructuredExceptionRegions, 1u);
  ASSERT_EQ(High.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::SEHTry);
  ASSERT_EQ(Try.Body.size(), 1u);
  EXPECT_EQ(Try.Body.front().Kind, StmtKind::Call);
  EXPECT_EQ(Try.Body.front().Addr, FunctionVA + 4);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  ASSERT_EQ(Try.EHClauseBodies.front().size(), 1u);
  EXPECT_EQ(Try.EHClauseBodies.front().front().Kind, StmtKind::Call);
  EXPECT_EQ(Try.EHClauseBodies.front().front().Addr, HandlerVA);

  ASSERT_EQ(High.Body.size(), 2u);
  EXPECT_EQ(High.Body.back().Kind, StmtKind::Return);
  EXPECT_EQ(High.Body.back().Addr, ContinuationVA);
  size_t HandlerStatements = 0;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    HandlerStatements += Stmt.Addr == HandlerVA;
  });
  EXPECT_EQ(HandlerStatements, 1u);

  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  const size_t Except = Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {");
  ASSERT_NE(Except, std::string::npos);
  const size_t Marker = Source.find("sub_140009000();", Except);
  ASSERT_NE(Marker, std::string::npos);
  EXPECT_EQ(Source.find("goto L_140001020"), std::string::npos);
  EXPECT_EQ(Source.find("native handler target @"), std::string::npos);
  EXPECT_EQ(Source.find("sub_140009000();", Marker + 1), std::string::npos);
}

TEST(COFFExceptionIR, StructuresSingleBlockFH3CatchBody) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  constexpr va_t ContinuationVA = FunctionVA + 0x30;
  constexpr va_t HandlerMarkerVA = 0x14000a000;

  MedFunc Func =
      makeWindowsHandlerFixture("structured_fh3_handler", HandlerMarkerVA);
  Func.ExceptionMetadata = makeFH3Metadata({HandlerVA});
  ASSERT_TRUE(Func.ExceptionMetadata->Cxx->hasValidStateGraph());

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.StructuredExceptionRegions, 1u);
  ASSERT_EQ(High.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Try.Body.size(), 1u);
  EXPECT_EQ(Try.Body.front().Kind, StmtKind::Call);
  EXPECT_EQ(Try.Body.front().Addr, FunctionVA + 4);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  ASSERT_EQ(Try.EHClauseBodies.front().size(), 1u);
  EXPECT_EQ(Try.EHClauseBodies.front().front().Kind, StmtKind::Call);
  EXPECT_EQ(Try.EHClauseBodies.front().front().Addr, HandlerVA);

  ASSERT_EQ(High.Body.size(), 2u);
  EXPECT_EQ(High.Body.back().Kind, StmtKind::Return);
  EXPECT_EQ(High.Body.back().Addr, ContinuationVA);
  size_t HandlerStatements = 0;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    HandlerStatements += Stmt.Addr == HandlerVA;
  });
  EXPECT_EQ(HandlerStatements, 1u);

  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  const size_t CatchDescription = Source.find("catch (");
  ASSERT_NE(CatchDescription, std::string::npos);
  const size_t Marker = Source.find("sub_14000A000();", CatchDescription);
  ASSERT_NE(Marker, std::string::npos);
  EXPECT_EQ(Source.find("sub_14000A000();", Marker + 1), std::string::npos);
}

TEST(COFFExceptionIR, StructuresSingleBlockSEHFinallyBody) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  MedFunc Func =
      makeWindowsHandlerFixture("structured_seh_finally", 0x14000e000);

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Scope.Kind = SEHScopeKind::Finally;
  Scope.FilterOrFinallyVA = HandlerVA;
  Scope.HandlerVA = HandlerVA;
  SEHExceptionInfo SEH;
  SEH.Scopes.push_back(std::move(Scope));
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::SEHTry);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses.front().Kind, HighEHClauseKind::SEHFinally);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  ASSERT_EQ(Try.EHClauseBodies.front().size(), 1u);
  EXPECT_EQ(Try.EHClauseBodies.front().front().Addr, HandlerVA);
  ASSERT_EQ(High.Body.size(), 2u);
  EXPECT_EQ(High.Body.back().Addr, FunctionVA + 0x30);

  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  const size_t Finally = Source.find("} __finally {");
  ASSERT_NE(Finally, std::string::npos);
  EXPECT_NE(Source.find("sub_14000E000();", Finally), std::string::npos);
  EXPECT_EQ(Source.find("native finally funclet @"), std::string::npos);
}

TEST(COFFExceptionIR, StructuresSingleBlockFH3CleanupBody) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ActionVA = FunctionVA + 0x20;
  MedFunc Func =
      makeWindowsHandlerFixture("structured_fh3_cleanup", 0x14000f000);
  ExceptionFunction EH = makeFH3Metadata({0x180001000});
  ASSERT_TRUE(EH.Cxx.has_value());
  EH.Cxx->UnwindMap[0].ActionVA = ActionVA;
  EH.Cxx->UnwindMap[0].Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Func.ExceptionMetadata = std::move(EH);
  ASSERT_TRUE(Func.ExceptionMetadata->Cxx->hasValidStateGraph());

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Try.EHClauses.size(), 2u);
  ASSERT_EQ(Try.EHClauseBodies.size(), 2u);
  EXPECT_TRUE(Try.EHClauseBodies[0].empty());
  ASSERT_EQ(Try.EHClauseBodies[1].size(), 1u);
  EXPECT_EQ(Try.EHClauses[1].Kind, HighEHClauseKind::CxxCleanup);
  EXPECT_EQ(Try.EHClauseBodies[1].front().Addr, ActionVA);
  ASSERT_EQ(High.Body.size(), 2u);
  EXPECT_EQ(High.Body.back().Addr, FunctionVA + 0x30);

  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  const size_t Cleanup = Source.find("cleanup(state=0");
  ASSERT_NE(Cleanup, std::string::npos);
  EXPECT_NE(Source.find("sub_14000F000();", Cleanup), std::string::npos);
}

TEST(COFFExceptionIR, LeavesSharedFH3HandlerOutOfLineWithoutDuplication) {
  constexpr va_t HandlerVA = 0x140001020;
  MedFunc Func = makeWindowsHandlerFixture("shared_fh3_handler", 0x14000b000);
  Func.ExceptionMetadata = makeFH3Metadata({HandlerVA, HandlerVA});
  ASSERT_TRUE(Func.ExceptionMetadata->Cxx->hasValidStateGraph());

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.StructuredExceptionRegions, 1u);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Try.EHClauseBodies.size(), 2u);
  EXPECT_TRUE(Try.EHClauseBodies[0].empty());
  EXPECT_TRUE(Try.EHClauseBodies[1].empty());

  ASSERT_EQ(High.Body.size(), 3u);
  EXPECT_EQ(High.Body[1].Kind, StmtKind::Call);
  EXPECT_EQ(High.Body[1].Addr, HandlerVA);
  EXPECT_EQ(High.Body[2].Kind, StmtKind::Return);
  size_t HandlerStatements = 0;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    HandlerStatements += Stmt.Addr == HandlerVA;
  });
  EXPECT_EQ(HandlerStatements, 1u);

  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  const size_t FirstDescription = Source.find("handler @ 0x140001020");
  ASSERT_NE(FirstDescription, std::string::npos);
  EXPECT_NE(Source.find("handler @ 0x140001020", FirstDescription + 1),
            std::string::npos);
  const size_t Marker = Source.find("sub_14000B000();", FirstDescription);
  ASSERT_NE(Marker, std::string::npos);
  EXPECT_EQ(Source.find("sub_14000B000();", Marker + 1), std::string::npos);
}

TEST(COFFExceptionIR, LeavesAmbiguousFH3HandlerBlocksOutOfLine) {
  constexpr va_t HandlerVA = 0x140001020;
  MedFunc Func =
      makeWindowsHandlerFixture("ambiguous_fh3_handler", 0x14000c000, 2);
  Func.ExceptionMetadata = makeFH3Metadata({HandlerVA});
  ASSERT_TRUE(Func.ExceptionMetadata->Cxx->hasValidStateGraph());

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  EXPECT_TRUE(Try.EHClauseBodies.front().empty());

  size_t HandlerStatements = 0;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    HandlerStatements += Stmt.Addr == HandlerVA;
  });
  EXPECT_EQ(HandlerStatements, 2u);
  ASSERT_GE(High.Body.size(), 4u);
  EXPECT_EQ(High.Body.back().Kind, StmtKind::Return);
  EXPECT_EQ(High.Body.back().Addr, 0x140001030u);
}

TEST(COFFExceptionIR, DoesNotExtractFH3HandlerCrossingProtectedRange) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t HandlerVA = FunctionVA + 8;
  MedFunc Func = makeWindowsHandlerFixture("crossing_fh3_handler", 0x140010000);
  ASSERT_GE(Func.Blocks.size(), 3u);
  Func.Blocks[1].StartAddr = HandlerVA;
  Func.Blocks[1].EndAddr = FunctionVA + 0x18;
  ASSERT_FALSE(Func.Blocks[1].Ops.empty());
  Func.Blocks[1].Ops.front().Addr = HandlerVA;
  Func.ExceptionMetadata = makeFH3Metadata({HandlerVA});
  ASSERT_TRUE(Func.ExceptionMetadata->Cxx->hasValidStateGraph());

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  EXPECT_TRUE(Try.EHClauseBodies.front().empty());

  size_t HandlerStatements = 0;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    HandlerStatements += Stmt.Addr == HandlerVA;
  });
  EXPECT_EQ(HandlerStatements, 1u);
  ASSERT_EQ(Try.Body.size(), 2u);
  EXPECT_EQ(Try.Body[1].Addr, HandlerVA);
  EXPECT_EQ(High.Body.back().Addr, FunctionVA + 0x30);
}

TEST(COFFExceptionIR, LeavesExternalFH3HandlerAsAddressDescription) {
  constexpr va_t ExternalHandlerVA = 0x180001000;
  MedFunc Func =
      makeWindowsHandlerFixture("external_fh3_handler", 0x14000d000, 0);
  Func.ExceptionMetadata = makeFH3Metadata({ExternalHandlerVA});
  ASSERT_TRUE(Func.ExceptionMetadata->Cxx->hasValidStateGraph());

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  EXPECT_TRUE(Try.EHClauseBodies.front().empty());
  ASSERT_EQ(High.Body.size(), 2u);
  EXPECT_EQ(High.Body.back().Kind, StmtKind::Return);
  EXPECT_EQ(High.Body.back().Addr, 0x140001030u);

  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  EXPECT_NE(Source.find("handler @ 0x180001000"), std::string::npos);
  EXPECT_NE(Source.find("catch ("), std::string::npos);
}

TEST(COFFExceptionIR, SplitsProtectedRangesAndKeepsEdgesSeparate) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Segment Text;
  Text.VA = Img.Base + 0x1000;
  Text.Size = 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0x90, 0x90, 0x90, 0xc3}; // nop; nop; nop; ret
  Img.Segments.push_back(std::move(Text));

  ExceptionFunction EH;
  EH.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1004};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Img.Base + 0x1001, Img.Base + 0x1003};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Img.Base + 0x1003;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  LowFunc Func = Builder.build(Img, Dec, Img.Base + 0x1000, "seh_cfg");
  ASSERT_TRUE(Func.ExceptionMetadata.has_value());
  ASSERT_EQ(Func.Blocks.size(), 3u);
  EXPECT_EQ(Func.Blocks[0].StartAddr, Img.Base + 0x1000);
  EXPECT_EQ(Func.Blocks[1].StartAddr, Img.Base + 0x1001);
  EXPECT_EQ(Func.Blocks[2].StartAddr, Img.Base + 0x1003);
  ASSERT_EQ(Func.Blocks[1].ExceptionalSuccs.size(), 1u);
  EXPECT_EQ(Func.Blocks[1].ExceptionalSuccs[0].Kind,
            ExceptionalEdgeKind::SEHHandler);
  EXPECT_EQ(Func.Blocks[1].ExceptionalSuccs[0].BlockId, 2);
  EXPECT_EQ(Func.Blocks[1].Succs.size(), 1u);
  EXPECT_EQ(Func.Blocks[1].Succs[0], 2);
}

TEST(COFFExceptionIR, DecompileRetainsFaithfulCxxAnnotation) {
  HighFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "cxx_annotation_test";
  Func.ReturnType = NdType::makeInt(8);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 8);
  Func.Body.push_back(std::move(Return));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap.push_back({-1, Func.Entry + 0x30});
  Cxx.UnwindMap.push_back({0, Func.Entry + 0x34});
  Cxx.UnwindMap.back().Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Cxx.UnwindMap.back().ObjectOffset = -16;
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.TypeDescriptorVA = 0x140003000;
  Catch.CatchObjectOffset = -32;
  Catch.HandlerVA = Func.Entry + 0x20;
  Catch.ParentFrameOffset = -8;
  Catch.ContinuationVAs.push_back(Func.Entry + 0x38);
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS));
  OS.flush();
  EXPECT_NE(Source.find("neverd.exception: encoding=x64-unwind-v1"),
            std::string::npos);
  EXPECT_NE(Source.find("personality=__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(Source.find("cxx.try[0]"), std::string::npos);
  EXPECT_NE(Source.find("handler=0x140001020"), std::string::npos);
  EXPECT_NE(Source.find("kind=destructor-object-pointer"), std::string::npos);
  EXPECT_NE(Source.find("object_offset=-16"), std::string::npos);
  EXPECT_NE(Source.find("adjectives=0x40"), std::string::npos);
  EXPECT_NE(Source.find("parent_frame_offset=-8"), std::string::npos);
  EXPECT_NE(Source.find("continuations=0x140001038"), std::string::npos);
}

TEST(COFFExceptionIR, PdataRangeStartsKeepTailJmpFromFusingCallee) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t First = 0x140001000;
  constexpr va_t Second = 0x140001005;
  Img.Entry = First;

  Segment Text;
  Text.Name = ".text";
  Text.VA = First;
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  // jmp rel8 to Second, then the callee body `mov eax, 0x2b; ret`.
  Text.Data[0] = 0xeb;
  Text.Data[1] = 0x03;
  Text.Data[Second - First] = 0xb8;
  Text.Data[Second - First + 1] = 0x2b;
  Text.Data[Second - First + 2] = 0;
  Text.Data[Second - First + 3] = 0;
  Text.Data[Second - First + 4] = 0;
  Text.Data[Second - First + 5] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = First;
  TextSection.Size = 0x10;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(First, Second);
  Img.KnownCodeRanges.emplace_back(Second, Second + 6);
  Img.Symbols.push_back(Symbol::makeFunc(First, Second - First));
  ExceptionFunction EH;
  EH.CodeRange = {First, Second};
  EH.Kind = RuntimeFunctionKind::Primary;
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(First);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_EQ(Source.find("0x2b"), std::string::npos)
      << "tail jmp to the next pdata function must not fuse its body:\n"
      << Source;
}

TEST(COFFExceptionIR, PdataIndexKeepsTailJmpFromFusingUnmaterializedCallee) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t First = 0x140001000;
  constexpr va_t Second = 0x140002000;
  Img.Entry = First;

  Segment Text;
  Text.Name = ".text";
  Text.VA = First;
  Text.Size = 0x1010;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  // jmp rel32 from First to a non-neighbor pdata start.
  const uint32_t Rel = static_cast<uint32_t>(Second - (First + 5));
  Text.Data[0] = 0xe9;
  Text.Data[1] = static_cast<uint8_t>(Rel);
  Text.Data[2] = static_cast<uint8_t>(Rel >> 8);
  Text.Data[3] = static_cast<uint8_t>(Rel >> 16);
  Text.Data[4] = static_cast<uint8_t>(Rel >> 24);
  Text.Data[Second - First] = 0xb8;
  Text.Data[Second - First + 1] = 0x2b;
  Text.Data[Second - First + 2] = 0;
  Text.Data[Second - First + 3] = 0;
  Text.Data[Second - First + 4] = 0;
  Text.Data[Second - First + 5] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = First;
  TextSection.Size = 0x1010;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(First, First + 5);
  Img.COFFPDataRecords.push_back({0x1000, 0x1005, 0, 0});
  Img.COFFPDataRecords.push_back({0x2000, 0x2006, 0, 0});
  Img.Symbols.push_back(Symbol::makeFunc(First, 5));
  ExceptionFunction EH;
  EH.CodeRange = {First, First + 5};
  EH.Kind = RuntimeFunctionKind::Primary;
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();
  ASSERT_TRUE(Img.hasKnownFunctionEntryAt(Second));
  ASSERT_FALSE(Img.ExceptionMetadata.findFunction(Second));

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(First);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  ASSERT_EQ(Result.LowFuncs.size(), 1u);
  size_t Ops = 0;
  for (const auto &B : Result.LowFuncs[0].Blocks)
    Ops += B.Ops.size();
  EXPECT_LT(Ops, 8u) << "far tail jmp must not fuse the pdata callee";
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_EQ(Source.find("0x2b"), std::string::npos)
      << "tail jmp to an unmaterialized pdata start must not fuse its body:\n"
      << Source;
}

TEST(COFFExceptionIR, OnlyFunctionEntriesSkipsUnrequestedFunctions) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t First = 0x140001000;
  constexpr va_t Second = 0x140001010;
  Img.Entry = First;

  Segment Text;
  Text.Name = ".text";
  Text.VA = First;
  Text.Size = 0x20;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  Text.Data[0] = 0xc3;
  Text.Data[Second - First] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = First;
  TextSection.Size = 0x20;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(First, First + 1);
  Img.KnownCodeRanges.emplace_back(Second, Second + 1);
  Img.Symbols.push_back(Symbol::makeFunc(First, 1));
  Img.Symbols.push_back(Symbol::makeFunc(Second, 1));

  llvm::LLVMContext Ctx;
  PipelineOptions All;
  All.EmitDumpOutput = false;
  auto AllResult = Pipeline().run(Img, Ctx, All);
  ASSERT_TRUE(AllResult.Success) << AllResult.Error;
  ASSERT_GE(AllResult.HighFuncs.size(), 2u);

  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(Second);
  auto OneResult = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(OneResult.Success) << OneResult.Error;
  ASSERT_EQ(OneResult.HighFuncs.size(), 1u);
  EXPECT_EQ(OneResult.HighFuncs[0].Entry, Second);
  ASSERT_EQ(OneResult.LowFuncs.size(), 1u);
  EXPECT_EQ(OneResult.LowFuncs[0].Entry, Second);
}

TEST(COFFExceptionIR, OnlyFunctionEntriesSkipsHugeOwnerUnwindGraph) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t First = 0x140001000;
  constexpr uint64_t Huge = limits::kMaxOnlyFunctionEHOwnerSize + 0x1000;
  Img.Entry = First;

  Segment Text;
  Text.Name = ".text";
  Text.VA = First;
  Text.Size = Huge;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x20, 0xcc);
  Text.Data[0] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = First;
  TextSection.Size = Huge;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(First, First + 1);
  Img.Symbols.push_back(Symbol::makeFunc(First, 1));

  ExceptionFunction EH;
  EH.CodeRange = {First, First + Huge};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 8;
  Cxx.UnwindMap.assign(8, {-1, 0});
  for (unsigned I = 0; I < Cxx.UnwindMap.size(); ++I) {
    Cxx.UnwindMap[I].ActionVA =
        First + limits::kMaxOverlapDistance + 0x1000 + 0x20 * I;
    Img.KnownCodeRanges.emplace_back(Cxx.UnwindMap[I].ActionVA,
                                     Cxx.UnwindMap[I].ActionVA + 1);
    Img.Symbols.push_back(Symbol::makeFunc(Cxx.UnwindMap[I].ActionVA, 1));
  }
  EH.Cxx = std::move(Cxx);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(First);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  EXPECT_EQ(Result.HighFuncs[0].Entry, First);
}

TEST(COFFExceptionIR, OnlyFunctionEntriesSkipsInteriorUnwindAction) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t First = 0x140001000;
  constexpr va_t Foreign = 0x140010000;
  constexpr va_t Action = 0x140011000;
  constexpr uint64_t ForeignSize = limits::kMaxOnlyFunctionEHOwnerSize + 0x2000;
  Img.Entry = First;

  Segment Text;
  Text.Name = ".text";
  Text.VA = First;
  Text.Size = Foreign + ForeignSize - First;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x40, 0xcc);
  Text.Data[0] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = First;
  TextSection.Size = Foreign + ForeignSize - First;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(First, First + 1);
  Img.KnownCodeRanges.emplace_back(Foreign, Foreign + ForeignSize);
  Img.Symbols.push_back(Symbol::makeFunc(First, 1));

  ExceptionFunction Small;
  Small.CodeRange = {First, First + 0x20};
  Small.Kind = RuntimeFunctionKind::Primary;
  Small.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 1;
  Cxx.UnwindMap.push_back({-1, Action});
  Small.Cxx = std::move(Cxx);
  Img.ExceptionMetadata.Functions.push_back(std::move(Small));

  ExceptionFunction Huge;
  Huge.CodeRange = {Foreign, Foreign + ForeignSize};
  Huge.Kind = RuntimeFunctionKind::Primary;
  Img.ExceptionMetadata.Functions.push_back(std::move(Huge));
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(First);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  EXPECT_EQ(Result.HighFuncs[0].Entry, First);
}

} // namespace
