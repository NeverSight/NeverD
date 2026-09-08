//===- PipelineOutcomeTests.cpp - Pipeline completion contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PipelineHighIRDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

class PipelineTestPeer {
public:
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

} // namespace
} // namespace neverd
