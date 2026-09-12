//===- PipelineOutcomeTests.cpp - Pipeline completion contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PipelineHighIRDetail.h"
#include "PipelineLLVMDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace neverd {

class PipelineTestPeer {
public:
  using LLVMEmissionResult = Pipeline::LLVMEmissionResult;

  static LLVMEmissionResult runShards(
      const std::vector<MedFunc> &Functions, llvm::LLVMContext &Context,
      unsigned Shards, unsigned Threads, bool NoOpt,
      llvm::function_ref<LLVMEmissionResult(unsigned, llvm::LLVMContext &)>
          Emit) {
    return Pipeline::runLLVMShardPipeline(Functions, Context, Shards, Threads,
                                          NoOpt, Emit);
  }

  static LLVMEmissionResult emitShards(const std::vector<MedFunc> &Functions,
                                       llvm::LLVMContext &Context,
                                       const BinaryImage &Image,
                                       unsigned Threads) {
    return Pipeline::emitLLVMSharded(Functions, Context, Image.Arch, {}, Image,
                                     Image.Format, true, Threads);
  }

  static void buildHighIR(const BinaryImage &Image, PipelineResult &Result) {
    Pipeline().buildHighIR(Image, {}, Result);
  }

  static bool runLift(const BinaryImage &Image, llvm::LLVMContext &Context,
                      PipelineResult &Result) {
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.NoOpt = true;
    return Pipeline().runPatchLiftMode(Image, Context, Options, Result);
  }
};

namespace {

struct ScopedThreadCount {
  unsigned Previous = workerThreadOverride().load();
  explicit ScopedThreadCount(unsigned Count) { setWorkerThreadCount(Count); }
  ~ScopedThreadCount() { setWorkerThreadCount(Previous); }
};

MedFunc sourceFunction(va_t Entry, std::string Name) {
  MedFunc Function;
  Function.Entry = Entry;
  Function.Name = std::move(Name);
  Function.ReturnType = NdType::makeVoid();
  return Function;
}

HighFunc convertedIdentity(const MedFunc &Source) {
  HighFunc Function;
  Function.Entry = Source.Entry;
  Function.Name = Source.Name;
  return Function;
}

TEST(PipelineOutcome, UnsupportedArchitectureRetainsDecoderDiagnostic) {
  BinaryImage Image;
  Image.Arch = Arch::Unknown;
  llvm::LLVMContext Context;
  const PipelineResult Result = Pipeline().run(Image, Context);
  EXPECT_FALSE(Result.Success);
  EXPECT_NE(Result.Error.find("decoder"), std::string::npos);
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_EQ(Result.LlvmModule, nullptr);
}

TEST(PipelineOutcome, RejectedLLVMEmissionRetainsStageDiagnostic) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));

  PipelineResult Result;
  MedFunc Source = sourceFunction(0x1000, "unresolved_code_address");
  Source.ReturnType = NdType::makeInt(8);
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Source.Entry;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(
      MedVar::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress));
  Block.Ops.push_back(std::move(Return));
  Source.Blocks.push_back(std::move(Block));
  Result.MedFuncs.push_back(std::move(Source));
  LowFunc Low;
  Low.Entry = Result.MedFuncs.front().Entry;
  Low.Name = Result.MedFuncs.front().Name;
  Result.LowFuncs.push_back(std::move(Low));

  llvm::LLVMContext Context;
  EXPECT_FALSE(PipelineTestPeer::runLift(Image, Context, Result));
  EXPECT_EQ(Result.LlvmModule, nullptr);
  EXPECT_FALSE(Result.LLVMVerifierFailed);
  EXPECT_NE(Result.Error.find("LLVM emission"), std::string::npos);
}

TEST(PipelineOutcome, HighIRFailureWithdrawsStaleAndPartialOutput) {
  PipelineResult Result;
  Result.Success = true;
  Result.Error = "old diagnostic";
  Result.LowFuncs.resize(1);
  Result.LowFuncs.front().Name = "source_evidence";
  Result.MedFuncs.push_back(sourceFunction(0x1200, "source_function"));
  Result.HighFuncs.push_back(convertedIdentity(Result.MedFuncs.front()));
  bool SawEmptyOutput = false;
  const bool Completed = pipeline_detail::runHighIRStage(Result, [&] {
    SawEmptyOutput = Result.HighFuncs.empty();
    Result.HighFuncs.push_back(convertedIdentity(Result.MedFuncs.front()));
    throw std::runtime_error("source_function conversion detail");
  });
  EXPECT_FALSE(Completed);
  EXPECT_FALSE(Result.Success);
  EXPECT_TRUE(SawEmptyOutput);
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_EQ(Result.Error,
            "HighIR conversion failed: source_function conversion detail");
  ASSERT_EQ(Result.LowFuncs.size(), 1u);
  EXPECT_EQ(Result.LowFuncs.front().Name, "source_evidence");
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  EXPECT_EQ(Result.MedFuncs.front().Name, "source_function");
}

TEST(PipelineOutcome, HighIRContainsNonStandardExceptions) {
  PipelineResult Result;
  Result.Success = true;
  EXPECT_FALSE(pipeline_detail::runHighIRStage(Result, [] { throw 73; }));
  EXPECT_FALSE(Result.Success);
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_EQ(Result.Error, "HighIR conversion failed: unknown exception");
}

TEST(PipelineOutcome, HighIRWorkerFailuresAreContainedAfterJoining) {
  ScopedThreadCount Threads(2);
  PipelineResult Result;
  Result.MedFuncs = {sourceFunction(0, "first"), sourceFunction(16, "second")};
  std::atomic<unsigned> Finished{0};
  EXPECT_FALSE(pipeline_detail::runHighIRStage(Result, [&] {
    Result.HighFuncs.resize(2);
    parallelForEach(2, [&](auto Claim, size_t Total) {
      for (size_t I; (I = Claim()) < Total;) {
        struct Completion {
          std::atomic<unsigned> &Count;
          ~Completion() { Count.fetch_add(1); }
        } OnExit{Finished};
        if (I == 1)
          throw std::runtime_error("second function failed");
        Result.HighFuncs[I] = convertedIdentity(Result.MedFuncs[I]);
      }
    });
  }));
  EXPECT_EQ(Finished.load(), 2u);
  EXPECT_FALSE(Result.Success);
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_NE(Result.Error.find("second function failed"), std::string::npos);
}

TEST(PipelineOutcome, HighIRRejectsMissingAndMismatchedFunctionIdentities) {
  for (unsigned Fault = 0; Fault < 4; ++Fault) {
    SCOPED_TRACE(Fault);
    PipelineResult Result;
    Result.MedFuncs.push_back(sourceFunction(0, "entry_zero"));
    EXPECT_FALSE(pipeline_detail::runHighIRStage(Result, [&] {
      if (Fault == 0)
        return;
      Result.HighFuncs.push_back(convertedIdentity(Result.MedFuncs.front()));
      if (Fault == 1)
        Result.HighFuncs.front().Entry = 4;
      else if (Fault == 2)
        Result.HighFuncs.front().Name = "different_function";
      else
        Result.HighFuncs.front().Name.clear();
    }));
    EXPECT_FALSE(Result.Success);
    EXPECT_TRUE(Result.HighFuncs.empty());
    EXPECT_FALSE(Result.Error.empty());
  }
}

TEST(PipelineOutcome, HighIRAcceptsAnEmptySourceBatch) {
  PipelineResult Result;
  Result.HighFuncs.resize(1);
  EXPECT_TRUE(pipeline_detail::runHighIRStage(Result, [] {}));
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_TRUE(Result.Error.empty());
}

TEST(PipelineOutcome, RealHighIRConversionPreservesValidEmptyBodies) {
  PipelineResult Result;
  Result.MedFuncs.push_back(sourceFunction(0, "empty_body"));
  BinaryImage Image;
  Image.Arch = Arch::X64;
  ASSERT_TRUE(pipeline_detail::runHighIRStage(Result, [&] {
    PipelineTestPeer::buildHighIR(Image, Result);
  })) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  EXPECT_EQ(Result.HighFuncs.front().Entry, 0u);
  EXPECT_EQ(Result.HighFuncs.front().Name, "empty_body");
  EXPECT_TRUE(Result.HighFuncs.front().Body.empty());
}

TEST(PipelineOutcome,
     RealWeightedHighIRConversionPublishesEverySourceFunction) {
  for (unsigned Count : {1u, 3u}) {
    SCOPED_TRACE(Count);
    ScopedThreadCount Threads(Count);
    PipelineResult Result;
    for (unsigned I = 0; I < 3; ++I) {
      MedFunc Source = sourceFunction(I * 16, "completed_" + std::to_string(I));
      Source.OriginalSize = 8 + I;
      Source.DebugName = "debug_" + Source.Name;
      Source.SourceFile = "fixture.c";
      Source.SourceLine = 10 + I;
      MedBlock Block;
      Block.Id = 0;
      // The heaviest function is last in source order, so one-worker weighted
      // dispatch visits the batch in reverse while publication retains order.
      Block.Ops.resize(I + 1);
      for (MedOp &Op : Block.Ops)
        Op.Opcode = NdOp::NOP;
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Block.Ops.push_back(Return);
      Source.Blocks.push_back(std::move(Block));
      Result.MedFuncs.push_back(std::move(Source));
    }
    BinaryImage Image;
    Image.Arch = Arch::X64;
    ASSERT_TRUE(pipeline_detail::runHighIRStage(Result, [&] {
      PipelineTestPeer::buildHighIR(Image, Result);
    })) << Result.Error;
    ASSERT_EQ(Result.HighFuncs.size(), Result.MedFuncs.size());
    for (size_t I = 0; I < Result.HighFuncs.size(); ++I) {
      const HighFunc &High = Result.HighFuncs[I];
      const MedFunc &Med = Result.MedFuncs[I];
      EXPECT_EQ(High.Entry, Med.Entry);
      EXPECT_EQ(High.Name, Med.Name);
      EXPECT_EQ(High.OriginalSize, Med.OriginalSize);
      EXPECT_EQ(High.DebugName, Med.DebugName);
      EXPECT_EQ(High.SourceFile, Med.SourceFile);
      EXPECT_EQ(High.SourceLine, Med.SourceLine);
    }
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    CEmitterOptions Options;
    Options.TheArch = Image.Arch;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, Options));
    for (const MedFunc &Function : Result.MedFuncs)
      EXPECT_NE(Source.find("void " + Function.Name + "(void) {\n"),
                std::string::npos)
          << Source;
  }
}

std::unique_ptr<llvm::Module> shardModule(llvm::LLVMContext &Context,
                                          const std::string &Name,
                                          bool Valid = true) {
  auto Module = std::make_unique<llvm::Module>("shard", Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::GlobalValue::ExternalLinkage, Name, *Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  if (Valid)
    llvm::IRBuilder<>(Entry).CreateRetVoid();
  return Module;
}

TEST(PipelineOutcome, ShardExceptionsReturnAfterEveryWorkerFinishes) {
  for (unsigned Threads : {1u, 2u}) {
    SCOPED_TRACE(Threads);
    // An explicit executor count must not be replaced by the global setting.
    ScopedThreadCount GlobalThreads(8);
    llvm::LLVMContext Context;
    std::atomic<unsigned> Started{0}, Finished{0}, Active{0}, Peak{0};
    std::atomic<bool> WrongInlineThread{false};
    const std::thread::id Caller = std::this_thread::get_id();
    auto Result = PipelineTestPeer::runShards(
        {}, Context, 8, Threads, true,
        [&](unsigned Shard, llvm::LLVMContext &ShardContext)
            -> PipelineTestPeer::LLVMEmissionResult {
          Started.fetch_add(1);
          if (Threads == 1 && std::this_thread::get_id() != Caller)
            WrongInlineThread.store(true);
          const unsigned Now = Active.fetch_add(1) + 1;
          unsigned Previous = Peak.load();
          while (Previous < Now && !Peak.compare_exchange_weak(Previous, Now)) {
          }
          struct Completion {
            std::atomic<unsigned> &Active;
            std::atomic<unsigned> &Finished;
            ~Completion() {
              Active.fetch_sub(1);
              Finished.fetch_add(1);
            }
          } OnExit{Active, Finished};
          // A bounded delay encourages overlap without requiring another
          // worker to exist: native launch failure can fall back to inline.
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          if (Shard == 1)
            throw std::runtime_error("original emitter detail");
          if (Shard == 6)
            throw 73;
          PipelineTestPeer::LLVMEmissionResult Emission;
          Emission.Module =
              shardModule(ShardContext, "finished_" + std::to_string(Shard));
          Emission.UnhandledValueIntrinsics = 2;
          return Emission;
        });
    EXPECT_EQ(Started.load(), 8u);
    EXPECT_EQ(Finished.load(), 8u);
    EXPECT_EQ(Active.load(), 0u);
    EXPECT_LE(Peak.load(), Threads);
    EXPECT_FALSE(WrongInlineThread.load());
    EXPECT_EQ(Result.Module, nullptr);
    EXPECT_FALSE(Result.LLVMVerifierFailed);
    EXPECT_EQ(Result.UnhandledValueIntrinsics, 12u);
    const size_t Standard = Result.Error.find(
        "LLVM shard 1 emission failed: original emitter detail");
    const size_t NonStandard =
        Result.Error.find("LLVM shard 6 emission failed: unknown exception");
    ASSERT_NE(Standard, std::string::npos) << Result.Error;
    ASSERT_NE(NonStandard, std::string::npos) << Result.Error;
    EXPECT_LT(Standard, NonStandard);
  }
}

TEST(PipelineOutcome,
     ShardVerifierRetainsOriginalDiagnosticAndRejectsPartialOutput) {
  llvm::LLVMContext Context;
  std::atomic<unsigned> Finished{0};
  auto Result = PipelineTestPeer::runShards(
      {}, Context, 3, 2, true,
      [&](unsigned Shard, llvm::LLVMContext &ShardContext) {
        PipelineTestPeer::LLVMEmissionResult Emission;
        Emission.Module = shardModule(
            ShardContext, "function_" + std::to_string(Shard), Shard != 1);
        Emission.UnhandledValueIntrinsics = Shard + 1;
        Finished.fetch_add(1);
        return Emission;
      });
  EXPECT_EQ(Finished.load(), 3u);
  EXPECT_EQ(Result.Module, nullptr);
  EXPECT_TRUE(Result.LLVMVerifierFailed);
  EXPECT_EQ(Result.UnhandledValueIntrinsics, 6u);
  EXPECT_NE(Result.Error.find("LLVM shard 1 verification failed"),
            std::string::npos)
      << Result.Error;
  EXPECT_NE(Result.Error.find("does not have terminator"), std::string::npos)
      << Result.Error;
  EXPECT_NE(Result.Error.find("function_1"), std::string::npos) << Result.Error;
}

TEST(PipelineOutcome, MissingShardModuleHasAnOwnedDiagnostic) {
  llvm::LLVMContext Context;
  auto Result = PipelineTestPeer::runShards(
      {}, Context, 3, 2, true,
      [&](unsigned Shard, llvm::LLVMContext &ShardContext) {
        PipelineTestPeer::LLVMEmissionResult Emission;
        if (Shard != 1)
          Emission.Module =
              shardModule(ShardContext, "function_" + std::to_string(Shard));
        return Emission;
      });
  EXPECT_EQ(Result.Module, nullptr);
  EXPECT_FALSE(Result.LLVMVerifierFailed);
  EXPECT_EQ(Result.Error,
            "LLVM shard 1 emission failed: emitter returned no module");
}

TEST(PipelineOutcome,
     SuccessfulShardsLinkInCallerContextAndRestoreSourceOrder) {
  for (bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    llvm::LLVMContext Context;
    std::vector<MedFunc> Functions;
    for (unsigned I = 0; I < 5; ++I)
      Functions.push_back(
          sourceFunction(I * 16, "source_" + std::to_string(I)));
    auto Result = PipelineTestPeer::runShards(
        Functions, Context, 5, 2, NoOpt,
        [&](unsigned Shard, llvm::LLVMContext &ShardContext) {
          PipelineTestPeer::LLVMEmissionResult Emission;
          Emission.Module =
              shardModule(ShardContext, Functions[4 - Shard].Name);
          Emission.UnhandledValueIntrinsics = 1u << Shard;
          return Emission;
        });
    ASSERT_NE(Result.Module, nullptr) << Result.Error;
    EXPECT_EQ(&Result.Module->getContext(), &Context);
    EXPECT_FALSE(llvm::verifyModule(*Result.Module, &llvm::errs()));
    EXPECT_EQ(Result.UnhandledValueIntrinsics, 31u);
    EXPECT_FALSE(Result.LLVMVerifierFailed);
    EXPECT_TRUE(Result.Error.empty());
    unsigned Index = 0;
    for (const auto &Function : *Result.Module) {
      ASSERT_LT(Index, Functions.size());
      EXPECT_FALSE(Function.isDeclaration());
      EXPECT_EQ(Function.getName(), Functions[Index++].Name);
    }
    EXPECT_EQ(Index, Functions.size());
  }
}

TEST(PipelineOutcome, RealShardedEmissionDefinesEverySourceFunction) {
  llvm::LLVMContext Context;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  std::vector<MedFunc> Functions;
  for (unsigned I = 0; I < 8; ++I) {
    MedFunc Function =
        sourceFunction(0x1000 + I * 16, "source_" + std::to_string(I));
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    Functions.push_back(std::move(Function));
  }
  auto Result = PipelineTestPeer::emitShards(Functions, Context, Image, 2);
  ASSERT_NE(Result.Module, nullptr) << Result.Error;
  EXPECT_TRUE(Result.Error.empty());
  EXPECT_FALSE(Result.LLVMVerifierFailed);
  EXPECT_FALSE(llvm::verifyModule(*Result.Module, &llvm::errs()));
  size_t Index = 0;
  for (const auto &Function : *Result.Module) {
    if (Function.isDeclaration())
      continue;
    ASSERT_LT(Index, Functions.size());
    EXPECT_EQ(Function.getName(), Functions[Index++].Name);
  }
  EXPECT_EQ(Index, Functions.size());
}

TEST(PipelineOutcome, LiftPreservesTheFailedShardDiagnostic) {
  ScopedThreadCount Threads(2);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));
  PipelineResult Result;
  for (unsigned I = 0; I < 8; ++I) {
    MedFunc Function =
        sourceFunction(0x1000 + I * 16, "source_" + std::to_string(I));
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    if (I == 1) {
      Function.ReturnType = NdType::makeInt(8);
      Return.addInput(
          MedVar::makeConst(0x10e0, 8, ConstantAddressProvenance::CodeAddress));
    }
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    LowFunc Low;
    Low.Entry = Function.Entry;
    Low.Name = Function.Name;
    Result.LowFuncs.push_back(std::move(Low));
    Result.MedFuncs.push_back(std::move(Function));
  }
  llvm::LLVMContext Context;
  EXPECT_FALSE(PipelineTestPeer::runLift(Image, Context, Result));
  EXPECT_EQ(Result.LlvmModule, nullptr);
  EXPECT_FALSE(Result.LLVMVerifierFailed);
  EXPECT_NE(Result.Error.find("LLVM shard "), std::string::npos)
      << Result.Error;
  EXPECT_NE(Result.Error.find("emission failed: emitter returned no module"),
            std::string::npos)
      << Result.Error;
}

class PipelineStageTrace : public ::testing::Test {
protected:
  void SetUp() override {
    PreviousDeathStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
  void TearDown() override {
    GTEST_FLAG_SET(death_test_style, PreviousDeathStyle);
  }

private:
  std::string PreviousDeathStyle;
};

class ScopedPipelineTraceEnvironment {
public:
  ScopedPipelineTraceEnvironment() {
    if (const char *Value = std::getenv("NEVERD_NATIVE_PHASES")) {
      HadValue = true;
      Previous = Value;
    }
  }
  ~ScopedPipelineTraceEnvironment() {
    EXPECT_EQ(set(HadValue ? Previous.c_str() : nullptr), 0);
  }
  int set(const char *Value) const {
#ifdef _WIN32
    return _putenv_s("NEVERD_NATIVE_PHASES", Value ? Value : "");
#else
    return Value ? setenv("NEVERD_NATIVE_PHASES", Value, 1)
                 : unsetenv("NEVERD_NATIVE_PHASES");
#endif
  }

private:
  bool HadValue = false;
  std::string Previous;
};

BinaryImage stageTraceImage(Arch Architecture) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = "__text";
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // Owned mov-return bodies; no fixture compiler or external image is used.
  Text.Data =
      Architecture == Arch::AArch64
          ? std::vector<uint8_t>{0x40, 0x05, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6}
          : std::vector<uint8_t>{0xb8, 0x2a, 0, 0, 0, 0xc3};
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Symbols.push_back({"owned_trace_return", Image.Entry, Text.Size, true});
  Image.Segments.push_back(std::move(Text));
  return Image;
}

PipelineOptions stageTraceOptions(Arch Architecture) {
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.ReturnType = NdType::makeInt(4);
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error)) << Error;
  Options.SourceTypeHints.emplace(0x1000, std::move(Hint));
  return Options;
}

struct CapturedPipelineStage {
  PipelineResult Result;
  std::string Diagnostic;
  int ErrorNumber;
};

CapturedPipelineStage capturePipelineStage(const BinaryImage &Image,
                                           llvm::LLVMContext &Context,
                                           const PipelineOptions &Options) {
  testing::internal::CaptureStderr();
  errno = E2BIG;
  auto Result = Pipeline().run(Image, Context, Options);
  const int ErrorNumber = errno;
  auto Diagnostic = testing::internal::GetCapturedStderr();
  return {std::move(Result), std::move(Diagnostic), ErrorNumber};
}

void expectStageRecords(const std::string &Diagnostic, unsigned Invocation,
                        std::initializer_list<const char *> Stages,
                        const char *FinalEvent = "completed") {
  llvm::StringRef Remaining(Diagnostic);
  size_t StageIndex = 0;
  for (const char *Stage : Stages) {
    for (bool Begin : {true, false}) {
      const auto LineAndRest = Remaining.split('\n');
      const llvm::StringRef Line = LineAndRest.first;
      ASSERT_LT(Line.size(), Remaining.size()) << Diagnostic;
      ASSERT_LE(Line.size() + 1, 192u);
      llvm::StringRef Fields = Line;
      const std::string Prefix =
          "[neverd-pipeline-stage] invocation=" + std::to_string(Invocation) +
          " stage=" + Stage + " event=" +
          (Begin                             ? "begin"
           : StageIndex + 1 == Stages.size() ? FinalEvent
                                             : "completed") +
          " elapsed_ms=";
      ASSERT_TRUE(Fields.consume_front(Prefix)) << Line.str();
      ASSERT_FALSE(Fields.empty());
      EXPECT_TRUE(std::all_of(Fields.begin(), Fields.end(), [](char C) {
        return C >= '0' && C <= '9';
      })) << Line.str();
      uint64_t Elapsed = 0;
      ASSERT_FALSE(Fields.getAsInteger(10, Elapsed)) << Line.str();
      if (Begin)
        EXPECT_EQ(Fields, "0");
      Remaining = LineAndRest.second;
    }
    ++StageIndex;
  }
  EXPECT_TRUE(Remaining.empty()) << Remaining.str();
  // Full line consumption also forbids disclosed image names and extra fields.
}

void expectOwnedStageResult(const BinaryImage &Image,
                            const PipelineResult &Result, std::string &Source) {
  ASSERT_TRUE(Result.Success) << Result.Error;
  EXPECT_TRUE(Result.Error.empty());
  EXPECT_EQ(Result.SourceImage, &Image);
  ASSERT_EQ(Result.LowFuncs.size(), 1u);
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  EXPECT_EQ(Result.LowFuncs.front().Entry, 0x1000u);
  EXPECT_EQ(Result.MedFuncs.front().Entry, 0x1000u);
  EXPECT_EQ(Result.HighFuncs.front().Entry, 0x1000u);
  EXPECT_EQ(Result.LowFuncs.front().Name, "owned_trace_return");
  EXPECT_EQ(Result.MedFuncs.front().Name, "owned_trace_return");
  EXPECT_EQ(Result.HighFuncs.front().Name, "owned_trace_return");
  ASSERT_EQ(Result.FunctionAudits.size(), 1u);
  const auto &Audit = Result.FunctionAudits.front();
  EXPECT_EQ(Audit.Entry, 0x1000u);
  EXPECT_EQ(Audit.Name, "owned_trace_return");
  EXPECT_EQ(Audit.Disposition, PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(Audit.DecodedInstructions, 2u);
  EXPECT_EQ(Audit.LiftedInstructions, 2u);
  EXPECT_TRUE(Audit.DecodeFailures.empty());
  EXPECT_TRUE(Audit.UnsupportedInstructions.empty());
  EXPECT_TRUE(Audit.TruncatedPaths.empty());
  EXPECT_TRUE(Audit.HasLowIR);
  EXPECT_TRUE(Audit.HasMedIR);
  EXPECT_TRUE(Audit.MedIRVerified);
  EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
  EXPECT_EQ(Result.BackendUnhandledValueIntrinsics, 0u);
  EXPECT_FALSE(Result.LLVMVerifierFailed);
  EXPECT_EQ(Result.LlvmModule, nullptr);
  EXPECT_EQ(Result.EVM, nullptr);
  EXPECT_EQ(Result.SBF, nullptr);
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, Options));
  EXPECT_NE(Source.find("owned_trace_return"), std::string::npos);
  EXPECT_NE(Source.find("42"), std::string::npos);
}

void exerciseOwnedPipelineStages() {
  ScopedPipelineTraceEnvironment Environment;
  ScopedThreadCount Threads(1);
  unsigned Invocation = 0;
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(getArchName(Architecture));
    const auto Image = stageTraceImage(Architecture);
    const auto Options = stageTraceOptions(Architecture);
    llvm::LLVMContext BeforeContext;
    ASSERT_EQ(Environment.set(nullptr), 0);
    const auto Before = capturePipelineStage(Image, BeforeContext, Options);
    EXPECT_TRUE(Before.Diagnostic.empty()) << Before.Diagnostic;
    std::string BeforeSource;
    expectOwnedStageResult(Image, Before.Result, BeforeSource);
    llvm::LLVMContext AfterContext;
    ASSERT_EQ(Environment.set("1"), 0);
    const auto After = capturePipelineStage(Image, AfterContext, Options);
    expectStageRecords(After.Diagnostic, ++Invocation,
                       {"decoder", "function_detection", "low_ir",
                        "candidate_cleanup", "med_ir", "noreturn_verify",
                        "high_ir"});
    std::string AfterSource;
    expectOwnedStageResult(Image, After.Result, AfterSource);
    EXPECT_EQ(AfterSource, BeforeSource);
    EXPECT_EQ(After.ErrorNumber, Before.ErrorNumber);
  }
}

TEST_F(PipelineStageTrace, PreservesOwnedNativeResults) {
  ASSERT_EXIT(
      {
        exerciseOwnedPipelineStages();
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

void exerciseExactPipelineTraceEnvironment() {
  ScopedPipelineTraceEnvironment Environment;
  ScopedThreadCount Threads(1);
  const auto Image = stageTraceImage(Arch::X64);
  const auto Options = stageTraceOptions(Image.Arch);
  const char *Values[] = {nullptr, "",   "0",  "01", "11",
                          "true",  "1 ", " 1", "1\n"};
  for (const char *Value : Values) {
    SCOPED_TRACE(Value ? Value : "unset");
    ASSERT_EQ(Environment.set(Value), 0);
    llvm::LLVMContext Context;
    const auto Captured = capturePipelineStage(Image, Context, Options);
    EXPECT_TRUE(Captured.Diagnostic.empty()) << Captured.Diagnostic;
    std::string Source;
    expectOwnedStageResult(Image, Captured.Result, Source);
  }
  ASSERT_EQ(Environment.set("1"), 0);
  llvm::LLVMContext Context;
  const auto Captured = capturePipelineStage(Image, Context, Options);
  expectStageRecords(Captured.Diagnostic, 1,
                     {"decoder", "function_detection", "low_ir",
                      "candidate_cleanup", "med_ir", "noreturn_verify",
                      "high_ir"});
  std::string Source;
  expectOwnedStageResult(Image, Captured.Result, Source);
}

TEST_F(PipelineStageTrace, RequiresExactEnvironmentValue) {
  ASSERT_EXIT(
      {
        exerciseExactPipelineTraceEnvironment();
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

void exercisePipelineStageFailureAndEarlyReturn() {
  ScopedPipelineTraceEnvironment Environment;
  ScopedThreadCount Threads(1);
  BinaryImage Unknown;
  Unknown.Arch = Arch::Unknown;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext BeforeContext;
  ASSERT_EQ(Environment.set(nullptr), 0);
  const auto Before = capturePipelineStage(Unknown, BeforeContext, Options);
  ASSERT_FALSE(Before.Result.Success);
  EXPECT_EQ(Before.Result.Error,
            "failed to initialize decoder for architecture unknown");
  ASSERT_FALSE(Before.Diagnostic.empty());
  llvm::LLVMContext AfterContext;
  ASSERT_EQ(Environment.set("1"), 0);
  const auto After = capturePipelineStage(Unknown, AfterContext, Options);
  EXPECT_FALSE(After.Result.Success);
  EXPECT_EQ(After.Result.Error, Before.Result.Error);
  EXPECT_TRUE(After.Result.LowFuncs.empty());
  EXPECT_TRUE(After.Result.HighFuncs.empty());
  EXPECT_EQ(After.Result.LlvmModule, nullptr);
  std::string Trace = After.Diagnostic;
  const size_t OriginalError = Trace.find(Before.Diagnostic);
  ASSERT_NE(OriginalError, std::string::npos) << Trace;
  Trace.erase(OriginalError, Before.Diagnostic.size());
  expectStageRecords(Trace, 1, {"decoder"}, "failed");

  unsigned Invocation = 1;
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    const auto Image = stageTraceImage(Architecture);
    auto Intermediate = stageTraceOptions(Architecture);
    Intermediate.DumpLow = true;
    llvm::LLVMContext Context;
    const auto Captured = capturePipelineStage(Image, Context, Intermediate);
    ASSERT_TRUE(Captured.Result.Success) << Captured.Result.Error;
    ASSERT_EQ(Captured.Result.LowFuncs.size(), 1u);
    ASSERT_EQ(Captured.Result.MedFuncs.size(), 1u);
    EXPECT_TRUE(Captured.Result.HighFuncs.empty());
    EXPECT_EQ(Captured.Result.LlvmModule, nullptr);
    expectStageRecords(Captured.Diagnostic, ++Invocation,
                       {"decoder", "function_detection", "low_ir",
                        "candidate_cleanup", "med_ir", "noreturn_verify"});
  }
}

TEST_F(PipelineStageTrace, KeepsFailuresAndIntermediateReturns) {
  ASSERT_EXIT(
      {
        exercisePipelineStageFailureAndEarlyReturn();
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

void exercisePipelineTraceAdmission() {
  ScopedPipelineTraceEnvironment Environment;
  ScopedThreadCount Threads(1);
  ASSERT_EQ(Environment.set("1"), 0);
  size_t TotalDiagnosticBytes = 0;
  std::string ReferenceSource;
  const auto Image = stageTraceImage(Arch::AArch64);
  const auto Options = stageTraceOptions(Image.Arch);
  for (unsigned Run = 1; Run <= 35; ++Run) {
    SCOPED_TRACE(Run);
    llvm::LLVMContext Context;
    const auto Captured = capturePipelineStage(Image, Context, Options);
    std::string Source;
    expectOwnedStageResult(Image, Captured.Result, Source);
    if (Run == 1)
      ReferenceSource = Source;
    else
      EXPECT_EQ(Source, ReferenceSource);
    if (Run <= 32)
      expectStageRecords(Captured.Diagnostic, Run,
                         {"decoder", "function_detection", "low_ir",
                          "candidate_cleanup", "med_ir", "noreturn_verify",
                          "high_ir"});
    else
      EXPECT_TRUE(Captured.Diagnostic.empty()) << Captured.Diagnostic;
    TotalDiagnosticBytes += Captured.Diagnostic.size();
  }
  EXPECT_LE(TotalDiagnosticBytes, 32u * 14u * 192u);
  EXPECT_FALSE(ReferenceSource.empty());
}

TEST_F(PipelineStageTrace, AdmissionDoesNotLimitAnalysis) {
  ASSERT_EXIT(
      {
        exercisePipelineTraceAdmission();
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

void exercisePipelineTraceLift() {
  ScopedPipelineTraceEnvironment Environment;
  ScopedThreadCount Threads(1);
  ASSERT_EQ(Environment.set("1"), 0);
  unsigned Invocation = 0;
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    const auto Image = stageTraceImage(Architecture);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.NoOpt = true;
    Options.LiftMode = true;
    llvm::LLVMContext Context;
    const auto Captured = capturePipelineStage(Image, Context, Options);
    ASSERT_TRUE(Captured.Result.Success) << Captured.Result.Error;
    ASSERT_NE(Captured.Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Captured.Result.LlvmModule));
    const auto *Function =
        Captured.Result.LlvmModule->getFunction("owned_trace_return");
    ASSERT_NE(Function, nullptr);
    EXPECT_FALSE(Function->isDeclaration());
    EXPECT_TRUE(Captured.Result.HighFuncs.empty());
    ASSERT_EQ(Captured.Result.FunctionAudits.size(), 1u);
    EXPECT_TRUE(Captured.Result.FunctionAudits.front().HasLLVMDefinition);
    expectStageRecords(Captured.Diagnostic, ++Invocation,
                       {"decoder", "function_detection", "low_ir",
                        "candidate_cleanup", "med_ir", "noreturn_verify",
                        "llvm_emission"});
  }
}

TEST_F(PipelineStageTrace, KeepsLiftBranchAndItsNativeDefinition) {
  ASSERT_EXIT(
      {
        exercisePipelineTraceLift();
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

#ifndef _WIN32
void exercisePipelineTraceSink(bool Full) {
  ScopedPipelineTraceEnvironment Environment;
  ScopedThreadCount Threads(1);
  const auto Image = stageTraceImage(Arch::X64);
  const auto Options = stageTraceOptions(Image.Arch);
  ASSERT_EQ(Environment.set(nullptr), 0);
  llvm::LLVMContext BeforeContext;
  const auto Before = capturePipelineStage(Image, BeforeContext, Options);
  std::string BeforeSource;
  expectOwnedStageResult(Image, Before.Result, BeforeSource);
  ASSERT_FALSE(llvm::errs().has_error());
  ASSERT_EQ(Environment.set("1"), 0);
  const int Saved = dup(STDERR_FILENO);
  ASSERT_GE(Saved, 0);
  if (Full) {
    const int Sink = open("/dev/full", O_WRONLY);
    ASSERT_GE(Sink, 0);
    ASSERT_GE(dup2(Sink, STDERR_FILENO), 0);
    close(Sink);
  } else {
    ASSERT_EQ(close(STDERR_FILENO), 0);
  }
  errno = 0;
  const bool ActualWriteFailure =
      ::write(STDERR_FILENO, "x", 1) == -1 && errno == (Full ? ENOSPC : EBADF);
  llvm::LLVMContext AfterContext;
  errno = E2BIG;
  const auto Result = Pipeline().run(Image, AfterContext, Options);
  const int AfterErrno = errno;
  const int Restored = dup2(Saved, STDERR_FILENO);
  close(Saved);
  ASSERT_GE(Restored, 0);
  EXPECT_TRUE(ActualWriteFailure);
  EXPECT_FALSE(llvm::errs().has_error());
  EXPECT_EQ(AfterErrno, Before.ErrorNumber);
  std::string AfterSource;
  expectOwnedStageResult(Image, Result, AfterSource);
  EXPECT_EQ(AfterSource, BeforeSource);
  // Do not clear the global stream: that would hide diagnostic contamination.
  llvm::errs() << "pipeline-sink-restored\n";
  llvm::errs().flush();
  EXPECT_FALSE(llvm::errs().has_error());
}

TEST_F(PipelineStageTrace, ClosedStderrPreservesNativeResult) {
  ASSERT_EXIT(
      {
        exercisePipelineTraceSink(false);
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "^pipeline-sink-restored\n$");
}
#endif

#ifdef __linux__
TEST_F(PipelineStageTrace, FullStderrPreservesNativeResult) {
  ASSERT_EXIT(
      {
        exercisePipelineTraceSink(true);
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "^pipeline-sink-restored\n$");
}
#endif

} // namespace
} // namespace neverd
