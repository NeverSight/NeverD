//===- COFFExceptionIRTests.cpp - Windows EH IR carriage tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../core/SourceCallExecution.h"
#include "CSourceText.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/CxxObjectActions.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
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

#include <algorithm>
#include <optional>
#include <regex>
#include <tuple>

#ifndef NEVERD_RUNTIME_FIXTURE_COMPILER
#define NEVERD_RUNTIME_FIXTURE_COMPILER ""
#endif

namespace {

using namespace neverd;

// PHI edges may use a temporary so all sources are read before any PHI is
// overwritten. Check the value reaching the destination in either spelling.
size_t findEdgeCopy(const std::string &Source, const std::string &Destination,
                    const std::string &Value, size_t Start) {
  const auto Direct = Source.find(Destination + " = " + Value + ";", Start);
  if (Direct != std::string::npos)
    return Direct;
  const std::string Assignment = " = " + Value + ";";
  for (auto At = Source.find(Assignment, Start); At != std::string::npos;
       At = Source.find(Assignment, At + Assignment.size())) {
    const auto NameStart = Source.find_last_of(" \n", At - 1);
    if (NameStart == std::string::npos)
      continue;
    const std::string Temporary =
        Source.substr(NameStart + 1, At - NameStart - 1);
    const auto Copy = Source.find(Destination + " = " + Temporary + ";", At);
    const auto BlockEnd = Source.find('}', At);
    if (Copy != std::string::npos && Copy < BlockEnd)
      return Copy;
  }
  return std::string::npos;
}

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

void checkStandaloneSEHSource(llvm::StringRef Source,
                              bool CompileObject = false) {
  ASSERT_TRUE(hasHostFixtureCompiler());
  // Supply the small platform-header interface independently of the emitter.
  // The compiler still checks the real SEH intrinsic scopes, and the source
  // itself must include the header rather than relying on a test prelude.
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-seh-headers", Directory));
  llvm::FileRemover RemoveDirectory(Directory);
  const std::string HeaderPath = Directory.str().str() + "/excpt.h";
  llvm::FileRemover RemoveHeader(HeaderPath);
  std::error_code Error;
  {
    llvm::raw_fd_ostream Header(HeaderPath, Error);
    ASSERT_FALSE(Error);
    Header << "#define EXCEPTION_EXECUTE_HANDLER 1\n"
              "#define EXCEPTION_CONTINUE_SEARCH 0\n"
              "#define EXCEPTION_CONTINUE_EXECUTION (-1)\n"
              "unsigned long _exception_code(void);\n"
              "void *_exception_info(void);\n"
              "#define exception_code _exception_code\n"
              "#define GetExceptionCode() _exception_code()\n"
              "#define GetExceptionInformation() _exception_info()\n"
              "int _abnormal_termination(void);\n"
              "#define AbnormalTermination _abnormal_termination\n";
  }
  const auto Result = runCCompiler(
      Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions", "-std=c11",
               "-fsyntax-only", "-isystem", Directory});
  EXPECT_EQ(Result.ExitCode, 0) << Result.Error << "\n" << Source.str();
  if (CompileObject) {
    const std::string ObjectPath = Directory.str().str() + "/seh.obj";
    llvm::FileRemover RemoveObject(ObjectPath);
    for (llvm::StringRef Level : {"-O0", "-O2"}) {
      // SEH catches machine faults as well as language-level throwing calls.
      // In particular, a trap in a nested protected region must keep the
      // parent's unwind registration and escaped filter locals at O0.
      const auto Object = runCCompiler(
          Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                   "-fasync-exceptions", "-std=c11", Level, "-c", "-o",
                   ObjectPath, "-isystem", Directory});
      EXPECT_EQ(Object.ExitCode, 0) << Object.Error << "\n" << Source.str();
    }
  }
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
                                  unsigned HandlerBlockCopies = 1,
                                  va_t FunctionVA = 0x140001000) {
  const va_t HandlerVA = FunctionVA + 0x20;
  const va_t ContinuationVA = FunctionVA + 0x30;
  const uint16_t PointerSize = FunctionVA > UINT32_MAX ? 8 : 4;

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
  ProtectedMarker.addInput(MedVar::makeConst(FunctionVA + 0x7000, PointerSize));
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
    Marker.addInput(MedVar::makeConst(HandlerMarkerVA + I * 0x10, PointerSize));
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

// The runtime tries the scopes of one range in table order, and a C __try
// has a single handler: such scopes print as nested tries, the first
// innermost.  The outermost arm takes its handler block.  An inner arm leaves
// its handler where it is, after both tries: the outer try does not protect
// that block.
TEST(COFFExceptionIR, HighCSEHScopesSharingARangeNestInTableOrder) {
  constexpr va_t FunctionVA = 0x140001000;
  for (bool FinallyInside : {true, false}) {
    SCOPED_TRACE(FinallyInside);
    MedFunc Func = makeWindowsHandlerFixture("shared_range", 0x140009000);
    ExceptionFunction EH;
    EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Personality = ExceptionPersonality::CSpecificHandler;
    SEHScopeRecord Finally;
    Finally.GuardedRange = {FunctionVA, FunctionVA + 0x10};
    Finally.Kind = SEHScopeKind::Finally;
    Finally.FilterOrFinallyVA = 0x14000C000;
    SEHScopeRecord Except;
    Except.GuardedRange = Finally.GuardedRange;
    Except.Kind = SEHScopeKind::CatchAll;
    Except.HandlerVA = FunctionVA + 0x20;
    EH.SEH.emplace();
    EH.SEH->Scopes = FinallyInside ? std::vector{Finally, Except}
                                   : std::vector{Except, Finally};
    Func.ExceptionMetadata = std::move(EH);

    const HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
    unsigned Tries = 0;
    walkStmts(High.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::SEHTry)
        return;
      ++Tries;
      EXPECT_TRUE(S.EHIsReducible);
      EXPECT_EQ(S.EHClauses.size(), 1u);
    });
    EXPECT_EQ(Tries, 2u);
    const std::string Source = emitHighC({High});
    EXPECT_EQ(Source.find("unstructured SEH"), std::string::npos) << Source;
    const size_t Outer = Source.find("__try {");
    ASSERT_NE(Outer, std::string::npos) << Source;
    const size_t Inner = Source.find("__try {", Outer + 1);
    ASSERT_NE(Inner, std::string::npos) << Source;
    EXPECT_EQ(Source.find_first_not_of(" \n", Outer + 7), Inner) << Source;
    const size_t FinallyArm = Source.find("} __finally {");
    const size_t ExceptArm =
        Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {");
    const size_t Handler = Source.find("sub_140009000();", Outer);
    ASSERT_NE(FinallyArm, std::string::npos) << Source;
    ASSERT_NE(ExceptArm, std::string::npos) << Source;
    ASSERT_NE(Handler, std::string::npos) << Source;
    EXPECT_EQ(FinallyArm < ExceptArm, FinallyInside) << Source;
    // The handler's code is the outermost arm's, or follows both tries:
    // never inside the outer try.
    const size_t OuterArm = FinallyInside ? ExceptArm : FinallyArm;
    const size_t OuterEnd = Source.find('}', OuterArm + 1);
    ASSERT_NE(OuterEnd, std::string::npos) << Source;
    if (FinallyInside)
      EXPECT_TRUE(Handler > OuterArm && Handler < OuterEnd) << Source;
    else
      EXPECT_GT(Handler, OuterEnd) << Source;
    EXPECT_EQ(Source.find("sub_140009000();", Handler + 1), std::string::npos)
        << Source;
  }
}

// MSVC maps the continuation tail of a catch funclet back to a state of the
// try block it handles.  A handler is outside its own try block, so that tail
// is not protected by it; a try block of another handler still protects it.
// A try block whose states the funclet's code never holds dispatches nothing
// from it either, though the funclet shares its parent's table.
TEST(COFFExceptionIR, HighCCatchFuncletIsOutsideItsOwnTry) {
  constexpr va_t ParentVA = 0x140001000;
  constexpr va_t FuncletVA = 0x140002000;
  for (bool OwnTry : {true, false}) {
    SCOPED_TRACE(OwnTry);
    MedFunc Funclet;
    Funclet.Entry = FuncletVA;
    Funclet.Name = "catch_funclet";
    Funclet.ReturnType = NdType::makeVoid();
    MedBlock Body;
    Body.Id = 0;
    Body.StartAddr = FuncletVA;
    Body.EndAddr = FuncletVA + 8;
    Body.Succs = {1};
    MedOp Work;
    Work.Opcode = NdOp::CALL;
    Work.Addr = FuncletVA + 4;
    Work.addInput(MedVar::makeConst(0x140009000, 8));
    Body.Ops.push_back(Work);
    MedBlock Tail;
    Tail.Id = 1;
    Tail.StartAddr = FuncletVA + 8;
    Tail.EndAddr = FuncletVA + 0x10;
    Tail.Preds = {0};
    Work.Addr = FuncletVA + 8;
    Work.Inputs[0] = MedVar::makeConst(0x14000A000, 8);
    Tail.Ops.push_back(Work);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = FuncletVA + 0xc;
    Tail.Ops.push_back(Return);
    Funclet.Blocks = {Body, Tail};

    ExceptionFunction EH;
    EH.CodeRange = {FuncletVA, FuncletVA + 0x10};
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    CxxExceptionInfo Cxx;
    Cxx.MaxState = 3;
    Cxx.UnwindMap = {{-1, 0}, {-1, 0}, {-1, 0}};
    Cxx.IPMap = {{ParentVA, -1},      {ParentVA + 4, 0},     {ParentVA + 8, 2},
                 {ParentVA + 0xc, 0}, {ParentVA + 0x10, -1}, {FuncletVA, 1},
                 {FuncletVA + 8, 0}};
    CxxTryBlock Try;
    Try.TryLow = 0;
    Try.TryHigh = 0;
    Try.CatchHigh = 1;
    CxxCatchHandler Catch;
    Catch.HandlerVA = OwnTry ? FuncletVA : ParentVA + 0x20;
    Try.Handlers.push_back(Catch);
    Cxx.TryBlocks.push_back(Try);
    // A try block of the parent alone, at state 2.
    Try.TryLow = Try.TryHigh = Try.CatchHigh = 2;
    Try.Handlers.front().HandlerVA = ParentVA + 0x30;
    Cxx.TryBlocks.push_back(Try);
    EH.Cxx = std::move(Cxx);
    Funclet.ExceptionMetadata = std::move(EH);

    const HighFunc High = MedToHighConverter().convert(Funclet, Arch::X64);
    bool Protected = false;
    walkStmts(High.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::CxxTry)
        return;
      Protected = true;
      for (const HighEHClause &Clause : S.EHClauses)
        EXPECT_EQ(Clause.HandlerVA, ParentVA + 0x20);
    });
    EXPECT_EQ(Protected, !OwnTry);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.EmitIncludes = false;
    Options.StructuredExceptionSyntax = false;
    EXPECT_TRUE(HighCEmitter().emit({High}, OS, Options)) << Source;
    EXPECT_NE(Source.find("sub_14000A000();"), std::string::npos) << Source;
  }
}

TEST(COFFExceptionIR, HighCCxxTryRendersCatchSyntaxInReadableListing) {
  HighFunc Function;
  Function.Name = "cxx_try_fn";
  Function.Entry = 0x140001000;
  Function.ReturnType = NdType::makeVoid();

  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHIsReducible = true;
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
  Try.EHIsReducible = true;
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
  Try.EHIsReducible = true;
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
  Try.EHIsReducible = true;
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

TEST(COFFExceptionIR, HighCStandaloneSEHIncludesItsPlatformDeclarations) {
  REQUIRE_HOST_FIXTURE_COMPILER();
  for (bool NamedFilter : {false, true}) {
    SCOPED_TRACE(NamedFilter);
    HighFunc Function;
    Function.Name = "seh_header_dependencies";
    Function.Entry = 0x140001000;
    Function.ReturnType = NdType::makeInt(4, false);
    HighStmt Try;
    Try.Kind = StmtKind::SEHTry;
    Try.EHIsReducible = true;
    Try.EHRange = {Function.Entry, Function.Entry + 0x20};
    HighStmt Work;
    Work.Kind = StmtKind::Call;
    Work.CallExpr = HighExpr::makeCall("may_raise", 0x140002000, {});
    Try.Body.push_back(Work);
    HighEHClause Clause;
    Clause.Kind = HighEHClauseKind::SEHExcept;
    Clause.FilterOrActionVA = NamedFilter ? 0x140003000 : 0;
    Clause.HandlerVA = Function.Entry + 0x20;
    Try.EHClauses.push_back(Clause);
    MedVar Code;
    Code.Kind = MedVar::SEHExceptionCode;
    Code.Size = 4;
    Code.ConstVal = Clause.HandlerVA;
    HighStmt HandlerReturn;
    HandlerReturn.Kind = StmtKind::Return;
    HandlerReturn.RetVal = HighExpr::makeVar(Code, Function.ReturnType);
    Try.EHClauseBodies.push_back({HandlerReturn});
    // Header collection must visit regions nested inside ordinary blocks.
    HighStmt Outer;
    Outer.Kind = StmtKind::If;
    Outer.Cond = HighExpr::makeConst(1, 1);
    Outer.Body.push_back(Try);
    Function.Body.push_back(Outer);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(0, 4);
    Function.Body.push_back(Return);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::COFF;
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    ASSERT_TRUE(HighCEmitter().emit({Function}, Stream, Options));
    EXPECT_NE(Source.find("GetExceptionCode()"), std::string::npos);
    EXPECT_NE(Source.find("return seh_exception_code;"), std::string::npos)
        << Source;
    checkStandaloneSEHSource(Source);
  }
}

// A try at Begin whose one __except clause calls the filter at Filter and
// resumes at Handler, around one call.
HighStmt sehTryAround(va_t Begin, va_t End, va_t Filter, va_t Handler,
                      const char *Work, std::vector<HighStmt> Arm = {}) {
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.EHIsReducible = true;
  Try.Addr = Begin;
  Try.EHRange = {Begin, End};
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.Addr = Begin;
  Call.CallExpr = HighExpr::makeCall(Work, 0x140002000, {});
  Try.Body.push_back(std::move(Call));
  HighEHClause Except;
  Except.Kind = HighEHClauseKind::SEHExcept;
  Except.FilterOrActionVA = Filter;
  Except.HandlerVA = Handler;
  Try.EHClauses.push_back(Except);
  Try.EHClauseBodies.push_back(std::move(Arm));
  return Try;
}

ExceptionFunction sehMetadata(va_t Begin, va_t End) {
  ExceptionFunction EH;
  EH.CodeRange = {Begin, End};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  return EH;
}

TEST(COFFExceptionIR, HighCSEHFilterIsNamedAsTheFunctionListNamesIt) {
  // A MinGW object's `.text` section symbol and a linker label start where
  // its first function does; the function's own symbol names the filter
  // there.  An export names a filter no symbol does.
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  for (const char *Name : {".text", "___tls_start__"}) {
    Symbol Label;
    Label.Name = Name;
    Label.Addr = 0x140001100;
    Label.IsSection = Label.Name == ".text";
    Image.Symbols.push_back(Label);
  }
  Symbol Filter;
  Filter.Name = "_gnu_exception_handler";
  Filter.Addr = 0x140001100;
  Filter.IsFunc = true;
  Image.Symbols.push_back(Filter);
  Export Exported;
  Exported.Name = "startup_filter";
  Exported.Addr = 0x140001300;
  Image.Exports.push_back(Exported);

  std::vector<HighFunc> Functions(2);
  for (size_t I = 0; I < Functions.size(); ++I) {
    HighFunc &Function = Functions[I];
    Function.Name = I ? "exported_filter_fn" : "symbol_filter_fn";
    Function.Entry = 0x140001000 + I * 0x40;
    Function.ReturnType = NdType::makeVoid();
    Function.Body.push_back(sehTryAround(Function.Entry, Function.Entry + 0x10,
                                         I ? 0x140001300 : 0x140001100,
                                         0x140001200, "work"));
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.Image = &Image;
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("_gnu_exception_handler(GetExceptionInformation())"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("startup_filter(GetExceptionInformation())"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("text(GetExceptionInformation())"), std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, HighCEmptyExceptArmResumingAfterItsTryPrintsNoJump) {
  // MSVC's nested __try levels at -O0: an empty __except resumes at a lone
  // jmp that ordinary flow takes too, past the inner try.  That handler
  // prints nothing, so the arm ends where its try falls through.
  HighFunc Function;
  Function.Name = "nested_seh";
  Function.Entry = 0x140004A00;
  Function.ReturnType = NdType::makeVoid();
  Function.ExceptionMetadata = sehMetadata(0x140004A00, 0x140004A60);
  HighStmt Anchor;
  Anchor.Kind = StmtKind::Block;
  Anchor.Addr = 0x140004A24;
  HighStmt Recover;
  Recover.Kind = StmtKind::Call;
  Recover.Addr = 0x140004A26;
  Recover.CallExpr = HighExpr::makeCall("recover", 0x140002010, {});
  HighStmt Outer = sehTryAround(0x140004A12, 0x140004A26, 0x1400083C0,
                                0x140004A26, "outer_work", {Recover});
  Outer.Body = {sehTryAround(0x140004A12, 0x140004A24, 0x14000838C, 0x140004A24,
                             "inner_work"),
                Anchor};
  Function.Body.push_back(std::move(Outer));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.Addr = 0x140004A57;
  Function.Body.push_back(Ret);

  const std::string Source = emitHighC({Function});
  const size_t Inner =
      Source.find("} __except (sub_14000838C(GetExceptionInformation())) {");
  ASSERT_NE(Inner, std::string::npos) << Source;
  const size_t Close = Source.find('}', Inner + 1);
  ASSERT_NE(Close, std::string::npos) << Source;
  EXPECT_EQ(Source.find_first_not_of(" \n", Source.find('\n', Inner)), Close)
      << Source;
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  EXPECT_EQ(Source.find("L_140004A24"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, HighCLeftOutTryTailKeepsTheLabelAJumpLandsOn) {
  // A switch's cases leave a __try body for its last statement, an empty
  // anchor the body's printed end leaves out.  Its label stays at the end of
  // the body, where control runs off it.
  HighFunc Function;
  Function.Name = "try_tail_label";
  Function.Entry = 0x140004E90;
  Function.ReturnType = NdType::makeVoid();
  Function.ExceptionMetadata = sehMetadata(0x140004E90, 0x140004F78);
  HighStmt Try =
      sehTryAround(0x140004EA1, 0x140004F4D, 0x140008618, 0x140004F4D, "work");
  HighStmt Leave;
  Leave.Kind = StmtKind::Goto;
  Leave.Addr = 0x140004EC0;
  Leave.GotoTarget = 0x140004F4B;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Addr = 0x140004EB8;
  Branch.Cond = HighExpr::makeCall("failed", 0x140002020, {});
  Branch.Body.push_back(Leave);
  Try.Body.push_back(Branch);
  HighStmt More;
  More.Kind = StmtKind::Call;
  More.Addr = 0x140004F30;
  More.CallExpr = HighExpr::makeCall("more", 0x140002030, {});
  Try.Body.push_back(More);
  HighStmt Anchor;
  Anchor.Kind = StmtKind::Block;
  Anchor.Addr = 0x140004F4B;
  Try.Body.push_back(Anchor);
  Function.Body.push_back(std::move(Try));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.Addr = 0x140004F70;
  Function.Body.push_back(Ret);

  const std::string Source = emitHighC({Function});
  const size_t Jump = Source.find("goto L_140004F4B;");
  const size_t Label = Source.find("L_140004F4B:");
  const size_t Except = Source.find("} __except (");
  ASSERT_NE(Jump, std::string::npos) << Source;
  ASSERT_NE(Label, std::string::npos) << Source;
  EXPECT_LT(Jump, Label) << Source;
  EXPECT_LT(Label, Except) << Source;
}

// `*(int32_t *)*(int64_t *)(arg1 + 0x30) = Value`: a write through the slot
// that the second argument of an x64 termination handler points at.
HighStmt storeThroughEstablisherSlot(uint32_t Value) {
  MedVar Frame;
  Frame.Kind = MedVar::Param;
  Frame.Id = 1;
  Frame.Size = 8;
  Frame.TheArch = Arch::X64;
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeLoad(
      HighExpr::makeBinop(NdOp::INT_ADD,
                          HighExpr::makeVar(Frame, NdType::makeInt(8)),
                          HighExpr::makeConst(0x30, 8)),
      NdType::makeInt(8));
  Store.StoreVal = HighExpr::makeConst(Value, 4);
  return Store;
}

TEST(COFFExceptionIR, HighCSEHHandlerArmReadsItsOwnSecondArgument) {
  // xcpt4's Test61Part2 nests a __try in its __finally. MSVC's termination
  // handler (`push rbx; push rbp; sub rsp, 0x28; mov rbp, rdx`) receives the
  // parent's establisher frame as its second argument and guards its own
  // range. The unwinder resumes that range's __except arm with the RBP the
  // range holds, so the arm reaches the parent's slot through the argument,
  // as the protected body does. Only a catch body attached to a C++ try reads
  // RDX as the frame of the function printed around it.
  constexpr va_t Entry = 0x140004BF0;
  constexpr va_t Handler = 0x140004C17;
  for (bool Cxx : {false, true}) {
    SCOPED_TRACE(Cxx);
    HighFunc Function;
    Function.Name = Cxx ? "catch_parent" : "termination_handler";
    Function.Entry = Entry;
    Function.FrameSize = 0x38;
    Function.ReturnType = NdType::makeVoid();
    Function.Params = {{"arg0", NdType::makeInt(8)},
                       {"arg1", NdType::makeInt(8)}};
    HighStmt Try;
    if (Cxx) {
      Function.ExceptionMetadata = makeFH3Metadata({Handler});
      Try.Kind = StmtKind::CxxTry;
      Try.EHIsReducible = true;
      Try.Addr = Entry + 0xA;
      Try.EHRange = {Entry + 0xA, Handler};
      HighStmt Work;
      Work.Kind = StmtKind::Call;
      Work.Addr = Entry + 0xA;
      Work.CallExpr = HighExpr::makeCall("may_raise", 0x140002000, {});
      Try.Body.push_back(std::move(Work));
      HighEHClause Catch;
      Catch.Kind = HighEHClauseKind::CxxCatch;
      Catch.HandlerVA = Handler;
      Try.EHClauses.push_back(std::move(Catch));
      Try.EHClauseBodies.push_back({storeThroughEstablisherSlot(5)});
    } else {
      ExceptionFunction EH = sehMetadata(Entry, Entry + 0x39);
      SEHScopeRecord Scope;
      Scope.GuardedRange = {Entry + 0xA, Handler};
      Scope.Kind = SEHScopeKind::CatchAll;
      Scope.HandlerVA = Handler;
      Scope.ContinuationVA = Handler;
      EH.SEH.emplace();
      EH.SEH->Scopes.push_back(Scope);
      Function.ExceptionMetadata = std::move(EH);
      Try = sehTryAround(Entry + 0xA, Handler, /*Filter=*/0, Handler,
                         "may_raise", {storeThroughEstablisherSlot(5)});
    }
    Try.Body.insert(Try.Body.begin(), storeThroughEstablisherSlot(2));
    Function.Body.push_back(std::move(Try));
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.Addr = Entry + 0x37;
    Function.Body.push_back(std::move(Return));

    const std::string Source = emitHighC({Function});
    const size_t Arm = Source.find(Cxx ? "catch (" : "} __except (");
    ASSERT_NE(Arm, std::string::npos) << Source;
    if (Cxx) {
      // The attached catch funclet's RDX still names the parent's slot.
      EXPECT_NE(Source.find("var_m8", Arm), std::string::npos) << Source;
      EXPECT_EQ(Source.find("arg1", Arm), std::string::npos) << Source;
      continue;
    }
    EXPECT_TRUE(std::regex_search(Source.substr(Arm),
                                  std::regex(R"(\barg1\)? \+ 0x30\b)")))
        << Source;
    EXPECT_EQ(Source.find("var_m8"), std::string::npos) << Source;

    // A setjmp provider stands in for the unwinder: it resumes the arm with
    // the same argument, which must reach the parent's slot.
    const std::string Harness = R"(
#include <setjmp.h>
#include <stdint.h>
#define EXCEPTION_EXECUTE_HANDLER 1
static jmp_buf destination;
static int raising;
#define __try if (setjmp(destination) == 0)
#define __except(filter) else
int may_raise(void) {
  if (raising)
    longjmp(destination, 1);
  return 0;
}
)";
    const std::string Main = R"(
int main(void) {
  for (raising = 0; raising != 2; ++raising) {
    int32_t counter = 0;
    uint64_t frame[8] = {0};
    frame[6] = (uint64_t)(uintptr_t)&counter;
    termination_handler(1, (int64_t)(uintptr_t)frame);
    if (counter != (raising ? 5 : 2))
      return 1;
  }
  return 0;
}
)";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Harness + Source + Main,
          {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
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
  auto *FuncletEntry = llvm::BasicBlock::Create(Context, "entry", Funclet);
  auto *FuncletCleanup = llvm::BasicBlock::Create(Context, "cleanup", Funclet);
  auto *FuncletCont = llvm::BasicBlock::Create(Context, "cont", Funclet);
  auto *FuncletHelper =
      llvm::Function::Create(VoidType, llvm::GlobalValue::ExternalLinkage,
                             "may_raise_funclet", Module);
  llvm::IRBuilder<>(FuncletEntry)
      .CreateInvoke(FuncletHelper, FuncletCont, FuncletCleanup);
  llvm::IRBuilder<>(FuncletCont).CreateRetVoid();
  llvm::IRBuilder<> FuncletBuilder(FuncletCleanup);
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

TEST(COFFExceptionIR, LLVMCRealignedFrameKeepsItsEstablisherCoordinates) {
  llvm::LLVMContext Context;
  llvm::Module Module("realigned-cxx-frame", Context);
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::GlobalValue::ExternalLinkage, "realigned", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();
  ExceptionFunction EH;
  EH.CodeRange = {0x401000, 0x401100};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Registration.emplace().RealignedFrame =
      RegistrationRealignedFrame{6, 0x40100f, 16, 0x270, -0x26c, -20};
  Function->setMetadata(windows_eh_md::FunctionAttachment,
                        windows_eh_md::getCanonicalFunctionMetadata(
                            Context, EH, Arch::X86, BinaryFormat::COFF));
  const auto Source = emitLLVMC(Module);
  EXPECT_NE(
      Source.find("registration.frame: base=esi, definition=0x40100F, "
                  "alignment=16, allocation=624, establisher_offset=-620, "
                  "saved_parent_offset=-20"),
      std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("metadata-invalid"), std::string::npos) << Source;
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

  ExceptionFunction CookieOnly;
  CookieOnly.CodeRange = {0x140006000, 0x140006080};
  CookieOnly.Encoding = ExceptionEncoding::X64UnwindV1;
  CookieOnly.Personality = ExceptionPersonality::GSHandlerCheck;
  CookieOnly.PersonalityName =
      getExceptionPersonalityName(CookieOnly.Personality);
  CookieOnly.GSCookie.emplace();
  CookieOnly.GSCookie->ParseStatus = ExceptionParseStatus::Complete;
  CookieOnly.GSCookie->CookieOffset = 48;
  AddFunction("cookie_only", CookieOnly);

  const std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("personality=__GSHandlerCheck\n"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("gs.cookie_offset=48, ehandler=0, uhandler=0"),
            std::string::npos)
      << Source;
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
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context),
                                   windows_eh_md::SchemaVersion + 1));
      });
  EXPECT_NE(BadVersion.find("unsupported metadata schema version " +
                            std::to_string(windows_eh_md::SchemaVersion + 1)),
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
  Cxx.Cxx->MaxState = 2;
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

  ExceptionFunction InvertedTry = Cxx;
  InvertedTry.Cxx->TryBlocks[0].TryLow = 1;
  InvertedTry.Cxx->TryBlocks[0].TryHigh = 0;
  InvertedTry.Cxx->TryBlocks[0].CatchHigh = 1;
  const std::string InvertedStates =
      EmitCorrupt(InvertedTry, [](llvm::LLVMContext &, auto &) {});
  EXPECT_NE(InvertedStates.find("metadata-invalid (cxx.try state range)"),
            std::string::npos)
      << InvertedStates;
  EXPECT_EQ(InvertedStates.find("cxx.try[0]:"), std::string::npos)
      << InvertedStates;

  ExceptionFunction MissingCatchState = Cxx;
  MissingCatchState.Cxx->TryBlocks[0].TryLow = 0;
  MissingCatchState.Cxx->TryBlocks[0].TryHigh = 0;
  MissingCatchState.Cxx->TryBlocks[0].CatchHigh = 0;
  const std::string MissingCatchGap =
      EmitCorrupt(MissingCatchState, [](llvm::LLVMContext &, auto &) {});
  EXPECT_NE(MissingCatchGap.find("metadata-invalid (cxx.try state range)"),
            std::string::npos)
      << MissingCatchGap;
  EXPECT_EQ(MissingCatchGap.find("cxx.try[0]:"), std::string::npos)
      << MissingCatchGap;

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
  llvm::FunctionType *Type =
      llvm::FunctionType::get(llvm::PointerType::getUnqual(Context), false);
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

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
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
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
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

  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  std::string Standalone;
  llvm::raw_string_ostream Stream(Standalone);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
  if (hasHostFixtureCompiler())
    checkStandaloneSEHSource(Standalone);
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
  llvm::FunctionType *FilterType =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              {llvm::PointerType::getUnqual(Context)}, false);
  llvm::Function *Filter = llvm::Function::Create(
      FilterType, llvm::GlobalValue::ExternalLinkage, "probe_filter", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_try_fn", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
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
  EXPECT_NE(
      Source.find("} __except (probe_filter(GetExceptionInformation())) {"),
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

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Finally =
      llvm::BasicBlock::Create(Context, "finally", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Function *Helper = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  EntryBuilder.CreateInvoke(Helper, Cont, Finally);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Cont);
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCleanupRet(Cleanup, Dispatch);
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

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
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
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateStore(llvm::ConstantInt::get(I32, 41), Sink);
  HandlerBuilder.CreateBr(Cont);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  const auto ExceptAt = Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {");
  const auto StoreAt = Source.find("ProbeSink = 0x29");
  EXPECT_NE(ExceptAt, std::string::npos) << Source;
  EXPECT_NE(StoreAt, std::string::npos) << Source;
  EXPECT_LT(ExceptAt, StoreAt) << Source;
  EXPECT_EQ(Source.find(
                "/* recovered handler labels remain in the protected body */"),
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

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
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
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
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
  // Cross-scope edges name the continuation explicitly. Its label and body
  // must both stay outside __except, and every call is still emitted once.
  const auto ContinuationAt = Source.find("\nL_cont:\n", ExceptClose);
  ASSERT_NE(ContinuationAt, std::string::npos) << Source;
  EXPECT_LT(ContinuationAt, AfterAt) << Source;
  EXPECT_EQ(Source.find("handler_step();", HandleAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("after_step();", AfterAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("may_raise();", RaiseAt + 1), std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCProjectsNonAdjacentInvokeNormalEdges) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-except-exit-order", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType =
      llvm::FunctionType::get(I32, /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::FunctionType *FnType =
      llvm::FunctionType::get(Void, {llvm::Type::getInt1Ty(Context)}, false);
  llvm::Function *Function = llvm::Function::Create(
      FnType, llvm::GlobalValue::ExternalLinkage, "seh_exit_order", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::BasicBlock *Inside =
      llvm::BasicBlock::Create(Context, "inside", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *ExitStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "exit_step", Module);
  llvm::Function *InsideStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "inside_step", Module);
  llvm::Function *AfterStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateCondBr(Function->getArg(0), Exit, Inside);
  llvm::IRBuilder<> ExitBuilder(Exit);
  ExitBuilder.CreateInvoke(ExitStep, Join, Dispatch);
  llvm::IRBuilder<> InsideBuilder(Inside);
  InsideBuilder.CreateInvoke(InsideStep, Exit, Dispatch);
  llvm::IRBuilder<> JoinBuilder(Join);
  JoinBuilder.CreateCall(AfterStep);
  JoinBuilder.CreateRetVoid();
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Join);
  EXPECT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto BodyAt = Source.find("seh_exit_order(");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto ExitAt = Source.find("exit_step();", BodyAt);
  const auto InsideAt = Source.find("inside_step();", BodyAt);
  const auto AfterAt = Source.find("after_step();", BodyAt);
  const auto ExceptAt = Source.find("} __except (", BodyAt);
  const auto ExitGotoAt = Source.find("goto L_join;", ExitAt);
  const auto InsideGotoAt = Source.find("goto L_exit;", InsideAt);
  ASSERT_NE(ExitAt, std::string::npos) << Source;
  ASSERT_NE(InsideAt, std::string::npos) << Source;
  ASSERT_NE(AfterAt, std::string::npos) << Source;
  ASSERT_NE(ExceptAt, std::string::npos) << Source;
  ASSERT_NE(ExitGotoAt, std::string::npos) << Source;
  ASSERT_NE(InsideGotoAt, std::string::npos) << Source;
  EXPECT_LT(ExitAt, InsideAt) << Source;
  EXPECT_LT(ExitGotoAt, InsideAt) << Source;
  EXPECT_LT(InsideGotoAt, ExceptAt) << Source;
  EXPECT_LT(ExceptAt, AfterAt) << Source;
  EXPECT_NE(Source.find("L_exit:", BodyAt), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_join:", BodyAt), std::string::npos) << Source;
}

TEST(COFFExceptionIR, LLVMCProjectsInternalInvokePhiBridgeCopy) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-internal-phi-bridge", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(I32, /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(Void, {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "seh_internal_phi_bridge", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::BasicBlock *Inside =
      llvm::BasicBlock::Create(Context, "inside", Function);
  llvm::BasicBlock *Bridge =
      llvm::BasicBlock::Create(Context, "bridge", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *ExitStep = llvm::Function::Create(
      llvm::FunctionType::get(Void, false), llvm::GlobalValue::ExternalLinkage,
      "exit_step", Module);
  llvm::Function *InsideStep = llvm::Function::Create(
      llvm::FunctionType::get(Void, false), llvm::GlobalValue::ExternalLinkage,
      "inside_step", Module);
  llvm::Function *Consume = llvm::Function::Create(
      llvm::FunctionType::get(Void, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "consume_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::AllocaInst *Slot = EntryBuilder.CreateAlloca(I32, nullptr, "slot");
  EntryBuilder.CreateCondBr(Function->getArg(0), Exit, Inside);
  llvm::IRBuilder<> ExitBuilder(Exit);
  llvm::PHINode *Incoming = ExitBuilder.CreatePHI(I32, 2, "incoming");
  Incoming->addIncoming(ExitBuilder.getInt32(7), Entry);
  Incoming->addIncoming(ExitBuilder.getInt32(13), Bridge);
  ExitBuilder.CreateStore(Incoming, Slot);
  ExitBuilder.CreateCall(Consume, {ExitBuilder.CreateLoad(I32, Slot)});
  ExitBuilder.CreateInvoke(ExitStep, Join, Dispatch);
  llvm::IRBuilder<> InsideBuilder(Inside);
  InsideBuilder.CreateInvoke(InsideStep, Bridge, Dispatch);
  llvm::IRBuilder<> BridgeBuilder(Bridge);
  BridgeBuilder.CreateBr(Exit);
  llvm::IRBuilder<> JoinBuilder(Join);
  JoinBuilder.CreateRetVoid();
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Join);
  ASSERT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto InsideAt = Source.find("inside_step();");
  ASSERT_NE(InsideAt, std::string::npos) << Source;
  const auto CopyAt = Source.find(" = 0xD;", InsideAt);
  const auto GotoAt = Source.find("goto L_exit;", InsideAt);
  ASSERT_NE(CopyAt, std::string::npos) << Source;
  ASSERT_NE(GotoAt, std::string::npos) << Source;
  EXPECT_LT(CopyAt, GotoAt) << Source;
  EXPECT_EQ(Source.find(" = 0xD;", GotoAt), std::string::npos) << Source;
  if (hasHostFixtureCompiler()) {
    const std::string Prelude =
        "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n"
        "void exit_step(void);\nvoid inside_step(void);\n"
        "void consume_step(uint32_t);\n";
    CompilerResult Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

TEST(COFFExceptionIR, LLVMCProjectsFallbackAdjacentInvokePhiBridgeCopy) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-fallback-phi-bridge", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(I32, /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(
          Void,
          {llvm::Type::getInt1Ty(Context), llvm::Type::getInt1Ty(Context)},
          false),
      llvm::GlobalValue::ExternalLinkage, "seh_fallback_phi_bridge", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::BasicBlock *Inside =
      llvm::BasicBlock::Create(Context, "inside", Function);
  llvm::BasicBlock *Bridge =
      llvm::BasicBlock::Create(Context, "bridge", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::BasicBlock *Gate = llvm::BasicBlock::Create(Context, "gate", Function);
  llvm::BasicBlock *EarlyReturn =
      llvm::BasicBlock::Create(Context, "early_return", Function);
  Inside->moveBefore(Exit);
  Gate->moveBefore(Inside);
  EarlyReturn->moveBefore(Join);
  llvm::Function *ExitStep = llvm::Function::Create(
      llvm::FunctionType::get(Void, false), llvm::GlobalValue::ExternalLinkage,
      "exit_step", Module);
  llvm::Function *InsideStep = llvm::Function::Create(
      llvm::FunctionType::get(Void, false), llvm::GlobalValue::ExternalLinkage,
      "inside_step", Module);
  llvm::Function *Consume = llvm::Function::Create(
      llvm::FunctionType::get(Void, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "consume_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::AllocaInst *Slot = EntryBuilder.CreateAlloca(I32, nullptr, "slot");
  EntryBuilder.CreateCondBr(Function->getArg(0), Gate, EarlyReturn);
  llvm::IRBuilder<> GateBuilder(Gate);
  GateBuilder.CreateCondBr(Function->getArg(1), Exit, Inside);
  llvm::IRBuilder<> EarlyReturnBuilder(EarlyReturn);
  EarlyReturnBuilder.CreateRetVoid();
  llvm::IRBuilder<> ExitBuilder(Exit);
  llvm::PHINode *Incoming = ExitBuilder.CreatePHI(I32, 2, "incoming");
  Incoming->addIncoming(ExitBuilder.getInt32(7), Gate);
  Incoming->addIncoming(ExitBuilder.getInt32(13), Bridge);
  ExitBuilder.CreateStore(Incoming, Slot);
  ExitBuilder.CreateCall(Consume, {ExitBuilder.CreateLoad(I32, Slot)});
  ExitBuilder.CreateInvoke(ExitStep, Join, Dispatch);
  llvm::IRBuilder<> InsideBuilder(Inside);
  InsideBuilder.CreateInvoke(InsideStep, Bridge, Dispatch);
  llvm::IRBuilder<> BridgeBuilder(Bridge);
  BridgeBuilder.CreateBr(Exit);
  llvm::IRBuilder<> JoinBuilder(Join);
  JoinBuilder.CreateRetVoid();
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto InsideAt = Source.find("inside_step();");
  ASSERT_NE(InsideAt, std::string::npos) << Source;
  const auto CopyAt = Source.find(" = 0xD;", InsideAt);
  const auto GotoAt = Source.find("goto L_exit;", InsideAt);
  ASSERT_NE(CopyAt, std::string::npos) << Source;
  ASSERT_NE(GotoAt, std::string::npos) << Source;
  EXPECT_LT(CopyAt, GotoAt) << Source;
  EXPECT_EQ(Source.find(" = 0xD;", GotoAt), std::string::npos) << Source;
  if (hasHostFixtureCompiler()) {
    const std::string Prelude =
        "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n"
        "void exit_step(void);\nvoid inside_step(void);\n"
        "void consume_step(uint32_t);\n";
    CompilerResult Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

TEST(COFFExceptionIR, LLVMCProjectsAddressPhiThroughInlinedExpression) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-address-phi", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(I32, /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I64, I64}, false),
      llvm::GlobalValue::ExternalLinkage, "seh_address_phi", Module);
  Function->setPersonalityFn(Personality);
  Function->getArg(0)->setName("normal_address");
  Function->getArg(1)->setName("handler_address");
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *Step = llvm::Function::Create(
      llvm::FunctionType::get(Void, false), llvm::GlobalValue::ExternalLinkage,
      "may_raise", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateInvoke(Step, Join, Dispatch);
  llvm::IRBuilder<> JoinBuilder(Join);
  llvm::PHINode *Address = JoinBuilder.CreatePHI(I64, 2, "address");
  Address->addIncoming(Function->getArg(0), Entry);
  Address->addIncoming(Function->getArg(1), Handler);
  llvm::Value *Offset =
      JoinBuilder.CreateAdd(Address, JoinBuilder.getInt64(32));
  llvm::Value *Pointer =
      JoinBuilder.CreateIntToPtr(Offset, llvm::PointerType::getUnqual(Context));
  JoinBuilder.CreateRet(JoinBuilder.CreateLoad(I32, Pointer));
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Join);
  ASSERT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto ExceptAt = Source.find("} __except (");
  const auto NormalCopy = Source.find(" = normal_address;");
  const auto HandlerCopy = Source.find(" = handler_address;");
  ASSERT_NE(ExceptAt, std::string::npos) << Source;
  ASSERT_NE(NormalCopy, std::string::npos) << Source;
  ASSERT_NE(HandlerCopy, std::string::npos) << Source;
  EXPECT_LT(NormalCopy, ExceptAt) << Source;
  EXPECT_GT(HandlerCopy, ExceptAt) << Source;
  if (hasHostFixtureCompiler()) {
    const std::string Prelude =
        "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n"
        "void may_raise(void);\n";
    CompilerResult Syntax =
        runCCompiler(Prelude + Source,
                     {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                      "-std=c11", "-fsyntax-only", "-Werror=uninitialized"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

TEST(COFFExceptionIR, LLVMCProjectsInvokeNormalEdgesWithHandlerPhiCopy) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-except-phi-exit-order", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *PersonalityType =
      llvm::FunctionType::get(I32, /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::FunctionType *FnType =
      llvm::FunctionType::get(Void, {llvm::Type::getInt1Ty(Context)}, false);
  llvm::Function *Function = llvm::Function::Create(
      FnType, llvm::GlobalValue::ExternalLinkage, "seh_phi_exit_order", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::BasicBlock *Inside =
      llvm::BasicBlock::Create(Context, "inside", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *ExitStep = llvm::Function::Create(
      llvm::FunctionType::get(I32, false), llvm::GlobalValue::ExternalLinkage,
      "exit_step", Module);
  llvm::Function *InsideStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "inside_step", Module);
  llvm::Function *AfterStep = llvm::Function::Create(
      llvm::FunctionType::get(Void, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateCondBr(Function->getArg(0), Exit, Inside);
  llvm::IRBuilder<> ExitBuilder(Exit);
  llvm::Value *Result = ExitBuilder.CreateInvoke(ExitStep, Join, Dispatch);
  llvm::IRBuilder<> InsideBuilder(Inside);
  InsideBuilder.CreateInvoke(InsideStep, Exit, Dispatch);
  llvm::IRBuilder<> JoinBuilder(Join);
  llvm::PHINode *Joined = JoinBuilder.CreatePHI(I32, 2, "joined");
  Joined->addIncoming(Result, Exit);
  Joined->addIncoming(JoinBuilder.getInt32(99), Handler);
  JoinBuilder.CreateCall(AfterStep, {Joined});
  JoinBuilder.CreateRetVoid();
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Join);
  EXPECT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto BodyAt = test::cDefinitionAt(Source, "seh_phi_exit_order");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto ExitAt = Source.find("exit_step()", BodyAt);
  const auto InsideAt = Source.find("inside_step();", BodyAt);
  const auto ExceptAt = Source.find("} __except (", BodyAt);
  const auto AfterAt = Source.find("after_step(", BodyAt);
  const auto ExitGotoAt = Source.find("goto L_join;", ExitAt);
  const auto InsideGotoAt = Source.find("goto L_exit;", InsideAt);
  const auto InvokeCopyAt = Source.find("joined", ExitAt);
  ASSERT_NE(ExitAt, std::string::npos) << Source;
  ASSERT_NE(InsideAt, std::string::npos) << Source;
  ASSERT_NE(ExceptAt, std::string::npos) << Source;
  ASSERT_NE(AfterAt, std::string::npos) << Source;
  ASSERT_NE(ExitGotoAt, std::string::npos) << Source;
  ASSERT_NE(InsideGotoAt, std::string::npos) << Source;
  ASSERT_NE(InvokeCopyAt, std::string::npos) << Source;
  const auto InvokeCopyNameEnd = Source.find(" = ", InvokeCopyAt);
  ASSERT_NE(InvokeCopyNameEnd, std::string::npos) << Source;
  ASSERT_LT(InvokeCopyNameEnd, ExitGotoAt) << Source;
  const std::string JoinedName =
      Source.substr(InvokeCopyAt, InvokeCopyNameEnd - InvokeCopyAt);
  const auto InvokeCopyValueEnd = Source.find(';', InvokeCopyNameEnd);
  ASSERT_NE(InvokeCopyValueEnd, std::string::npos) << Source;
  EXPECT_NE(Source.substr(InvokeCopyNameEnd + 3,
                          InvokeCopyValueEnd - InvokeCopyNameEnd - 3),
            "99")
      << Source;
  const auto HandlerCopyAt = findEdgeCopy(Source, JoinedName, "99", ExceptAt);
  ASSERT_NE(HandlerCopyAt, std::string::npos) << Source;
  EXPECT_LT(ExitAt, ExitGotoAt) << Source;
  EXPECT_LT(InvokeCopyAt, ExitGotoAt) << Source;
  EXPECT_LT(ExitGotoAt, InsideAt) << Source;
  EXPECT_LT(InsideGotoAt, ExceptAt) << Source;
  EXPECT_LT(HandlerCopyAt, AfterAt) << Source;
  EXPECT_LT(ExceptAt, AfterAt) << Source;
  EXPECT_NE(Source.find("L_join:", BodyAt), std::string::npos) << Source;
  EXPECT_NE(Source.find("after_step(" + JoinedName + ");", BodyAt),
            std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCProjectsBothConstantPhiCopiesAfterExcept) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-two-constant-phi-edges", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(I32, /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(Void, {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "seh_phi_constant_edges", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::BasicBlock *Inside =
      llvm::BasicBlock::Create(Context, "inside", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *ExitStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "exit_step", Module);
  llvm::Function *InsideStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "inside_step", Module);
  llvm::Function *AfterStep = llvm::Function::Create(
      llvm::FunctionType::get(Void, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateCondBr(Function->getArg(0), Exit, Inside);
  llvm::IRBuilder<> ExitBuilder(Exit);
  ExitBuilder.CreateInvoke(ExitStep, Join, Dispatch);
  llvm::IRBuilder<> InsideBuilder(Inside);
  InsideBuilder.CreateInvoke(InsideStep, Exit, Dispatch);
  llvm::IRBuilder<> JoinBuilder(Join);
  llvm::PHINode *Joined = JoinBuilder.CreatePHI(I32, 2, "joined");
  Joined->addIncoming(JoinBuilder.getInt32(7), Exit);
  Joined->addIncoming(JoinBuilder.getInt32(99), Handler);
  JoinBuilder.CreateCall(AfterStep, {Joined});
  JoinBuilder.CreateRetVoid();
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Join);
  EXPECT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto BodyAt = test::cDefinitionAt(Source, "seh_phi_constant_edges");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto ExitAt = Source.find("exit_step();", BodyAt);
  const auto ExceptAt = Source.find("} __except (", BodyAt);
  const auto AfterAt = Source.find("after_step(", BodyAt);
  ASSERT_NE(ExitAt, std::string::npos) << Source;
  ASSERT_NE(ExceptAt, std::string::npos) << Source;
  ASSERT_NE(AfterAt, std::string::npos) << Source;
  const auto NormalCopyAt = Source.find("joined", ExitAt);
  ASSERT_NE(NormalCopyAt, std::string::npos) << Source;
  const auto NameEnd = Source.find(" = ", NormalCopyAt);
  ASSERT_NE(NameEnd, std::string::npos) << Source;
  ASSERT_LT(NameEnd, ExceptAt) << Source;
  const std::string JoinedName =
      Source.substr(NormalCopyAt, NameEnd - NormalCopyAt);
  const auto NormalValueAt = findEdgeCopy(Source, JoinedName, "7", ExitAt);
  ASSERT_NE(NormalValueAt, std::string::npos) << Source;
  EXPECT_LT(NormalValueAt, ExceptAt) << Source;
  const auto HandlerCopyAt = findEdgeCopy(Source, JoinedName, "99", ExceptAt);
  ASSERT_NE(HandlerCopyAt, std::string::npos) << Source;
  EXPECT_LT(HandlerCopyAt, AfterAt) << Source;
  EXPECT_NE(Source.find("after_step(" + JoinedName + ");", AfterAt),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("goto L_join;", ExitAt), std::string::npos) << Source;
  if (hasHostFixtureCompiler()) {
    // The unit declares its callees itself.
    const std::string Prelude =
        "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n";
    CompilerResult Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

TEST(COFFExceptionIR, LLVMCFallbackProjectsInvokeNormalEdgesInsideTry) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-fallback-exit-order", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(I32, /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(Void, {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "seh_fallback_exit", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::BasicBlock *Inside =
      llvm::BasicBlock::Create(Context, "inside", Function);
  // Layout places a later normal block before the join.  The first block in
  // AfterWrap is therefore not the invoke's normal destination.
  llvm::BasicBlock *Done = llvm::BasicBlock::Create(Context, "done", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *ExitStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "exit_step", Module);
  llvm::Function *InsideStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "inside_step", Module);
  llvm::Function *AfterStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  llvm::Function *Dtor = llvm::Function::Create(
      llvm::FunctionType::get(Void, {llvm::PointerType::getUnqual(Context)},
                              false),
      llvm::GlobalValue::ExternalLinkage, "object_dtor", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateCondBr(Function->getArg(0), Exit, Inside);
  llvm::IRBuilder<> ExitBuilder(Exit);
  ExitBuilder.CreateInvoke(ExitStep, Join, Dispatch);
  llvm::IRBuilder<> InsideBuilder(Inside);
  InsideBuilder.CreateInvoke(InsideStep, Exit, Dispatch);
  llvm::IRBuilder<> DoneBuilder(Done);
  DoneBuilder.CreateCall(Dtor, {llvm::ConstantPointerNull::get(
                                   llvm::PointerType::getUnqual(Context))});
  DoneBuilder.CreateRetVoid();
  llvm::IRBuilder<> JoinBuilder(Join);
  JoinBuilder.CreateCall(AfterStep);
  JoinBuilder.CreateBr(Done);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Done);
  EXPECT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto BodyAt = test::cDefinitionAt(Source, "seh_fallback_exit");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto ExitAt = Source.find("exit_step();", BodyAt);
  const auto InsideAt = Source.find("inside_step();", BodyAt);
  const auto AfterAt = Source.find("after_step();", BodyAt);
  const auto ExceptAt = Source.find("} __except (", BodyAt);
  const auto ExitGotoAt = Source.find("goto L_join;", ExitAt);
  const auto InsideGotoAt = Source.find("goto L_exit;", InsideAt);
  ASSERT_NE(ExitAt, std::string::npos) << Source;
  ASSERT_NE(InsideAt, std::string::npos) << Source;
  ASSERT_NE(AfterAt, std::string::npos) << Source;
  ASSERT_NE(ExceptAt, std::string::npos) << Source;
  ASSERT_NE(ExitGotoAt, std::string::npos) << Source;
  ASSERT_NE(InsideGotoAt, std::string::npos) << Source;
  EXPECT_LT(ExitAt, ExitGotoAt) << Source;
  EXPECT_LT(ExitGotoAt, InsideAt) << Source;
  EXPECT_LT(InsideGotoAt, AfterAt) << Source;
  EXPECT_LT(ExceptAt, AfterAt) << Source;
  EXPECT_NE(Source.find("L_exit:", BodyAt), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_join:", BodyAt), std::string::npos) << Source;
  const auto HandlerDtorAt = Source.find("object_dtor(", ExceptAt);
  EXPECT_NE(HandlerDtorAt, std::string::npos) << Source;
  EXPECT_EQ(Source.find("goto L_done;", ExceptAt), std::string::npos) << Source;
  if (hasHostFixtureCompiler()) {
    // The unit declares its callees itself.
    const std::string Prelude =
        "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n";
    CompilerResult Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

TEST(COFFExceptionIR, LLVMCFallbackKeepsInvokeTargetReturnLabel) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-fallback-shared-return-label", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(I32, /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(Void, {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "seh_ret_label", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::BasicBlock *Other =
      llvm::BasicBlock::Create(Context, "other", Function);
  llvm::BasicBlock *Inside =
      llvm::BasicBlock::Create(Context, "inside", Function);
  llvm::BasicBlock *Done = llvm::BasicBlock::Create(Context, "done", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *ExitStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "exit_step", Module);
  llvm::Function *InsideStep = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "inside_step", Module);
  llvm::Function *Dtor = llvm::Function::Create(
      llvm::FunctionType::get(Void, {llvm::PointerType::getUnqual(Context)},
                              false),
      llvm::GlobalValue::ExternalLinkage, "object_dtor", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateCondBr(Function->getArg(0), Inside, Other);
  llvm::IRBuilder<> ExitBuilder(Exit);
  ExitBuilder.CreateInvoke(ExitStep, Done, Dispatch);
  llvm::IRBuilder<> OtherBuilder(Other);
  OtherBuilder.CreateBr(Done);
  llvm::IRBuilder<> InsideBuilder(Inside);
  InsideBuilder.CreateInvoke(InsideStep, Exit, Dispatch);
  llvm::IRBuilder<> DoneBuilder(Done);
  DoneBuilder.CreateCall(Dtor, {llvm::ConstantPointerNull::get(
                                   llvm::PointerType::getUnqual(Context))});
  DoneBuilder.CreateRetVoid();
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Done);
  EXPECT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  const std::string Source = emitLLVMC(Module);
  const auto BodyAt = test::cDefinitionAt(Source, "seh_ret_label");
  ASSERT_NE(BodyAt, std::string::npos) << Source;
  const auto ExitAt = Source.find("exit_step();", BodyAt);
  const auto ExceptAt = Source.find("} __except (", BodyAt);
  ASSERT_NE(ExitAt, std::string::npos) << Source;
  ASSERT_NE(ExceptAt, std::string::npos) << Source;
  const auto ExitGotoAt = Source.find("goto L_done;", ExitAt);
  ASSERT_NE(ExitGotoAt, std::string::npos) << Source;
  ASSERT_LT(ExitGotoAt, ExceptAt) << Source;
  const auto LabelAt = Source.find("\nL_done:\n", BodyAt);
  ASSERT_NE(LabelAt, std::string::npos) << Source;
  EXPECT_EQ(Source.find("\nL_done:\n", LabelAt + 1), std::string::npos)
      << Source;
  EXPECT_LT(ExitGotoAt, LabelAt) << Source;
  EXPECT_NE(Source.find("object_dtor(", LabelAt), std::string::npos) << Source;
  EXPECT_NE(Source.find("object_dtor(", ExceptAt), std::string::npos) << Source;
  EXPECT_EQ(Source.find("goto L_done;", ExceptAt), std::string::npos) << Source;
  if (hasHostFixtureCompiler()) {
    // The unit declares its callees itself.
    const std::string Prelude =
        "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n";
    CompilerResult Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

TEST(COFFExceptionIR, LLVMCFallbackMovesSharedContinuationOutOfTry) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-fallback-illegal-entry", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(I32, /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(Void, false), llvm::GlobalValue::ExternalLinkage,
      "seh_illegal_entry", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Done = llvm::BasicBlock::Create(Context, "done", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::BasicBlock *Dispatch =
      llvm::BasicBlock::Create(Context, "dispatch", Function);
  llvm::BasicBlock *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
  llvm::BasicBlock *Handler =
      llvm::BasicBlock::Create(Context, "handler", Function);
  llvm::Function *ExitStep = llvm::Function::Create(
      llvm::FunctionType::get(I32, false), llvm::GlobalValue::ExternalLinkage,
      "exit_step", Module);
  llvm::Function *AfterStep = llvm::Function::Create(
      llvm::FunctionType::get(Void, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Value *Result = EntryBuilder.CreateInvoke(ExitStep, Join, Dispatch);
  llvm::IRBuilder<> DoneBuilder(Done);
  DoneBuilder.CreateRetVoid();
  llvm::IRBuilder<> JoinBuilder(Join);
  llvm::PHINode *Joined = JoinBuilder.CreatePHI(I32, 2, "joined");
  Joined->addIncoming(Result, Entry);
  JoinBuilder.CreateCall(AfterStep, {Joined});
  JoinBuilder.CreateBr(Done);
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  llvm::CatchPadInst *CatchPad = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  auto *Code = PadBuilder.CreateIntrinsic(llvm::Intrinsic::eh_exceptioncode, {},
                                          {CatchPad}, nullptr, "code");
  Joined->addIncoming(Code, Handler);
  PadBuilder.CreateCatchRet(CatchPad, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateBr(Join);
  EXPECT_FALSE(llvm::verifyFunction(*Function, &llvm::errs()));

  // LLVM orders Done before Join. Both continuations must stay outside the
  // wrapper, reached through the same PHI edges as in the original CFG.
  rewrite_source::setOriginalVA(*Function, 0x140001000);
  const std::string Unmapped = emitLLVMC(Module);
  CSourceMap Map;
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.SourceMap = &Map;
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
  EXPECT_EQ(Source, Unmapped);
  ASSERT_FALSE(Map.Variables.empty());
  for (const auto &[Key, Variable] : Map.Variables) {
    if (!Variable.Bytes) {
      EXPECT_TRUE(Variable.Ambiguous);
      EXPECT_FALSE(Variable.Type);
    }
  }
  const auto Except = Source.find("} __except (");
  const auto JoinAt = Source.find("\nL_join:\n");
  ASSERT_NE(Except, std::string::npos) << Source;
  ASSERT_NE(JoinAt, std::string::npos) << Source;
  EXPECT_LT(Except, JoinAt) << Source;
  EXPECT_NE(Source.find(" = GetExceptionCode();", Except), std::string::npos)
      << Source;
  const std::string Prelude =
      "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n"
      "unsigned long GetExceptionCode(void);\n";
  if (hasHostFixtureCompiler()) {
    const auto Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    ASSERT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
  // A setjmp provider exercises the generated normal/handler CFG on the
  // host. The target compile above separately validates legal SEH scopes.
  const std::string Harness = R"(
#include <setjmp.h>
static jmp_buf destination;
static unsigned raising, observed, calls;
unsigned long GetExceptionCode(void) { return 99; }
#define __try if (setjmp(destination) == 0)
#define __except(filter) else
uint32_t exit_step(void) { if (raising) longjmp(destination, 1); return 37; }
void after_step(uint32_t value) { ++calls; observed = value; }
)";
  const std::string Main = R"(
int main(void) {
  for (raising = 0; raising != 2; ++raising) {
    calls = observed = 0;
    seh_illegal_entry();
    if (calls != 1 || observed != (raising ? 99 : 37)) return 1;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(
        Prelude + Harness + Source + Main,
        {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(COFFExceptionIR, LLVMCTryBeginSplitAfterLoopKeepsRangeAndHandlerEdges) {
  for (bool MultipleRanges : {false, true}) {
    SCOPED_TRACE(MultipleRanges);
    for (bool LateBegin : {false, true}) {
      SCOPED_TRACE(LateBegin);
      llvm::LLVMContext Context;
      llvm::Module Module("seh-range-loop", Context);
      Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
      llvm::IRBuilder<> B(Context);
      auto *Void = B.getVoidTy();
      auto *I32 = B.getInt32Ty();
      auto Declare = [&](llvm::StringRef Name, llvm::Type *Result,
                         llvm::ArrayRef<llvm::Type *> Args = {}) {
        return llvm::Function::Create(
            llvm::FunctionType::get(Result, Args, false),
            llvm::GlobalValue::ExternalLinkage, Name, Module);
      };
      auto *Personality = Declare("__C_specific_handler", I32);
      auto *F = Declare("range_loop", Void);
      F->setPersonalityFn(Personality);
      auto Block = [&](llvm::StringRef Name) {
        return llvm::BasicBlock::Create(Context, Name, F);
      };
      auto *Entry = Block("entry");
      auto *Header = Block("header");
      auto *Begin = Block("begin");
      auto *Body = Block("body");
      auto *End = Block("end");
      auto *Between = MultipleRanges ? Block("between") : nullptr;
      auto *BeginAgain = MultipleRanges ? Block("begin_again") : nullptr;
      auto *BodyAgain = MultipleRanges ? Block("body_again") : nullptr;
      auto *EndAgain = MultipleRanges ? Block("end_again") : nullptr;
      auto *Tail = Block("tail");
      auto *Done = Block("done");
      auto *Dispatch = Block("dispatch");
      auto *Pad = Block("pad");
      auto *Handler = Block("handler");
      auto *Before = Declare("before_step", I32);
      auto *Step = Declare("range_step", I32, {I32});
      auto *BetweenStep = Declare("between_step", Void);
      auto *After = Declare("after_step", Void, {I32});
      auto *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::seh_try_begin);
      auto *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::seh_try_end);
      B.SetInsertPoint(Entry);
      B.CreateBr(Header);
      B.SetInsertPoint(Header);
      B.CreateCondBr(B.CreateICmpNE(B.CreateCall(Before), B.getInt32(0)), Begin,
                     Done);
      B.SetInsertPoint(Begin);
      B.CreateInvoke(TryBegin, Body, Dispatch);
      B.SetInsertPoint(Body);
      llvm::Value *Value = B.CreateInvoke(Step, End, Dispatch, {B.getInt32(0)});
      B.SetInsertPoint(End);
      B.CreateInvoke(TryEnd, MultipleRanges ? Between : Tail, Dispatch);
      if (MultipleRanges) {
        B.SetInsertPoint(Between);
        B.CreateCall(BetweenStep);
        B.CreateBr(BeginAgain);
        B.SetInsertPoint(BeginAgain);
        B.CreateInvoke(TryBegin, BodyAgain, Dispatch);
        B.SetInsertPoint(BodyAgain);
        Value = B.CreateInvoke(Step, EndAgain, Dispatch, {B.getInt32(1)});
        B.SetInsertPoint(EndAgain);
        B.CreateInvoke(TryEnd, Tail, Dispatch);
      }
      B.SetInsertPoint(Tail);
      auto *Joined = B.CreatePHI(I32, 2, "joined");
      Joined->addIncoming(Value, MultipleRanges ? EndAgain : End);
      Joined->addIncoming(B.getInt32(99), Handler);
      B.CreateCall(After, {Joined});
      B.CreateBr(Header);
      B.SetInsertPoint(Done);
      B.CreateRetVoid();
      B.SetInsertPoint(Dispatch);
      auto *Switch = B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context),
                                         nullptr, 1);
      Switch->addHandler(Pad);
      B.SetInsertPoint(Pad);
      auto *CatchPad = B.CreateCatchPad(
          Switch, {llvm::ConstantPointerNull::get(B.getPtrTy())});
      B.CreateCatchRet(CatchPad, Handler);
      B.SetInsertPoint(Handler);
      B.CreateBr(Tail);
      if (LateBegin)
        Begin->moveBefore(Dispatch->getIterator());
      ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
      const std::string Source = emitLLVMC(Module);
      if (MultipleRanges) {
        // The unprotected operation must remain outside every emitted try.
        int Depth = 0, TryDepth = -1;
        for (llvm::StringRef Line :
             llvm::split(llvm::StringRef(Source), '\n')) {
          if (Line.contains("__try"))
            TryDepth = Depth;
          if (Line.contains("between_step();"))
            EXPECT_EQ(TryDepth, -1) << Source;
          for (char C : Line) {
            if (C == '{')
              ++Depth;
            if (C == '}') {
              --Depth;
              if (TryDepth == Depth)
                TryDepth = -1;
            }
          }
        }
      }
      const std::string Prelude =
          "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n";
      if (hasHostFixtureCompiler()) {
        const auto Syntax = runCCompiler(
            Prelude + Source, {"--target=x86_64-pc-windows-msvc",
                               "-fms-extensions", "-std=c11", "-fsyntax-only"});
        ASSERT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
      }
      const std::string Harness = R"(
#include <setjmp.h>
static jmp_buf destination;
static unsigned raising, iterations, calls, between_calls, bad;
#define __try if (setjmp(destination) == 0)
#define __except(filter) else
uint32_t before_step(void) { return iterations < 3; }
uint32_t range_step(uint32_t phase) {
  ++calls;
  if (raising & (1u << (iterations * 2 + phase))) longjmp(destination, 1);
  return 37 + phase * 4;
}
void between_step(void) { ++between_calls; }
void after_step(uint32_t value) {
  const unsigned mask = (raising >> (iterations * 2)) & (MULTIPLE ? 3u : 1u);
  bad |= value != (mask ? 99 : (MULTIPLE ? 41 : 37));
  ++iterations;
}
)";
      const std::string Main = R"(
int main(void) {
  for (raising = 0; raising != 64; ++raising) {
    iterations = calls = between_calls = bad = 0;
    range_loop();
    unsigned expected_between = 0;
    for (unsigned i = 0; i != 3; ++i)
      expected_between += MULTIPLE && !(raising & (1u << (i * 2)));
    if (iterations != 3 || calls != 3 + expected_between || bad ||
        between_calls != expected_between) return 1;
  }
  return 0;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Prelude + "#define MULTIPLE " + (MultipleRanges ? "1\n" : "0\n") +
                Harness + Source + Main,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      if (MultipleRanges) {
        auto *Branch = llvm::cast<llvm::BranchInst>(Header->getTerminator());
        Branch->setSuccessor(1, BodyAgain);
        ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
        EXPECT_THROW((void)emitLLVMC(Module), std::runtime_error);
        Branch->setSuccessor(1, Done);
        // Nested begin without closing the active range cannot be flattened
        // into independent try statements merely because the handler matches.
        llvm::cast<llvm::InvokeInst>(End->getTerminator())
            ->setCalledFunction(TryBegin);
        ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
        EXPECT_THROW((void)emitLLVMC(Module), std::runtime_error);
      }
    }
  }
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

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
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
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn, {},
                            {llvm::OperandBundleDef("funclet", Cleanup)});
  FinallyBuilder.CreateCleanupRet(Cleanup, nullptr);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateRetVoid();

  std::string Source = emitLLVMC(Module);
  const auto FinallyAt = Source.find("} __finally {");
  const auto CallAt = Source.find("run_finally();");
  EXPECT_NE(FinallyAt, std::string::npos) << Source;
  EXPECT_NE(CallAt, std::string::npos) << Source;
  EXPECT_LT(FinallyAt, CallAt) << Source;
  // The cleanup pad runs only while an exception unwinds the try; a C
  // __finally runs on its normal exit as well. This try holds the return,
  // which AbnormalTermination() would count, so the return sets a flag.
  const auto GuardAt = Source.find("if (!neverd_seh_normal) {", FinallyAt);
  EXPECT_LT(GuardAt, CallAt) << Source;
  const auto ReturnAt = Source.find("return;");
  ASSERT_NE(ReturnAt, std::string::npos) << Source;
  EXPECT_LT(ReturnAt, FinallyAt) << Source;
  EXPECT_EQ(Source.rfind("neverd_seh_normal = 1;", ReturnAt),
            Source.rfind(";", ReturnAt - 1) + 1 -
                std::string("neverd_seh_normal = 1;").size())
      << Source;
  EXPECT_EQ(Source.find(
                "/* recovered handler labels remain in the protected body */"),
            std::string::npos)
      << Source;
}

// A cleanup pad runs only while an exception unwinds its try, but C runs a
// __finally for a return out of the try as well, and AbnormalTermination()
// is then true too. Such a range takes the per-block normal-exit flag, which
// the return sets before it leaves.
TEST(COFFExceptionIR, LLVMCFinallyReturnInsideTheTrySkipsTheCleanup) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-finally-exit", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I1 = llvm::Type::getInt1Ty(Context);
  llvm::FunctionType *VoidType = llvm::FunctionType::get(Void, false);
  llvm::FunctionType *BoolType = llvm::FunctionType::get(I1, false);
  llvm::FunctionType *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_finally_exit", Module);
  Function->setPersonalityFn(Personality);
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Body = llvm::BasicBlock::Create(Context, "body", Function);
  llvm::BasicBlock *Test = llvm::BasicBlock::Create(Context, "test", Function);
  llvm::BasicBlock *More = llvm::BasicBlock::Create(Context, "more", Function);
  llvm::BasicBlock *End = llvm::BasicBlock::Create(Context, "end", Function);
  llvm::BasicBlock *Early =
      llvm::BasicBlock::Create(Context, "early", Function);
  llvm::BasicBlock *Finally =
      llvm::BasicBlock::Create(Context, "finally", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::Function *Raise = llvm::Function::Create(
      BoolType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  llvm::Function *Again = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "raise_again", Module);
  llvm::Function *CleanupFn = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "run_finally", Module);
  llvm::Function *Leave = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "leave_early", Module);
  auto *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_begin);
  auto *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_end);
  llvm::IRBuilder<>(Entry).CreateInvoke(TryBegin, Body, Finally);
  llvm::Value *Flag = nullptr;
  {
    llvm::IRBuilder<> B(Body);
    Flag = B.CreateInvoke(Raise, Test, Finally);
  }
  llvm::IRBuilder<>(Test).CreateCondBr(Flag, Early, More);
  llvm::IRBuilder<>(More).CreateInvoke(Again, End, Finally);
  llvm::IRBuilder<>(End).CreateInvoke(TryEnd, Cont, Finally);
  {
    llvm::IRBuilder<> B(Early);
    B.CreateCall(Leave);
    B.CreateRetVoid();
  }
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn, {},
                            {llvm::OperandBundleDef("funclet", Cleanup)});
  FinallyBuilder.CreateCleanupRet(Cleanup, nullptr);
  llvm::IRBuilder<>(Cont).CreateRetVoid();
  std::string Source = emitLLVMC(Module);
  EXPECT_EQ(Source.find("AbnormalTermination"), std::string::npos) << Source;
  const auto LeaveAt = Source.find("leave_early();");
  ASSERT_NE(LeaveAt, std::string::npos) << Source;
  const auto NormalAt = Source.find("neverd_seh_normal = 1;", LeaveAt);
  const auto ReturnAt = Source.find("return;", LeaveAt);
  ASSERT_NE(NormalAt, std::string::npos) << Source;
  ASSERT_NE(ReturnAt, std::string::npos) << Source;
  EXPECT_LT(NormalAt, ReturnAt) << Source;
  // Every __finally runs the cleanup only after an exception.
  size_t Finallies = 0;
  for (size_t At = Source.find("} __finally {"); At != std::string::npos;
       At = Source.find("} __finally {", At + 1), ++Finallies)
    EXPECT_EQ(Source.find_first_not_of(" \n", At + 13),
              Source.find("if (!neverd_seh_normal) {", At))
        << Source;
  EXPECT_GT(Finallies, 0u) << Source;
  if (hasHostFixtureCompiler()) {
    const std::string Prelude =
        "#include <stdint.h>\n"
        "uint8_t may_raise(void);\nvoid raise_again(void);\n"
        "void run_finally(void);\nvoid leave_early(void);\n";
    CompilerResult Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

// One finally range with a protected block laid out past its end marker:
// lexically that block would print outside the try, reached by a jump out of
// it and back in. The per-block normal-exit flag renders the range exactly.
TEST(COFFExceptionIR, LLVMCFinallyRangeLaidOutPastItsEndUsesTheExitFlag) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-finally-layout", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::FunctionType *BoolType =
      llvm::FunctionType::get(llvm::Type::getInt1Ty(Context), false);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function =
      llvm::Function::Create(VoidType, llvm::GlobalValue::ExternalLinkage,
                             "seh_finally_layout", Module);
  Function->setPersonalityFn(Personality);
  auto Block = [&](const char *Name) {
    return llvm::BasicBlock::Create(Context, Name, Function);
  };
  llvm::BasicBlock *Entry = Block("entry");
  llvm::BasicBlock *Body = Block("body");
  llvm::BasicBlock *Test = Block("test");
  llvm::BasicBlock *End = Block("end");
  llvm::BasicBlock *Cont = Block("cont");
  llvm::BasicBlock *Far = Block("far");
  llvm::BasicBlock *Finally = Block("finally");
  auto Declare = [&](llvm::FunctionType *Type, const char *Name) {
    return llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                  Name, Module);
  };
  llvm::Function *Raise = Declare(BoolType, "may_raise");
  llvm::Function *Again = Declare(VoidType, "raise_again");
  llvm::Function *CleanupFn = Declare(VoidType, "run_finally");
  auto *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_begin);
  auto *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_end);
  llvm::IRBuilder<>(Entry).CreateInvoke(TryBegin, Body, Finally);
  llvm::Value *Flag =
      llvm::IRBuilder<>(Body).CreateInvoke(Raise, Test, Finally);
  llvm::IRBuilder<>(Test).CreateCondBr(Flag, Far, End);
  llvm::IRBuilder<>(Far).CreateInvoke(Again, End, Finally);
  llvm::IRBuilder<>(End).CreateInvoke(TryEnd, Cont, Finally);
  llvm::IRBuilder<>(Cont).CreateRetVoid();
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn, {},
                            {llvm::OperandBundleDef("funclet", Cleanup)});
  FinallyBuilder.CreateCleanupRet(Cleanup, nullptr);

  std::string Source = emitLLVMC(Module);
  EXPECT_EQ(Source.find("AbnormalTermination"), std::string::npos) << Source;
  // raise_again() stays protected, and every __finally runs the cleanup
  // only after an exception.
  const auto AgainAt = Source.find("raise_again();");
  ASSERT_NE(AgainAt, std::string::npos) << Source;
  const auto TryAt = Source.rfind("__try {", AgainAt);
  ASSERT_NE(TryAt, std::string::npos) << Source;
  EXPECT_EQ(Source.find('}', TryAt), Source.find("} __finally {", TryAt))
      << Source;
  size_t Finallies = 0;
  for (size_t At = Source.find("} __finally {"); At != std::string::npos;
       At = Source.find("} __finally {", At + 1), ++Finallies)
    EXPECT_EQ(Source.find_first_not_of(" \n", At + 13),
              Source.find("if (!neverd_seh_normal) {", At))
        << Source;
  EXPECT_GT(Finallies, 0u) << Source;
  if (hasHostFixtureCompiler()) {
    const std::string Prelude =
        "#include <stdint.h>\n"
        "uint8_t may_raise(void);\nvoid raise_again(void);\n"
        "void run_finally(void);\n";
    CompilerResult Syntax = runCCompiler(
        Prelude + Source, {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                           "-std=c11", "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

// A jump out of the try that bypasses its end marker reaches a continuation
// the end marker also reaches outside the range. C would run the __finally
// on that jump, and no block scope fits the shared continuation, so LLVMC
// refuses the function instead of running the cleanup on a normal path.
TEST(COFFExceptionIR, LLVMCFinallyJumpPastItsEndMarkerIsRefused) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-finally-jump", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::FunctionType *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::FunctionType *BoolType =
      llvm::FunctionType::get(llvm::Type::getInt1Ty(Context), false);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", Module);
  llvm::Function *Function = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_finally_jump", Module);
  Function->setPersonalityFn(Personality);
  auto Block = [&](const char *Name) {
    return llvm::BasicBlock::Create(Context, Name, Function);
  };
  llvm::BasicBlock *Entry = Block("entry");
  llvm::BasicBlock *Body = Block("body");
  llvm::BasicBlock *Test = Block("test");
  llvm::BasicBlock *Early = Block("early");
  llvm::BasicBlock *More = Block("more");
  llvm::BasicBlock *End = Block("end");
  llvm::BasicBlock *Cont = Block("cont");
  llvm::BasicBlock *Finally = Block("finally");
  auto Declare = [&](llvm::FunctionType *Type, const char *Name) {
    return llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                  Name, Module);
  };
  llvm::Function *Raise = Declare(BoolType, "may_raise");
  llvm::Function *Again = Declare(VoidType, "raise_again");
  llvm::Function *Leave = Declare(VoidType, "leave_early");
  llvm::Function *CleanupFn = Declare(VoidType, "run_finally");
  auto *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_begin);
  auto *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_end);
  llvm::IRBuilder<>(Entry).CreateInvoke(TryBegin, Body, Finally);
  llvm::Value *Flag =
      llvm::IRBuilder<>(Body).CreateInvoke(Raise, Test, Finally);
  llvm::IRBuilder<>(Test).CreateCondBr(Flag, Early, More);
  llvm::IRBuilder<>(Early).CreateInvoke(Leave, Cont, Finally);
  llvm::IRBuilder<>(More).CreateInvoke(Again, End, Finally);
  llvm::IRBuilder<>(End).CreateInvoke(TryEnd, Cont, Finally);
  llvm::IRBuilder<>(Cont).CreateRetVoid();
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn, {},
                            {llvm::OperandBundleDef("funclet", Cleanup)});
  FinallyBuilder.CreateCleanupRet(Cleanup, nullptr);

  EXPECT_THROW((void)emitLLVMC(Module), std::runtime_error);
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
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_finally_cont", Module);
  Function->setPersonalityFn(Personality);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Body = llvm::BasicBlock::Create(Context, "body", Function);
  llvm::BasicBlock *End = llvm::BasicBlock::Create(Context, "end", Function);
  llvm::BasicBlock *Finally =
      llvm::BasicBlock::Create(Context, "finally", Function);
  llvm::BasicBlock *Cont = llvm::BasicBlock::Create(Context, "cont", Function);
  llvm::Function *Raise = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_raise", Module);
  llvm::Function *CleanupFn = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "run_finally", Module);
  llvm::Function *After = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "after_step", Module);
  // Range markers carry the normal continuation; cleanupret's destination
  // is an unwind edge and cannot point to an ordinary continuation block.
  auto *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_begin);
  auto *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_end);
  llvm::IRBuilder<>(Entry).CreateInvoke(TryBegin, Body, Finally);
  llvm::IRBuilder<>(Body).CreateInvoke(Raise, End, Finally);
  llvm::IRBuilder<>(End).CreateInvoke(TryEnd, Cont, Finally);
  llvm::IRBuilder<> FinallyBuilder(Finally);
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn, {},
                            {llvm::OperandBundleDef("funclet", Cleanup)});
  FinallyBuilder.CreateCleanupRet(Cleanup, nullptr);
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
  EXPECT_LT(Source.find("if (AbnormalTermination()) {", FinallyAt), RunAt)
      << Source;
  EXPECT_EQ(
      Source.substr(FinallyOpen, FinallyClose - FinallyOpen).find("return"),
      std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("goto ", BodyAt), std::string::npos) << Source;
  EXPECT_EQ(Source.find("run_finally();", RunAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("after_step();", AfterAt + 1), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("may_raise();", RaiseAt + 1), std::string::npos)
      << Source;
}

TEST(COFFExceptionIR, LLVMCRejectsCleanupReturnIntoOrdinaryContinuation) {
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
      VoidType, llvm::GlobalValue::ExternalLinkage, "seh_finally_cont", Module);
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
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  FinallyBuilder.CreateCall(CleanupFn);
  FinallyBuilder.CreateCleanupRet(Cleanup, Cont);
  llvm::IRBuilder<> ContBuilder(Cont);
  ContBuilder.CreateCall(After);
  ContBuilder.CreateRetVoid();

  // cleanupret transfers an exception to an EH pad or unwinds to the caller;
  // an ordinary continuation is not a legal exceptional successor.
  EXPECT_TRUE(llvm::verifyFunction(*Function, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  EXPECT_FALSE(LLVMCEmitter().emit(Module, OS, Options));
  EXPECT_TRUE(Source.empty());
}

TEST(COFFExceptionIR, LLVMCNestedFinallyInsideExceptContainsBothBodies) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-seh-nested-bodies", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
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
      llvm::FunctionType::get(Void, {I64, I64}, false),
      llvm::GlobalValue::ExternalLinkage, "nested_seh_bodies", Module);
  Function->getArg(0)->setName("normal_address");
  Function->getArg(1)->setName("handler_address");
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
  llvm::Function *RecordState = llvm::Function::Create(
      llvm::FunctionType::get(Void, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "record_state", Module);
  llvm::IRBuilder<> AfterOuterBuilder(AfterOuter);
  llvm::PHINode *Address = AfterOuterBuilder.CreatePHI(I64, 2, "address");
  Address->addIncoming(Function->getArg(0), EndOuter);
  Address->addIncoming(Function->getArg(1), Handler);
  llvm::PHINode *State = AfterOuterBuilder.CreatePHI(I32, 2, "state");
  State->addIncoming(AfterOuterBuilder.getInt32(7), EndOuter);
  State->addIncoming(AfterOuterBuilder.getInt32(99), Handler);
  llvm::Value *Offset =
      AfterOuterBuilder.CreateAdd(Address, AfterOuterBuilder.getInt64(36));
  llvm::Value *Pointer = AfterOuterBuilder.CreateIntToPtr(
      Offset, llvm::PointerType::getUnqual(Context));
  AfterOuterBuilder.CreateStore(AfterOuterBuilder.CreateLoad(I32, Pointer),
                                Sink);
  AfterOuterBuilder.CreateCall(RecordState, {State});
  AfterOuterBuilder.CreateRetVoid();

  llvm::IRBuilder<> DispatchBuilder(CatchDispatch);
  llvm::CatchSwitchInst *Switch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(CatchPad);
  llvm::IRBuilder<> PadBuilder(CatchPad);
  llvm::CatchPadInst *Catch = PadBuilder.CreateCatchPad(
      Switch,
      {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context))});
  PadBuilder.CreateCatchRet(Catch, Handler);
  llvm::IRBuilder<> HandlerBuilder(Handler);
  HandlerBuilder.CreateStore(llvm::ConstantInt::get(I32, 41), Sink);
  HandlerBuilder.CreateBr(AfterOuter);

  llvm::IRBuilder<> FinallyBuilder(FinallyDispatch);
  llvm::CleanupPadInst *Cleanup =
      FinallyBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
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
  const auto StoreAt = Source.find("ProbeSink = 0x29", BodyAt);
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
  const auto NormalCopy = Source.find(" = normal_address;", BodyAt);
  const auto HandlerCopy = Source.find(" = handler_address;", BodyAt);
  ASSERT_NE(NormalCopy, std::string::npos) << Source;
  ASSERT_NE(HandlerCopy, std::string::npos) << Source;
  EXPECT_LT(NormalCopy, ExceptKw) << Source;
  EXPECT_GT(HandlerCopy, ExceptKw) << Source;
  const auto NormalStateCopy = Source.find(" = 7;", OuterAt);
  ASSERT_NE(NormalStateCopy, std::string::npos) << Source;
  EXPECT_LT(NormalStateCopy, ExceptKw) << Source;
  const auto StateCall = Source.find("record_state(", HandlerCopy);
  ASSERT_NE(StateCall, std::string::npos) << Source;
  const auto StateNameBegin = StateCall + std::string("record_state(").size();
  const auto StateNameEnd = Source.find(')', StateNameBegin);
  ASSERT_NE(StateNameEnd, std::string::npos) << Source;
  const std::string StateName =
      Source.substr(StateNameBegin, StateNameEnd - StateNameBegin);
  const auto NormalState = findEdgeCopy(Source, StateName, "7", OuterAt);
  const auto HandlerState = findEdgeCopy(Source, StateName, "99", ExceptKw);
  ASSERT_NE(NormalState, std::string::npos) << Source;
  ASSERT_NE(HandlerState, std::string::npos) << Source;
  EXPECT_LT(NormalState, ExceptKw) << Source;
  EXPECT_LT(HandlerState, StateCall) << Source;
  if (hasHostFixtureCompiler()) {
    const std::string Prelude =
        "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n"
        "int _abnormal_termination(void);\n"
        "#define AbnormalTermination _abnormal_termination\n"
        "extern uint32_t ProbeSink;\n"
        "void arm_region(void);\nvoid may_raise(void);\n"
        "void outer_step(void);\nvoid run_finally(void);\n"
        "void record_state(uint32_t);\n";
    CompilerResult Syntax =
        runCCompiler(Prelude + Source,
                     {"--target=x86_64-pc-windows-msvc", "-fms-extensions",
                      "-std=c11", "-fsyntax-only", "-Werror=uninitialized"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << "\n" << Source;
  }
}

TEST(COFFExceptionIR, LLVMCSharedHandlerKeepsNestedUnwindOnlyCleanup) {
  llvm::LLVMContext Context;
  llvm::Module Module("shared-seh-nested-cleanup", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::IRBuilder<> B(Context);
  auto Declare = [&](llvm::StringRef Name, llvm::Type *Return,
                     llvm::ArrayRef<llvm::Type *> Args = {}) {
    return llvm::Function::Create(llvm::FunctionType::get(Return, Args, false),
                                  llvm::GlobalValue::ExternalLinkage, Name,
                                  Module);
  };
  auto *F = Declare("nested_cleanup", B.getInt32Ty(), {B.getInt32Ty()});
  F->getArg(0)->setName("take_inner");
  F->setPersonalityFn(Declare("__C_specific_handler", B.getInt32Ty()));
  auto Block = [&](llvm::StringRef Name) {
    return llvm::BasicBlock::Create(Context, Name, F);
  };
  auto *Entry = Block("entry");
  auto *Choice = Block("choice");
  auto *BeginInner = Block("begin_inner");
  auto *Body = Block("body");
  auto *EndInner = Block("end_inner");
  auto *AfterInner = Block("after_inner");
  auto *EndOuter = Block("end_outer");
  auto *EarlyEnd = Block("early_end");
  auto *Tail = Block("tail");
  auto *Dispatch = Block("dispatch");
  auto *PadBlock = Block("pad");
  auto *Handler = Block("handler");
  auto *CleanupBlock = Block("cleanup");
  auto *CleanupEnd = Block("cleanup_end");
  auto *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_begin);
  auto *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_end);
  auto *Step = Declare("protected_step", B.getVoidTy());
  auto *CleanupCall = Declare("cleanup_step", B.getVoidTy(), {B.getInt32Ty()});
  B.SetInsertPoint(Entry);
  B.CreateInvoke(TryBegin, Choice, Dispatch);
  B.SetInsertPoint(Choice);
  B.CreateCondBr(B.CreateICmpNE(F->getArg(0), B.getInt32(0)), BeginInner,
                 EarlyEnd);
  B.SetInsertPoint(BeginInner);
  B.CreateInvoke(TryBegin, Body, CleanupBlock);
  B.SetInsertPoint(Body);
  B.CreateInvoke(Step, EndInner, CleanupBlock);
  B.SetInsertPoint(EndInner);
  B.CreateInvoke(TryEnd, AfterInner, CleanupBlock);
  B.SetInsertPoint(AfterInner);
  B.CreateInvoke(CleanupCall, EndOuter, Dispatch, {B.getInt32(0)});
  B.SetInsertPoint(EndOuter);
  B.CreateInvoke(TryEnd, Tail, Dispatch);
  B.SetInsertPoint(EarlyEnd);
  B.CreateInvoke(TryEnd, Tail, Dispatch);
  B.SetInsertPoint(Tail);
  auto *Value = B.CreatePHI(B.getInt32Ty(), 3, "result");
  Value->addIncoming(B.getInt32(7), EndOuter);
  Value->addIncoming(B.getInt32(11), EarlyEnd);
  Value->addIncoming(B.getInt32(99), Handler);
  B.CreateRet(Value);
  B.SetInsertPoint(Dispatch);
  auto *Switch =
      B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(PadBlock);
  B.SetInsertPoint(PadBlock);
  auto *Pad =
      B.CreateCatchPad(Switch, {llvm::ConstantPointerNull::get(B.getPtrTy())});
  B.CreateCatchRet(Pad, Handler);
  B.SetInsertPoint(Handler);
  B.CreateBr(Tail);
  B.SetInsertPoint(CleanupBlock);
  auto *Cleanup = B.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  B.CreateInvoke(CleanupCall, CleanupEnd, Dispatch, {B.getInt32(1)});
  B.SetInsertPoint(CleanupEnd);
  B.CreateCleanupRet(Cleanup, Dispatch);
  ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
  const std::string Source = emitLLVMC(Module);
  EXPECT_NE(Source.find("if (!neverd_seh_normal)"), std::string::npos)
      << Source;
  const std::string Prelude =
      "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n";
  if (hasHostFixtureCompiler())
    for (llvm::StringRef Level : {"-O0", "-O2"}) {
      const auto Compiled = runCCompiler(
          Prelude + Source,
          {"--target=x86_64-pc-windows-msvc", "-fms-extensions", "-std=c11",
           "-Xclang", "-fasync-exceptions", "-c", Level});
      EXPECT_EQ(Compiled.ExitCode, 0) << Compiled.Error << "\n" << Source;
    }
  // A host check of normal paths: cleanup's normal counterpart runs exactly
  // once and the unwind-only body never substitutes for it. Native Windows
  // object generation above checks the actual nested SEH and every goto.
  const std::string Harness = R"(
#define __try if (1)
#define __except(filter) else
#define __finally if (1)
static unsigned normal_calls, unwind_calls, steps;
void protected_step(void) { ++steps; }
void cleanup_step(uint32_t abnormal) {
  if (abnormal) ++unwind_calls; else ++normal_calls;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(Prelude + Harness + Source + R"(
int main(void) {
  if (nested_cleanup(0) != 11 || normal_calls || unwind_calls || steps) return 1;
  if (nested_cleanup(1) != 7 || normal_calls != 1 || unwind_calls || steps != 1)
    return 2;
  return 0;
}
)",
                                              {Level});
}

TEST(COFFExceptionIR, LLVMCMixedHandlersKeepUnwindOnlyCleanup) {
  llvm::LLVMContext Context;
  llvm::Module Module("mixed-seh-scopes", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::IRBuilder<> B(Context);
  auto Declare = [&](llvm::StringRef Name, llvm::Type *Return,
                     llvm::ArrayRef<llvm::Type *> Args = {}) {
    return llvm::Function::Create(llvm::FunctionType::get(Return, Args, false),
                                  llvm::GlobalValue::ExternalLinkage, Name,
                                  Module);
  };
  auto *F = Declare("mixed_scopes", B.getInt32Ty(), {B.getInt32Ty()});
  F->setPersonalityFn(Declare("__C_specific_handler", B.getInt32Ty()));
  auto Block = [&](llvm::StringRef Name) {
    return llvm::BasicBlock::Create(Context, Name, F);
  };
  auto *Entry = Block("entry");
  auto *BeginA = Block("begin_a");
  auto *BodyA = Block("body_a");
  auto *EndA = Block("end_a");
  auto *TailA = Block("tail_a");
  auto *BeginCleanup = Block("begin_cleanup");
  auto *BeginB = Block("begin_b");
  auto *BodyB = Block("body_b");
  auto *EndB = Block("end_b");
  auto *EndCleanup = Block("end_cleanup");
  auto *TailB = Block("tail_b");
  auto *DispatchA = Block("dispatch_a");
  auto *PadA = Block("pad_a");
  auto *HandlerA = Block("handler_a");
  auto *DispatchB = Block("dispatch_b");
  auto *PadB = Block("pad_b");
  auto *HandlerB = Block("handler_b");
  auto *CleanupBlock = Block("cleanup");
  auto *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_begin);
  auto *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_try_end);
  auto *Step = Declare("mixed_step", B.getVoidTy(), {B.getInt32Ty()});
  auto *CleanupCall = Declare("mixed_cleanup", B.getVoidTy(), {B.getInt32Ty()});
  B.SetInsertPoint(Entry);
  B.CreateCondBr(B.CreateICmpEQ(F->getArg(0), B.getInt32(0)), BeginA,
                 BeginCleanup);
  B.SetInsertPoint(BeginA);
  B.CreateInvoke(TryBegin, BodyA, DispatchA);
  B.SetInsertPoint(BodyA);
  B.CreateInvoke(Step, EndA, DispatchA, {B.getInt32(10)});
  B.SetInsertPoint(EndA);
  B.CreateInvoke(TryEnd, TailA, DispatchA);
  B.SetInsertPoint(TailA);
  B.CreateRet(B.getInt32(17));
  B.SetInsertPoint(BeginCleanup);
  B.CreateInvoke(TryBegin, BeginB, CleanupBlock);
  B.SetInsertPoint(BeginB);
  B.CreateInvoke(TryBegin, BodyB, DispatchB);
  B.SetInsertPoint(BodyB);
  B.CreateInvoke(Step, EndB, DispatchB, {B.getInt32(20)});
  B.SetInsertPoint(EndB);
  B.CreateInvoke(TryEnd, EndCleanup, DispatchB);
  B.SetInsertPoint(EndCleanup);
  B.CreateInvoke(TryEnd, TailB, CleanupBlock);
  B.SetInsertPoint(TailB);
  B.CreateCall(CleanupCall, {B.getInt32(0)});
  B.CreateRet(B.getInt32(23));
  for (bool Nested : {false, true}) {
    auto *Dispatch = Nested ? DispatchB : DispatchA;
    auto *PadBlock = Nested ? PadB : PadA;
    auto *Handler = Nested ? HandlerB : HandlerA;
    B.SetInsertPoint(Dispatch);
    auto *Switch = B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context),
                                       Nested ? CleanupBlock : nullptr, 1);
    Switch->addHandler(PadBlock);
    B.SetInsertPoint(PadBlock);
    auto *Pad = B.CreateCatchPad(
        Switch, {llvm::ConstantPointerNull::get(B.getPtrTy())});
    B.CreateCatchRet(Pad, Handler);
    B.SetInsertPoint(Handler);
    B.CreateRet(B.getInt32(Nested ? 102 : 101));
  }
  B.SetInsertPoint(CleanupBlock);
  auto *Cleanup = B.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
  B.CreateCall(CleanupCall, {B.getInt32(1)},
               {llvm::OperandBundleDef("funclet", Cleanup)});
  B.CreateCleanupRet(Cleanup, nullptr);
  ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
  const std::string Source = emitLLVMC(Module);
  EXPECT_EQ(Source.find("return; /* __finally */"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("if (!neverd_seh_normal)"), std::string::npos)
      << Source;
  const std::string Prelude =
      "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n";
  if (hasHostFixtureCompiler())
    for (llvm::StringRef Level : {"-O0", "-O2"}) {
      const auto Compiled = runCCompiler(
          Prelude + Source,
          {"--target=x86_64-pc-windows-msvc", "-fms-extensions", "-std=c11",
           "-Xclang", "-fasync-exceptions", "-c", Level});
      EXPECT_EQ(Compiled.ExitCode, 0) << Compiled.Error << "\n" << Source;
    }
  // Execute the normal paths independently. Native Windows object emission
  // above checks the real nested SEH, including the cleanup on filter refusal.
  const std::string Harness = R"(
#define __try if (1)
#define __except(filter) else
#define __finally if (1)
static unsigned normal_calls, unwind_calls, steps;
void mixed_step(uint32_t n) { steps += n; }
void mixed_cleanup(uint32_t abnormal) {
  if (abnormal) ++unwind_calls; else ++normal_calls;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(Prelude + Harness + Source + R"(
int main(void) {
  if (mixed_scopes(0) != 17 || steps != 10 || normal_calls || unwind_calls) return 1;
  if (mixed_scopes(1) != 23 || steps != 30 || normal_calls != 1 || unwind_calls) return 2;
  return 0;
}
)",
                                              {Level});
}

TEST(COFFExceptionIR, LLVMCFinallyAndFilterShareTheParentLocalAddress) {
  REQUIRE_HOST_FIXTURE_COMPILER();
  for (bool FilterUsesFrame : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("seh-parent-local-area", Context);
    Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
    Module.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> Builder(Context);
    auto *Void = Builder.getVoidTy();
    auto *Pointer = Builder.getPtrTy();
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Void, false),
        llvm::GlobalValue::ExternalLinkage, "parent_frame", Module);
    auto Personality = Module.getOrInsertFunction(
        "__C_specific_handler",
        llvm::FunctionType::get(Builder.getInt32Ty(), true));
    Function->setPersonalityFn(
        llvm::cast<llvm::Constant>(Personality.getCallee()));
    auto Raise = Module.getOrInsertFunction("may_raise", Void);
    auto Finally = Module.getOrInsertFunction("use_parent", Void, Pointer);
    auto Filter = Module.getOrInsertFunction(
        "filter_parent",
        FilterUsesFrame
            ? llvm::FunctionType::get(Builder.getInt32Ty(), {Pointer, Pointer},
                                      false)
            : llvm::FunctionType::get(Builder.getInt32Ty(), {Pointer}, false));
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Normal = llvm::BasicBlock::Create(Context, "normal", Function);
    auto *Cleanup = llvm::BasicBlock::Create(Context, "cleanup", Function);
    auto *Dispatch = llvm::BasicBlock::Create(Context, "dispatch", Function);
    auto *Pad = llvm::BasicBlock::Create(Context, "pad", Function);
    auto *Handler = llvm::BasicBlock::Create(Context, "handler", Function);
    Builder.SetInsertPoint(Entry);
    Builder.CreateInvoke(Raise, Normal, Cleanup);
    Builder.SetInsertPoint(Normal);
    Builder.CreateRetVoid();
    Builder.SetInsertPoint(Cleanup);
    auto *CleanupToken =
        Builder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
    auto *Address = Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::localaddress));
    Builder.CreateCall(Finally, {Address},
                       {llvm::OperandBundleDef("funclet", CleanupToken)});
    Builder.CreateCleanupRet(CleanupToken, Dispatch);
    Builder.SetInsertPoint(Dispatch);
    auto *Switch = Builder.CreateCatchSwitch(
        llvm::ConstantTokenNone::get(Context), nullptr, 1);
    Switch->addHandler(Pad);
    Builder.SetInsertPoint(Pad);
    auto *Catch = Builder.CreateCatchPad(Switch, {Filter.getCallee()});
    Builder.CreateCatchRet(Catch, Handler);
    Builder.SetInsertPoint(Handler);
    Builder.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));

    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::COFF;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
    const auto Try = Source.find("__try {");
    const auto FinallyAt = Source.find("} __finally {");
    const auto Intrinsic = Source.find("__asm__(\"llvm.localaddress\")");
    ASSERT_NE(Try, std::string::npos) << Source;
    ASSERT_NE(FinallyAt, std::string::npos) << Source;
    ASSERT_NE(Intrinsic, std::string::npos) << Source;
    EXPECT_LT(Intrinsic, Try) << Source;
    EXPECT_EQ(Source.find("__asm__(\"llvm.localaddress\")", Intrinsic + 1),
              std::string::npos)
        << Source;
    EXPECT_NE(Source.find(" = seh_parent_frame", FinallyAt), std::string::npos)
        << Source;
    checkStandaloneSEHSource(Source, /*CompileObject=*/true);
  }
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

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
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
  llvm::CleanupPadInst *Cleanup =
      CleanupBuilder.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
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
  Function->setMetadata(
      windows_eh_md::FunctionAttachment,
      llvm::MDNode::get(Context, llvm::MDString::get(Context, "unwind")));
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Function *Helper =
      llvm::Function::Create(VoidType, llvm::GlobalValue::ExternalLinkage,
                             "unwind_only_helper", Module);
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
  Function->setMetadata(
      windows_eh_md::FunctionAttachment,
      llvm::MDNode::get(Context, llvm::MDString::get(Context, "fh3")));
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
  // LLVM aliases require a definition, even when the personality is only
  // inspected as metadata. Keep the fixture's body explicitly non-executable.
  llvm::IRBuilder<> HandlerBuilder(
      llvm::BasicBlock::Create(Context, "entry", Handler));
  HandlerBuilder.CreateCall(
      llvm::Intrinsic::getOrInsertDeclaration(&Module, llvm::Intrinsic::trap));
  HandlerBuilder.CreateUnreachable();
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
  // LLVM rejects NUL in value names. Keep a legal control byte here; the
  // HighC hostile-name fixture also covers NUL before LLVM admission.
  const char HostileArgumentBytes[] = "for\x01) { pwned(); }\nint injected";
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
  // `-` separates words, so both names read `collision_name`.
  AddAttachedEmptyFunction("collision-name");
  AddAttachedEmptyFunction("collision_name");

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
  EXPECT_NE(Source.find("collision_name()"), std::string::npos) << Source;
  EXPECT_NE(Source.find("collision_name_2()"), std::string::npos) << Source;

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
TEST(COFFExceptionIR, ObserversWatchTheConversionAGuardedFunctionKeeps) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "observed_eh";
  Func.ReturnType = NdType::makeVoid();
  for (unsigned Id : {0u, 1u}) {
    MedBlock Block;
    Block.Id = static_cast<int>(Id);
    Block.StartAddr = Func.Entry + 0x20 * Id;
    Block.EndAddr = Block.StartAddr + 0x10;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Block.StartAddr + 8;
    Return.OriginSeq = 0;
    Block.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Block));
  }
  ExceptionFunction Metadata;
  Metadata.CodeRange = {Func.Entry, Func.Entry + 0x30};
  Metadata.ParseStatus = ExceptionParseStatus::Complete;
  Metadata.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Func.Entry, Func.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Func.Entry + 0x20;
  SEH.Scopes.push_back(Scope);
  Metadata.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(Metadata);

  // What the observers record and the IR kept, with a way to forget the
  // observations and without one.
  const auto Convert = [&](bool Forgets, size_t &Resets) {
    MedToHighConverter Converter;
    std::vector<std::pair<va_t, StmtKind>> Seen;
    Converter.setStatementObserver([&](const MedOp &Op, const HighStmt &S) {
      Seen.push_back({Op.Addr, S.Kind});
    });
    if (Forgets)
      Converter.setObservationReset([&] {
        Seen.clear();
        ++Resets;
      });
    const HighFunc High = Converter.convert(Func, Arch::X64);
    EXPECT_EQ(High.UnstructuredExceptionRegions, 0u);
    std::string Dump;
    llvm::raw_string_ostream OS(Dump);
    Pipeline::dumpHighIR({High}, OS);
    return std::pair{Dump, Seen};
  };
  size_t Resets = 0, Unused = 0;
  const auto Kept = Convert(true, Resets);
  ASSERT_FALSE(Kept.second.empty());
  // The structured layout is the one kept, so nothing is forgotten, and the
  // observers saw exactly what they see when the conversion is repeated.
  EXPECT_EQ(Resets, 0u);
  EXPECT_EQ(Kept, Convert(false, Unused));
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
  // The object at the try's own state outlives what the try catches: the
  // runtime unwinds only down to that state before the handler runs.  Its
  // cleanup is a scope around the try, for exceptions that leave the try.
  ASSERT_EQ(HighCxx.StructuredExceptionRegions, 2u);
  ASSERT_EQ(HighCxx.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(HighCxx.Body.empty());
  const HighStmt &Lifetime = HighCxx.Body.front();
  EXPECT_EQ(Lifetime.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Lifetime.EHClauses.size(), 1u);
  const HighEHClause &Cleanup = Lifetime.EHClauses.front();
  EXPECT_EQ(Cleanup.Kind, HighEHClauseKind::CxxCleanup);
  EXPECT_EQ(Cleanup.UnwindActionKind,
            CxxUnwindAction::ActionKind::DestructorWithObjectPointer);
  EXPECT_EQ(Cleanup.UnwindObjectOffset, -0x20);
  ASSERT_FALSE(Lifetime.Body.empty());
  const HighStmt &Inner = Lifetime.Body.front();
  EXPECT_EQ(Inner.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Inner.EHClauses.size(), 1u);
  EXPECT_EQ(Inner.EHClauses.front().Kind, HighEHClauseKind::CxxCatch);
}

TEST(COFFExceptionIR, CleanupOnlyCxxStatesBecomeCxxTryNotSEH) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ActionVA = FunctionVA + 0x20;
  MedFunc Func = makeWindowsHandlerFixture("cleanup_only_cxx", 0x14000f000);
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
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} catch (...) {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("throw;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__wind"), std::string::npos) << Source;
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
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} catch (...) {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("throw;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__wind"), std::string::npos) << Source;
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
  Br.addInput(Arg0);
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
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} catch (...) {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("use_temp("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__wind"), std::string::npos) << Source;
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

  std::map<va_t, std::string> Names{
      {0x140002000, "before"},     {0x140002010, "outer_a"},
      {0x140002020, "inner_work"}, {0x140002030, "outer_b"},
      {0x14000F000, "dtor_outer"}, {0x14000F100, "dtor_inner"}};
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
  // Each object's lifetime is a try whose catch-all runs its cleanup and
  // rethrows; the inner object's sits inside the outer one's.
  const size_t OuterTry = Source.find("try {");
  const size_t InnerTry = Source.find("try {", OuterTry + 1);
  const size_t InnerCatch = Source.find("} catch (...) {", InnerTry);
  const size_t OuterCatch = Source.find("} catch (...) {", InnerCatch + 1);
  ASSERT_NE(InnerCatch, std::string::npos) << Source;
  ASSERT_NE(OuterCatch, std::string::npos) << Source;
  EXPECT_LT(Source.find("dtor_inner(", InnerCatch), OuterCatch) << Source;
  EXPECT_GT(Source.find("dtor_outer(", OuterCatch), OuterCatch) << Source;
  EXPECT_EQ(Source.find("__wind"), std::string::npos) << Source;
}

constexpr va_t kObjectInTryVA = 0x140001000;

/// try { Probe Obj; work(); } catch (...) { }: the object's destructor runs
/// while the exception leaves its lifetime, before the handler, and only once
/// it is constructed.  FH4 names the destructor itself as the action and the
/// object by its establisher-frame offset.  Without \p PassesObject the calls
/// take no argument, so no code computes the object's address.
HighFunc objectInTry(bool PassesObject) {
  constexpr va_t FunctionVA = kObjectInTryVA;
  constexpr va_t CtorVA = 0x140002000;
  constexpr va_t WorkVA = 0x140002010;
  constexpr va_t DtorVA = 0x14000F000;
  constexpr int32_t FrameSize = 0x48;
  constexpr int32_t ObjectOffset = 0x20;
  const auto &TRI = getTargetRegInfo(Arch::X64);
  MedFunc Med;
  Med.Entry = FunctionVA;
  Med.Name = "object_in_try";
  Med.ReturnType = NdType::makeVoid();
  Med.FrameSize = FrameSize;

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Id = 1;
  SP.RegOff = TRI.StackPointer;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  MedVar Object;
  Object.Kind = MedVar::Temp;
  Object.Id = 2;
  Object.Size = 8;
  Object.TheArch = Arch::X64;
  auto Call = [](va_t Addr, va_t Target, std::vector<MedVar> Args) {
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Addr = Addr;
    Op.addInput(MedVar::makeConst(Target, 8));
    for (const MedVar &Arg : Args)
      Op.addInput(Arg);
    Op.ExactArguments = true;
    return Op;
  };
  std::vector<MedVar> This;
  if (PassesObject)
    This.push_back(Object);
  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = FunctionVA;
  Body.EndAddr = FunctionVA + 0x40;
  MedOp Address;
  Address.Opcode = NdOp::INT_SUB;
  Address.Addr = FunctionVA;
  Address.Output = Object;
  Address.addInput(SP);
  Address.addInput(MedVar::makeConst(FrameSize - ObjectOffset, 8,
                                     ConstantAddressProvenance::Scalar));
  Body.Ops.push_back(Address);
  Body.Ops.push_back(Call(FunctionVA + 0x8, CtorVA, This));
  Body.Ops.push_back(Call(FunctionVA + 0x10, WorkVA, {}));
  Body.Ops.push_back(Call(FunctionVA + 0x20, DtorVA, This));
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = FunctionVA + 0x30;
  Body.Ops.push_back(std::move(Ret));
  Med.Blocks.push_back(std::move(Body));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler4;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 3;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {-1, 0}};
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::None;
  Cxx.UnwindMap[1].ActionVA = DtorVA;
  Cxx.UnwindMap[1].Kind = CxxUnwindAction::ActionKind::DestructorWithObject;
  Cxx.UnwindMap[1].ObjectOffset = ObjectOffset;
  Cxx.UnwindMap[2].Kind = CxxUnwindAction::ActionKind::None;
  Cxx.IPMap = {{FunctionVA, -1},
               {FunctionVA + 0x4, 0},
               {FunctionVA + 0xC, 1},
               {FunctionVA + 0x1C, 0},
               {FunctionVA + 0x24, -1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 1;
  Try.CatchHigh = 2;
  CxxCatchHandler Catch;
  Catch.HandlerVA = FunctionVA + 0x100;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  EXPECT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Med.ExceptionMetadata = std::move(EH);

  std::map<va_t, std::string> Names{
      {CtorVA, "probe_ctor"}, {WorkVA, "work"}, {DtorVA, "probe_dtor"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  return Converter.convert(Med, Arch::X64);
}

TEST(COFFExceptionIR, ObjectOfATryBodyIsACleanupScopeInsideTheTry) {
  constexpr va_t FunctionVA = kObjectInTryVA;
  const HighFunc High = objectInTry(/*PassesObject=*/true);
  const HighStmt *Handler = nullptr;
  const HighStmt *Scope = nullptr;
  walkStmts(High.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::CxxTry || S.EHClauses.size() != 1)
      return;
    if (S.EHClauses.front().Kind == HighEHClauseKind::CxxCatch)
      Handler = &S;
    if (S.EHClauses.front().Kind == HighEHClauseKind::CxxCleanup)
      Scope = &S;
  });
  ASSERT_NE(Handler, nullptr);
  ASSERT_NE(Scope, nullptr);
  bool ScopeInsideTry = false;
  walkStmts(Handler->Body,
            [&](const HighStmt &S) { ScopeInsideTry |= &S == Scope; });
  EXPECT_TRUE(ScopeInsideTry);
  EXPECT_EQ(Scope->EHRange.Begin, FunctionVA + 0xC);
  EXPECT_EQ(Scope->EHRange.End, FunctionVA + 0x1C);
  // The C++ view writes the action as the destructor called on the object;
  // the IR keeps the clause, as the C view shows the native action.
  ASSERT_EQ(Scope->EHClauseBodies.size(), 1u);
  EXPECT_TRUE(Scope->EHClauseBodies.front().empty());

  const std::string Source = emitHighC({High});
  const size_t OuterTry = Source.find("try {");
  const size_t InnerTry = Source.find("try {", OuterTry + 1);
  const size_t Cleanup = Source.find("} catch (...) {", InnerTry);
  const size_t Rethrow = Source.find("throw;", Cleanup);
  const size_t HandlerAt = Source.find("} catch (...) {", Rethrow);
  ASSERT_NE(InnerTry, std::string::npos) << Source;
  ASSERT_NE(Cleanup, std::string::npos) << Source;
  ASSERT_NE(Rethrow, std::string::npos) << Source;
  ASSERT_NE(HandlerAt, std::string::npos) << Source;
  EXPECT_LT(Source.find("work("), Cleanup) << Source;
  const size_t Dtor = Source.find("probe_dtor(", Cleanup);
  ASSERT_LT(Dtor, Rethrow) << Source;
  // The destructor's `this` is the object the constructor built.
  EXPECT_NE(Source.substr(Dtor, Rethrow - Dtor).find("&stack_storage.var_m28"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("probe_ctor((probe*)&stack_storage.var_m28)"),
            std::string::npos)
      << Source;
  EXPECT_LT(Source.find("probe_ctor("), InnerTry) << Source;
  EXPECT_EQ(Source.find("__unwind"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, AnObjectBuiltThroughAFrameAliasGetsItsDestructor) {
  // v0 = SP - 0x48; probe_ctor(v0 + 0x20): the object FH4 names at
  // establisher-frame offset 0x20, even though no code spells SP - 40 itself.
  // A local the function sets twice to one address still spells it.
  constexpr va_t DtorVA = 0x14000F000;
  const auto &TRI = getTargetRegInfo(Arch::X64);
  HighFunc Func;
  Func.Entry = 0x140001000;
  Func.FrameSize = 0x48;
  ExceptionFunction &EH = Func.ExceptionMetadata.emplace();
  EH.Personality = ExceptionPersonality::CxxFrameHandler4;
  EH.Cxx.emplace();
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Id = 1;
  SP.RegOff = TRI.StackPointer;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  MedVar Base;
  Base.Kind = MedVar::Temp;
  Base.Id = 2;
  Base.Size = 8;
  Base.TheArch = Arch::X64;
  Base.RenameTag = 0;
  const TypeRef Word = NdType::makeInt(8, /*S=*/false);
  auto At = [&](const MedVar &From, NdOp Op, uint64_t Bytes) {
    ExprPtr E = HighExpr::makeBinop(Op, HighExpr::makeVar(From, Word),
                                    HighExpr::makeConst(Bytes, 8));
    E->Type = Word;
    return E;
  };
  for (int Copy = 0; Copy < 2; ++Copy) {
    HighStmt Set;
    Set.Kind = StmtKind::Assign;
    Set.Dst = HighExpr::makeVar(Base, Word);
    Set.Val = At(SP, NdOp::INT_SUB, 0x48);
    Func.Body.push_back(std::move(Set));
  }
  HighStmt Ctor;
  Ctor.Kind = StmtKind::Call;
  Ctor.CallExpr = HighExpr::makeCall("probe_ctor", 0x140002000,
                                     {At(Base, NdOp::INT_ADD, 0x20)});
  Func.Body.push_back(std::move(Ctor));
  HighStmt Scope;
  Scope.Kind = StmtKind::CxxTry;
  Scope.EHIsReducible = true;
  HighStmt Work;
  Work.Kind = StmtKind::Call;
  Work.CallExpr = HighExpr::makeCall("work", 0x140002010, {});
  Scope.Body.push_back(std::move(Work));
  HighEHClause Cleanup;
  Cleanup.Kind = HighEHClauseKind::CxxCleanup;
  Cleanup.FilterOrActionVA = DtorVA;
  Cleanup.UnwindActionKind = CxxUnwindAction::ActionKind::DestructorWithObject;
  Cleanup.UnwindObjectOffset = 0x20;
  Scope.EHClauses.push_back(Cleanup);
  Scope.EHClauseBodies.emplace_back();
  Func.Body.push_back(std::move(Scope));
  HighStmt Dtor;
  Dtor.Kind = StmtKind::Call;
  Dtor.CallExpr =
      HighExpr::makeCall("probe_dtor", DtorVA, {At(Base, NdOp::INT_ADD, 0x20)});
  Func.Body.push_back(std::move(Dtor));

  attachCxxObjectActionCalls(Func, Arch::X64);
  const auto &Body = Func.Body[3].EHClauseBodies.front();
  ASSERT_EQ(Body.size(), 1u);
  ASSERT_EQ(Body.front().Kind, StmtKind::Call);
  const HighExpr &Call = *Body.front().CallExpr;
  EXPECT_EQ(Call.CallAddr, DtorVA);
  EXPECT_EQ(Call.CallTarget, "probe_dtor");
  ASSERT_EQ(Call.Operands.size(), 1u);
  const HighExpr &This = *Call.Operands.front();
  ASSERT_EQ(This.Kind, ExprKind::BinOp);
  EXPECT_EQ(This.Op, NdOp::INT_SUB);
  ASSERT_EQ(This.Operands.size(), 2u);
  EXPECT_EQ(This.Operands[0]->Var.RegOff, TRI.StackPointer);
  EXPECT_EQ(This.Operands[1]->ConstVal, 40u);

  // A local with two different values spells neither.
  Func.Body[1].Val = At(SP, NdOp::INT_SUB, 0x50);
  Func.Body[3].EHClauseBodies.front().clear();
  attachCxxObjectActionCalls(Func, Arch::X64);
  EXPECT_TRUE(Func.Body[3].EHClauseBodies.front().empty());
}

TEST(COFFExceptionIR, AnObjectNoCodeAddressesKeepsItsActionAsANote) {
  // The cleanup would name an object the source never shows.
  const std::string Source = emitHighC({objectInTry(/*PassesObject=*/false)});
  const size_t InnerTry = Source.find("try {", Source.find("try {") + 1);
  const size_t Cleanup = Source.find("} catch (...) {", InnerTry);
  const size_t Rethrow = Source.find("throw;", Cleanup);
  ASSERT_NE(Cleanup, std::string::npos) << Source;
  ASSERT_NE(Rethrow, std::string::npos) << Source;
  const std::string Action = Source.substr(Cleanup, Rethrow - Cleanup);
  EXPECT_NE(Action.find("/* cleanup @ 0x14000F000 */"), std::string::npos)
      << Source;
  EXPECT_EQ(Action.find("probe_dtor("), std::string::npos) << Source;
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
  Br.addInput(Arg0);
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
  Cxx.IPMap = {
      {FunctionVA, -1}, {FunctionVA + 0x20, 0}, {FunctionVA + 0x50, -1}};
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
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} catch (...) {"), std::string::npos) << Source;
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
  Br.addInput(Arg0);
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
  const size_t Wind = Source.find("try {");
  const size_t Unwind = Source.find("} catch (...) {", Wind);
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
  Br.addInput(Arg0);
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
  Cxx.IPMap = {{Func.Entry, -1}, {Func.Entry + 4, 1}, {Func.Entry + 0x18, -1}};
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
  Cxx.IPMap = {{Func.Entry, -1},        {Func.Entry + 4, 0},
               {Func.Entry + 0x10, 2},  {Func.Entry + 0x30, 0},
               {Func.Entry + 0x40, -1}, {Func.Entry + 0x44, 0}};
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

namespace {

/// Blocks of a nested SEH frame: \p Specs lists, per block, its start and end,
/// the call it makes (0 for none), whether it returns, where it branches (0
/// to fall into the next block, or nowhere when it returns), and where a
/// conditional branch ahead of that fall-through goes (0 for none).
struct NestedSEHBlockSpec {
  va_t Start;
  va_t End;
  va_t Callee;
  bool Returns;
  va_t BranchTo;
  va_t CondTo = 0;
};

MedFunc makeNestedSEHFixture(llvm::StringRef Name,
                             llvm::ArrayRef<NestedSEHBlockSpec> Specs,
                             llvm::ArrayRef<SEHScopeRecord> Scopes,
                             va_t CodeEnd) {
  MedFunc Func;
  Func.Entry = Specs.front().Start;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();
  // Conditional branches test a value the caller passes, which no pass can
  // decide.
  MedVar Flag;
  Flag.Kind = MedVar::Param;
  Flag.TheArch = Arch::X64;
  Flag.Size = 8;
  if (llvm::any_of(Specs, [](const NestedSEHBlockSpec &S) { return S.CondTo; }))
    Func.Params.push_back(Flag);
  auto IndexAt = [&](va_t Start) {
    for (size_t I = 0; I < Specs.size(); ++I)
      if (Specs[I].Start == Start)
        return static_cast<int>(I);
    ADD_FAILURE() << "no block starts at " << Start;
    return -1;
  };
  for (size_t I = 0; I < Specs.size(); ++I) {
    MedBlock Block;
    Block.Id = static_cast<int>(I);
    Block.StartAddr = Specs[I].Start;
    Block.EndAddr = Specs[I].End;
    if (Specs[I].Callee) {
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = Specs[I].Start + 4;
      Call.addInput(MedVar::makeConst(Specs[I].Callee, 8));
      Block.Ops.push_back(std::move(Call));
    }
    if (Specs[I].Returns) {
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = Specs[I].End - 1;
      Block.Ops.push_back(std::move(Return));
    } else if (Specs[I].CondTo) {
      MedOp Branch;
      Branch.Opcode = NdOp::COND_BR;
      Branch.Addr = Specs[I].End - 2;
      Branch.addInput(MedVar::makeConst(Specs[I].CondTo, 8));
      Branch.addInput(Flag);
      Block.Ops.push_back(std::move(Branch));
      Block.Succs = {IndexAt(Specs[I].CondTo), static_cast<int>(I + 1)};
    } else if (Specs[I].BranchTo) {
      MedOp Branch;
      Branch.Opcode = NdOp::BRANCH;
      Branch.Addr = Specs[I].End - 2;
      Branch.addInput(MedVar::makeConst(Specs[I].BranchTo, 8));
      Block.Ops.push_back(std::move(Branch));
      Block.Succs = {IndexAt(Specs[I].BranchTo)};
    } else {
      Block.Succs = {static_cast<int>(I + 1)};
    }
    Func.Blocks.push_back(std::move(Block));
  }
  for (const MedBlock &Block : Func.Blocks)
    for (int Succ : Block.Succs)
      Func.Blocks[Succ].Preds.push_back(Block.Id);

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, CodeEnd};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEH.Scopes.assign(Scopes.begin(), Scopes.end());
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);
  return Func;
}

SEHScopeRecord catchAllScope(va_t Begin, va_t End, va_t HandlerVA) {
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Begin, End};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = HandlerVA;
  return Scope;
}

} // namespace

TEST(COFFExceptionIR, NestedSEHHandlerInsideTheOuterRangeBecomesItsClause) {
  // __try { __try { A(); } __except (1) { H(); } B(); } __except (1) { G(); }
  // MSVC lays the inner handler out inside the outer range; the inner body
  // jumps over it.  Both the handler and the inner try are guarded by the
  // outer range alone, so the handler is the inner clause body.
  constexpr va_t F = 0x140001000;
  const NestedSEHBlockSpec Specs[] = {
      {F, F + 0x10, 0x140008000, false, F + 0x30},
      {F + 0x20, F + 0x30, 0x140009000, false, F + 0x30},
      {F + 0x30, F + 0x38, 0x14000A000, false, 0},
      {F + 0x38, F + 0x40, 0x14000C000, false, 0},
      {F + 0x40, F + 0x41, 0, true, 0},
      {F + 0x50, F + 0x60, 0x14000B000, false, F + 0x40}};
  const SEHScopeRecord Scopes[] = {catchAllScope(F, F + 0x10, F + 0x20),
                                   catchAllScope(F, F + 0x40, F + 0x50)};
  MedFunc Func = makeNestedSEHFixture("nested_seh_inline_handler", Specs,
                                      Scopes, F + 0x60);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.StructuredExceptionRegions, 2u);
  ASSERT_EQ(High.UnstructuredExceptionRegions, 0u);
  const HighStmt *Inner = nullptr;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind == StmtKind::SEHTry && Stmt.EHRange.End == F + 0x10)
      Inner = &Stmt;
  });
  ASSERT_NE(Inner, nullptr);
  ASSERT_EQ(Inner->EHClauseBodies.size(), 1u);
  size_t InClause = 0;
  walkStmts(Inner->EHClauseBodies.front(),
            [&](const HighStmt &Stmt) { InClause += Stmt.Addr == F + 0x24; });
  EXPECT_EQ(InClause, 1u);

  const std::string Source = emitHighC({High});
  const size_t InnerExcept = Source.find("__except");
  ASSERT_NE(InnerExcept, std::string::npos) << Source;
  const size_t Handler = Source.find("sub_140009000();", InnerExcept);
  ASSERT_NE(Handler, std::string::npos) << Source;
  EXPECT_LT(Handler, Source.find("__except", InnerExcept + 1)) << Source;
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
}

TEST(COFFExceptionIR, SEHHandlerOutsideAnOuterRangeStaysOutOfTheInnerClause) {
  // The inner try sits in the outer range but its handler was laid out past
  // it, so the outer __except does not guard the handler.  As the inner
  // clause body it would be guarded by the outer __try, so it stays where it
  // is and the clause jumps to it.
  constexpr va_t F = 0x140001000;
  const NestedSEHBlockSpec Specs[] = {
      {F, F + 0x10, 0x140008000, false, 0},
      {F + 0x10, F + 0x20, 0x14000A000, false, 0},
      {F + 0x20, F + 0x21, 0, true, 0},
      {F + 0x30, F + 0x40, 0x140009000, true, 0},
      {F + 0x50, F + 0x60, 0x14000B000, true, 0}};
  const SEHScopeRecord Scopes[] = {catchAllScope(F, F + 0x10, F + 0x30),
                                   catchAllScope(F, F + 0x20, F + 0x50)};
  MedFunc Func = makeNestedSEHFixture("nested_seh_distant_handler", Specs,
                                      Scopes, F + 0x60);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.StructuredExceptionRegions, 2u);
  const HighStmt *Outer = nullptr;
  for (const HighStmt &Stmt : High.Body)
    if (Stmt.Kind == StmtKind::SEHTry && Stmt.EHRange.End == F + 0x20)
      Outer = &Stmt;
  ASSERT_NE(Outer, nullptr);
  size_t Guarded = 0;
  walkStmts(Outer->Body,
            [&](const HighStmt &Stmt) { Guarded += Stmt.Addr == F + 0x34; });
  EXPECT_EQ(Guarded, 0u);
  size_t Anywhere = 0;
  walkStmts(High.Body,
            [&](const HighStmt &Stmt) { Anywhere += Stmt.Addr == F + 0x34; });
  EXPECT_EQ(Anywhere, 1u);
}

TEST(COFFExceptionIR, SEHHandlerEndingAnArmContinuesAfterTheIfElse) {
  // if (..) { __try { A(); } __except (1) { H(); } B(); return; } else C();
  // D();  The handler sits after C() and falls into D(), so it ends the else
  // arm and continues after the if/else; as the clause body it keeps that
  // path with a jump or a copy of what runs there.
  constexpr va_t F = 0x140001000;
  const NestedSEHBlockSpec Specs[] = {
      {F, F + 0x10, 0, false, 0, F + 0x30},
      {F + 0x10, F + 0x20, 0x140008000, false, 0},
      {F + 0x20, F + 0x30, 0x14000A000, true, 0},
      {F + 0x30, F + 0x40, 0x14000C000, false, F + 0x50},
      {F + 0x40, F + 0x50, 0x140009000, false, 0},
      {F + 0x50, F + 0x60, 0x14000D000, true, 0}};
  const SEHScopeRecord Scopes[] = {catchAllScope(F + 0x10, F + 0x20, F + 0x40)};
  MedFunc Func = makeNestedSEHFixture("seh_handler_ending_an_arm", Specs,
                                      Scopes, F + 0x60);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.StructuredExceptionRegions, 1u);
  const HighStmt *Try = nullptr;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind == StmtKind::SEHTry)
      Try = &Stmt;
  });
  ASSERT_NE(Try, nullptr);
  ASSERT_EQ(Try->EHClauseBodies.size(), 1u);
  size_t InClause = 0;
  walkStmts(Try->EHClauseBodies.front(),
            [&](const HighStmt &Stmt) { InClause += Stmt.Addr == F + 0x44; });
  EXPECT_EQ(InClause, 1u);
  size_t Anywhere = 0;
  walkStmts(High.Body,
            [&](const HighStmt &Stmt) { Anywhere += Stmt.Addr == F + 0x44; });
  EXPECT_EQ(Anywhere, 1u);

  const std::string Source = emitHighC({High});
  EXPECT_EQ(Source.find("goto L_140001040"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000D000();"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, UnstructuredSEHEntryMayHaveNoLiveRegisterCopies) {
  REQUIRE_HOST_FIXTURE_COMPILER();
  HighFunc Function;
  Function.Entry = 0x140001000;
  Function.Name = "empty_handler_entry";
  Function.ReturnType = NdType::makeInt(4);
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = Function.Entry + 0x40;
  HighStmt Region;
  Region.Kind = StmtKind::SEHTry;
  Region.EHRange = {Function.Entry, Function.Entry + 0x10};
  Region.EHIsReducible = false;
  HighEHClause Clause;
  Clause.Kind = HighEHClauseKind::SEHExcept;
  Clause.HandlerVA = Jump.GotoTarget;
  Region.EHClauses = {Clause};
  Region.EHClauseBodies = {{Jump}};
  Region.Body = {Jump};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = Jump.GotoTarget;
  Return.RetVal = HighExpr::makeConst(7, 4);
  Function.Body = {Region, Return};
  const std::string Source = emitHighC({Function});
  EXPECT_NE(Source.find("Native exception entry to the handler"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("goto L_140001040;"), std::string::npos) << Source;
  for (const char *Level : {"-O0", "-O2"}) {
    const auto Syntax = runCCompiler("#include <stdint.h>\n" + Source,
                                     {"-std=c11", Level, "-fsyntax-only"});
    EXPECT_EQ(Syntax.ExitCode, 0) << Syntax.Error << Source;
  }
  // Actual handler code still cannot be moved into an unstructured clause.
  Function.Body.front().EHClauseBodies.front().insert(
      Function.Body.front().EHClauseBodies.front().begin(), Return);
  EXPECT_THROW(emitHighC({Function}), std::invalid_argument);
}

TEST(COFFExceptionIR, SEHRangeLeavingAGuardedCallOutsideStaysUnstructured) {
  // X(); if (c) Z(); else Y(); with X and Y guarded and Z not.  The if/else
  // crosses the range, so the only slice a __try could wrap is Y alone, and
  // the guarded X would run unprotected.  The region is reported
  // unstructured instead, and every call, the handler's included, prints.
  constexpr va_t F = 0x140001000;
  const NestedSEHBlockSpec Specs[] = {
      {F, F + 0x10, 0x140008000, false, 0, F + 0x30},
      {F + 0x10, F + 0x20, 0x140009000, false, F + 0x40},
      {F + 0x30, F + 0x40, 0x14000A000, false, 0},
      {F + 0x40, F + 0x41, 0, true, 0},
      {F + 0x50, F + 0x60, 0x14000B000, false, F + 0x40}};
  const SEHScopeRecord Scopes[] = {catchAllScope(F, F + 0x20, F + 0x50)};
  MedFunc Func =
      makeNestedSEHFixture("seh_range_crossing_if", Specs, Scopes, F + 0x60);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  EXPECT_EQ(High.StructuredExceptionRegions, 0u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 1u);
  const std::string Source = emitHighC({High});
  for (const char *Call : {"sub_140008000();", "sub_140009000();",
                           "sub_14000A000();", "sub_14000B000();"})
    EXPECT_NE(Source.find(Call), std::string::npos) << Call << "\n" << Source;
}

TEST(COFFExceptionIR, OuterSEHRangeIsNeverNestedInsideTheTryItHolds) {
  // The inner handler runs into a copy of the tail and sits after the
  // return, so the outer range's statements are not one slice.  The inner
  // try's body is: wrapping the outer try there would make the inner
  // handler the second one an exception reaches instead of the first.
  constexpr va_t F = 0x140001000;
  const NestedSEHBlockSpec Specs[] = {
      {F, F + 0x10, 0x140008000, false, F + 0x30},
      {F + 0x20, F + 0x30, 0x140009000, false, 0},
      {F + 0x30, F + 0x40, 0x14000A000, false, 0},
      {F + 0x40, F + 0x41, 0, true, 0},
      {F + 0x50, F + 0x60, 0x14000B000, false, F + 0x40}};
  const SEHScopeRecord Scopes[] = {catchAllScope(F, F + 0x10, F + 0x20),
                                   catchAllScope(F, F + 0x40, F + 0x50)};
  MedFunc Func = makeNestedSEHFixture("nested_seh_scattered_outer", Specs,
                                      Scopes, F + 0x60);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  EXPECT_EQ(High.StructuredExceptionRegions + High.UnstructuredExceptionRegions,
            2u);
  std::function<void(const std::vector<HighStmt> &, const HighStmt *)> Check =
      [&](const std::vector<HighStmt> &List, const HighStmt *Outer) {
        for (const HighStmt &Stmt : List) {
          const bool IsTry = Stmt.Kind == StmtKind::SEHTry;
          if (IsTry && Outer)
            EXPECT_TRUE(Outer->EHRange.contains(Stmt.EHRange))
                << "try over [" << Stmt.EHRange.Begin << ", "
                << Stmt.EHRange.End << ") nested in one over ["
                << Outer->EHRange.Begin << ", " << Outer->EHRange.End << ")";
          Check(Stmt.Body, IsTry ? &Stmt : Outer);
          Check(Stmt.ElseBody, Outer);
          for (const auto &ClauseBody : Stmt.EHClauseBodies)
            Check(ClauseBody, nullptr);
        }
      };
  Check(High.Body, nullptr);
}

TEST(COFFExceptionIR, KeepsSEHHandlerPlacedAfterReturn) {
  // IovBuildAsynchronousFsdRequest: the __except handler follows the return
  // and its first instruction leaves no statement.  Only the exception
  // dispatcher enters it, so no goto anchors its address; it must still keep
  // its code and label.
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t ReturnVA = FunctionVA + 0x10;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  constexpr va_t HandlerCallVA = HandlerVA + 0xC;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "seh_handler_after_return";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = ReturnVA;
  Protected.Succs = {1};
  MedOp ProtectedCall;
  ProtectedCall.Opcode = NdOp::CALL;
  ProtectedCall.Addr = FunctionVA + 4;
  ProtectedCall.addInput(MedVar::makeConst(0x140008000, 8));
  Protected.Ops.push_back(std::move(ProtectedCall));
  Func.Blocks.push_back(std::move(Protected));

  MedBlock Exit;
  Exit.Id = 1;
  Exit.StartAddr = ReturnVA;
  Exit.EndAddr = ReturnVA + 1;
  Exit.Preds = {0};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ReturnVA;
  Exit.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Exit));

  MedBlock Handler;
  Handler.Id = 2;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerCallVA + 5;
  MedOp Folded;
  Folded.Opcode = NdOp::NOP;
  Folded.Addr = HandlerVA;
  Handler.Ops.push_back(std::move(Folded));
  MedOp HandlerCall;
  HandlerCall.Opcode = NdOp::CALL;
  HandlerCall.Addr = HandlerCallVA;
  HandlerCall.addInput(MedVar::makeConst(0x140009000, 8));
  Handler.Ops.push_back(std::move(HandlerCall));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, HandlerCallVA + 5};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {FunctionVA, ReturnVA};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = HandlerVA;
  SEHExceptionInfo SEH;
  SEH.Scopes.push_back(std::move(Scope));
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  EXPECT_NE(Source.find("sub_140009000();"), std::string::npos) << Source;
  if (Source.find("goto L_140001020") != std::string::npos)
    EXPECT_NE(Source.find("L_140001020:"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, SharedHandlerAndColdEntryKeepTheirPaths) {
  // RtlGuardIsValidStackPointer: a cold block jumps back to the start of the
  // protected range, and normal flow falls into the handler's code.  The
  // label must sit before `__try` (C forbids jumping into it) and the
  // shared handler block must stay reachable from the ordinary path.
  constexpr va_t F = 0x140001000;
  constexpr va_t Protected = F + 0x10;
  constexpr va_t Shared = F + 0x20;
  constexpr va_t Cold = F + 0x30;

  MedFunc Func;
  Func.Entry = F;
  Func.Name = "shared_handler";
  Func.ReturnType = NdType::makeVoid();
  auto AddBlock = [&](int Id, va_t Start, va_t End, std::vector<int> Succs,
                      std::vector<int> Preds) -> MedBlock & {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Start;
    Block.EndAddr = End;
    Block.Succs = std::move(Succs);
    Block.Preds = std::move(Preds);
    Func.Blocks.push_back(std::move(Block));
    return Func.Blocks.back();
  };
  auto Op = [](NdOp Code, va_t Addr, uint64_t Const) {
    MedOp O;
    O.Opcode = Code;
    O.Addr = Addr;
    O.addInput(MedVar::makeConst(Const, 8));
    return O;
  };
  AddBlock(0, F, F + 4, {3}, {}).Ops.push_back(Op(NdOp::BRANCH, F, Cold));
  {
    MedBlock &B = AddBlock(1, Protected, Protected + 8, {2}, {3});
    B.Ops.push_back(Op(NdOp::CALL, Protected, 0x140008000));
    B.Ops.push_back(Op(NdOp::BRANCH, Protected + 5, Shared));
  }
  {
    MedBlock &B = AddBlock(2, Shared, Shared + 8, {}, {1});
    B.Ops.push_back(Op(NdOp::CALL, Shared, 0x140009000));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Shared + 5;
    B.Ops.push_back(std::move(Return));
  }
  AddBlock(3, Cold, Cold + 4, {1}, {0})
      .Ops.push_back(Op(NdOp::BRANCH, Cold, Protected));

  ExceptionFunction EH;
  EH.CodeRange = {F, Cold + 4};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Protected, Protected + 8};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Shared;
  SEHExceptionInfo SEH;
  SEH.Scopes.push_back(std::move(Scope));
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  const size_t Try = Source.find("__try");
  ASSERT_NE(Try, std::string::npos) << Source;
  // The cold block's jump to the protected start is the fall-through into
  // the try once the cold block moves ahead of it. A jump that remains needs
  // its one label before `__try`.
  const size_t Label = Source.find("L_140001010:");
  if (Source.find("goto L_140001010;") != std::string::npos) {
    ASSERT_NE(Label, std::string::npos) << Source;
    EXPECT_LT(Label, Try) << Source;
    EXPECT_EQ(Source.find("L_140001010:", Label + 1), std::string::npos)
        << Source;
  } else {
    EXPECT_EQ(Label, std::string::npos) << Source;
  }
  // The shared block is printed once, outside the __except body.  Search the
  // body only; the file also declares the callee.
  const size_t Body = Source.find("neverd.entry");
  ASSERT_NE(Body, std::string::npos) << Source;
  const size_t Marker = Source.find("sub_140009000();", Body);
  ASSERT_NE(Marker, std::string::npos) << Source;
  EXPECT_EQ(Source.find("sub_140009000();", Marker + 1), std::string::npos)
      << Source;
  EXPECT_GT(Marker, Source.find("__except")) << Source;
}

TEST(COFFExceptionIR, GotoIntoTryBodySplitsTheTry) {
  // NtSetSystemTime: a cold path branches into the middle of a protected
  // range.  C forbids jumping into __try; x64 SEH protection is by address,
  // so the writer prints two consecutive tries with the label between them.
  constexpr va_t F = 0x140001000;
  HighFunc Func;
  Func.Name = "split_try";
  Func.Entry = F;
  Func.ReturnType = NdType::makeInt(4);
  auto Return = [](va_t Addr, uint64_t Value) {
    HighStmt S;
    S.Kind = StmtKind::Return;
    S.Addr = Addr;
    S.RetVal = HighExpr::makeConst(Value, 4);
    return S;
  };
  HighStmt Goto;
  Goto.Kind = StmtKind::Goto;
  Goto.Addr = F;
  Goto.GotoTarget = F + 0x14;
  Func.Body.push_back(std::move(Goto));
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = F + 0x10;
  Try.EHRange = {F + 0x10, F + 0x18};
  Try.EHIsReducible = true;
  HighEHClause Clause;
  Clause.Kind = HighEHClauseKind::SEHExcept;
  Clause.HandlerVA = F + 0x20;
  Try.EHClauses.push_back(Clause);
  Try.EHClauseBodies.emplace_back();
  Try.Body.push_back(Return(F + 0x10, 1));
  Try.Body.push_back(Return(F + 0x14, 2));
  Func.Body.push_back(std::move(Try));

  const std::string Source = emitHighC({Func});
  const size_t First = Source.find("__try");
  const size_t Label = Source.find("L_140001014:");
  ASSERT_NE(First, std::string::npos) << Source;
  ASSERT_NE(Label, std::string::npos) << Source;
  EXPECT_LT(Source.find("__except", First), Label) << Source;
  EXPECT_NE(Source.find("__try", Label), std::string::npos) << Source;
  EXPECT_EQ(Source.find("L_140001014:", Label + 1), std::string::npos)
      << Source;
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

TEST(COFFExceptionIR, StructuresX86RegistrationFinallyAtItsActualBody) {
  constexpr va_t FunctionVA = 0x401000;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  MedFunc Func = makeWindowsHandlerFixture("x86_registration_finally", 0x40E000,
                                           1, FunctionVA);
  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Personality = ExceptionPersonality::ExceptHandler3;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  RegistrationChainInfo &Chain = EH.Registration.emplace();
  Chain.SeededTryLevel = -1;
  Chain.Scopes.push_back({-1, 0, HandlerVA, true});
  Func.ExceptionMetadata = std::move(EH);
  auto &States = Func.RegistrationStates.emplace();
  States.Complete = true;
  States.Blocks = {
      {0, {FunctionVA, FunctionVA + 0x10}, {0}, false, false, true},
      {1, {HandlerVA, FunctionVA + 0x30}, {-1}, false, true, true},
      {2, {FunctionVA + 0x30, FunctionVA + 0x40}, {-1}, false, false, true}};
  HighFunc High = MedToHighConverter().convert(Func, Arch::X86);
  ASSERT_EQ(High.StructuredExceptionRegions, 1u);
  ASSERT_FALSE(High.Body.empty());
  const HighStmt &Try = High.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::SEHTry);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses.front().FilterOrActionVA, HandlerVA);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  ASSERT_EQ(Try.EHClauseBodies.front().size(), 1u);
  EXPECT_EQ(Try.EHClauseBodies.front().front().Addr, HandlerVA);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream));
  Stream.flush();
  EXPECT_NE(Source.find("sub_40E000();"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("handler @ 0x0"), std::string::npos) << Source;
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
  // State 0 is the try's own state: unwinding to its catch stops there, so
  // the state's object encloses the try and only an exception the try lets
  // through runs the cleanup.
  const HighStmt &Scope = High.Body.front();
  ASSERT_EQ(Scope.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Scope.EHClauses.size(), 1u);
  EXPECT_EQ(Scope.EHClauses[0].Kind, HighEHClauseKind::CxxCleanup);
  ASSERT_EQ(Scope.EHClauseBodies.size(), 1u);
  ASSERT_EQ(Scope.EHClauseBodies[0].size(), 1u);
  EXPECT_EQ(Scope.EHClauseBodies[0].front().Addr, ActionVA);
  ASSERT_FALSE(Scope.Body.empty());
  const HighStmt &Try = Scope.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::CxxTry);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses[0].Kind, HighEHClauseKind::CxxCatch);
  ASSERT_EQ(Try.EHClauseBodies.size(), 1u);
  EXPECT_TRUE(Try.EHClauseBodies[0].empty());
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

TEST(COFFExceptionIR, LoneJumpAtTryExitKeepsItsLabel) {
  // DbgkpSuppressDbgMsg: two paths in a __try jump to a block that holds only
  // `jmp done`, where the protected range ends.  Removing that jump as
  // redundant must keep the label the other gotos name.
  constexpr va_t F = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = F;
  const std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x18,                   // sub rsp, 18h
      0xc7, 0x04, 0x24, 0x00, 0x00, 0x00, 0x00, // mov dword [rsp], 0
      0x85, 0xc9,                               // try: test ecx, ecx
      0x79, 0x09,                               // jns checks
      0xc7, 0x04, 0x24, 0x01, 0x00, 0x00, 0x00, // mov dword [rsp], 1
      0xeb, 0x10,                               // jmp exit
      0x85, 0xd2,                               // checks: test edx, edx
      0x74, 0x0c,                               // je exit
      0x45, 0x85, 0xc0,                         // test r8d, r8d
      0x74, 0x07,                               // je exit
      0xc7, 0x04, 0x24, 0x02, 0x00, 0x00, 0x00, // mov dword [rsp], 2
      0xeb, 0x00,                               // exit: jmp done
      0x8b, 0x04, 0x24,                         // done: mov eax, [rsp]
      0x48, 0x83, 0xc4, 0x18,                   // add rsp, 18h
      0xc3,                                     // ret
      0xc7, 0x04, 0x24, 0x03, 0x00, 0x00, 0x00, // handler: mov dword [rsp], 3
      0xeb, 0xef};                              // jmp done
  Segment Text;
  Text.Name = ".text";
  Text.VA = F;
  Text.Size = 0x40;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  std::copy(Code.begin(), Code.end(), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = F;
  TextSection.Size = 0x40;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(F, F + Code.size());
  Img.Symbols.push_back(Symbol::makeFunc(F, Code.size()));
  ExceptionFunction EH;
  EH.CodeRange = {F, F + Code.size()};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.PrologueSize = 4;
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateSmall;
  Alloc.CodeOffset = 4;
  Alloc.StackOffset = 0x18;
  EH.UnwindOperations.push_back(Alloc);
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {F + 0x0b, F + 0x2a};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = F + 0x32;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(F);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  std::smatch Goto;
  for (auto It = Source.cbegin(); std::regex_search(
           It, Source.cend(), Goto, std::regex(R"(goto (L_\w+);)"));
       It = Goto.suffix().first)
    EXPECT_NE(Source.find(Goto[1].str() + ":"), std::string::npos)
        << Goto[1] << " has no label\n"
        << Source;
}

TEST(COFFExceptionIR, ExceptHandlerSeesTheEstablishedFrame) {
  // PnpInsertEventInQueue: the unwinder enters an __except handler with RSP
  // at the function body's frame.  The handler's `[rsp+30h]` is the RCX home
  // slot the prologue wrote, not a stack argument of the entry frame.
  constexpr va_t F = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = F;
  const std::vector<uint8_t> Code = {
      0x48, 0x89, 0x4c, 0x24, 0x08, // mov [rsp+8], rcx
      0x53,                         // push rbx
      0x48, 0x83, 0xec, 0x20,       // sub rsp, 20h
      0x8b, 0x01,                   // try: mov eax, [rcx]
      0xeb, 0x07,                   // jmp done
      0x48, 0x8b, 0x44, 0x24, 0x30, // handler: mov rax, [rsp+30h]
      0x8b, 0x00,                   // mov eax, [rax]
      0x48, 0x83, 0xc4, 0x20,       // done: add rsp, 20h
      0x5b,                         // pop rbx
      0xc3};                        // ret
  Segment Text;
  Text.Name = ".text";
  Text.VA = F;
  Text.Size = 0x40;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  std::copy(Code.begin(), Code.end(), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = F;
  TextSection.Size = 0x40;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(F, F + Code.size());
  Img.Symbols.push_back(Symbol::makeFunc(F, Code.size()));
  ExceptionFunction EH;
  EH.CodeRange = {F, F + Code.size()};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.PrologueSize = 10;
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateSmall;
  Alloc.CodeOffset = 10;
  Alloc.StackOffset = 0x20;
  EH.UnwindOperations.push_back(Alloc);
  UnwindOperation Push;
  Push.Kind = UnwindOperationKind::PushNonVolatile;
  Push.CodeOffset = 6;
  Push.Register = 3;
  EH.UnwindOperations.push_back(Push);
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {F + 0x0a, F + 0x0e};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = F + 0x0e;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(F);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  // The handler dereferences the value stored in the RCX home slot.  With RSP
  // at the entry value it read a stack argument the function does not have;
  // rebasing twice named an unwritten local instead.
  EXPECT_EQ(Source.find("arg1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("var_m"), std::string::npos) << Source;
  EXPECT_TRUE(
      std::regex_search(Source, std::regex(R"(= (\(uintptr_t\))?arg0;)")))
      << Source;
}

TEST(COFFExceptionIR, ExceptBodyKeepsTheBlockItRanInto) {
  // PoGetRequesterOld: the __except handler (`and qword [rsp+48h], 0;
  // mov r8, [rsp+40h]`) runs on into a tail its try body also jumps to.
  // Each path into the tail copies only the values it set, so the guarded
  // range is one slice: the __try wraps every statement of it, the
  // handler's statements are the __except arm, and the tail it runs on into
  // follows the region.
  constexpr va_t F = 0x1403F75E4;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = F;
  const std::vector<uint8_t> Code = {
      0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0xc2,
      0x84, 0xc9, 0x75, 0x23, 0x41, 0x83, 0x20, 0x00, 0x49, 0x89, 0x50, 0x08,
      0x48, 0x85, 0xc0, 0x0f, 0x84, 0xe0, 0x00, 0x00, 0x00, 0xba, 0x50, 0x6f,
      0x44, 0x67, 0x48, 0x8b, 0xc8, 0xe8, 0x4e, 0xe0, 0xe0, 0xff, 0xe9, 0xce,
      0x00, 0x00, 0x00, 0x48, 0x83, 0x64, 0x24, 0x48, 0x00, 0x65, 0x48, 0x8b,
      0x14, 0x25, 0x88, 0x01, 0x00, 0x00, 0xf7, 0x42, 0x74, 0x00, 0x04, 0x00,
      0x00, 0x75, 0x12, 0x80, 0xba, 0x4a, 0x02, 0x00, 0x00, 0x01, 0x74, 0x09,
      0x48, 0x8b, 0x92, 0xf0, 0x00, 0x00, 0x00, 0xeb, 0x02, 0x33, 0xd2, 0x48,
      0x85, 0xd2, 0x74, 0x6f, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x88, 0x01, 0x00,
      0x00, 0x48, 0x8b, 0x88, 0xb8, 0x00, 0x00, 0x00, 0x48, 0x83, 0xb9, 0x80,
      0x05, 0x00, 0x00, 0x00, 0x74, 0x3c, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x88,
      0x01, 0x00, 0x00, 0x48, 0x8b, 0x88, 0xb8, 0x00, 0x00, 0x00, 0x48, 0x8b,
      0x81, 0x80, 0x05, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x20, 0x0f, 0xb7,
      0x40, 0x08, 0xb9, 0x4c, 0x01, 0x00, 0x00, 0x66, 0x3b, 0xc1, 0x74, 0x0a,
      0xb9, 0xc4, 0x01, 0x00, 0x00, 0x66, 0x3b, 0xc1, 0x75, 0x08, 0x8b, 0x82,
      0x60, 0x2f, 0x00, 0x00, 0xeb, 0x07, 0x48, 0x8b, 0x82, 0x20, 0x17, 0x00,
      0x00, 0x48, 0x89, 0x44, 0x24, 0x48, 0xeb, 0x0b, 0x48, 0x83, 0x64, 0x24,
      0x48, 0x00, 0x4c, 0x8b, 0x44, 0x24, 0x40, 0x48, 0x8b, 0x44, 0x24, 0x48,
      0x48, 0xf7, 0xd8, 0x1b, 0xc9, 0xf7, 0xd9, 0xff, 0xc1, 0x41, 0x89, 0x08,
      0x65, 0x48, 0x8b, 0x04, 0x25, 0x88, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x88,
      0xb8, 0x00, 0x00, 0x00, 0x49, 0x89, 0x48, 0x08, 0x48, 0x8b, 0x44, 0x24,
      0x48, 0x41, 0x89, 0x40, 0x28, 0x48, 0x83, 0xc4, 0x28, 0xc3, 0xcc,
  };
  Segment Text;
  Text.Name = ".text";
  Text.VA = F;
  Text.Size = 0x110;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  std::copy(Code.begin(), Code.end(), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = F;
  TextSection.Size = 0x110;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(F, F + Code.size());
  Img.Symbols.push_back(Symbol::makeFunc(F, Code.size()));
  ExceptionFunction EH;
  EH.CodeRange = {F, F + Code.size()};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.PrologueSize = 9;
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateSmall;
  Alloc.CodeOffset = 9;
  Alloc.StackOffset = 0x28;
  EH.UnwindOperations.push_back(Alloc);
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {0x1403F7648, 0x1403F76AC};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = 0x1403F76AC;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(F);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_EQ(Result.HighFuncs.front().StructuredExceptionRegions, 1u);
  EXPECT_EQ(Result.HighFuncs.front().UnstructuredExceptionRegions, 0u);
  EXPECT_EQ(Source.find("unstructured SEH region"), std::string::npos)
      << Source;
  // Where each instruction's statements are: the guarded range in the
  // __try body, the handler in its __except arm, the rest outside both.
  enum class Place { Outside, TryBody, Handler };
  std::vector<std::pair<va_t, Place>> Placed;
  unsigned Tries = 0;
  std::function<void(const std::vector<HighStmt> &, Place)> Walk =
      [&](const std::vector<HighStmt> &Stmts, Place Where) {
        for (const HighStmt &S : Stmts) {
          if (S.Kind == StmtKind::SEHTry) {
            ++Tries;
            Walk(S.Body, Place::TryBody);
            for (const std::vector<HighStmt> &Clause : S.EHClauseBodies)
              Walk(Clause, Place::Handler);
            continue;
          }
          if (S.Addr)
            Placed.push_back({S.Addr, Where});
          Walk(S.Body, Where);
          Walk(S.ElseBody, Where);
          Walk(S.DefaultBody, Where);
          for (const SwitchCase &Case : S.Cases)
            Walk(Case.Body, Where);
        }
      };
  Walk(Result.HighFuncs.front().Body, Place::Outside);
  EXPECT_EQ(Tries, 1u) << Source;
  for (const auto &[Addr, Where] : Placed) {
    const Place Expected =
        Addr >= 0x1403F7648 && Addr < 0x1403F76AC   ? Place::TryBody
        : Addr >= 0x1403F76AC && Addr < 0x1403F76B7 ? Place::Handler
                                                    : Place::Outside;
    EXPECT_EQ(static_cast<int>(Where), static_cast<int>(Expected))
        << llvm::utohexstr(Addr) << '\n'
        << Source;
  }
  // The reads of [rdx+1720h] are copies of a block the guarded paths share,
  // with no address of their own.
  const size_t Try = Source.find("__try {");
  const size_t GuardedRead = Source.find("+ 0x1720)");
  const size_t Except = Source.find("} __except (EXCEPTION_EXECUTE_HANDLER) {");
  ASSERT_NE(GuardedRead, std::string::npos) << Source;
  EXPECT_LT(Try, GuardedRead) << Source;
  EXPECT_LT(GuardedRead, Except) << Source;
}

TEST(COFFExceptionIR, HandlerWhoseFirstCopyIsDeadKeepsItsLabel) {
  // PsOpenThread: the __except handler starts with `mov edi, eax`, whose
  // value nothing reads.  The empty __except arm prints as a goto to the
  // handler, so dropping that copy must keep the handler's label.
  constexpr va_t F = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = F;
  const std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x28,       // sub rsp, 28h
      0x31, 0xff,                   // xor edi, edi
      0x85, 0xc9,                   // try: test ecx, ecx
      0x75, 0x11,                   // jne ok
      0xe8, 0x19, 0x00, 0x00, 0x00, // call fail
      0x90,                         // nop
      0xeb, 0x0e,                   // jmp done
      0x89, 0xc6,                   // handler: mov esi, eax
      0xbf, 0x02, 0x00, 0x00, 0x00, // mov edi, 2
      0xeb, 0x05,                   // jmp done
      0xbf, 0x01, 0x00, 0x00, 0x00, // ok: mov edi, 1
      0x89, 0xf8,                   // done: mov eax, edi
      0x48, 0x83, 0xc4, 0x28,       // add rsp, 28h
      0xc3};                        // ret
  Segment Text;
  Text.Name = ".text";
  Text.VA = F;
  Text.Size = 0x40;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  std::copy(Code.begin(), Code.end(), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = F;
  TextSection.Size = 0x40;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(F, F + Code.size());
  Img.Symbols.push_back(Symbol::makeFunc(F, Code.size()));
  Img.Segments.front().Data[0x28] = 0xc3; // fail: ret
  Img.KnownCodeRanges.emplace_back(F + 0x28, F + 0x29);
  Img.Symbols.push_back(Symbol::makeFunc(F + 0x28, 1));
  ExceptionFunction EH;
  EH.CodeRange = {F, F + Code.size()};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.PrologueSize = 4;
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateSmall;
  Alloc.CodeOffset = 4;
  Alloc.StackOffset = 0x28;
  EH.UnwindOperations.push_back(Alloc);
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {F + 0x06, F + 0x12};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = F + 0x12;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(F);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  std::smatch Goto;
  for (auto It = Source.cbegin(); std::regex_search(
           It, Source.cend(), Goto, std::regex(R"(goto (L_\w+);)"));
       It = Goto.suffix().first)
    EXPECT_NE(Source.find(Goto[1].str() + ":"), std::string::npos)
        << Goto[1] << " has no label\n"
        << Source;
}

TEST(COFFExceptionIR, LargeImageStillResolvesAnImageRelativeJumpTable) {
  // ntoskrnl: with 35k unwind entries, a switch of a few dozen cases priced
  // each target as a scan of every function's metadata and was rejected,
  // losing the whole switch.  The code-owner index prices it as a lookup.
  constexpr va_t Base = 0x140000000;
  constexpr va_t F = 0x140001000;
  constexpr va_t Table = 0x140003000;
  constexpr unsigned Cases = 40;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = Base;
  Img.Entry = F;
  std::vector<uint8_t> Code = {
      0x83, 0xf9, Cases - 1,                // cmp ecx, Cases-1
      0x0f, 0x87, 0,         0,    0, 0,    // ja default (patched)
      0x4c, 0x8d, 0x0d,      0,    0, 0, 0, // lea r9, [rip+Base-next] (patched)
      0x8b, 0xc1,                           // mov eax, ecx
      0x41, 0x8b, 0x84,      0x81, 0, 0, 0,
      0,                // mov eax, [r9+rax*4+Table] (patched)
      0x4c, 0x01, 0xc8, // add rax, r9
      0xff, 0xe0};      // jmp rax
  auto Put32 = [&](size_t At, uint32_t V) {
    for (unsigned I = 0; I < 4; ++I)
      Code[At + I] = static_cast<uint8_t>(V >> (8 * I));
  };
  Put32(12, static_cast<uint32_t>(Base - (F + 16)));
  Put32(22, static_cast<uint32_t>(Table - Base));
  std::vector<uint32_t> Entries;
  for (unsigned I = 0; I < Cases; ++I) {
    Entries.push_back(static_cast<uint32_t>(F + Code.size() - Base));
    Code.insert(Code.end(), {0xb8, static_cast<uint8_t>(0x10 + I), 0, 0, 0,
                             0xc3}); // mov eax, 10h+I; ret
  }
  const size_t Default = Code.size();
  Put32(5, static_cast<uint32_t>(Default - 9));
  Code.insert(Code.end(), {0x31, 0xc0, 0xc3}); // default: xor eax, eax; ret

  Segment Text;
  Text.Name = ".text";
  Text.VA = F;
  Text.Size = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  std::copy(Code.begin(), Code.end(), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Segment RData;
  RData.Name = ".rdata";
  RData.VA = Table;
  RData.Size = 0x1000;
  RData.Flags = SegmentFlags::Readable;
  RData.Data.assign(RData.Size, 0);
  for (unsigned I = 0; I < Cases; ++I)
    for (unsigned B = 0; B < 4; ++B)
      RData.Data[4 * I + B] = static_cast<uint8_t>(Entries[I] >> (8 * B));
  Img.Segments.push_back(std::move(RData));
  // Many one-byte functions elsewhere, each with its own unwind entry.
  constexpr va_t Filler = 0x140100000;
  constexpr size_t FillerCount = 40000;
  Segment Pad;
  Pad.Name = ".text2";
  Pad.VA = Filler;
  Pad.Size = FillerCount;
  Pad.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Pad.Data.assign(Pad.Size, 0xc3);
  Img.Segments.push_back(std::move(Pad));
  for (const auto &[Name, VA, Size, Flags] :
       {std::tuple{".text", F, uint64_t{0x1000},
                   SegmentFlags::Readable | SegmentFlags::Executable},
        std::tuple{".rdata", Table, uint64_t{0x1000}, SegmentFlags::Readable},
        std::tuple{".text2", Filler, uint64_t{FillerCount},
                   SegmentFlags::Readable | SegmentFlags::Executable}}) {
    Section Sec;
    Sec.Name = Name;
    Sec.VA = VA;
    Sec.Size = Size;
    Sec.Flags = Flags;
    Img.Sections.push_back(std::move(Sec));
  }
  Img.KnownCodeRanges.emplace_back(F, F + Code.size());
  Img.Symbols.push_back(Symbol::makeFunc(F, Code.size()));
  ExceptionFunction EH;
  EH.CodeRange = {F, F + Code.size()};
  EH.Kind = RuntimeFunctionKind::Primary;
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  for (size_t I = 0; I < FillerCount; ++I) {
    ExceptionFunction Leaf;
    Leaf.CodeRange = {Filler + I, Filler + I + 1};
    Leaf.Kind = RuntimeFunctionKind::Primary;
    Img.ExceptionMetadata.Functions.push_back(std::move(Leaf));
  }
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(F);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_EQ(Source.find("L_FFFFFFFFFFFFFFFF"), std::string::npos) << Source;
  // Every case keeps its return value.
  EXPECT_NE(Source.find("return 0x10;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return 0x37;"), std::string::npos) << Source;
}

TEST(COFFExceptionIR, SymbolHeavyImageStillResolvesAnImageRelativeJumpTable) {
  // ntoskrnl: 41k symbols.  Checking whether each target is a function
  // symbol was priced as a scan of every symbol, which exhausted the proof
  // budget for a 43-case switch.  It is a lookup in a sorted index.
  constexpr va_t Base = 0x140000000;
  constexpr va_t F = 0x140001000;
  constexpr va_t Table = 0x140003000;
  constexpr unsigned Cases = 40;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = Base;
  Img.Entry = F;
  std::vector<uint8_t> Code = {
      0x83, 0xf9, Cases - 1,                // cmp ecx, Cases-1
      0x0f, 0x87, 0,         0,    0, 0,    // ja default (patched)
      0x4c, 0x8d, 0x0d,      0,    0, 0, 0, // lea r9, [rip+Base-next] (patched)
      0x8b, 0xc1,                           // mov eax, ecx
      0x41, 0x8b, 0x84,      0x81, 0, 0, 0,
      0,                // mov eax, [r9+rax*4+Table] (patched)
      0x4c, 0x01, 0xc8, // add rax, r9
      0xff, 0xe0};      // jmp rax
  auto Put32 = [&](size_t At, uint32_t V) {
    for (unsigned I = 0; I < 4; ++I)
      Code[At + I] = static_cast<uint8_t>(V >> (8 * I));
  };
  Put32(12, static_cast<uint32_t>(Base - (F + 16)));
  Put32(22, static_cast<uint32_t>(Table - Base));
  std::vector<uint32_t> Entries;
  for (unsigned I = 0; I < Cases; ++I) {
    Entries.push_back(static_cast<uint32_t>(F + Code.size() - Base));
    Code.insert(Code.end(), {0xb8, static_cast<uint8_t>(0x10 + I), 0, 0, 0,
                             0xc3}); // mov eax, 10h+I; ret
  }
  const size_t Default = Code.size();
  Put32(5, static_cast<uint32_t>(Default - 9));
  Code.insert(Code.end(), {0x31, 0xc0, 0xc3}); // default: xor eax, eax; ret

  Segment Text;
  Text.Name = ".text";
  Text.VA = F;
  Text.Size = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  std::copy(Code.begin(), Code.end(), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Segment RData;
  RData.Name = ".rdata";
  RData.VA = Table;
  RData.Size = 0x1000;
  RData.Flags = SegmentFlags::Readable;
  RData.Data.assign(RData.Size, 0);
  for (unsigned I = 0; I < Cases; ++I)
    for (unsigned B = 0; B < 4; ++B)
      RData.Data[4 * I + B] = static_cast<uint8_t>(Entries[I] >> (8 * B));
  Img.Segments.push_back(std::move(RData));
  // Many one-byte functions elsewhere, each with its own unwind entry.
  constexpr va_t Filler = 0x140100000;
  constexpr size_t FillerCount = 16;
  Segment Pad;
  Pad.Name = ".text2";
  Pad.VA = Filler;
  Pad.Size = FillerCount;
  Pad.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Pad.Data.assign(Pad.Size, 0xc3);
  Img.Segments.push_back(std::move(Pad));
  for (const auto &[Name, VA, Size, Flags] :
       {std::tuple{".text", F, uint64_t{0x1000},
                   SegmentFlags::Readable | SegmentFlags::Executable},
        std::tuple{".rdata", Table, uint64_t{0x1000}, SegmentFlags::Readable},
        std::tuple{".text2", Filler, uint64_t{FillerCount},
                   SegmentFlags::Readable | SegmentFlags::Executable}}) {
    Section Sec;
    Sec.Name = Name;
    Sec.VA = VA;
    Sec.Size = Size;
    Sec.Flags = Flags;
    Img.Sections.push_back(std::move(Sec));
  }
  Img.KnownCodeRanges.emplace_back(F, F + Code.size());
  Img.Symbols.push_back(Symbol::makeFunc(F, Code.size()));
  ExceptionFunction EH;
  EH.CodeRange = {F, F + Code.size()};
  EH.Kind = RuntimeFunctionKind::Primary;
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  for (size_t I = 0; I < FillerCount; ++I) {
    ExceptionFunction Leaf;
    Leaf.CodeRange = {Filler + I, Filler + I + 1};
    Leaf.Kind = RuntimeFunctionKind::Primary;
    Img.ExceptionMetadata.Functions.push_back(std::move(Leaf));
  }
  for (size_t I = 0; I < 250000; ++I) {
    Symbol Label;
    Label.Addr = 0x140200000 + I;
    Label.IsFunc = true;
    Img.Symbols.push_back(std::move(Label));
  }
  Img.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  PipelineOptions One;
  One.EmitDumpOutput = false;
  One.OnlyFunctionEntries.insert(F);
  auto Result = Pipeline().run(Img, Ctx, One);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_EQ(Source.find("L_FFFFFFFFFFFFFFFF"), std::string::npos) << Source;
  // Every case keeps its return value.
  EXPECT_NE(Source.find("return 0x10;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return 0x37;"), std::string::npos) << Source;
}
