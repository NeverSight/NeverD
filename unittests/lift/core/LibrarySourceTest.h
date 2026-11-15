//===- LibrarySourceTest.h - Published library source coverage -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_LIBRARYSOURCETEST_H
#define NEVERD_TEST_LIBRARYSOURCETEST_H

#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

inline void verifyLibrarySource(const neverd::BinaryImage &Image,
                                neverd::DebugContext *Debug,
                                const neverd::sigs::SignatureDB &DB,
                                llvm::StringRef Name, llvm::StringRef Rule,
                                llvm::StringRef CallerArgument = "arg1",
                                bool AnalysisOnlyEH = false,
                                std::optional<neverd::va_t> Address = {},
                                bool ExpectMapped = true) {
  using namespace neverd;
  const auto *Symbol = Image.findSymbol(Name);
  ASSERT_TRUE(Symbol || Address) << Name.str();
  const auto Entry = Address.value_or(Symbol ? Symbol->Addr : 0);
  for (unsigned Route = 0; Route != 3; ++Route) {
    const bool LLVM = Route != 0;
    const bool NoOpt = Route == 2;
    SCOPED_TRACE(Name.str() + (LLVM ? " LLVMC " : " HighC ") +
                 (NoOpt ? "no-opt " : "") + Rule.str());
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LibraryFeatures = &DB.featurePacks();
    Options.OnlyFunctionEntries.insert(Entry);
    Options.LiftMode = LLVM;
    Options.SourceProjection = LLVM;
    Options.NoOpt = NoOpt || AnalysisOnlyEH;
    auto Result = Pipeline().run(Image, Context, Options, Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    if (LLVM && !NoOpt && AnalysisOnlyEH) {
      // The session source API uses this existing transaction: unsupported
      // rewrite contracts retain EH metadata and permit only SSA promotion.
      const auto Optimized = Pipeline::optimizeOrPromoteModule(
          *Result.LlvmModule, Result.LLVMSources.get());
      ASSERT_NE(Optimized.Stop, OptimizationStopReason::VerificationFailed);
    }
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    CSourceMap Map;
    Map.Recognitions = &Result.LibraryRecognitions;
    Map.HighSources = &Result.HighSources;
    COptions.LLVMSources = Result.LLVMSources.get();
    std::string Ordinary, Mapped;
    llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      LLVMCEmitter Emitter;
      ASSERT_TRUE(
          Emitter.emit(*Result.LlvmModule, OrdinaryOS, COptions, Debug));
      COptions.SourceMap = &Map;
      ASSERT_TRUE(Emitter.emit(*Result.LlvmModule, MappedOS, COptions, Debug));
    } else {
      HighCEmitter Emitter;
      ASSERT_TRUE(Emitter.emit(Result.HighFuncs, OrdinaryOS, COptions, Debug));
      COptions.SourceMap = &Map;
      ASSERT_TRUE(Emitter.emit(Result.HighFuncs, MappedOS, COptions, Debug));
    }
    EXPECT_EQ(Ordinary, Mapped);
    bool Found = false;
    bool Recognized = false;
    for (const auto &Region : Map.Regions) {
      const auto &Recognition = Result.LibraryRecognitions[Region.Recognition];
      if (Recognition.Rule != Rule)
        continue;
      Recognized = true;
      Found |= Region.Mapped;
      for (const auto &Span : Region.Spans) {
        ASSERT_LT(Span.Begin, Span.End);
        ASSERT_LE(Span.End, Mapped.size());
        // Every accessor wrapper deliberately combines its result with this
        // independent argument. A library fold must leave that work visible.
        if (!CallerArgument.empty())
          EXPECT_EQ(Mapped.substr(Span.Begin, Span.End - Span.Begin)
                        .find(Debug && CallerArgument == "arg1"
                                  ? "salt"
                                  : CallerArgument.str()),
                    std::string::npos);
      }
    }
    EXPECT_TRUE(Recognized);
    EXPECT_EQ(Found, ExpectMapped) << Mapped;
  }
}
#endif
