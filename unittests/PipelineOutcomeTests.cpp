//===- PipelineOutcomeTests.cpp - Pipeline completion contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PipelineHighIRDetail.h"
#include "PipelineLLVMDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/llvm/LLVMCallContract.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/Parallel.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <set>
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
                                       unsigned Threads,
                                       bool OmitRefusedBodies = false) {
    return Pipeline::emitLLVMSharded(
        Functions, Context, Image.Arch, {}, Image, Image.Format, true, Threads,
        false, {}, nullptr, {}, nullptr, false, {}, OmitRefusedBodies);
  }

  static void buildHighIR(const BinaryImage &Image, PipelineResult &Result) {
    Pipeline().buildHighIR(Image, {}, Result);
  }

  static void buildLowIR(const BinaryImage &Image, PipelineResult &Result,
                         const std::vector<va_t> &Entries,
                         PipelineAnalysisCache *Cache = nullptr) {
    PipelineOptions Options;
    Options.AnalysisCache = Cache;
    Options.OnlyFunctionEntries.insert(Entries.begin(), Entries.end());
    std::vector<std::pair<va_t, std::string>> Candidates;
    for (va_t Entry : Entries)
      Candidates.emplace_back(Entry, Image.getFunctionNameAt(Entry));
    Pipeline().buildLowIR(Image, Candidates, Options, nullptr, Result);
  }

  static std::shared_ptr<const void>
  analysisFacts(const PipelineAnalysisCache &Cache) {
    return Cache.Facts;
  }

  static std::shared_ptr<const void>
  selectionFacts(const PipelineAnalysisCache &Cache) {
    return Cache.SelectionFacts;
  }

  static std::shared_ptr<const void>
  symbolInventory(const PipelineAnalysisCache &Cache) {
    return Cache.Inventory;
  }

  static void buildLowIR(const BinaryImage &Image, PipelineResult &Result) {
    buildLowIR(Image, Result, {Image.Entry});
  }

  static bool runLift(const BinaryImage &Image, llvm::LLVMContext &Context,
                      PipelineResult &Result) {
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.NoOpt = true;
    return Pipeline().runPatchLiftMode(Image, Context, Options, Result,
                                       /*Dbg=*/nullptr);
  }

  static bool runLift(const BinaryImage &Image, llvm::LLVMContext &Context,
                      const PipelineOptions &Options, PipelineResult &Result) {
    return Pipeline().runPatchLiftMode(Image, Context, Options, Result,
                                       /*Dbg=*/nullptr);
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

BinaryImage selectionCacheImage(bool Floating) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = Text.FileSz = 0x2000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const auto Wrapper = [&](va_t From, va_t To) {
    const uint32_t Offset = static_cast<uint32_t>(To - From - 5);
    Text.Data[From - Text.VA] = 0xe8;
    for (unsigned I = 0; I != 4; ++I)
      Text.Data[From - Text.VA + I + 1] = Offset >> (I * 8);
    Text.Data[From - Text.VA + 5] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(From, 6));
  };
  Wrapper(0x1000, 0x1800);
  Wrapper(0x1100, 0x1800);
  Wrapper(0x1800, 0x2000);
  // The two callers share a forwarding chain, which must keep the physical
  // floating return or incoming integer argument across navigation.
  Text.Data[0x2000 - Text.VA] = Floating ? 0xd9 : 0x89;
  Text.Data[0x2001 - Text.VA] = Floating ? 0xe8 : 0xf8;
  Text.Data[0x2002 - Text.VA] = 0xc3;
  Image.Symbols.push_back(Symbol::makeFunc(0x2000, 3));
  Text.Data[0x1200 - Text.VA] = 0xc3; // Valid, initially unnamed entry.
  Image.Segments.push_back(std::move(Text));
  return Image;
}

void expectSameSelection(const PipelineResult &Kept,
                         const PipelineResult &Cold) {
  std::string KeptText, ColdText;
  llvm::raw_string_ostream KeptStream(KeptText), ColdStream(ColdText);
  Pipeline::dumpLowIR(Kept.LowFuncs, KeptStream);
  Pipeline::dumpLowIR(Cold.LowFuncs, ColdStream);
  EXPECT_EQ(KeptText, ColdText);
  ASSERT_EQ(Kept.LowFuncs.size(), Cold.LowFuncs.size());
  for (size_t I = 0; I != Kept.LowFuncs.size(); ++I)
    EXPECT_EQ(Kept.LowFuncs[I].hasCompleteInstructionLift(),
              Cold.LowFuncs[I].hasCompleteInstructionLift());
  EXPECT_EQ(Kept.CallMayWriteGPRs, Cold.CallMayWriteGPRs);
  EXPECT_EQ(Kept.CallEntryReadGPRs, Cold.CallEntryReadGPRs);
  EXPECT_EQ(Kept.CallEntryStackArgs, Cold.CallEntryStackArgs);
  EXPECT_EQ(Kept.CallVariadicFrom, Cold.CallVariadicFrom);
  EXPECT_EQ(Kept.CallDispatchThunks, Cold.CallDispatchThunks);
  EXPECT_EQ(Kept.CallFloatReturns, Cold.CallFloatReturns);
}

TEST(PipelineOutcome, AnalysisCachePreservesFloatingAndIntegerCallChains) {
  for (unsigned Count : {1u, 4u}) {
    ScopedThreadCount Threads(Count);
    for (bool Floating : {false, true})
      for (bool Owned : {false, true}) {
        SCOPED_TRACE(Count);
        SCOPED_TRACE(Floating);
        SCOPED_TRACE(Owned);
        BinaryImage Image = selectionCacheImage(Floating);
        Image.X87ContextOverride = Owned;
        PipelineAnalysisCache Cache;
        PipelineResult First;
        PipelineTestPeer::buildLowIR(Image, First, {0x1000}, &Cache);
        const auto Facts = PipelineTestPeer::analysisFacts(Cache);
        ASSERT_NE(Facts, nullptr);
        for (va_t Entry : {0x1100, 0x1000}) {
          PipelineResult Kept, Cold;
          PipelineTestPeer::buildLowIR(Image, Kept, {Entry}, &Cache);
          PipelineTestPeer::buildLowIR(Image, Cold, {Entry});
          EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), Facts);
          expectSameSelection(Kept, Cold);
          ASSERT_EQ(Kept.LowFuncs.size(), 1u);
          EXPECT_EQ(Kept.LowFuncs[0].Entry, Entry);
          bool FoundCall = false;
          for (const auto &Block : Kept.LowFuncs[0].Blocks)
            for (const auto &Op : Block.Ops)
              if (Op.Opcode == NdOp::CALL) {
                FoundCall = true;
                // A typed long double returns in st0; an owned context
                // transports it instead.
                EXPECT_EQ(Op.Output.Size, Floating && !Owned ? 10u : 8u);
                if (Floating && Owned)
                  EXPECT_EQ(Op.Output, NdVar::reg(x86reg::RAX, 8));
              }
          EXPECT_TRUE(FoundCall);
          if (Floating && Owned) {
            unsigned Captures = 0, Exports = 0, Imports = 0;
            for (const auto &B : Kept.LowFuncs[0].Blocks)
              for (const auto &Op : B.Ops)
                if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                    Op.Inputs[0].isConst()) {
                  Captures += Op.Inputs[0].Offset ==
                              unsigned(Intrinsic::X87ContextBegin);
                  Exports += Op.Inputs[0].Offset ==
                             unsigned(Intrinsic::X87ContextExport);
                  Imports += Op.Inputs[0].Offset ==
                             unsigned(Intrinsic::X87ContextRefresh);
                }
            // The wrapper returns its call's state directly: one export
            // before the call, no refresh/export pair before its RET.
            EXPECT_EQ(Captures, 1u);
            EXPECT_EQ(Exports, 1u);
            EXPECT_EQ(Imports, 0u);
          }
        }
      }
  }
}

BinaryImage aarch64SelectionCacheImage() {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = Text.FileSz = 0x2000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  const auto Word = [&](va_t At, uint32_t Insn) {
    for (unsigned I = 0; I != 4; ++I)
      Text.Data[At - Text.VA + I] = static_cast<uint8_t>(Insn >> (I * 8));
  };
  // stp x29, x30, [sp, #-16]!; bl To; ldp x29, x30, [sp], #16; ret
  const auto Wrapper = [&](va_t From, va_t To) {
    Word(From, 0xa9bf7bfd);
    Word(From + 4, 0x94000000 | static_cast<uint32_t>(((To - From - 4) >> 2) &
                                                      0x3ffffff));
    Word(From + 8, 0xa8c17bfd);
    Word(From + 12, 0xd65f03c0);
    Image.Symbols.push_back(Symbol::makeFunc(From, 16));
  };
  Wrapper(0x1000, 0x1800);
  Wrapper(0x1100, 0x1800);
  Wrapper(0x1800, 0x2000);
  // The callers share a chain to a leaf that returns its second argument:
  // mov x0, x1; ret.
  Word(0x2000, 0xaa0103e0);
  Word(0x2004, 0xd65f03c0);
  Image.Symbols.push_back(Symbol::makeFunc(0x2000, 8));
  Image.Segments.push_back(std::move(Text));
  return Image;
}

TEST(PipelineOutcome, AnalysisCacheServesAArch64Selections) {
  // AArch64 selections share the analysis cache as x64 ones do: the facts
  // of the symbol inventory serve every symbol start, and each run reads
  // what a cold run does.
  const BinaryImage Image = aarch64SelectionCacheImage();
  PipelineAnalysisCache Cache;
  PipelineResult First;
  PipelineTestPeer::buildLowIR(Image, First, {0x1000}, &Cache);
  const auto Facts = PipelineTestPeer::analysisFacts(Cache);
  const auto Inventory = PipelineTestPeer::symbolInventory(Cache);
  ASSERT_NE(Facts, nullptr);
  ASSERT_NE(Inventory, nullptr);
  for (const va_t Entry :
       {va_t{0x1100}, va_t{0x1800}, va_t{0x2000}, va_t{0x1000}}) {
    SCOPED_TRACE(Entry);
    PipelineResult Kept, Cold;
    PipelineTestPeer::buildLowIR(Image, Kept, {Entry}, &Cache);
    PipelineTestPeer::buildLowIR(Image, Cold, {Entry});
    EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), Facts);
    EXPECT_EQ(PipelineTestPeer::symbolInventory(Cache), Inventory);
    expectSameSelection(Kept, Cold);
    EXPECT_EQ(Kept.CallEntryArgumentCounts, Cold.CallEntryArgumentCounts);
    EXPECT_EQ(Kept.CallArgumentSources, Cold.CallArgumentSources);
  }
}

BinaryImage arm32SelectionCacheImage() {
  BinaryImage Image;
  Image.Arch = Arch::ARM;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = Text.FileSz = 0x2000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  const auto Word = [&](va_t At, uint32_t Insn) {
    for (unsigned I = 0; I != 4; ++I)
      Text.Data[At - Text.VA + I] = static_cast<uint8_t>(Insn >> (I * 8));
  };
  // push {r4, lr}; bl To; pop {r4, pc}
  const auto Wrapper = [&](va_t From, va_t To) {
    Word(From, 0xe92d4010);
    Word(From + 4, 0xeb000000 | static_cast<uint32_t>(((To - From - 12) >> 2) &
                                                      0xffffff));
    Word(From + 8, 0xe8bd8010);
    Image.Symbols.push_back(Symbol::makeFunc(From, 12));
  };
  Wrapper(0x1000, 0x1800);
  Wrapper(0x1100, 0x1800);
  Wrapper(0x1800, 0x2000);
  Word(0x2000, 0xe1a00001); // mov r0, r1
  Word(0x2004, 0xe12fff1e); // bx lr
  Image.Symbols.push_back(Symbol::makeFunc(0x2000, 8));
  Image.Segments.push_back(std::move(Text));
  return Image;
}

TEST(PipelineOutcome, AnalysisCacheBindsExactEntryInventories) {
  BinaryImage Image = selectionCacheImage(true);
  PipelineAnalysisCache Cache;
  // A run whose exact inventory its kind of facts was not built for builds
  // new ones, the other kind stays, and either way it equals a cold run.
  auto Check = [&](va_t Entry, bool AddsEntry, bool Fresh) {
    const auto Facts = PipelineTestPeer::analysisFacts(Cache);
    const auto Selection = PipelineTestPeer::selectionFacts(Cache);
    PipelineResult Kept, Cold;
    PipelineTestPeer::buildLowIR(Image, Kept, {Entry}, &Cache);
    PipelineTestPeer::buildLowIR(Image, Cold, {Entry});
    const auto NowFacts = PipelineTestPeer::analysisFacts(Cache);
    const auto NowSelection = PipelineTestPeer::selectionFacts(Cache);
    EXPECT_EQ(AddsEntry ? NowSelection != Selection : NowFacts != Facts, Fresh);
    EXPECT_EQ(AddsEntry ? NowFacts : NowSelection,
              AddsEntry ? Facts : Selection);
    expectSameSelection(Kept, Cold);
  };
  Check(0x1000, false, true);
  // A selected entry that starts no symbol adds to the known boundaries.
  Check(0x1200, true, true);
  Check(0x1000, false, false);
  Check(0x1200, true, false);
  Image.VerifiedFunctionEntries.insert(0x1200);
  Check(0x1000, false, true);
  Check(0x1200, true, true);
  // Equal cardinality is insufficient, and removing an entry matters too.
  Image.VerifiedFunctionEntries = {0x1300};
  Check(0x1000, false, true);
  Image.VerifiedFunctionEntries.clear();
  Check(0x1000, false, true);
}

TEST(PipelineOutcome, AnalysisCacheKeepsUnnamedCallersIndependent) {
  for (unsigned Count : {1u, 4u}) {
    ScopedThreadCount Threads(Count);
    for (Arch TheArch : {Arch::X64, Arch::AArch64, Arch::ARM}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(static_cast<unsigned>(TheArch));
      BinaryImage Image = TheArch == Arch::AArch64
                              ? aarch64SelectionCacheImage()
                          : TheArch == Arch::ARM ? arm32SelectionCacheImage()
                                                 : selectionCacheImage(true);
      // Neither caller has a symbol. Each adds a different boundary to the
      // callee analysis, while the relocation/owner/name indexes read the same
      // image. Keep those indexes alive through both kinds of selection.
      std::erase_if(Image.Symbols, [](const Symbol &Sym) {
        return Sym.Addr == 0x1000 || Sym.Addr == 0x1100;
      });
      PipelineAnalysisCache Cache;
      const auto Check = [&](va_t Entry) {
        PipelineResult Kept, Cold;
        PipelineTestPeer::buildLowIR(Image, Kept, {Entry}, &Cache);
        PipelineTestPeer::buildLowIR(Image, Cold, {Entry});
        ASSERT_EQ(Kept.LowFuncs.size(), 1u);
        EXPECT_TRUE(Kept.LowFuncs.front().hasCompleteInstructionLift());
        expectSameSelection(Kept, Cold);
        EXPECT_EQ(Kept.CallEntryArgumentCounts, Cold.CallEntryArgumentCounts);
        EXPECT_EQ(Kept.CallArgumentSources, Cold.CallArgumentSources);
      };
      for (va_t Entry : {0x1000, 0x1100, 0x1800, 0x1000, 0x1100})
        Check(Entry);
      // A newly confirmed boundary invalidates both kinds, even when the
      // symbol inventory is unchanged. Return to a former snapshot as well.
      Image.VerifiedFunctionEntries.insert(0x1100);
      Check(0x1000);
      Check(0x1800);
      Image.VerifiedFunctionEntries.clear();
      Check(0x1100);
      // A reset retires every index before the backing code changes.
      Cache.reset();
      if (TheArch == Arch::AArch64)
        Image.Segments[0].Data[0x2002 - 0x1000] = 0x00; // mov x0, x0
      else if (TheArch == Arch::ARM)
        Image.Segments[0].Data[0x2000 - 0x1000] = 0x00; // mov r0, r0
      else {
        Image.Segments[0].Data[0x2000 - 0x1000] = 0x89; // mov eax, edi
        Image.Segments[0].Data[0x2001 - 0x1000] = 0xf8;
      }
      Check(0x1000);
    }
  }
}

TEST(PipelineOutcome, AnalysisCacheResetObservesChangedCalleeBytes) {
  BinaryImage Image = selectionCacheImage(true);
  PipelineAnalysisCache Cache;
  PipelineResult First;
  PipelineTestPeer::buildLowIR(Image, First, {0x1000}, &Cache);
  const auto Before = PipelineTestPeer::analysisFacts(Cache);
  ASSERT_NE(Before, nullptr);
  Cache.reset();
  Image.Segments[0].Data[0x2000 - 0x1000] = 0x89; // mov eax, edi; ret
  Image.Segments[0].Data[0x2001 - 0x1000] = 0xf8;
  PipelineResult Kept, Cold;
  PipelineTestPeer::buildLowIR(Image, Kept, {0x1100}, &Cache);
  PipelineTestPeer::buildLowIR(Image, Cold, {0x1100});
  EXPECT_NE(PipelineTestPeer::analysisFacts(Cache), Before);
  expectSameSelection(Kept, Cold);
  ASSERT_EQ(Kept.LowFuncs.size(), 1u);
  for (const auto &Block : Kept.LowFuncs[0].Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL)
        EXPECT_EQ(Op.Output.Size, 8u);
}

TEST(PipelineOutcome, AnalysisCacheSeparatesImagesAndSkipsWholeImageRuns) {
  BinaryImage FirstImage = selectionCacheImage(true);
  BinaryImage SecondImage = selectionCacheImage(false);
  PipelineAnalysisCache Cache;
  PipelineResult First;
  PipelineTestPeer::buildLowIR(FirstImage, First, {0x1000}, &Cache);
  const auto Before = PipelineTestPeer::analysisFacts(Cache);
  PipelineResult Second, Cold;
  PipelineTestPeer::buildLowIR(SecondImage, Second, {0x1000}, &Cache);
  PipelineTestPeer::buildLowIR(SecondImage, Cold, {0x1000});
  EXPECT_NE(PipelineTestPeer::analysisFacts(Cache), Before);
  expectSameSelection(Second, Cold);
  Cache.reset();
  PipelineOptions Options;
  Options.AnalysisCache = &Cache;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const auto Whole = Pipeline().run(SecondImage, Context, Options);
  EXPECT_TRUE(Whole.Success) << Whole.Error;
  EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), nullptr);
}

TEST(PipelineOutcome, WarmedAnalysisCacheLeavesSelectedRunsUnchanged) {
  for (unsigned Count : {1u, 4u}) {
    ScopedThreadCount Threads(Count);
    for (bool Floating : {false, true}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(Floating);
      const BinaryImage Image = selectionCacheImage(Floating);
      std::vector<va_t> Functions;
      for (const auto &Sym : Image.Symbols)
        if (Sym.IsFunc)
          Functions.push_back(Sym.Addr);
      PipelineAnalysisCache Cache;
      PipelineOptions Options;
      Options.AnalysisCache = &Cache;
      const auto Warmup =
          Pipeline().warmAnalysisCache(Image, Options, nullptr, Functions);
      ASSERT_TRUE(Warmup);
      EXPECT_FALSE(Warmup->Full);
      // The floating callee's own x87 stack keeps its body out of the cache.
      EXPECT_EQ(Warmup->Prepared,
                Floating ? Functions.size() - 1 : Functions.size());
      const auto Facts = PipelineTestPeer::analysisFacts(Cache);
      ASSERT_NE(Facts, nullptr);
      for (va_t Entry : Functions) {
        PipelineResult Kept, Cold;
        PipelineTestPeer::buildLowIR(Image, Kept, {Entry}, &Cache);
        PipelineTestPeer::buildLowIR(Image, Cold, {Entry});
        // A selected symbol start shares the warmed state.
        EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), Facts);
        expectSameSelection(Kept, Cold);
      }
    }
  }
}

TEST(PipelineOutcome, AnalysisCacheReadsTheSymbolInventoryOnce) {
  // Every run reads the image's boundary guesses and function-symbol starts.
  // The cache reads them once for its image and each run reads what a cold
  // run does; a grown symbol table is read again, and a reset drops them.
  BinaryImage Image = selectionCacheImage(true);
  Symbol Guess = Symbol::makeFunc(0x1200, 1);
  Guess.IsBoundaryGuess = true;
  Image.Symbols.push_back(Guess);
  PipelineAnalysisCache Cache;
  const auto Check = [&](va_t Entry) {
    PipelineResult Kept, Cold;
    PipelineTestPeer::buildLowIR(Image, Kept, {Entry}, &Cache);
    PipelineTestPeer::buildLowIR(Image, Cold, {Entry});
    expectSameSelection(Kept, Cold);
  };
  Check(0x1000);
  const auto Inventory = PipelineTestPeer::symbolInventory(Cache);
  const auto Facts = PipelineTestPeer::analysisFacts(Cache);
  ASSERT_NE(Inventory, nullptr);
  for (const va_t Entry : {va_t{0x1100}, va_t{0x1200}, va_t{0x1000}}) {
    Check(Entry);
    EXPECT_EQ(PipelineTestPeer::symbolInventory(Cache), Inventory);
  }
  // A guess is no symbol start, so its run shares the symbol facts too.
  EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), Facts);
  EXPECT_EQ(PipelineTestPeer::selectionFacts(Cache), nullptr);

  // A symbol that is no guess confirms the start.
  Image.Symbols.push_back(Symbol::makeFunc(0x1200, 1));
  Check(0x1000);
  EXPECT_NE(PipelineTestPeer::symbolInventory(Cache), Inventory);
  EXPECT_NE(PipelineTestPeer::analysisFacts(Cache), Facts);
  Check(0x1200);

  Cache.reset();
  EXPECT_EQ(PipelineTestPeer::symbolInventory(Cache), nullptr);
}

TEST(PipelineOutcome, SelectionsOutsideTheSymbolInventoryKeepTheirOwnFacts) {
  const BinaryImage Image = selectionCacheImage(false);
  PipelineAnalysisCache Cache;
  PipelineResult Symbol, Selected, Again, ColdSymbol, ColdSelected;
  PipelineTestPeer::buildLowIR(Image, Symbol, {0x1000}, &Cache);
  const auto Facts = PipelineTestPeer::analysisFacts(Cache);
  ASSERT_NE(Facts, nullptr);
  EXPECT_EQ(PipelineTestPeer::selectionFacts(Cache), nullptr);
  // 0x1200 starts no symbol, so its run knows one more entry and keys facts
  // of its own, beside the symbol inventory's.
  PipelineTestPeer::buildLowIR(Image, Selected, {0x1200}, &Cache);
  const auto Selection = PipelineTestPeer::selectionFacts(Cache);
  ASSERT_NE(Selection, nullptr);
  EXPECT_NE(Selection, Facts);
  EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), Facts);
  PipelineTestPeer::buildLowIR(Image, ColdSelected, {0x1200});
  expectSameSelection(Selected, ColdSelected);
  // The next symbol start finds the symbol inventory's facts in place.
  PipelineTestPeer::buildLowIR(Image, Again, {0x1000}, &Cache);
  EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), Facts);
  EXPECT_EQ(PipelineTestPeer::selectionFacts(Cache), Selection);
  PipelineTestPeer::buildLowIR(Image, ColdSymbol, {0x1000});
  expectSameSelection(Again, ColdSymbol);
  // Warming adds to them and leaves the selection's.
  PipelineOptions Options;
  Options.AnalysisCache = &Cache;
  EXPECT_TRUE(Pipeline().warmAnalysisCache(Image, Options, nullptr, {0x1000}));
  EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), Facts);
  EXPECT_EQ(PipelineTestPeer::selectionFacts(Cache), Selection);
  // Facts for another image serve no run of this one, so warming replaces
  // them; a reset retires both kinds.
  const BinaryImage Other = selectionCacheImage(false);
  EXPECT_TRUE(Pipeline().warmAnalysisCache(Other, Options, nullptr, {0x1000}));
  EXPECT_NE(PipelineTestPeer::analysisFacts(Cache), Facts);
  Cache.reset();
  EXPECT_EQ(PipelineTestPeer::analysisFacts(Cache), nullptr);
  EXPECT_EQ(PipelineTestPeer::selectionFacts(Cache), nullptr);
  // i386 runs share the symbol inventory's facts too, which warming builds.
  BinaryImage Narrow = selectionCacheImage(false);
  Narrow.Arch = Arch::X86;
  Narrow.Bits = Bitness::Bits32;
  PipelineAnalysisCache NarrowCache;
  Options.AnalysisCache = &NarrowCache;
  EXPECT_TRUE(Pipeline().warmAnalysisCache(Narrow, Options, nullptr, {0x1000}));
  EXPECT_NE(PipelineTestPeer::analysisFacts(NarrowCache), nullptr);
  // Without a cache, or on an architecture whose runs share none, nothing is
  // kept to warm.
  EXPECT_FALSE(Pipeline().warmAnalysisCache(Image, PipelineOptions(), nullptr,
                                            {0x1000}));
  BinaryImage Bytecode = selectionCacheImage(false);
  Bytecode.Arch = Arch::EVM;
  PipelineAnalysisCache BytecodeCache;
  Options.AnalysisCache = &BytecodeCache;
  EXPECT_FALSE(
      Pipeline().warmAnalysisCache(Bytecode, Options, nullptr, {0x1000}));
  EXPECT_EQ(PipelineTestPeer::analysisFacts(BytecodeCache), nullptr);
}

TEST(PipelineOutcome, WarmingStopsAtTheCacheBoundWithoutEvicting) {
  const BinaryImage Image = selectionCacheImage(false);
  const std::set<va_t> Entries = {0x1000, 0x1100, 0x1800, 0x2000};
  const auto Cache = createX87CallGraphCache(
      nullptr, nullptr, /*RetainCallRegisterBodies=*/true,
      {/*Entries=*/2, 8 * 1024 * 1024, 32 * 1024 * 1024});
  const auto Warm = [&](std::vector<va_t> Functions) {
    return CFGBuilder::warmCallGraphs(Cache, Image, Functions, &Entries,
                                      nullptr, nullptr, nullptr, nullptr, 1);
  };
  const auto First = Warm({0x2000, 0x1800});
  EXPECT_EQ(First.Retained, 2u);
  EXPECT_FALSE(First.Full);
  const auto Second = Warm({0x1000, 0x1100});
  EXPECT_EQ(Second.Retained, 0u);
  EXPECT_TRUE(Second.Full);
  // What the cache held before stays there.
  EXPECT_TRUE(CFGBuilder::cachedCallRegisterBody(
      Cache, Image, 0x2000, &Entries, nullptr, nullptr, nullptr, nullptr));
}

TEST(PipelineOutcome, ParallelCalleeFrontiersPreserveEveryRegisterSummary) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = 0x5000;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  auto emitCall = [&](va_t From, va_t To) {
    const uint32_t Relative = static_cast<uint32_t>(To - (From + 5));
    const size_t Offset = static_cast<size_t>(From - Text.VA);
    Text.Data[Offset] = 0xe8;
    for (unsigned I = 0; I < 4; ++I)
      Text.Data[Offset + 1 + I] = static_cast<uint8_t>(Relative >> (I * 8));
  };
  // More than one scheduling window, followed by a second complete frontier.
  // Sized symbols give independent ownership and enough estimated work for
  // the parallel path without padding the decoded bodies with real work.
  constexpr unsigned CalleeCount = 40;
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry, 0x100));
  for (unsigned I = 0; I < CalleeCount; ++I) {
    const va_t Callee = 0x2000 + I * 0x80;
    const va_t Grandchild = 0x4000 + I * 0x80;
    Image.Symbols.push_back(Symbol::makeFunc(Callee, 0x80));
    Image.Symbols.push_back(Symbol::makeFunc(Grandchild, 0x80));
    emitCall(Image.Entry + I * 5, Callee);
    emitCall(Callee, Grandchild);
    Text.Data[Callee - Text.VA + 5] = 0xc3;
    // Alternate which SysV argument each grandchild reads into its result.
    Text.Data[Grandchild - Text.VA] = 0x89;
    Text.Data[Grandchild - Text.VA + 1] = I % 2 ? 0xf0 : 0xf8;
    Text.Data[Grandchild - Text.VA + 2] = 0xc3;
  }
  Text.Data[CalleeCount * 5] = 0xc3;
  Image.Segments.push_back(std::move(Text));
  PipelineResult Serial;
  PipelineResult Parallel;
  {
    ScopedThreadCount Threads(1);
    PipelineTestPeer::buildLowIR(Image, Serial);
  }
  {
    ScopedThreadCount Threads(4);
    PipelineTestPeer::buildLowIR(Image, Parallel);
  }
  ASSERT_EQ(Serial.LowFuncs.size(), 1u);
  ASSERT_EQ(Parallel.LowFuncs.size(), 1u);
  EXPECT_EQ(Serial.LowFuncs.front().Entry, Image.Entry);
  EXPECT_EQ(Parallel.LowFuncs.front().Entry, Image.Entry);
  EXPECT_EQ(Serial.CallMayWriteGPRs.size(), 2 * CalleeCount + 1);
  EXPECT_EQ(Parallel.CallMayWriteGPRs, Serial.CallMayWriteGPRs);
  EXPECT_EQ(Parallel.CallEntryReadGPRs, Serial.CallEntryReadGPRs);
  EXPECT_EQ(Parallel.CallEntryStackArgs, Serial.CallEntryStackArgs);
  EXPECT_EQ(Parallel.CallVariadicFrom, Serial.CallVariadicFrom);
  EXPECT_EQ(Parallel.CallDispatchThunks, Serial.CallDispatchThunks);
}

TEST(PipelineOutcome, SelectedX87WrappersPreserveReturnsAcrossParallelBuilds) {
  for (auto [Architecture, Format] :
       {std::pair{Arch::X86, BinaryFormat::ELF},
        std::pair{Arch::X64, BinaryFormat::ELF},
        std::pair{Arch::X64, BinaryFormat::COFF}}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    SCOPED_TRACE(static_cast<int>(Format));
    BinaryImage Image;
    Image.Arch = Architecture;
    Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
    Image.Format = Format;
    const bool Owned =
        x87ContextOwnsStack(Architecture, Format, Image.X87ContextOverride);
    Image.Entry = 0x1000;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Image.Entry;
    Text.Size = 0x2000;
    Text.FileSz = Text.Size;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0xcc);
    auto emitWrapper = [&](va_t From, va_t To) {
      const uint32_t Relative = static_cast<uint32_t>(To - (From + 5));
      const size_t Offset = static_cast<size_t>(From - Text.VA);
      Text.Data[Offset] = 0xe8;
      for (unsigned I = 0; I != 4; ++I)
        Text.Data[Offset + 1 + I] = static_cast<uint8_t>(Relative >> (I * 8));
      Text.Data[Offset + 5] = 0xc3;
      Image.Symbols.push_back(Symbol::makeFunc(From, 6));
    };
    // Enough independent roots to take the production parallel path. None
    // contains an x87 instruction: the result comes through a shared wrapper.
    constexpr unsigned WrapperCount = 32;
    constexpr va_t Forwarder = 0x1800, Leaf = 0x2000;
    std::vector<va_t> Entries;
    for (unsigned I = 0; I != WrapperCount; ++I) {
      Entries.push_back(Image.Entry + I * 0x10);
      emitWrapper(Entries.back(), Forwarder);
    }
    emitWrapper(Forwarder, Leaf);
    Text.Data[Leaf - Text.VA] = 0xd9; // fld1; ret
    Text.Data[Leaf - Text.VA + 1] = 0xe8;
    Text.Data[Leaf - Text.VA + 2] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(Leaf, 3));
    Image.Segments.push_back(std::move(Text));

    PipelineResult Serial, Parallel;
    {
      ScopedThreadCount Threads(1);
      PipelineTestPeer::buildLowIR(Image, Serial, Entries);
    }
    {
      ScopedThreadCount Threads(4);
      PipelineTestPeer::buildLowIR(Image, Parallel, Entries);
    }
    for (const PipelineResult *Result : {&Serial, &Parallel}) {
      ASSERT_EQ(Result->LowFuncs.size(), WrapperCount);
      for (size_t I = 0; I != Entries.size(); ++I) {
        const auto &Function = Result->LowFuncs[I];
        SCOPED_TRACE(Function.Entry);
        EXPECT_EQ(Function.Entry, Entries[I]);
        ASSERT_TRUE(Function.hasCompleteInstructionLift());
        size_t Calls = 0, Returns = 0;
        const auto ResultSlot =
            Owned ? NdVar::reg(x86reg::RAX, 8)
                  : NdVar::reg(x86reg::ST7, x86reg::FPURegSize);
        unsigned Captures = 0, Exports = 0, Imports = 0;
        for (const auto &Block : Function.Blocks)
          for (const auto &Op : Block.Ops) {
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].isConst()) {
              Captures +=
                  Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextBegin);
              Exports +=
                  Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextExport);
              Imports +=
                  Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextRefresh);
            }
            if (Op.Opcode == NdOp::CALL) {
              ++Calls;
              ASSERT_GT(Op.NumInputs, 0u);
              EXPECT_TRUE(Op.Inputs[0].isConst());
              EXPECT_EQ(Op.Inputs[0].Offset, Forwarder);
              EXPECT_EQ(Op.Output, ResultSlot);
            } else if (Op.Opcode == NdOp::RETURN) {
              ++Returns;
              ASSERT_EQ(Op.NumInputs, 1u);
              EXPECT_EQ(Op.Inputs[0], ResultSlot);
            }
          }
        EXPECT_EQ(Calls, 1u);
        EXPECT_EQ(Returns, 1u);
        // Each wrapper returns its call's state directly (call; ret).
        EXPECT_EQ(Captures, Owned ? 1u : 0u);
        EXPECT_EQ(Exports, Owned ? 1u : 0u);
        EXPECT_EQ(Imports, 0u);
      }
    }
    EXPECT_EQ(Parallel.CallMayWriteGPRs, Serial.CallMayWriteGPRs);
    EXPECT_EQ(Parallel.CallEntryReadGPRs, Serial.CallEntryReadGPRs);
    EXPECT_EQ(Parallel.CallEntryStackArgs, Serial.CallEntryStackArgs);
    EXPECT_EQ(Parallel.CallVariadicFrom, Serial.CallVariadicFrom);
    EXPECT_EQ(Parallel.CallDispatchThunks, Serial.CallDispatchThunks);
  }
}

TEST(PipelineOutcome, PatchDiscoversStandaloneELFEntryButPreservesCRTEntry) {
  llvm::LLVMContext Context;
  auto run = [&Context](bool HasInterpreter) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Entry = 0x1000;

    Segment Text;
    Text.Name = ".text";
    Text.VA = Image.Entry;
    Text.Size = 16;
    Text.FileSz = Text.Size;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0x90);
    Text.Data[0] = 0xc3; // ret
    Image.Segments.push_back(Text);

    Section TextSection;
    TextSection.Name = ".text";
    TextSection.VA = Text.VA;
    TextSection.Size = Text.Size;
    TextSection.FileSz = Text.FileSz;
    TextSection.Flags = Text.Flags;
    TextSection.Data = Text.Data;
    Image.Sections.push_back(std::move(TextSection));
    if (HasInterpreter) {
      Section Interpreter;
      Interpreter.Name = ".interp";
      Image.Sections.push_back(std::move(Interpreter));
    }
    EXPECT_TRUE(Image.recordRuntimeFunction(Image.Entry));

    PipelineOptions Options;
    Options.PatchMode = true;
    Options.NoOpt = true;
    return Pipeline().run(Image, Context, Options);
  };

  PipelineResult Standalone = run(false);
  ASSERT_TRUE(Standalone.Success) << Standalone.Error;
  ASSERT_EQ(Standalone.LowFuncs.size(), 1u);
  EXPECT_EQ(Standalone.LowFuncs.front().Entry, 0x1000u);

  PipelineResult CRT = run(true);
  ASSERT_TRUE(CRT.Success) << CRT.Error;
  EXPECT_TRUE(CRT.LowFuncs.empty());
}

TEST(PipelineOutcome, UnsupportedBodiesCannotPublishSuccessfulEmptyModules) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Patch : {false, true})
        for (bool Selected : {false, true}) {
          SCOPED_TRACE(getArchName(Architecture));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(Patch);
          SCOPED_TRACE(Selected);
          BinaryImage Image;
          Image.Arch = Architecture;
          Image.Bits =
              Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
          Image.Format = Format;
          Image.Entry = 0x1000;
          Segment Text;
          Text.Name = ".text";
          Text.VA = Image.Entry;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          // MOV EAX, ESI; XACQUIRE LOCK XADD [RDI/EDI], EAX; RET.
          Text.Data = {0x89, 0xf0, 0xf2, 0xf0, 0x0f, 0xc1, 0x07, 0xc3};
          Text.Size = Text.FileSz = Text.Data.size();
          Image.Symbols.push_back(
              {"unsupported_hle", Image.Entry, Text.Size, true});
          Image.Segments.push_back(std::move(Text));
          PipelineOptions Options;
          Options.LiftMode = !Patch;
          Options.PatchMode = Patch;
          Options.NoOpt = true;
          if (Selected)
            Options.OnlyFunctionEntries.insert(Image.Entry);
          llvm::LLVMContext Context;
          auto Result = Pipeline().run(Image, Context, Options);
          EXPECT_FALSE(Result.Success);
          EXPECT_EQ(Result.LlvmModule, nullptr);
          EXPECT_TRUE(Result.MedFuncs.empty());
          EXPECT_NE(Result.Error.find("function 0x1000 was not admitted"),
                    std::string::npos)
              << Result.Error;
          EXPECT_NE(Result.Error.find("unsupported instruction at 0x1002"),
                    std::string::npos)
              << Result.Error;
          ASSERT_EQ(Result.FunctionAudits.size(), 1u);
          EXPECT_EQ(Result.FunctionAudits.front().UnsupportedInstructions,
                    std::vector<va_t>{0x1002});
          EXPECT_TRUE(
              Result.FunctionAudits.front().NativeInstructionAddresses.empty());

          if (!Selected) {
            Options.LiftMode = Options.PatchMode = false;
            Options.MedAnalysisOnly = true;
            auto Analysis = Pipeline().run(Image, Context, Options);
            EXPECT_TRUE(Analysis.Success) << Analysis.Error;
            EXPECT_EQ(Analysis.LlvmModule, nullptr);
            EXPECT_TRUE(Analysis.MedFuncs.empty());
            ASSERT_EQ(Analysis.FunctionAudits.size(), 1u);
            EXPECT_EQ(Analysis.FunctionAudits.front().UnsupportedInstructions,
                      std::vector<va_t>{0x1002});
          }
          Options.LiftMode = Options.PatchMode = Options.MedAnalysisOnly =
              false;
          Options.DeferMedIR = true;
          auto Selection = Pipeline().run(Image, Context, Options);
          EXPECT_TRUE(Selection.Success) << Selection.Error;
          EXPECT_TRUE(Selection.LowFuncs.empty());
          EXPECT_TRUE(Selection.MedFuncs.empty());
          EXPECT_EQ(Selection.LlvmModule, nullptr);
          ASSERT_EQ(Selection.FunctionAudits.size(), 1u);
          const auto &Audit = Selection.FunctionAudits.front();
          EXPECT_EQ(Audit.UnsupportedInstructions, std::vector<va_t>{0x1002});
          EXPECT_FALSE(Audit.HasLowIR);
          EXPECT_NE(std::find(Audit.NativeInstructionAddresses.begin(),
                              Audit.NativeInstructionAddresses.end(), 0x1002),
                    Audit.NativeInstructionAddresses.end());
        }
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

TEST(PipelineOutcome, HighIRSkeletonsWhenConvertYieldsEmptyBody) {
  PipelineResult Result;
  MedFunc Source = sourceFunction(0x401000, "empty_struct");
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x401000;
  Source.Blocks.push_back(Block);
  Result.MedFuncs.push_back(std::move(Source));
  BinaryImage Image;
  Image.Arch = Arch::X86;
  ASSERT_TRUE(pipeline_detail::runHighIRStage(Result, [&] {
    PipelineTestPeer::buildHighIR(Image, Result);
  })) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  EXPECT_FALSE(Result.HighFuncs.front().Body.empty())
      << "non-empty MedIR must not become an empty HighFunc body";
}

TEST(PipelineOutcome, HighIRFallsBackToLowIRGotoSkeleton) {
  PipelineResult Result;
  Result.MedFuncs.push_back(sourceFunction(0x401000, "from_low"));
  LowFunc LF;
  LF.Name = "from_low";
  LF.Entry = 0x401000;
  LowBlock B0;
  B0.Id = 0;
  B0.StartAddr = 0x401000;
  B0.Succs = {1};
  LowBlock B1;
  B1.Id = 1;
  B1.StartAddr = 0x401010;
  LF.Blocks = {B0, B1};
  Result.LowFuncs.push_back(std::move(LF));
  BinaryImage Image;
  Image.Arch = Arch::X86;
  ASSERT_TRUE(pipeline_detail::runHighIRStage(Result, [&] {
    PipelineTestPeer::buildHighIR(Image, Result);
  })) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  const HighFunc &HF = Result.HighFuncs.front();
  EXPECT_EQ(HF.Entry, 0x401000u);
  EXPECT_EQ(HF.Name, "from_low");
  const bool HasGoto =
      std::any_of(HF.Body.begin(), HF.Body.end(),
                  [](const HighStmt &S) { return S.Kind == StmtKind::Goto; });
  EXPECT_TRUE(HasGoto)
      << "empty keepIdentity must not survive when LowIR exists";

  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, Options));
  EXPECT_NE(Source.find("goto L_"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("no structured body"), std::string::npos) << Source;
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

TEST(PipelineOutcome, ResolvedSignaturesAreReconciledBeforeShardPublication) {
  for (bool NoOpt : {false, true})
    for (bool Deferred : {false, true})
      for (bool Missing : {false, true}) {
        SCOPED_TRACE(Missing);
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Deferred);
        llvm::LLVMContext Context;
        testing::internal::CaptureStderr();
        auto Result = PipelineTestPeer::runShards(
            {}, Context, 2, 2, NoOpt,
            [&](unsigned Shard, llvm::LLVMContext &C) {
              PipelineTestPeer::LLVMEmissionResult E;
              E.Module = shardModule(C, Shard ? "other" : "caller");
              E.Module->setDataLayout("e-p:64:64");
              auto *I64 = llvm::Type::getInt64Ty(C);
              auto *Actual = llvm::FunctionType::get(
                  I64, std::vector<llvm::Type *>(Missing ? 2 : 0, I64), false);
              llvm::Value *Target = nullptr;
              if (Shard == 1 || !Deferred)
                Target = llvm::Function::Create(
                    Actual, llvm::GlobalValue::ExternalLinkage, "late_import",
                    *E.Module);
              else
                Target = new llvm::GlobalVariable(
                    *E.Module, llvm::Type::getInt8Ty(C), false,
                    llvm::GlobalValue::ExternalLinkage, nullptr, "late_import");
              if (Shard == 1) {
                auto *F = llvm::cast<llvm::Function>(Target);
                llvm::IRBuilder<> Builder(
                    llvm::BasicBlock::Create(C, "entry", F));
                Builder.CreateRet(Builder.getInt64(42));
              }
              if (Shard == 0) {
                auto *F = E.Module->getFunction("caller");
                llvm::IRBuilder<> Builder(F->getEntryBlock().getTerminator());
                auto *Wrong = llvm::FunctionType::get(I64, {I64}, false);
                Builder.CreateCall(Wrong, Target, {Builder.getInt64(7)});
              }
              // Opaque pointers allow this semantic mismatch through LLVM's
              // verifier; before linking, the deferred case has no Function.
              EXPECT_FALSE(llvm::verifyModule(*E.Module, &llvm::errs()));
              return E;
            });
        const auto Diagnostic = testing::internal::GetCapturedStderr();
        if (Missing) {
          EXPECT_EQ(Result.Module, nullptr);
          EXPECT_NE(Result.Error.find("resolved call signature mismatch"),
                    std::string::npos)
              << Result.Error;
          EXPECT_NE(Diagnostic.find("lacks recovered arguments"),
                    std::string::npos)
              << Diagnostic;
        } else {
          ASSERT_NE(Result.Module, nullptr) << Result.Error;
          EXPECT_FALSE(llvm::verifyModule(*Result.Module, &llvm::errs()));
          unsigned Calls = 0;
          for (auto &B : *Result.Module->getFunction("caller"))
            for (auto &I : B)
              if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
                ++Calls;
                ASSERT_NE(Call->getCalledFunction(), nullptr);
                EXPECT_EQ(Call->getFunctionType(),
                          Call->getCalledFunction()->getFunctionType());
                EXPECT_EQ(Call->arg_size(), 0u);
              }
          EXPECT_EQ(Calls, 1u);
        }
      }
}

TEST(PipelineOutcome,
     LateCallNormalizationPreservesNarrowBitsAndSourceIdentity) {
  llvm::LLVMContext C;
  auto Module = shardModule(C, "unused");
  Module->setDataLayout("e-p:64:64");
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *I64 = llvm::Type::getInt64Ty(C);
  auto *TargetType = llvm::FunctionType::get(I32, {I32}, false);
  auto *Target = llvm::Function::Create(
      TargetType, llvm::GlobalValue::ExternalLinkage, "narrow", *Module);
  auto *Caller = llvm::Function::Create(llvm::FunctionType::get(I64, false),
                                        llvm::GlobalValue::ExternalLinkage,
                                        "caller", *Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", Caller));
  auto *Call =
      B.CreateCall(llvm::FunctionType::get(I64, {I64, I64}, false), Target,
                   {B.getInt64(0x123400000017ULL), B.getInt64(99)});
  Call->setMetadata(
      llvm::LLVMContext::MD_range,
      llvm::MDNode::get(C, {llvm::ConstantAsMetadata::get(B.getInt64(0)),
                            llvm::ConstantAsMetadata::get(B.getInt64(100))}));
  B.CreateRet(Call);
  LLVMSourceMap Sources;
  Sources.Observations.push_back({0x1000, {0x1004, 0}, Call});
  ASSERT_TRUE(normalizeResolvedLLVMCalls(*Module, &Sources));
  EXPECT_TRUE(validateResolvedLLVMCallSignatures(*Module));
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto *Normalized = llvm::dyn_cast_or_null<llvm::CallInst>(
      static_cast<llvm::Value *>(Sources.Observations[0].Value));
  ASSERT_NE(Normalized, nullptr);
  EXPECT_EQ(Normalized->getCalledFunction(), Target);
  ASSERT_EQ(Normalized->arg_size(), 1u);
  auto *Argument =
      llvm::dyn_cast<llvm::ConstantInt>(Normalized->getArgOperand(0));
  ASSERT_NE(Argument, nullptr);
  EXPECT_EQ(Argument->getZExtValue(), 23u);
  EXPECT_EQ(Argument->getType(), I32);
  EXPECT_EQ(Normalized->getMetadata(llvm::LLVMContext::MD_range), nullptr);
  auto *Return =
      llvm::cast<llvm::ReturnInst>(Caller->getEntryBlock().getTerminator());
  auto *Extend = llvm::dyn_cast<llvm::ZExtInst>(Return->getReturnValue());
  ASSERT_NE(Extend, nullptr);
  EXPECT_EQ(Extend->getOperand(0), Normalized);
  EXPECT_EQ(Extend->getType(), I64);
}

TEST(PipelineOutcome, ResolvedCallsKeepIndependentArgumentBanksAndStackSlots) {
  for (bool Invoke : {false, true}) {
    SCOPED_TRACE(Invoke);
    llvm::LLVMContext C;
    llvm::Module Module("argument_banks", C);
    Module.setDataLayout("e-p:64:64");
    auto *I64 = llvm::Type::getInt64Ty(C);
    auto *Vector = llvm::FixedVectorType::get(I64, 2);
    auto *Actual =
        llvm::FunctionType::get(I64, {I64, Vector, Vector, I64, I64}, false);
    auto *Target = llvm::Function::Create(
        Actual, llvm::GlobalValue::ExternalLinkage, "target", Module);
    setLLVMArgumentBanks(*Target, 1, 2);
    auto *Caller = llvm::Function::Create(llvm::FunctionType::get(I64, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "caller", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", Caller));
    std::vector<llvm::Value *> Values = {B.getInt64(11), B.getInt64(99)};
    for (unsigned I : {21u, 22u, 98u})
      Values.push_back(llvm::ConstantVector::getSplat(
          llvm::ElementCount::getFixed(2), B.getInt64(I)));
    Values.push_back(B.getInt64(31));
    Values.push_back(B.getInt64(32));
    auto *Recovered = llvm::FunctionType::get(
        I64, {I64, I64, Vector, Vector, Vector, I64, I64}, false);
    llvm::CallBase *Call;
    if (Invoke) {
      auto *Normal = llvm::BasicBlock::Create(C, "normal", Caller);
      auto *Unwind = llvm::BasicBlock::Create(C, "unwind", Caller);
      auto *Personality = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt32Ty(), true),
          llvm::GlobalValue::ExternalLinkage, "personality", Module);
      Caller->setPersonalityFn(Personality);
      Call = B.CreateInvoke(Recovered, Target, Normal, Unwind, Values);
      B.SetInsertPoint(Unwind);
      auto *Landing = B.CreateLandingPad(
          llvm::StructType::get(B.getPtrTy(), B.getInt32Ty()), 0);
      Landing->setCleanup(true);
      B.CreateResume(Landing);
      B.SetInsertPoint(Normal);
    } else {
      Call = B.CreateCall(Recovered, Target, Values);
    }
    setLLVMArgumentBanks(*Call, 2, 3);
    Call->addParamAttr(2, llvm::Attribute::NoUndef);
    B.CreateRet(Call);
    LLVMSourceMap Sources;
    Sources.Observations.push_back({0x1000, {0x1004, 0}, Call});
    ASSERT_TRUE(normalizeResolvedLLVMCalls(Module, &Sources));
    EXPECT_TRUE(validateResolvedLLVMCallSignatures(Module));
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    auto *Bound = llvm::dyn_cast_or_null<llvm::CallBase>(
        static_cast<llvm::Value *>(Sources.Observations[0].Value));
    ASSERT_NE(Bound, nullptr);
    ASSERT_EQ(Bound->arg_size(), 5u);
    const unsigned Expected[] = {0, 2, 3, 5, 6};
    for (unsigned I = 0; I < 5; ++I)
      EXPECT_EQ(Bound->getArgOperand(I), Values[Expected[I]]);
    EXPECT_TRUE(Bound->paramHasAttr(1, llvm::Attribute::NoUndef));
    EXPECT_FALSE(Bound->paramHasAttr(0, llvm::Attribute::NoUndef));
    EXPECT_EQ(llvm::isa<llvm::InvokeInst>(Bound), Invoke);
    EXPECT_TRUE(normalizeResolvedLLVMCalls(Module, &Sources));
  }
}

TEST(PipelineOutcome, ResolvedCallsRefuseMissingValuesInEitherArgumentBank) {
  for (bool MissingFloating : {false, true}) {
    SCOPED_TRACE(MissingFloating);
    llvm::LLVMContext C;
    llvm::Module Module("missing_argument_bank", C);
    Module.setDataLayout("e-p:64:64");
    auto *I64 = llvm::Type::getInt64Ty(C);
    auto *Vector = llvm::FixedVectorType::get(I64, 2);
    auto *Actual = llvm::FunctionType::get(I64, {I64, Vector}, false);
    auto *Target = llvm::Function::Create(
        Actual, llvm::GlobalValue::ExternalLinkage, "target", Module);
    setLLVMArgumentBanks(*Target, 1, 1);
    auto *Caller = llvm::Function::Create(llvm::FunctionType::get(I64, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "caller", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", Caller));
    auto *Carrier = MissingFloating ? I64 : static_cast<llvm::Type *>(Vector);
    auto *Wrong = llvm::FunctionType::get(I64, {Carrier, Carrier}, false);
    auto *Value = llvm::Constant::getNullValue(Carrier);
    auto *Call = B.CreateCall(Wrong, Target, {Value, Value});
    setLLVMArgumentBanks(*Call, MissingFloating ? 2 : 0,
                         MissingFloating ? 0 : 2);
    B.CreateRet(Call);
    testing::internal::CaptureStderr();
    EXPECT_FALSE(normalizeResolvedLLVMCalls(Module));
    EXPECT_NE(testing::internal::GetCapturedStderr().find(
                  "lacks recovered argument bank or stack values"),
              std::string::npos);
    EXPECT_EQ(Call->getFunctionType(), Wrong);
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  }
}

TEST(PipelineOutcome, ResolvedCallsPreserveAllBytesAcrossStackStoreWidths) {
  for (bool WideStore : {false, true})
    for (bool MissingByte : {false, true}) {
      SCOPED_TRACE(WideStore);
      SCOPED_TRACE(MissingByte);
      llvm::LLVMContext C;
      llvm::Module Module("stack_argument_bytes", C);
      Module.setDataLayout("e-p:64:64");
      auto *I64 = llvm::Type::getInt64Ty(C);
      auto *Target = llvm::Function::Create(
          llvm::FunctionType::get(I64, {I64, I64}, false),
          llvm::GlobalValue::ExternalLinkage, "target", Module);
      setLLVMArgumentBanks(*Target, 0, 0);
      auto *Caller = llvm::Function::Create(llvm::FunctionType::get(I64, false),
                                            llvm::GlobalValue::ExternalLinkage,
                                            "caller", Module);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", Caller));
      std::vector<llvm::Value *> Values;
      std::vector<llvm::Type *> Types;
      if (WideStore) {
        const unsigned Width = MissingByte ? 120 : 128;
        llvm::APInt Bits(128, 0x123456789abcdef0ULL);
        Bits |= llvm::APInt(128, 0xfedcba9876543210ULL).shl(64);
        Values.push_back(llvm::ConstantInt::get(C, Bits.trunc(Width)));
      } else {
        for (uint32_t Value : {0x9abcdef0u, 0x12345678u, 0x76543210u})
          Values.push_back(B.getInt32(Value));
        Values.push_back(MissingByte ? B.getIntN(24, 0xdcba98)
                                     : B.getInt32(0xfedcba98));
      }
      for (auto *Value : Values)
        Types.push_back(Value->getType());
      auto *Call = B.CreateCall(llvm::FunctionType::get(I64, Types, false),
                                Target, Values);
      setLLVMArgumentBanks(*Call, 0, 0);
      Call->addParamAttr(0, llvm::Attribute::NoUndef);
      B.CreateRet(Call);
      testing::internal::CaptureStderr();
      const bool Normalized = normalizeResolvedLLVMCalls(Module);
      const auto Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Normalized, !MissingByte) << Diagnostic;
      EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      if (MissingByte) {
        EXPECT_NE(
            Diagnostic.find("lacks recovered argument bank or stack values"),
            std::string::npos);
        continue;
      }
      const auto *Bound =
          llvm::cast<llvm::CallBase>(&*Caller->getEntryBlock().begin());
      ASSERT_EQ(Bound->arg_size(), 2u);
      const uint64_t Expected[] = {0x123456789abcdef0ULL,
                                   0xfedcba9876543210ULL};
      for (unsigned I = 0; I < 2; ++I) {
        const auto *Value =
            llvm::dyn_cast<llvm::ConstantInt>(Bound->getArgOperand(I));
        ASSERT_NE(Value, nullptr);
        EXPECT_EQ(Value->getZExtValue(), Expected[I]);
        EXPECT_FALSE(Bound->paramHasAttr(I, llvm::Attribute::NoUndef));
      }
    }
}

TEST(PipelineOutcome, AnalystCallDeclarationsRequireExplicitSourceProjection) {
  llvm::LLVMContext Context;
  llvm::Module Module("analyst_call_scope", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Original = llvm::FunctionType::get(I64, {I64, I64}, false);
  auto *Target = llvm::Function::Create(
      Original, llvm::GlobalValue::ExternalLinkage, "target", Module);
  auto *Caller = llvm::Function::Create(llvm::FunctionType::get(I64, false),
                                        llvm::GlobalValue::ExternalLinkage,
                                        "caller", Module);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Caller));
  auto *Selected = llvm::FunctionType::get(I64, {I64}, false);
  auto *Call = Builder.CreateCall(Selected, Target, {Builder.getInt64(7)});
  Call->setMetadata("neverd.source.analyst-call",
                    llvm::MDNode::get(Context, {}));
  Builder.CreateRet(Call);
  EXPECT_TRUE(normalizeResolvedLLVMCalls(Module, nullptr, true));
  EXPECT_TRUE(validateResolvedLLVMCallSignatures(Module, true));
  EXPECT_EQ(Target->getFunctionType(), Original);
  EXPECT_EQ(Call->getFunctionType(), Selected);
  testing::internal::CaptureStderr();
  EXPECT_FALSE(normalizeResolvedLLVMCalls(Module));
  EXPECT_FALSE(validateResolvedLLVMCallSignatures(Module));
  EXPECT_FALSE(testing::internal::GetCapturedStderr().empty());
  EXPECT_EQ(Call->arg_size(), 1u);
}

TEST(PipelineOutcome, OmittedPointerDeclarationsStayInSourceModules) {
  ScopedThreadCount Threads(2);
  constexpr va_t Target = 0x1100;
  for (unsigned Count : {1u, 8u}) {
    SCOPED_TRACE(Count);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Entry = 0x1000;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Image.Entry;
    Text.Size = 0x104;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size, 0xcc);
    std::set<va_t> Entries;
    for (unsigned I = 0; I != Count; ++I) {
      const va_t Entry = Image.Entry + I * 0x20;
      std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 8, 0xe8};
      const uint32_t Displacement = Target - (Entry + 9);
      for (unsigned Byte = 0; Byte != 4; ++Byte)
        Code.push_back(Displacement >> (Byte * 8));
      Code.insert(Code.end(), {0x48, 0x83, 0xc4, 8, 0xc3});
      std::copy(Code.begin(), Code.end(),
                Text.Data.begin() + (Entry - Text.VA));
      auto Function = Symbol::makeFunc(Entry);
      Function.Name = "forward_pointer_" + std::to_string(I);
      Image.Symbols.push_back(std::move(Function));
      Entries.insert(Entry);
    }
    const uint8_t Read[] = {0x48, 0x8b, 0x07, 0xc3}; // mov rax,[rdi]; ret
    std::copy(std::begin(Read), std::end(Read), Text.Data.begin() + 0x100);
    Image.Segments.push_back(std::move(Text));
    auto Function = Symbol::makeFunc(Target);
    Function.Name = "read_pointer";
    Image.Symbols.push_back(std::move(Function));
    for (bool HasException : {false, true}) {
      SCOPED_TRACE(HasException);
      if (HasException) {
        ExceptionFunction Exception;
        Exception.CodeRange = {Target, Target + sizeof(Read)};
        Exception.Encoding = ExceptionEncoding::DwarfFDE;
        Exception.Personality = ExceptionPersonality::None;
        Exception.ParseStatus = ExceptionParseStatus::Complete;
        Image.ExceptionMetadata.Functions.push_back(std::move(Exception));
        Image.ExceptionMetadata.rebuildIndex();
      }
      PipelineAnalysisCache Cache;
      // Reuse the same image cache across native, source and patch requests.
      // Eight callers also exercise linked LLVM shards with one shared callee.
      for (const auto [Source, Patch] : {std::pair{false, false},
                                         {true, false},
                                         {true, true},
                                         {true, false}}) {
        SCOPED_TRACE(Source);
        SCOPED_TRACE(Patch);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.AnalysisCache = &Cache;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = Entries;
        Options.MedAnalysisOnly = true;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        Options.MedAnalysisOnly = false;
        Options.LiftMode = true;
        Options.SourceProjection = Source;
        Options.PatchMode = Patch;
        ASSERT_TRUE(PipelineTestPeer::runLift(Image, Context, Options, Result))
            << Result.Error;
        ASSERT_TRUE(Result.LlvmModule);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        // An exception-bearing body is outside the declaration contract. Its
        // existing zero-stack summary must avoid a redundant signature lift.
        if (HasException) {
          ASSERT_TRUE(Result.OmittedCallees);
          EXPECT_TRUE(Result.OmittedCallees->Bodies.empty());
        }
        for (unsigned I = 0; I != Count; ++I) {
          auto *Caller = Result.LlvmModule->getFunction("forward_pointer_" +
                                                        std::to_string(I));
          ASSERT_TRUE(Caller);
          unsigned Calls = 0;
          for (const auto &Block : *Caller)
            for (const auto &Instruction : Block)
              if (const auto *Call =
                      llvm::dyn_cast<llvm::CallBase>(&Instruction);
                  Call && Call->getCalledFunction() &&
                  !Call->getCalledFunction()->isIntrinsic()) {
                const auto *Callee = Call->getCalledFunction();
                ASSERT_TRUE(Callee->isDeclaration());
                ASSERT_EQ(Callee->arg_size(), 1u);
                EXPECT_EQ(Callee->getArg(0)->getType()->isPointerTy(),
                          Source && !Patch && !HasException);
                ++Calls;
              }
          EXPECT_EQ(Calls, 1u);
        }
      }
    }
  }
}

TEST(PipelineOutcome, ResolvedCallsPlaceNarrowStackValuesInTheirSlots) {
  // -O0 code passes each int stack argument in the low half of its
  // eight-byte System V slot: `mov [rsp], r11d`, `mov [rsp+8], r10d`. The
  // next slot also holds a spill. Each callee slot takes its own value, the
  // rest of the slot being padding: neither the next value's bytes nor the
  // spill. A slot no value was stored in still refuses.
  for (bool MissingSlot : {false, true}) {
    SCOPED_TRACE(MissingSlot);
    llvm::LLVMContext C;
    llvm::Module Module("stack_argument_slots", C);
    Module.setDataLayout("e-p:64:64");
    auto *I32 = llvm::Type::getInt32Ty(C);
    auto *I64 = llvm::Type::getInt64Ty(C);
    auto *Target = llvm::Function::Create(
        llvm::FunctionType::get(I32, {I32, I64, I64}, false),
        llvm::GlobalValue::ExternalLinkage, "target", Module);
    setLLVMArgumentBanks(*Target, 1, 0);
    auto *Caller = llvm::Function::Create(llvm::FunctionType::get(I32, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "caller", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", Caller));
    std::vector<llvm::Value *> Values = {B.getInt32(1), B.getInt32(7),
                                         B.getInt32(9),
                                         B.getInt64(0x2d9c127c45ac13abULL)};
    std::vector<int64_t> Offsets = {0, 8, 16};
    if (MissingSlot) {
      Values.erase(Values.begin() + 2);
      Offsets = {0, 16};
    }
    std::vector<llvm::Type *> Types;
    for (auto *Value : Values)
      Types.push_back(Value->getType());
    auto *Call = B.CreateCall(llvm::FunctionType::get(I32, Types, false),
                              Target, Values);
    setLLVMArgumentBanks(*Call, 1, 0, Offsets, 8);
    B.CreateRet(Call);
    testing::internal::CaptureStderr();
    const bool Normalized = normalizeResolvedLLVMCalls(Module);
    const auto Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Normalized, !MissingSlot) << Diagnostic;
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    if (MissingSlot) {
      EXPECT_NE(
          Diagnostic.find("lacks recovered argument bank or stack values"),
          std::string::npos);
      continue;
    }
    const auto *Bound =
        llvm::cast<llvm::CallBase>(&*Caller->getEntryBlock().begin());
    ASSERT_EQ(Bound->arg_size(), 3u);
    const uint64_t Expected[] = {1, 7, 9};
    for (unsigned I = 0; I < 3; ++I) {
      const auto *Value =
          llvm::dyn_cast<llvm::ConstantInt>(Bound->getArgOperand(I));
      ASSERT_NE(Value, nullptr);
      EXPECT_EQ(Value->getZExtValue(), Expected[I]);
    }
  }
}

TEST(PipelineOutcome, ResolvedCallConventionMustAgreeEvenWhenTypesMatch) {
  for (bool CalleePops : {false, true}) {
    SCOPED_TRACE(CalleePops);
    llvm::LLVMContext C;
    auto Module = shardModule(C, "caller");
    Module->setDataLayout("e-p:32:32");
    auto *Type = llvm::FunctionType::get(llvm::Type::getInt32Ty(C),
                                         {llvm::Type::getInt32Ty(C)}, false);
    auto *Target = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "callee", *Module);
    Target->setCallingConv(CalleePops ? llvm::CallingConv::X86_StdCall
                                      : llvm::CallingConv::C);
    llvm::IRBuilder<> B(
        Module->getFunction("caller")->getEntryBlock().getTerminator());
    auto *Call = B.CreateCall(Type, Target, {B.getInt32(42)});
    Call->setCallingConv(CalleePops ? llvm::CallingConv::C
                                    : llvm::CallingConv::X86_StdCall);
    // The types match and the verifier accepts the call, but x86 caller and
    // callee disagree about who pops the argument. Do not publish that ABI.
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    testing::internal::CaptureStderr();
    EXPECT_FALSE(validateResolvedLLVMCallSignatures(*Module));
    EXPECT_FALSE(normalizeResolvedLLVMCalls(*Module));
    const auto Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_NE(Diagnostic.find("calling convention"), std::string::npos)
        << Diagnostic;
  }
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

// A body that refuses a stale address costs the unit only that definition:
// its shard is emitted again without it, and the result names it with the
// error the emitter wrote.
TEST(PipelineOutcome, RealShardedEmissionLeavesOutOnlyTheRefusedBody) {
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
    if (I == 3) {
      // A page fragment whose low bits never arrive is stored as is.
      MedVar Pointer;
      Pointer.Kind = MedVar::Param;
      Pointer.TheArch = Arch::X64;
      Pointer.Id = 0;
      Pointer.Size = 8;
      Pointer.RegOff = x86reg::RDI;
      Function.Params.push_back(Pointer);
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(Pointer);
      Store.addInput(MedVar::makeConst(
          0x2000, 8, ConstantAddressProvenance::AddressFragment));
      Block.Ops.push_back(Store);
    }
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    Functions.push_back(std::move(Function));
  }
  // A run whose output names nothing it leaves out, as a lift's, fails.
  testing::internal::CaptureStderr();
  auto Failed = PipelineTestPeer::emitShards(Functions, Context, Image, 2);
  (void)testing::internal::GetCapturedStderr();
  EXPECT_EQ(Failed.Module, nullptr);
  EXPECT_TRUE(Failed.LeftOut.empty());

  testing::internal::CaptureStderr();
  auto Result = PipelineTestPeer::emitShards(Functions, Context, Image, 2,
                                             /*OmitRefusedBodies=*/true);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();
  ASSERT_NE(Result.Module, nullptr) << Result.Error << Diagnostic;
  EXPECT_TRUE(Result.Error.empty());
  EXPECT_FALSE(llvm::verifyModule(*Result.Module, &llvm::errs()));
  ASSERT_EQ(Result.LeftOut.size(), 1u) << Diagnostic;
  EXPECT_EQ(Result.LeftOut[0].first, 3u);
  EXPECT_NE(Result.LeftOut[0].second.find("source_3"), std::string::npos)
      << Result.LeftOut[0].second;
  EXPECT_NE(Result.LeftOut[0].second.find("refusing stale-address fallback"),
            std::string::npos)
      << Result.LeftOut[0].second;
  for (unsigned I = 0; I < 8; ++I) {
    const llvm::Function *Function =
        Result.Module->getFunction(Functions[I].Name);
    // Linking drops the left-out body's declaration where nothing calls it.
    if (I == 3) {
      EXPECT_TRUE(!Function || Function->isDeclaration());
      continue;
    }
    ASSERT_NE(Function, nullptr) << Functions[I].Name;
    EXPECT_FALSE(Function->isDeclaration()) << Functions[I].Name;
  }
}

// A body whose contract the emitter refuses before any module, as an
// original SP memory operation with an image, fails only the shard that
// writes it, and a unit that names what it leaves out keeps the others.
TEST(PipelineOutcome, RealShardedEmissionLeavesOutABodyItsContractRefuses) {
  llvm::LLVMContext Context;
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  // `ldapurb w8, [sp, #0x20]`: an RCpc read keeps its original SP word,
  // which no reconstructed source frame supplies.
  constexpr uint32_t OrderedStackRead = 0x194203e8;
  const auto Instruction = *aarch64AddressMemoryInstruction(OrderedStackRead);
  ASSERT_TRUE(Instruction.stackBase());
  std::vector<MedFunc> Functions;
  for (unsigned I = 0; I < 8; ++I) {
    MedFunc Function =
        sourceFunction(0x1000 + I * 16, "source_" + std::to_string(I));
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    if (I == 3) {
      MedVar Stack;
      Stack.Kind = MedVar::Reg;
      Stack.TheArch = Arch::AArch64;
      Stack.Id = 1;
      Stack.Size = kAddressMemoryPointerBytes;
      Stack.RegOff = a64reg::SP;
      MedVar Byte;
      Byte.Kind = MedVar::Temp;
      Byte.TheArch = Arch::AArch64;
      Byte.Id = 2;
      Byte.Size = static_cast<uint16_t>(Instruction.accessBytes());
      MedOp Read;
      Read.Opcode = NdOp::INTRINSIC;
      Read.Addr = Function.Entry;
      Read.Output = Byte;
      Read.addInput(
          MedVar::makeConst(static_cast<uint64_t>(Instruction.operation().Id),
                            kAddressMemoryIdentifierBytes));
      Read.addInput(Stack);
      Read.addInput(MedVar::makeConst(0, Instruction.indexBytes()));
      Read.addInput(
          MedVar::makeConst(OrderedStackRead, kAddressMemoryWordBytes));
      ASSERT_TRUE(aarch64AddressMemoryMedShapeIsValid(Read));
      Block.Ops.push_back(Read);
    }
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    Functions.push_back(std::move(Function));
  }
  // A run whose output names nothing it leaves out, as a lift's, fails with
  // the refusal, from the one shard that writes the body.
  auto Failed = PipelineTestPeer::emitShards(Functions, Context, Image, 2);
  EXPECT_EQ(Failed.Module, nullptr);
  EXPECT_TRUE(Failed.LeftOut.empty());
  EXPECT_EQ(llvm::StringRef(Failed.Error)
                .count(kAddressMemoryImmediateImageUnsupported),
            1u)
      << Failed.Error;

  auto Result = PipelineTestPeer::emitShards(Functions, Context, Image, 2,
                                             /*OmitRefusedBodies=*/true);
  ASSERT_NE(Result.Module, nullptr) << Result.Error;
  EXPECT_TRUE(Result.Error.empty());
  EXPECT_FALSE(llvm::verifyModule(*Result.Module, &llvm::errs()));
  ASSERT_EQ(Result.LeftOut.size(), 1u);
  EXPECT_EQ(Result.LeftOut[0].first, 3u);
  EXPECT_EQ(Result.LeftOut[0].second, kAddressMemoryImmediateImageUnsupported);
  for (unsigned I = 0; I < 8; ++I) {
    const llvm::Function *Function =
        Result.Module->getFunction(Functions[I].Name);
    if (I == 3) {
      EXPECT_TRUE(!Function || Function->isDeclaration());
      continue;
    }
    ASSERT_NE(Function, nullptr) << Functions[I].Name;
    EXPECT_FALSE(Function->isDeclaration()) << Functions[I].Name;
  }
}

// Two functions with one name, as a Mach-O's local symbols of one generic in
// two codegen units, each keep their own body, in one module or in two
// shards: the later one takes its entry address after the name.
TEST(PipelineOutcome, ShardedEmissionKeepsBothFunctionsOfOneName) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  std::vector<MedFunc> Functions;
  for (unsigned I = 0; I < 8; ++I) {
    MedFunc Function = sourceFunction(
        0x1000 + I * 16,
        I == 1 || I == 6 ? "twin" : "source_" + std::to_string(I));
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    Functions.push_back(std::move(Function));
  }
  const auto OriginalVA = [](const llvm::Function &Function) -> uint64_t {
    auto Address = rewrite_source::getOriginalVA(Function);
    if (!Address) {
      llvm::consumeError(Address.takeError());
      return 0;
    }
    return Address->value_or(0);
  };
  for (unsigned Threads : {1u, 2u}) {
    SCOPED_TRACE(Threads);
    llvm::LLVMContext Context;
    auto Result =
        PipelineTestPeer::emitShards(Functions, Context, Image, Threads);
    ASSERT_NE(Result.Module, nullptr) << Result.Error;
    EXPECT_FALSE(llvm::verifyModule(*Result.Module, &llvm::errs()));
    const llvm::Function *First = Result.Module->getFunction("twin");
    const llvm::Function *Second = Result.Module->getFunction("twin_1060");
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);
    EXPECT_FALSE(First->isDeclaration());
    EXPECT_FALSE(Second->isDeclaration());
    EXPECT_EQ(OriginalVA(*First), 0x1010u);
    EXPECT_EQ(OriginalVA(*Second), 0x1060u);
  }
}

// A real symbol may already use the address suffix chosen for a duplicate.
// Preserve each body's value and the caller's target even across chained
// collisions, reversed input order and independent LLVM shards.
TEST(PipelineOutcome, FunctionAddressSuffixesDoNotAliasExistingNames) {
  const auto OriginalVA = [](const llvm::Function &Function) -> uint64_t {
    auto Address = rewrite_source::getOriginalVA(Function);
    if (!Address) {
      llvm::consumeError(Address.takeError());
      return 0;
    }
    return Address->value_or(0);
  };
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (bool Reverse : {false, true}) {
        BinaryImage Image;
        Image.Arch = Architecture;
        Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                         ? Bitness::Bits32
                         : Bitness::Bits64;
        Image.Format = Format;
        std::vector<MedFunc> Functions;
        for (unsigned I = 0; I != 8; ++I) {
          std::string Name = "source_" + std::to_string(I);
          if (I == 0)
            Name = "twin_1060_1060";
          else if (I == 2)
            Name = "twin_1060";
          else if (I == 1 || I == 6)
            Name = "twin";
          MedFunc Function = sourceFunction(0x1000 + I * 16, std::move(Name));
          Function.ReturnType = NdType::makeInt(Image.getPointerSize());
          MedBlock Block;
          Block.Id = 0;
          Block.StartAddr = Function.Entry;
          const auto &Registers = getTargetRegInfo(Architecture);
          MedVar Value;
          Value.Kind = MedVar::Reg;
          Value.TheArch = Architecture;
          Value.Id = 1;
          Value.SSAVer = 1;
          Value.Size = Image.getPointerSize();
          Value.RegOff = Registers.IntReturnReg;
          MedOp Produce;
          Produce.Opcode = I == 7 ? NdOp::CALL : NdOp::COPY;
          Produce.Addr = Function.Entry;
          Produce.Output = Value;
          Produce.addInput(MedVar::makeConst(I == 7 ? 0x1060 : I + 1,
                                             Image.getPointerSize()));
          Block.Ops.push_back(Produce);
          MedOp Return;
          Return.Opcode = NdOp::RETURN;
          if (Registers.ReturnOperandIsValue)
            Return.addInput(Value);
          Block.Ops.push_back(Return);
          Function.Blocks.push_back(std::move(Block));
          Functions.push_back(std::move(Function));
        }
        if (Reverse)
          std::reverse(Functions.begin(), Functions.end());
        for (unsigned Threads : {1u, 2u}) {
          SCOPED_TRACE(::testing::Message()
                       << unsigned(Architecture) << ':' << unsigned(Format)
                       << ':' << Reverse << ':' << Threads);
          llvm::LLVMContext Context;
          auto Result =
              PipelineTestPeer::emitShards(Functions, Context, Image, Threads);
          ASSERT_NE(Result.Module, nullptr) << Result.Error;
          ASSERT_FALSE(llvm::verifyModule(*Result.Module, &llvm::errs()));
          for (unsigned I = 0; I != 8; ++I) {
            const llvm::Function *Found = nullptr;
            for (const llvm::Function &Emitted : *Result.Module) {
              if (OriginalVA(Emitted) != 0x1000 + I * 16)
                continue;
              ASSERT_EQ(Found, nullptr);
              Found = &Emitted;
            }
            ASSERT_NE(Found, nullptr) << I;
            ASSERT_EQ(Found->size(), 1u);
            if (I != 7) {
              const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
                  Found->getEntryBlock().getTerminator());
              ASSERT_NE(Return, nullptr);
              const auto *Value = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                  Return->getReturnValue());
              ASSERT_NE(Value, nullptr);
              EXPECT_EQ(Value->getZExtValue(), I + 1);
            } else {
              unsigned Calls = 0;
              for (const llvm::Instruction &Instruction :
                   Found->getEntryBlock()) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
                if (!Call)
                  continue;
                const auto *Callee = llvm::dyn_cast<llvm::Function>(
                    Call->getCalledOperand()->stripPointerCasts());
                ASSERT_NE(Callee, nullptr);
                EXPECT_EQ(OriginalVA(*Callee), 0x1060u);
                ++Calls;
              }
              EXPECT_EQ(Calls, 1u);
            }
          }
        }
      }
    }
  }
}

MedFunc unhandledIntrinsicFunction(Arch Architecture, uint16_t OutputSize,
                                   va_t Entry = 0x1000) {
  MedFunc Function = sourceFunction(Entry, "unsupported_intrinsic");
  MedVar Output;
  Output.Kind = MedVar::Temp;
  Output.TheArch = Architecture;
  Output.Id = 1;
  Output.Size = OutputSize;
  MedOp Operation;
  Operation.Opcode = NdOp::INTRINSIC;
  Operation.Addr = Entry;
  Operation.addInput(MedVar::makeConst(0xffff, 2));
  if (OutputSize)
    Operation.Output = Output;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  if (OutputSize) {
    Return.addInput(Output);
    Function.ReturnType = NdType::makeInt(OutputSize);
  }
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Entry;
  Block.Ops = {Operation, Return};
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

TEST(PipelineOutcome, UnhandledIntrinsicNeverPublishesZeroOrAnEmptyEffect) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (uint16_t Size : {0, 4}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << ':' << Size);
      llvm::LLVMContext Context;
      MedLLVMEmitter Emitter;
      auto Function = unhandledIntrinsicFunction(Architecture, Size);
      std::unique_ptr<llvm::Module> Module;
      try {
        Module = Emitter.emit({Function}, Context, "unsupported", Architecture);
        ADD_FAILURE() << "an unsupported intrinsic published a module";
      } catch (const std::runtime_error &Error) {
        EXPECT_NE(std::string(Error.what()).find("unsupported LLVM intrinsic"),
                  std::string::npos);
        EXPECT_NE(std::string(Error.what()).find("65535"), std::string::npos);
      }
      EXPECT_EQ(Module, nullptr);
      EXPECT_EQ(Emitter.unhandledValueIntrinsicCount(), Size ? 1u : 0u);

      // Reuse the same emitter after refusal. No old LLVM values or refusal
      // counter may contaminate a later, valid generation.
      auto &Operation = Function.Blocks[0].Ops[0];
      Operation.Opcode = NdOp::COPY;
      Operation.NumInputs = 0;
      if (Size)
        Operation.addInput(MedVar::makeConst(7, Size));
      else
        Function.Blocks[0].Ops.erase(Function.Blocks[0].Ops.begin());
      Module = Emitter.emit({Function}, Context, "supported", Architecture);
      ASSERT_NE(Module, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      EXPECT_EQ(Emitter.unhandledValueIntrinsicCount(), 0u);
    }
  }
}

TEST(PipelineOutcome,
     UnhandledIntrinsicRefusalsAgreeAcrossSingleAndShardedEmission) {
  ScopedThreadCount Threads(2);
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (unsigned Count : {1u, 8u})
      for (bool NoOpt : {false, true})
        for (bool SourceProjection : {false, true})
          for (uint16_t Size : {0, 4}) {
            SCOPED_TRACE(::testing::Message()
                         << unsigned(Architecture) << ':' << Count << ':'
                         << NoOpt << ':' << SourceProjection << ':' << Size);
            llvm::LLVMContext Context;
            BinaryImage Image;
            Image.Arch = Architecture;
            Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                             ? Bitness::Bits32
                             : Bitness::Bits64;
            Image.Format = BinaryFormat::ELF;
            PipelineResult Result;
            for (unsigned Index = 0; Index != Count; ++Index) {
              auto Function = unhandledIntrinsicFunction(
                  Architecture, Size, 0x1000 + Index * 0x100);
              Function.Name += "_" + std::to_string(Index);
              if (Index + 1 != Count) {
                Function.Blocks[0].Ops.erase(Function.Blocks[0].Ops.begin());
                Function.Blocks[0].Ops[0].NumInputs = 0;
                Function.ReturnType = NdType::makeVoid();
              }
              Result.MedFuncs.push_back(std::move(Function));
            }
            PipelineOptions Options;
            Options.LiftMode = true;
            Options.SourceProjection = SourceProjection;
            Options.RetainLLVMSourceObservations = true;
            Options.NoOpt = NoOpt;
            EXPECT_FALSE(
                PipelineTestPeer::runLift(Image, Context, Options, Result));
            EXPECT_EQ(Result.LlvmModule, nullptr);
            EXPECT_EQ(Result.LLVMSources, nullptr);
            EXPECT_FALSE(Result.LLVMVerifierFailed);
            EXPECT_NE(Result.Error.find("unsupported LLVM intrinsic"),
                      std::string::npos)
                << Result.Error;
            if (Count == 8)
              EXPECT_NE(Result.Error.find("LLVM shard "), std::string::npos);
          }
}

TEST(PipelineOutcome, SourceEHRefusalsAgreeAcrossSingleAndShardedEmission) {
  ScopedThreadCount Threads(2);
  for (const auto &[Architecture, Format] :
       {std::pair{Arch::X86, BinaryFormat::COFF},
        std::pair{Arch::X64, BinaryFormat::ELF},
        std::pair{Arch::X64, BinaryFormat::MachO},
        std::pair{Arch::AArch64, BinaryFormat::ELF},
        std::pair{Arch::AArch64, BinaryFormat::MachO}}) {
    for (unsigned Count : {1u, 8u}) {
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Architecture) << ':' << unsigned(Format) << ':'
                     << Count << ':' << NoOpt);
        llvm::LLVMContext Context;
        BinaryImage Image;
        Image.Arch = Architecture;
        Image.Bits =
            Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
        Image.Format = Format;
        PipelineResult Result;
        for (unsigned I = 0; I != Count; ++I) {
          auto Function =
              sourceFunction(0x1000 + I * 0x100, "source_" + std::to_string(I));
          MedBlock Block;
          Block.Id = 0;
          Block.StartAddr = Function.Entry;
          Block.EndAddr = Function.Entry + 8;
          MedOp Call;
          Call.Opcode = NdOp::CALL;
          Call.Addr = Function.Entry;
          Call.addInput(MedVar::makeConst(0x9000, Image.getPointerSize()));
          Block.Ops.push_back(Call);
          MedOp Return;
          Return.Opcode = NdOp::RETURN;
          Return.Addr = Function.Entry + 4;
          Block.Ops.push_back(Return);
          Function.Blocks.push_back(std::move(Block));
          if (I == 0) {
            ExceptionFunction EH;
            EH.CodeRange = {Function.Entry, Function.Entry + 0x40};
            EH.ParseStatus = ExceptionParseStatus::Complete;
            if (Architecture == Arch::X86) {
              EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
              EH.Personality = ExceptionPersonality::ExceptHandler3;
              EH.Registration.emplace();
              EH.Registration->Scopes.push_back(
                  {-1, 0, Function.Entry + 0x20, true});
            } else {
              EH.Encoding = ExceptionEncoding::DwarfFDE;
              EH.Personality = ExceptionPersonality::GxxPersonalityV0;
              EH.Itanium.emplace();
              EH.Itanium->IsCallSiteAddressForm = true;
              ItaniumCallSite Site;
              Site.GuardedRange = {Function.Entry, Function.Entry + 8};
              Site.LandingPadVA = Function.Entry + 0x20;
              EH.Itanium->CallSites.push_back(Site);
            }
            Function.ExceptionMetadata = std::move(EH);
          }
          Result.MedFuncs.push_back(std::move(Function));
        }
        const auto OriginalFunctions = Result.MedFuncs;
        PipelineOptions Options;
        Options.LiftMode = Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        ASSERT_TRUE(PipelineTestPeer::runLift(Image, Context, Options, Result))
            << Result.Error;
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(Result.LLVMVerifierFailed);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        auto Contract = exception_rewrite::validateExceptionRewriteContracts(
            *Result.LlvmModule);
        ASSERT_FALSE(bool(Contract));
        llvm::consumeError(Contract.takeError());
        auto *Function = Result.LlvmModule->getFunction("source_0");
        ASSERT_NE(Function, nullptr);
        size_t Calls = 0;
        for (const auto &Block : *Function)
          for (const auto &Instruction : Block)
            Calls += llvm::isa<llvm::CallInst>(Instruction);
        EXPECT_EQ(Calls, 1u);
        EXPECT_NE(Function->getMetadata(exception_rewrite::FunctionAttachment),
                  nullptr);

        // Source projection cannot bypass the patch optimizer's native-EH
        // requirement, even if a caller supplies both mode flags.
        if (Count == 1 && !NoOpt) {
          PipelineResult Patch;
          Patch.MedFuncs = OriginalFunctions;
          Options.PatchMode = true;
          EXPECT_FALSE(
              PipelineTestPeer::runLift(Image, Context, Options, Patch));
          EXPECT_NE(Patch.Error.find("input-invalid"), std::string::npos)
              << Patch.Error;
        }
      }
    }
  }
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
  EXPECT_NE(Source.find("0x2A"), std::string::npos);
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
