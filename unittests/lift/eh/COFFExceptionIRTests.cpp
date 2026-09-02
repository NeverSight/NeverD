//===- COFFExceptionIRTests.cpp - Windows EH IR carriage tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
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
  EXPECT_NE(Source.find("__except (1)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_140009000();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000A000();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000B000();"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("extern int sub_"), std::string::npos) << Source;

  REQUIRE_HOST_FIXTURE_COMPILER();
  CompilerResult Syntax = runCCompiler(Source, {"-std=c11", "-fsyntax-only"});
  ASSERT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  CompilerResult Preprocessed = runCCompiler(Source, {"-std=c11", "-E", "-P"});
  ASSERT_EQ(Preprocessed.ExitCode, 0) << Preprocessed.Error;
  EXPECT_NE(Preprocessed.Output.find("__builtin_trap"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("sub_1400"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("__except"), std::string::npos)
      << Preprocessed.Output;
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
  EXPECT_NE(Source.find("funclet@0x180001000"), std::string::npos) << Source;

  REQUIRE_HOST_FIXTURE_COMPILER();
  CompilerResult Preprocessed = runCCompiler(Source, {"-std=c11", "-E", "-P"});
  ASSERT_EQ(Preprocessed.ExitCode, 0) << Preprocessed.Error;
  EXPECT_NE(Preprocessed.Output.find("__builtin_trap"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("sub_1400"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("funclet@"), std::string::npos)
      << Preprocessed.Output;
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
  EXPECT_EQ(Source.find("funclet@"), std::string::npos) << Source;
  EXPECT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;

  REQUIRE_HOST_FIXTURE_COMPILER();
  CompilerResult Syntax = runCCompiler(Source, {"-std=c11", "-fsyntax-only"});
  EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
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
  EXPECT_EQ(Source.find("void unsafe_attached_helper(void);"),
            std::string::npos)
      << Source;

  REQUIRE_HOST_FIXTURE_COMPILER();
  CompilerResult Syntax = runCCompiler(Source, {"-std=c11", "-fsyntax-only"});
  ASSERT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  CompilerResult Preprocessed = runCCompiler(Source, {"-std=c11", "-E", "-P"});
  ASSERT_EQ(Preprocessed.ExitCode, 0) << Preprocessed.Error;
  EXPECT_NE(Preprocessed.Output.find("__builtin_trap"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("unsafe_"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("catchswitch"), std::string::npos)
      << Preprocessed.Output;
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

  REQUIRE_HOST_FIXTURE_COMPILER();
  CompilerResult Syntax = runCCompiler(Source, {"-std=c11", "-fsyntax-only"});
  ASSERT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  CompilerResult Preprocessed = runCCompiler(Source, {"-std=c11", "-E", "-P"});
  ASSERT_EQ(Preprocessed.ExitCode, 0) << Preprocessed.Error;
  EXPECT_NE(Preprocessed.Output.find("__builtin_trap"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_NE(Preprocessed.Output.find("highc_unwind_only"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("pwned();"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("void injected"), std::string::npos)
      << Preprocessed.Output;
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
  EXPECT_NE(Source.find("collision_x2D_name(void)"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("collision_x2D_name_2(void)"), std::string::npos)
      << Source;

  REQUIRE_HOST_FIXTURE_COMPILER();
  CompilerResult Syntax = runCCompiler(Source, {"-std=c11", "-fsyntax-only"});
  ASSERT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  CompilerResult Preprocessed = runCCompiler(Source, {"-std=c11", "-E", "-P"});
  ASSERT_EQ(Preprocessed.ExitCode, 0) << Preprocessed.Error;
  EXPECT_NE(Preprocessed.Output.find("__builtin_trap"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("unsafe_alias_helper"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("unsafe_forwarder_helper"),
            std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("unsafe_provenance_helper"),
            std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("CxxFrameHandler"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("personality_seh0"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("pwned();"), std::string::npos)
      << Preprocessed.Output;
  EXPECT_EQ(Preprocessed.Output.find("void injected"), std::string::npos)
      << Preprocessed.Output;
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
  const size_t Except = Source.find("} __except (1) {");
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
  const size_t CatchDescription = Source.find("funclet@0x140001020");
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
  const size_t FirstDescription = Source.find("funclet@0x140001020");
  ASSERT_NE(FirstDescription, std::string::npos);
  EXPECT_NE(Source.find("funclet@0x140001020", FirstDescription + 1),
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
  EXPECT_NE(Source.find("funclet@0x180001000"), std::string::npos);
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

} // namespace
