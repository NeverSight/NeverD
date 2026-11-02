//===- LibcLibraryRecognitionTests.cpp - Stripped libc evidence ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LibrarySourceTest.h"
#include "gtest/gtest.h"

#include "neverd/loader/Loader.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>

using namespace neverd;
using namespace neverd::sigs;

TEST(LibcLibraryRecognition, StrippedPublishedFunctionsAndLookalikes) {
  const auto Root = std::filesystem::path(NEVERD_LIBRARY_LIBC_FIXTURE_DIR);
  const auto Path = Root / "library-memory.elf";
  auto Reader = Loader::create(Path);
  ASSERT_TRUE(Reader);
  auto Loaded = Reader->load(Path);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  auto &Image = *Loaded;
  SignatureDB DB;
  auto Error =
      DB.loadFeaturePack(std::filesystem::path(NEVERD_LIBRARY_FEATURE_DIR) /
                         "musl-1.2.5-x64-clang22-memory.json");
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  auto Data = llvm::MemoryBuffer::getFile(
      (Root / "library-memory.truth.json").string());
  ASSERT_TRUE(Data);
  auto Truth = llvm::json::parse((*Data)->getBuffer());
  ASSERT_TRUE(static_cast<bool>(Truth)) << llvm::toString(Truth.takeError());
  ASSERT_TRUE(Truth->getAsArray());
  std::vector<uint64_t> Entries;
  std::map<va_t, std::string> Expected;
  for (const auto &Entry : *Truth->getAsArray()) {
    const auto *Object = Entry.getAsObject();
    ASSERT_NE(Object, nullptr);
    auto Address = Object->getString("address");
    ASSERT_TRUE(Address);
    va_t VA;
    ASSERT_FALSE(Address->getAsInteger(0, VA));
    Entries.push_back(VA);
    if (Object->getBoolean("from_library").value_or(false)) {
      const auto *Names = Object->getArray("names");
      ASSERT_NE(Names, nullptr);
      ASSERT_EQ(Names->size(), 1u);
      ASSERT_TRUE(Names->front().getAsString());
      Expected.emplace(VA, "musl." + Names->front().getAsString()->str());
    }
  }
  const auto Matches = DB.recognizeFeatureBytes(
      Image, Entries, [](const auto &, const auto &Rule, va_t) {
        return Rule.Identity == LibraryFeatureIdentity::ByteSignature
                   ? "whole-byte-pattern"
                   : "";
      });
  ASSERT_EQ(Expected.size(), 5u);
  ASSERT_EQ(Matches.size(), Expected.size());
  for (const auto &Match : Matches) {
    ASSERT_TRUE(Expected.contains(Match.Function));
    EXPECT_EQ(Match.Rule, Expected.at(Match.Function));
    EXPECT_TRUE(Match.LinkageName.empty());
    EXPECT_EQ(Match.Scope, LibraryFeatureScope::WholeFunction);
    EXPECT_GT(Match.ByteLength, 0u);
    // This exact memmove has a tail shared with memcpy. A single-entry CFG
    // expands that tail; its proven identity remains annotated but unfolded.
    verifyLibrarySource(Image, nullptr, DB, Match.Rule, Match.Rule, "", false,
                        Match.Function, Match.Rule != "musl.memmove");
  }
  EXPECT_TRUE(
      DB.recognizeFeatureBytes(
            Image, Entries, [](const auto &, const auto &, va_t) { return ""; })
          .empty());
  auto Conflicting = DB.featurePacks();
  auto Copy = Conflicting.begin()->second;
  Copy.Id += "-other-source";
  Conflicting.emplace(Copy.Id, std::move(Copy));
  EXPECT_TRUE(
      recognizeLibraryFeatureBytes(
          Image, Conflicting, Entries,
          [](const auto &, const auto &, va_t) { return "whole-byte-pattern"; })
          .empty());

  for (unsigned Route = 0; Route != 3; ++Route) {
    SCOPED_TRACE(Route);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LibraryFeatures = &DB.featurePacks();
    Options.LiftMode = Route != 0;
    Options.SourceProjection = Route != 0;
    Options.NoOpt = Route == 2;
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    std::map<va_t, std::string> Whole;
    for (const auto &R : Result.LibraryRecognitions)
      if (R.Scope == LibraryFeatureScope::WholeFunction)
        Whole.emplace(R.Function, R.Rule);
    EXPECT_EQ(Whole, Expected);
    if (Route) {
      ASSERT_TRUE(Result.LLVMSources);
      for (const auto &F : Result.MedFuncs) {
        const auto At = Result.LLVMSources->Functions.find(F.Entry);
        ASSERT_NE(At, Result.LLVMSources->Functions.end());
        const auto *Bound = llvm::dyn_cast_or_null<llvm::Function>(At->second);
        ASSERT_NE(Bound, nullptr);
        EXPECT_EQ(Bound->getParent(), Result.LlvmModule.get());
      }
    }
    CSourceMap SourceMap;
    SourceMap.Recognitions = &Result.LibraryRecognitions;
    SourceMap.HighSources = &Result.HighSources;
    SourceMap.LLVMSources = Result.LLVMSources.get();
    CEmitterOptions EmitOptions;
    EmitOptions.TheArch = Image.Arch;
    EmitOptions.Format = Image.Format;
    EmitOptions.Image = &Image;
    EmitOptions.SourceMap = &SourceMap;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    if (Route)
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, EmitOptions));
    else
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, EmitOptions));
    unsigned Calls = 0, MappedCalls = 0;
    std::map<va_t, std::string> MappedCallees;
    for (const auto &Region : SourceMap.Regions) {
      const auto &R = Result.LibraryRecognitions[Region.Recognition];
      if (R.Scope != LibraryFeatureScope::CallSite)
        continue;
      ++Calls;
      MappedCalls += Region.Mapped;
      // The memmove body itself owns its memcpy tail call. Its whole-function
      // view subsumes that nested fold; independent caller sites still map.
      EXPECT_EQ(Region.Mapped, !Expected.contains(R.Function))
          << R.Rule << " @ " << R.Function;
      ASSERT_TRUE(R.Callee);
      if (Region.Mapped)
        MappedCallees.emplace(*R.Callee, R.Rule);
      EXPECT_TRUE(Expected.contains(*R.Callee));
      EXPECT_NE(R.Function, *R.Callee);
      ASSERT_EQ(R.Occurrences.size(), 1u);
      bool OriginalCall = false;
      for (const auto &F : Result.MedFuncs)
        if (F.Entry == R.Function)
          for (const auto &B : F.Blocks)
            for (const auto &O : B.Ops)
              if (O.Addr == R.Occurrences.front().Address &&
                  O.OriginSeq == R.Occurrences.front().Sequence)
                OriginalCall |= O.Opcode == NdOp::CALL && O.NumInputs &&
                                O.Inputs[0].isConst() &&
                                O.Inputs[0].ConstVal == *R.Callee;
      EXPECT_TRUE(OriginalCall);
    }
    EXPECT_GE(Calls, Expected.size());
    EXPECT_GE(MappedCalls, Expected.size());
    EXPECT_EQ(MappedCallees, Expected);
  }
}
