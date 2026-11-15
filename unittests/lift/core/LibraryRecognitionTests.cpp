//===- LibraryRecognitionTests.cpp - Library recognition evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LibrarySourceTest.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/debug/DWARFLoader.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedLibraryRecognition.h"
#include "neverd/loader/Loader.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>

using namespace neverd;
using namespace neverd::sigs;

namespace {

constexpr const char *Libcxx =
    "libcxx-23git-arm64-macos-abi1-alternate-clang22";

class LibraryRecognitionTest : public testing::Test {
protected:
  void SetUp() override {
    auto Error =
        DB.loadFeaturePack(std::filesystem::path(NEVERD_LIBRARY_FEATURE_DIR) /
                           (std::string(Libcxx) + ".json"));
    ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
    auto Path = std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) /
                "accessors-inline.o";
    auto Reader = Loader::create(Path);
    ASSERT_TRUE(Reader);
    auto Loaded = Reader->load(Path);
    ASSERT_TRUE(static_cast<bool>(Loaded))
        << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
  }

  const LibraryFeatureRule &rule(llvm::StringRef Name) {
    const auto &Rules = DB.featurePacks().at(Libcxx).Rules;
    auto I = llvm::find_if(Rules, [&](const auto &R) { return R.Id == Name; });
    EXPECT_NE(I, Rules.end());
    return *I;
  }

  MedFunc lift(llvm::StringRef Name) {
    const Symbol *S = Image.findSymbol(Name);
    EXPECT_NE(S, nullptr) << Name.str();
    if (!S)
      return {};
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.DumpMed = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries.insert(S->Addr);
    auto Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
    EXPECT_EQ(Result.MedFuncs.size(), 1u);
    return Result.MedFuncs.size() == 1 ? std::move(Result.MedFuncs.front())
                                       : MedFunc{};
  }

  auto analyze(const MedFunc &F,
               llvm::ArrayRef<MedLibraryReceiver> Receivers = {},
               size_t Budget = 250000) {
    return recognizeMedLibraryOperations(F, Image, DB.featurePacks(), Receivers,
                                         Budget);
  }

  SignatureDB DB;
  BinaryImage Image;
};

std::string body(const MedFunc &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  auto Var = [&](const MedVar &V) {
    OS << int(V.Kind) << ':' << V.Id << ':' << V.SSAVer << ':' << V.Size << ':';
    if (V.isConst())
      OS << V.ConstVal;
    else if (V.Kind == MedVar::Reg || V.Kind == MedVar::Param)
      OS << V.RegOff;
  };
  OS << F.Entry << ':' << F.Name << ':' << F.OriginalSize;
  for (const MedBlock &B : F.Blocks) {
    OS << " block " << B.Id << ':' << B.StartAddr << ':' << B.EndAddr;
    for (int S : B.Succs)
      OS << " ->" << S;
    for (const MedOp &O : B.Ops) {
      OS << "\n op " << ndOpName(O.Opcode) << ':' << O.Addr << ':'
         << O.OriginSeq << ':' << int(O.MemoryOrdering) << ':' << O.Dead;
      Var(O.Output);
      for (unsigned I = 0; I < O.NumInputs; ++I)
        Var(O.Inputs[I]);
    }
  }
  return Text;
}

TEST_F(LibraryRecognitionTest,
       CompiledVectorDataHasTraceableWholeFunctionEvidence) {
  const auto &Rule = rule("libcxx.vector-u32.data");
  ASSERT_EQ(Rule.WholeFunctionSymbols.size(), 1u);
  MedFunc F = lift(Rule.WholeFunctionSymbols.front());
  const std::string Before = body(F);
  const size_t Symbols = Image.Symbols.size();
  const auto Result = analyze(F);
  ASSERT_FALSE(Result.BudgetExhausted);
  ASSERT_EQ(Result.Matches.size(), 1u);
  const auto &Match = Result.Matches.front();
  EXPECT_EQ(Match.Rule, Rule.Id);
  EXPECT_EQ(Match.Scope, LibraryFeatureScope::WholeFunction);
  EXPECT_EQ(Match.LinkageName, Rule.WholeFunctionSymbols.front());
  EXPECT_EQ(Match.IdentityEvidence, "stated-member-symbol");
  EXPECT_EQ(Match.ProfileSHA256, DB.featurePacks().at(Libcxx).ProfileSHA256);
  EXPECT_TRUE(Match.Isolated);
  EXPECT_FALSE(Match.Occurrences.empty());
  for (const auto &Occurrence : Match.Occurrences) {
    EXPECT_GE(Occurrence.Sequence, 0);
    EXPECT_GE(Occurrence.Address, F.Entry);
  }
  EXPECT_EQ(body(F), Before);
  EXPECT_EQ(Image.Symbols.size(), Symbols);
}

TEST_F(LibraryRecognitionTest,
       SameCompiledShapeWithoutIndependentIdentityAbstains) {
  const auto &Rule = rule("libcxx.vector-u32.data");
  auto F = lift(Rule.WholeFunctionSymbols.front());
  for (Symbol &S : Image.Symbols)
    if (S.Addr == F.Entry)
      S.Origin = NameOrigin::Analysis;
  std::erase_if(Image.Exports,
                [&](const auto &E) { return E.Addr == F.Entry; });
  EXPECT_TRUE(analyze(F).Matches.empty());
  // An analysis/user display string cannot become receiver evidence.
  F.Name = Rule.WholeFunctionSymbols.front();
  F.DebugName = Rule.ReceiverType;
  EXPECT_TRUE(analyze(F).Matches.empty());
}

TEST_F(LibraryRecognitionTest, WrapperNeverBecomesAStandaloneLibraryCallee) {
  const auto &Rule = rule("libcxx.vector-u32.data");
  auto F = lift("_nd_vector_u32_data_inline");
  EXPECT_TRUE(analyze(F).Matches.empty());
  MedLibraryReceiver Receiver{getTargetRegInfo(Image.Arch).IntParamRegs.front(),
                              Rule.ReceiverType, 24,
                              "authenticated-debug-parameter"};
  const auto Result = analyze(F, {Receiver});
  ASSERT_EQ(Result.Matches.size(), 1u);
  EXPECT_EQ(Result.Matches.front().Rule, Rule.Id);
  EXPECT_EQ(Result.Matches.front().Scope,
            LibraryFeatureScope::InlineExpression);
  EXPECT_TRUE(Result.Matches.front().LinkageName.empty());
  Receiver.Type = "user::same_layout";
  EXPECT_TRUE(analyze(F, {Receiver}).Matches.empty());
  Receiver.Type = Rule.ReceiverType;
  Receiver.ObjectBytes = 32;
  EXPECT_TRUE(analyze(F, {Receiver}).Matches.empty());
}

TEST_F(LibraryRecognitionTest, OrderedLoadCannotMatch) {
  const auto &Rule = rule("libcxx.vector-u32.data");
  auto F = lift(Rule.WholeFunctionSymbols.front());
  bool Changed = false;
  for (auto &B : F.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::LOAD) {
        O.MemoryOrdering = NdMemoryOrdering::Acquire;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  EXPECT_TRUE(analyze(F).Matches.empty());
}

TEST_F(LibraryRecognitionTest, ExhaustionAndUnversionedIRPublishNothing) {
  const auto &Rule = rule("libcxx.vector-u32.data");
  auto F = lift(Rule.WholeFunctionSymbols.front());
  const auto Result = analyze(F, {}, 1);
  EXPECT_TRUE(Result.BudgetExhausted);
  EXPECT_TRUE(Result.Matches.empty());
  F.SkippedSSA = true;
  EXPECT_TRUE(analyze(F).Matches.empty());
}

TEST_F(LibraryRecognitionTest, ConflictingProfilesDoNotChooseByLoadOrder) {
  const auto &Rule = rule("libcxx.vector-u32.data");
  auto F = lift(Rule.WholeFunctionSymbols.front());
  auto Packs = DB.featurePacks();
  auto Other = Packs.at(Libcxx);
  Other.Id = "conflicting-profile";
  Packs.emplace(Other.Id, Other);
  EXPECT_TRUE(recognizeMedLibraryOperations(F, Image, Packs).Matches.empty());
}

TEST_F(LibraryRecognitionTest, CompiledVectorAccessorsMatchDistinctElements) {
  for (llvm::StringRef Element : {"u32", "u64"})
    for (llvm::StringRef Operation : {"size", "empty", "data", "capacity"}) {
      auto Name = ("libcxx.vector-" + Element + "." + Operation).str();
      SCOPED_TRACE(Name);
      const auto &Rule = rule(Name);
      auto F = lift(Rule.WholeFunctionSymbols.front());
      const auto Result = analyze(F);
      ASSERT_EQ(Result.Matches.size(), 1u);
      EXPECT_EQ(Result.Matches.front().Rule, Name);
      EXPECT_EQ(Result.Matches.front().Scope,
                LibraryFeatureScope::WholeFunction);
    }
}

TEST_F(LibraryRecognitionTest, CompiledStringSSOAccessorsMatchBothCharacters) {
  for (const auto &Rule : DB.featurePacks().at(Libcxx).Rules) {
    if (Rule.ReceiverType.find("basic_string<") == std::string::npos)
      continue;
    SCOPED_TRACE(Rule.Id);
    ASSERT_EQ(Rule.WholeFunctionSymbols.size(), 1u);
    const auto F = lift(Rule.WholeFunctionSymbols.front());
    const auto Before = body(F);
    const auto Result = analyze(F);
    ASSERT_FALSE(Result.BudgetExhausted);
    ASSERT_EQ(Result.Matches.size(), 1u) << body(F);
    EXPECT_EQ(Result.Matches.front().Rule, Rule.Id);
    EXPECT_EQ(Result.Matches.front().Scope, LibraryFeatureScope::WholeFunction);
    EXPECT_EQ(body(F), Before);
  }
}

TEST_F(LibraryRecognitionTest,
       CompiledInlineAccessorsHaveAuthenticatedReceivers) {
  const auto Path =
      std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o";
  auto Debug = DWARFDebugContext::load(Path, Image.Format,
                                       DWARFLoadTrust::InImage, Image.Raw);
  ASSERT_TRUE(Debug);
  for (const auto &Rule : DB.featurePacks().at(Libcxx).Rules) {
    SCOPED_TRACE(Rule.Id);
    std::string Wrapper = "_nd_" + Rule.Id.substr(7) + "_inline";
    std::replace(Wrapper.begin(), Wrapper.end(), '-', '_');
    std::replace(Wrapper.begin(), Wrapper.end(), '.', '_');
    if (auto At = Wrapper.find("wchar"); At != std::string::npos)
      Wrapper.replace(At, 5, "wide");
    const auto *Symbol = Image.findSymbol(Wrapper);
    ASSERT_NE(Symbol, nullptr);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.DumpMed = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries.insert(Symbol->Addr);
    Options.LibraryFeatures = &DB.featurePacks();
    auto Result = Pipeline().run(Image, Context, Options, Debug.get());
    ASSERT_TRUE(Result.Success) << Result.Error;
    auto I = llvm::find_if(Result.LibraryRecognitions,
                           [&](const auto &R) { return R.Rule == Rule.Id; });
    EXPECT_NE(I, Result.LibraryRecognitions.end())
        << body(Result.MedFuncs.front());
    if (I != Result.LibraryRecognitions.end()) {
      EXPECT_EQ(I->Scope, LibraryFeatureScope::InlineExpression);
      EXPECT_TRUE(I->LinkageName.empty());
    }
  }
}

TEST_F(LibraryRecognitionTest, SSOTagSignLayoutAndCapacityNearMissesAbstain) {
  for (llvm::StringRef Operation : {"size", "data", "capacity"}) {
    SCOPED_TRACE(Operation.str());
    const auto &Rule = rule(("libcxx.string-char." + Operation).str());
    auto F = lift(Rule.WholeFunctionSymbols.front());
    bool Changed = false;
    for (auto &Block : F.Blocks)
      for (auto &Op : Block.Ops) {
        if (Operation == "data" && Op.Opcode == NdOp::INT_SEXT) {
          Op.Opcode = NdOp::INT_ZEXT;
          Changed = true;
        }
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst()) {
            if (Operation == "size" && Op.Opcode == NdOp::INT_ADD &&
                Op.Inputs[I].ConstVal == 23) {
              Op.Inputs[I].ConstVal = 15;
              Changed = true;
            } else if (Operation == "capacity" && Op.Opcode == NdOp::INT_AND &&
                       Op.Inputs[I].ConstVal == 0x7fffffffffffffffULL) {
              Op.Inputs[I].ConstVal = 0x3fffffffffffffffULL;
              Changed = true;
            }
          }
      }
    ASSERT_TRUE(Changed);
    EXPECT_TRUE(analyze(F).Matches.empty());
  }
}

TEST_F(LibraryRecognitionTest, UnanchoredLoadCannotAuthorizeARegion) {
  const auto &Rule = rule("libcxx.vector-u32.data");
  auto F = lift(Rule.WholeFunctionSymbols.front());
  for (auto &Block : F.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD)
        Op.OriginSeq = -1;
  EXPECT_TRUE(analyze(F).Matches.empty());
}

TEST_F(LibraryRecognitionTest, WrongFieldOffsetDoesNotBorrowAnotherOperation) {
  const auto &Rule = rule("libcxx.vector-u32.capacity");
  auto F = lift(Rule.WholeFunctionSymbols.front());
  bool Changed = false;
  for (auto &B : F.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::INT_ADD && O.NumInputs == 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].ConstVal == 16) {
        O.Inputs[1].ConstVal = 8;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  EXPECT_TRUE(analyze(F).Matches.empty());
}

TEST_F(LibraryRecognitionTest, PipelineUsesAuthenticatedCompiledReceiver) {
  const auto Path =
      std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o";
  auto Debug = DWARFDebugContext::load(Path, Image.Format,
                                       DWARFLoadTrust::InImage, Image.Raw);
  ASSERT_TRUE(Debug);
  ASSERT_TRUE(Debug->hasAuthenticatedFunctionSignatures());
  ASSERT_TRUE(Debug->hasAuthenticatedObjectExtents());
  const auto *Symbol = Image.findSymbol("_nd_vector_u32_data_inline");
  ASSERT_NE(Symbol, nullptr);
  const auto Records =
      Debug->resolveAuthenticatedRecordParameters(Symbol->Addr);
  ASSERT_EQ(Records.size(), 1u);
  SCOPED_TRACE(Records.front().QualifiedType);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LibraryFeatures = &DB.featurePacks();
  Options.OnlyFunctionEntries.insert(Symbol->Addr);
  Options.DumpMed = true;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(Image, Context, Options, Debug.get());
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.LibraryRecognitions.size(), 1u);
  EXPECT_EQ(Result.LibraryRecognitions.front().Rule, "libcxx.vector-u32.data");
  EXPECT_EQ(Result.LibraryRecognitions.front().Scope,
            LibraryFeatureScope::InlineExpression);
  Options.LibraryFeatures = nullptr;
  auto Plain = Pipeline().run(Image, Context, Options, Debug.get());
  ASSERT_TRUE(Plain.Success) << Plain.Error;
  ASSERT_EQ(Plain.MedFuncs.size(), Result.MedFuncs.size());
  EXPECT_EQ(body(Plain.MedFuncs.front()), body(Result.MedFuncs.front()));
}

TEST_F(LibraryRecognitionTest, HighCMapsOriginalWholeAndInlineText) {
  const auto Path =
      std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o";
  auto Debug = DWARFDebugContext::load(Path, Image.Format,
                                       DWARFLoadTrust::InImage, Image.Raw);
  ASSERT_TRUE(Debug);
  const auto &Rule = rule("libcxx.vector-u32.data");
  for (const std::string &Name : {Rule.WholeFunctionSymbols.front(),
                                  std::string("_nd_vector_u32_data_inline")}) {
    SCOPED_TRACE(Name);
    const auto *Symbol = Image.findSymbol(Name);
    ASSERT_NE(Symbol, nullptr);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LibraryFeatures = &DB.featurePacks();
    Options.OnlyFunctionEntries.insert(Symbol->Addr);
    auto Result = Pipeline().run(Image, Context, Options, Debug.get());
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.LibraryRecognitions.size(), 1u);
    ASSERT_FALSE(Result.HighSources.empty());
    HighCEmitter Emitter;
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    std::string Ordinary, Mapped;
    llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
    ASSERT_TRUE(Emitter.emit(Result.HighFuncs, OrdinaryOS, COptions));
    CSourceMap Map;
    Map.Recognitions = &Result.LibraryRecognitions;
    Map.HighSources = &Result.HighSources;
    COptions.SourceMap = &Map;
    ASSERT_TRUE(Emitter.emit(Result.HighFuncs, MappedOS, COptions));
    EXPECT_EQ(Ordinary, Mapped);
    ASSERT_EQ(Map.Regions.size(), 1u);
    ASSERT_TRUE(Map.Regions.front().Mapped) << Mapped;
    ASSERT_FALSE(Map.Regions.front().Spans.empty());
    for (auto Span : Map.Regions.front().Spans) {
      ASSERT_LT(Span.Begin, Span.End);
      ASSERT_LE(Span.End, Mapped.size());
      if (Name == "_nd_vector_u32_data_inline")
        EXPECT_EQ(Mapped.substr(Span.Begin, Span.End - Span.Begin).find('^'),
                  std::string::npos);
    }
  }
}

TEST_F(LibraryRecognitionTest, InlineAccessorsMapBothSourceRoutes) {
  const auto Path =
      std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o";
  auto Debug = DWARFDebugContext::load(Path, Image.Format,
                                       DWARFLoadTrust::InImage, Image.Raw);
  ASSERT_TRUE(Debug);
  for (const auto &Rule : DB.featurePacks().begin()->second.Rules) {
    std::string Wrapper = "_nd_" + Rule.Id.substr(7) + "_inline";
    std::replace(Wrapper.begin(), Wrapper.end(), '-', '_');
    std::replace(Wrapper.begin(), Wrapper.end(), '.', '_');
    if (auto At = Wrapper.find("wchar"); At != std::string::npos)
      Wrapper.replace(At, 5, "wide");
    verifyLibrarySource(Image, Debug.get(), DB, Wrapper, Rule.Id);
  }
}

std::string llvmText(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  Module.print(OS, nullptr);
  return Text;
}

TEST_F(LibraryRecognitionTest, FeatureAnalysisDoesNotChangeLLVMOrMedIR) {
  const auto Path =
      std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o";
  auto Debug = DWARFDebugContext::load(Path, Image.Format,
                                       DWARFLoadTrust::InImage, Image.Raw);
  ASSERT_TRUE(Debug);
  llvm::LLVMContext WithContext, WithoutContext;
  PipelineOptions Options;
  Options.OnlyFunctionEntries.insert(
      Image.findSymbol("_nd_vector_u32_data_inline")->Addr);
  Options.LiftMode = true;
  Options.SourceProjection = true;
  auto Plain = Pipeline().run(Image, WithoutContext, Options, Debug.get());
  Options.LibraryFeatures = &DB.featurePacks();
  auto Recognized = Pipeline().run(Image, WithContext, Options, Debug.get());
  ASSERT_TRUE(Plain.Success) << Plain.Error;
  ASSERT_TRUE(Recognized.Success) << Recognized.Error;
  ASSERT_EQ(Plain.MedFuncs.size(), 1u);
  ASSERT_EQ(Recognized.MedFuncs.size(), 1u);
  ASSERT_FALSE(Recognized.LibraryRecognitions.empty());
  EXPECT_EQ(body(Plain.MedFuncs.front()), body(Recognized.MedFuncs.front()));
  ASSERT_TRUE(Plain.LlvmModule);
  ASSERT_TRUE(Recognized.LlvmModule);
  EXPECT_EQ(llvmText(*Plain.LlvmModule), llvmText(*Recognized.LlvmModule));
}

TEST_F(LibraryRecognitionTest, LLVMCMapsOptimizedAndUnoptimizedOriginalText) {
  const auto Path =
      std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o";
  auto Debug = DWARFDebugContext::load(Path, Image.Format,
                                       DWARFLoadTrust::InImage, Image.Raw);
  ASSERT_TRUE(Debug);
  const auto &Rule = rule("libcxx.vector-u32.data");
  for (bool NoOpt : {false, true})
    for (const std::string &Name :
         {Rule.WholeFunctionSymbols.front(),
          std::string("_nd_vector_u32_data_inline")}) {
      SCOPED_TRACE(Name + (NoOpt ? " no-opt" : " optimized"));
      const auto *Symbol = Image.findSymbol(Name);
      ASSERT_NE(Symbol, nullptr);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.LibraryFeatures = &DB.featurePacks();
      Options.OnlyFunctionEntries.insert(Symbol->Addr);
      Options.LiftMode = true;
      Options.SourceProjection = true;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options, Debug.get());
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.LibraryRecognitions.size(), 1u);
      ASSERT_TRUE(Result.LlvmModule);
      ASSERT_TRUE(Result.LLVMSources);
      ASSERT_FALSE(Result.LLVMSources->Observations.empty());
      const std::string Before = llvmText(*Result.LlvmModule);
      LLVMCEmitter Emitter;
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      COptions.LLVMSources = Result.LLVMSources.get();
      std::string Ordinary, Mapped;
      llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
      ASSERT_TRUE(Emitter.emit(*Result.LlvmModule, OrdinaryOS, COptions));
      CSourceMap Map;
      Map.Recognitions = &Result.LibraryRecognitions;
      COptions.SourceMap = &Map;
      ASSERT_TRUE(Emitter.emit(*Result.LlvmModule, MappedOS, COptions));
      EXPECT_EQ(Ordinary, Mapped);
      EXPECT_EQ(llvmText(*Result.LlvmModule), Before);
      ASSERT_EQ(Map.Regions.size(), 1u);
      ASSERT_TRUE(Map.Regions.front().Mapped) << Mapped;
      ASSERT_FALSE(Map.Regions.front().Spans.empty());
      for (auto Span : Map.Regions.front().Spans) {
        ASSERT_LT(Span.Begin, Span.End);
        ASSERT_LE(Span.End, Mapped.size());
        if (Name == "_nd_vector_u32_data_inline")
          EXPECT_EQ(Mapped.substr(Span.Begin, Span.End - Span.Begin).find('^'),
                    std::string::npos);
      }
      if (Name == "_nd_vector_u32_data_inline") {
        Result.LLVMSources->Observations.clear();
        std::string Unknown;
        llvm::raw_string_ostream UnknownOS(Unknown);
        ASSERT_TRUE(Emitter.emit(*Result.LlvmModule, UnknownOS, COptions));
        EXPECT_EQ(Unknown, Ordinary);
        ASSERT_EQ(Map.Regions.size(), 1u);
        EXPECT_FALSE(Map.Regions.front().Mapped);
        EXPECT_TRUE(Map.Regions.front().Spans.empty());
      }
    }
}

TEST_F(LibraryRecognitionTest, LLVMShardsKeepOnlyLiveSourceHandles) {
  const auto Path =
      std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o";
  auto Debug = DWARFDebugContext::load(Path, Image.Format,
                                       DWARFLoadTrust::InImage, Image.Raw);
  ASSERT_TRUE(Debug);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LibraryFeatures = &DB.featurePacks();
  Options.LiftMode = true;
  Options.SourceProjection = true;
  auto Result = Pipeline().run(Image, Context, Options, Debug.get());
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_GE(Result.MedFuncs.size(), 8u);
  ASSERT_TRUE(Result.LLVMSources);
  const auto *Symbol = Image.findSymbol("_nd_vector_u32_data_inline");
  ASSERT_NE(Symbol, nullptr);
  auto At = Result.LLVMSources->Functions.find(Symbol->Addr);
  ASSERT_NE(At, Result.LLVMSources->Functions.end());
  auto *Function = llvm::dyn_cast_or_null<llvm::Function>(At->second);
  ASSERT_NE(Function, nullptr);
  EXPECT_EQ(Function->getParent(), Result.LlvmModule.get());
  CSourceMap Map;
  Map.Recognitions = &Result.LibraryRecognitions;
  CEmitterOptions COptions;
  COptions.LLVMSources = Result.LLVMSources.get();
  COptions.TheArch = Image.Arch;
  COptions.Format = Image.Format;
  COptions.Image = &Image;
  COptions.SourceMap = &Map;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions, nullptr,
                                  &Image, Function));
  bool Found = false;
  for (const auto &Region : Map.Regions)
    if (Result.LibraryRecognitions[Region.Recognition].Function ==
            Symbol->Addr &&
        Result.LibraryRecognitions[Region.Recognition].Rule ==
            "libcxx.vector-u32.data") {
      EXPECT_TRUE(Region.Mapped) << Text;
      Found = true;
    }
  EXPECT_TRUE(Found);
}

} // namespace
