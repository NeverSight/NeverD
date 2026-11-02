//===- WindowsEHModesCorpusTests.cpp - Windows EH mode gates ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

struct ArtifactSelector {
  StringRef Name;
  StringRef Toolchain;
  StringRef Architecture;
  StringRef CxxFormat;
  bool SecurityCookie = false;
  StringRef Optimization;
  int VisualStudioYear = 2022;
};

Expected<std::vector<WindowsEHArtifactExpectation>> loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "windows-eh.json";
  return loadWindowsEHCorpusManifest(ManifestPath.string(), true);
}

bool matches(const WindowsEHArtifactExpectation &Expectation,
             const ArtifactSelector &Selector) {
  return Expectation.Name == Selector.Name &&
         Expectation.Toolchain == Selector.Toolchain &&
         Expectation.Architecture == Selector.Architecture &&
         Expectation.CxxFormat == Selector.CxxFormat &&
         Expectation.SecurityCookie == Selector.SecurityCookie &&
         Expectation.Optimization == Selector.Optimization &&
         Expectation.VisualStudioYear == Selector.VisualStudioYear;
}

Expected<const WindowsEHArtifactExpectation *>
selectUnique(ArrayRef<WindowsEHArtifactExpectation> Expectations,
             const ArtifactSelector &Selector) {
  const WindowsEHArtifactExpectation *Match = nullptr;
  for (const WindowsEHArtifactExpectation &Expectation : Expectations) {
    if (!matches(Expectation, Selector))
      continue;
    if (Match)
      return createStringError(inconvertibleErrorCode(),
                               "duplicate Windows EH corpus selector");
    Match = &Expectation;
  }
  if (!Match)
    return createStringError(inconvertibleErrorCode(),
                             "missing Windows EH corpus selector");
  return Match;
}

Expected<std::string> decompileToSource(const std::filesystem::path &Input,
                                        unsigned TimeoutSeconds = 120,
                                        ArrayRef<StringRef> Options = {}) {
  SmallString<128> OutputPath;
  if (std::error_code EC = sys::fs::createTemporaryFile(
          "neverd-windows-eh-mode", "c", OutputPath))
    return createStringError(EC, "cannot create HighC output");
  FileRemover RemoveOutput(OutputPath);

  SmallString<128> StdoutPath;
  if (std::error_code EC = sys::fs::createTemporaryFile(
          "neverd-windows-eh-mode", "stdout", StdoutPath))
    return createStringError(EC, "cannot create stdout capture");
  FileRemover RemoveStdout(StdoutPath);

  SmallString<128> StderrPath;
  if (std::error_code EC = sys::fs::createTemporaryFile(
          "neverd-windows-eh-mode", "stderr", StderrPath))
    return createStringError(EC, "cannot create stderr capture");
  FileRemover RemoveStderr(StderrPath);

  const std::string InputString = Input.string();
  const std::string OutputString = OutputPath.str().str();
  SmallVector<StringRef, 10> Args{
      NEVERD_BINARY,       "decompile", "--no-debug", "--no-opt",
      "--language=source", "-o",        OutputString,
  };
  Args.append(Options.begin(), Options.end());
  Args.push_back(InputString);
  std::optional<StringRef> Redirects[] = {std::nullopt, StdoutPath.str(),
                                          StderrPath.str()};
  std::string ProgramError;
  const int Exit =
      sys::ExecuteAndWait(NEVERD_BINARY, Args, std::nullopt, Redirects,
                          TimeoutSeconds, 0, &ProgramError);
  if (Exit != 0) {
    std::string Diagnostic = ProgramError;
    if (auto StderrOrErr = MemoryBuffer::getFile(StderrPath.str()))
      Diagnostic += (*StderrOrErr)->getBuffer().str();
    return createStringError(inconvertibleErrorCode(),
                             "source decompile failed with exit %d: %s", Exit,
                             Diagnostic.c_str());
  }

  auto OutputOrErr = MemoryBuffer::getFile(OutputPath.str());
  if (!OutputOrErr)
    return createStringError(OutputOrErr.getError(),
                             "cannot read HighC output");
  if ((*OutputOrErr)->getBuffer().empty())
    return createStringError(inconvertibleErrorCode(), "HighC output is empty");
  return (*OutputOrErr)->getBuffer().str();
}

struct AnalysisModeCase {
  ArtifactSelector Selector;
  std::vector<std::vector<StringRef>> RequiredGuardedTokenGroups;
};

std::vector<StringRef> guardedAnalysisBlocks(StringRef Output) {
  constexpr StringLiteral Marker("/* neverd.analysis-only:");
  std::vector<StringRef> Blocks;
  size_t Cursor = 0;
  while (Cursor < Output.size()) {
    const size_t Begin = Output.find(Marker, Cursor);
    if (Begin == StringRef::npos)
      break;
    const size_t Next = Output.find(Marker, Begin + Marker.size());
    const size_t End = Next == StringRef::npos ? Output.size() : Next;
    Blocks.push_back(Output.slice(Begin, End));
    Cursor = End;
  }
  return Blocks;
}

bool containsEveryToken(StringRef Block, ArrayRef<StringRef> Tokens) {
  return llvm::all_of(Tokens,
                      [&](StringRef Token) { return Block.contains(Token); });
}

TEST(WindowsEHModesCorpus,
     DecompileMatrixFailsClosedWithAuditableExceptionMetadata) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());

  const std::array<AnalysisModeCase, 11> Cases{{
      {{"cxx_eh_probe", "msvc", "x86_64", "fh3", false, "o0"},
       {{"cxx.format=fh3", "encoding=x64-"}}},
      {{"cxx_eh_probe", "clang-cl", "x86_64", "fh3", true, "o2"},
       {{"cxx.format=fh3", "encoding=x64-"}}},
      {{"seh_probe", "clang-cl", "x86_64", "fh3", false, "o0"},
       {{"seh.scope[", "encoding=x64-"}}},
      {{"cxx_eh_probe", "msvc", "x86_64", "fh4", false, "o2"},
       {{"cxx.format=fh4", "encoding=x64-"}}},
      {{"xcpt4", "msvc", "x86_64", "fh4", true, "o0"},
       {{"seh.scope[", "gs.cookie_offset="}}},
      {{"cxx_eh_probe", "clang-cl", "x86", "native", false, "o0"},
       {{"encoding=x86-cxx-funcinfo", "ip_states=0"}}},
      {{"seh_probe", "msvc", "x86", "native", true, "o2"},
       {{"encoding=x86-scope-table-eh4", "personality=_except_handler4"}}},
      {{"cxx_eh_probe", "msvc", "arm", "native", true, "o0"},
       {{"cxx.format=fh3", "encoding=arm32-", "gs.cookie_offset="}}},
      {{"seh_probe", "msvc", "arm", "native", false, "o2"},
       {{"seh.scope[", "encoding=arm32-"}}},
      {{"cxx_eh_probe", "msvc", "aarch64", "native", true, "o0"},
       {{"cxx.format=fh3", "encoding=arm64-", "gs.cookie_offset="}}},
      {{"seh_probe", "clang-cl", "aarch64", "native", false, "o2"},
       {{"seh.scope[", "encoding=arm64-"}}},
  }};

  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);
  for (const AnalysisModeCase &TestCase : Cases) {
    SCOPED_TRACE((TestCase.Selector.Toolchain + "/" +
                  TestCase.Selector.Architecture + "/" +
                  TestCase.Selector.CxxFormat + "/" +
                  (TestCase.Selector.SecurityCookie ? "gs" : "no-gs") + "/" +
                  TestCase.Selector.Optimization + "/" + TestCase.Selector.Name)
                     .str());
    auto ExpectationOrErr = selectUnique(*ExpectationsOrErr, TestCase.Selector);
    ASSERT_TRUE(static_cast<bool>(ExpectationOrErr))
        << toString(ExpectationOrErr.takeError());
    // Structured EH belongs to the source view. Explicit C retains native
    // callbacks and cannot invent a separate C ABI for an embedded handler.
    auto OutputOrErr =
        decompileToSource(CorpusRoot / (*ExpectationOrErr)->Path);
    if (!OutputOrErr) {
      ADD_FAILURE() << toString(OutputOrErr.takeError());
      continue;
    }
    StringRef Output(*OutputOrErr);
    const std::vector<StringRef> GuardedBlocks = guardedAnalysisBlocks(Output);
    ASSERT_FALSE(GuardedBlocks.empty());
    for (const std::vector<StringRef> &RequiredTokens :
         TestCase.RequiredGuardedTokenGroups) {
      // A region without a provable lexical range keeps its native dispatch
      // as an annotated block instead of a try the runtime would not run.
      const bool Found = llvm::any_of(GuardedBlocks, [&](StringRef Block) {
        return Block.contains("neverd.exception") &&
               Block.contains("highir.structured_regions=") &&
               Block.contains("fallback_regions=") &&
               (Block.contains("__try") || Block.contains("try {") ||
                Block.contains("unstructured SEH region [") ||
                Block.contains("C++ unwind-table region [")) &&
               containsEveryToken(Block, RequiredTokens);
      });
      std::string TokenList;
      for (StringRef Token : RequiredTokens) {
        if (!TokenList.empty())
          TokenList += ", ";
        TokenList += Token.str();
      }
      EXPECT_TRUE(Found) << "no guarded analysis-only function contains: "
                         << TokenList;
    }
  }
}

TEST(WindowsEHModesCorpus, DecompilesOptimizedFH3ProtectedSourceProjection) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);
  for (bool SecurityCookie : {false, true}) {
    SCOPED_TRACE(SecurityCookie ? "gs" : "no-gs");
    auto ExpectationOrErr =
        selectUnique(*ExpectationsOrErr, {"cxx_eh_probe", "msvc", "x86_64",
                                          "fh3", SecurityCookie, "o2"});
    ASSERT_TRUE(static_cast<bool>(ExpectationOrErr))
        << toString(ExpectationOrErr.takeError());
    auto OutputOrErr =
        decompileToSource(CorpusRoot / (*ExpectationOrErr)->Path);
    ASSERT_TRUE(static_cast<bool>(OutputOrErr))
        << toString(OutputOrErr.takeError());
    EXPECT_TRUE(StringRef(*OutputOrErr).contains("cxx.format=fh3"));
    EXPECT_FALSE(guardedAnalysisBlocks(*OutputOrErr).empty());
  }
}

TEST(WindowsEHModesCorpus, DecompilesRegistrationCalleeProofCycles) {
  const std::filesystem::path CorpusRoot =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "corpus/windows-eh";
  const std::array<StringRef, 5> Inputs{{
      "msvc/vs2019/x86/native/gs/o2/windows-seh-tests/"
      "xcpt4-msvc-x86-native-gs-o2.exe",
      "msvc/x86/native/gs/o0/windows-seh-tests/"
      "xframe_eh_exe-msvc-x86-native-gs-o0.exe",
      "msvc/x86/native/gs/o0/windows-seh-tests/"
      "xframe_eh_dll-msvc-x86-native-gs-o0.dll",
      "msvc/x86/native/no-gs/o0/windows-seh-tests/"
      "xframe_eh_dll-msvc-x86-native-no-gs-o0.dll",
      "msvc/x86/native/gs/o2/windows-seh-tests/"
      "xframe_eh_dll-msvc-x86-native-gs-o2.dll",
  }};
  for (StringRef Input : Inputs) {
    SCOPED_TRACE(Input.str());
    // These are complete test applications, including the multi-megabyte
    // optimized xcpt4 source projection. Allow slower workers more than the
    // small probes' 120 seconds, within this corpus's 300-second CTest limit.
    auto OutputOrErr =
        decompileToSource(CorpusRoot / Input.str(), /*TimeoutSeconds=*/240);
    ASSERT_TRUE(static_cast<bool>(OutputOrErr))
        << toString(OutputOrErr.takeError());
    EXPECT_TRUE(StringRef(*OutputOrErr).contains("encoding=x86-"));
    EXPECT_FALSE(guardedAnalysisBlocks(*OutputOrErr).empty());
    if (Input == Inputs.front())
      for (const char *Address : {"402812", "40288B", "402953", "4065C3"}) {
        SCOPED_TRACE(Address);
        const std::string Marker =
            std::string("/* neverd.entry: 0x") + Address + " */";
        const size_t Start = OutputOrErr->find(Marker);
        ASSERT_NE(Start, std::string::npos);
        StringRef Body =
            StringRef(*OutputOrErr).drop_front(Start + Marker.size());
        Body = Body.take_front(Body.find("/* neverd.entry:"));
        EXPECT_TRUE(Body.contains(std::string("int32_t sub_") + Address + "("));
        EXPECT_FALSE(Body.contains("unknown return value"));
      }
  }
}

enum class NativeTargetRecord : uint8_t {
  FH4,
  GSWrappedCxx,
  X86EH4Registration,
  ArmCxx,
};

struct NativeClassificationCase {
  ArtifactSelector Selector;
  NativeTargetRecord TargetRecord;
  std::vector<WindowsEHNativeSourceReason> ExpectedReasons;
  size_t ExpectedEligibleRecords = 0;
};

Arch expectedArch(StringRef Architecture) {
  if (Architecture == "x86")
    return Arch::X86;
  if (Architecture == "x86_64")
    return Arch::X64;
  if (Architecture == "arm")
    return Arch::ARM;
  if (Architecture == "aarch64")
    return Arch::AArch64;
  return Arch::Unknown;
}

bool isTargetRecord(const ExceptionFunction &Function,
                    NativeTargetRecord Target) {
  switch (Target) {
  case NativeTargetRecord::FH4:
    return Function.Kind == RuntimeFunctionKind::Primary && Function.Cxx &&
           Function.Personality == ExceptionPersonality::CxxFrameHandler4 &&
           Function.Cxx->NativeEncoding == CxxExceptionInfo::Encoding::FH4;
  case NativeTargetRecord::GSWrappedCxx:
    return Function.Kind == RuntimeFunctionKind::Primary && Function.Cxx &&
           Function.GSCookie && isGSWrappedPersonality(Function.Personality);
  case NativeTargetRecord::X86EH4Registration:
    return Function.Kind == RuntimeFunctionKind::Primary &&
           Function.model() == ExceptionModel::WindowsRegistration &&
           Function.Encoding == ExceptionEncoding::X86ScopeTableEH4 &&
           Function.Registration.has_value();
  case NativeTargetRecord::ArmCxx:
    return Function.Kind == RuntimeFunctionKind::Primary &&
           Function.ParseStatus != ExceptionParseStatus::Malformed &&
           Function.Cxx && isCxxPersonality(Function.Personality);
  }
  return false;
}

TEST(WindowsEHModesCorpus, PinsTargetedNativeSourceClassificationReasons) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());

  const std::array<NativeClassificationCase, 5> Cases{{
      {{"cxx_eh_probe", "msvc", "x86_64", "fh4", false, "o2"},
       NativeTargetRecord::FH4,
       {WindowsEHNativeSourceReason::InvalidCxxStateGraph,
        WindowsEHNativeSourceReason::UnsupportedCxxFlags}},
      {{"cxx_eh_probe", "msvc", "x86_64", "fh3", true, "o0"},
       NativeTargetRecord::GSWrappedCxx,
       {WindowsEHNativeSourceReason::GSWrappedPersonality}},
      {{"seh_probe", "msvc", "x86", "native", true, "o2"},
       NativeTargetRecord::X86EH4Registration,
       {WindowsEHNativeSourceReason::IncompleteRegistrationFrame,
        WindowsEHNativeSourceReason::Eligible},
       2},
      {{"cxx_eh_probe", "msvc", "arm", "native", true, "o0"},
       NativeTargetRecord::ArmCxx,
       {WindowsEHNativeSourceReason::UnsupportedArchitecture}},
      {{"cxx_eh_probe", "msvc", "aarch64", "native", true, "o0"},
       NativeTargetRecord::ArmCxx,
       {WindowsEHNativeSourceReason::UnsupportedCxxSeparated,
        WindowsEHNativeSourceReason::UnsupportedCxxCatchFunclet,
        WindowsEHNativeSourceReason::UnsupportedArchitecture}},
  }};

  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);
  for (const NativeClassificationCase &TestCase : Cases) {
    SCOPED_TRACE((TestCase.Selector.Toolchain + "/" +
                  TestCase.Selector.Architecture + "/" +
                  TestCase.Selector.CxxFormat + "/" + TestCase.Selector.Name)
                     .str());
    auto ExpectationOrErr = selectUnique(*ExpectationsOrErr, TestCase.Selector);
    ASSERT_TRUE(static_cast<bool>(ExpectationOrErr))
        << toString(ExpectationOrErr.takeError());

    const std::filesystem::path ArtifactPath =
        CorpusRoot / (*ExpectationOrErr)->Path;
    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    ASSERT_NE(ImageLoader, nullptr);
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    ASSERT_TRUE(static_cast<bool>(ImageOrErr))
        << toString(ImageOrErr.takeError());
    EXPECT_EQ(ImageOrErr->Format, BinaryFormat::COFF);
    EXPECT_EQ(ImageOrErr->Arch, expectedArch(TestCase.Selector.Architecture));

    size_t TargetRecords = 0;
    size_t EligibleRecords = 0;
    std::vector<bool> SawExpectedReason(TestCase.ExpectedReasons.size(), false);
    for (const ExceptionFunction &Function :
         ImageOrErr->ExceptionMetadata.Functions) {
      if (!isTargetRecord(Function, TestCase.TargetRecord))
        continue;
      ++TargetRecords;
      const WindowsEHNativeSourceClassification Classification =
          classifyWindowsEHNativeSource(Function, ImageOrErr->Arch,
                                        ImageOrErr->Format);
      const auto Expected =
          llvm::find(TestCase.ExpectedReasons, Classification.Reason);
      EXPECT_NE(Expected, TestCase.ExpectedReasons.end())
          << "target record was classified as "
          << getWindowsEHNativeSourceReasonName(Classification.Reason);
      if (Expected != TestCase.ExpectedReasons.end())
        SawExpectedReason[std::distance(TestCase.ExpectedReasons.begin(),
                                        Expected)] = true;
      EXPECT_EQ(Classification.canRegenerateLanguageMetadata(),
                Classification.Reason == WindowsEHNativeSourceReason::Eligible);
      if (Classification.canRegenerateLanguageMetadata()) {
        ++EligibleRecords;
        // The two direct EH4 prologues publish their allocated pushed-cookie
        // storage. Other records still lack a complete frame. This source
        // classification does not replace native IR or installation checks.
        ASSERT_EQ(TestCase.TargetRecord,
                  NativeTargetRecord::X86EH4Registration);
        ASSERT_TRUE(Function.Registration);
        EXPECT_EQ(Function.Registration->RegistrationOffset, -16);
        EXPECT_EQ(Function.Registration->TryLevelOffset, -4);
        EXPECT_TRUE(
            Function.CodeRange.contains(Function.Registration->ChainInstallVA));
      }
    }
    EXPECT_GT(TargetRecords, 0u) << "no target language record was found";
    EXPECT_EQ(EligibleRecords, TestCase.ExpectedEligibleRecords);
    for (size_t I = 0; I < TestCase.ExpectedReasons.size(); ++I)
      EXPECT_TRUE(SawExpectedReason[I])
          << "no target record was classified as "
          << getWindowsEHNativeSourceReasonName(TestCase.ExpectedReasons[I]);
  }
}

TEST(WindowsEHModesCorpus,
     LoadsRealAArch64NoGSCatchAllWithExactOutputBoundary) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());

  struct AArch64Case {
    ArtifactSelector Selector;
    bool OutputEligible = false;
    WindowsEHNativeSourceReason OutputReason =
        WindowsEHNativeSourceReason::Eligible;
  };
  const std::array<AArch64Case, 4> Cases{{
      {{"xcpt4", "msvc", "aarch64", "native", false, "o0"},
       false,
       WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph},
      {{"xcpt4", "msvc", "aarch64", "native", false, "o2"},
       false,
       WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph},
      {{"xcpt4", "clang-cl", "aarch64", "native", false, "o0"},
       true,
       WindowsEHNativeSourceReason::Eligible},
      {{"xcpt4", "clang-cl", "aarch64", "native", false, "o2"},
       true,
       WindowsEHNativeSourceReason::Eligible},
  }};

  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);
  for (const AArch64Case &TestCase : Cases) {
    const ArtifactSelector &Selector = TestCase.Selector;
    SCOPED_TRACE((Selector.Toolchain + "/" + Selector.Optimization).str());
    auto ExpectationOrErr = selectUnique(*ExpectationsOrErr, Selector);
    ASSERT_TRUE(static_cast<bool>(ExpectationOrErr))
        << toString(ExpectationOrErr.takeError());

    const std::filesystem::path ArtifactPath =
        CorpusRoot / (*ExpectationOrErr)->Path;
    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    ASSERT_NE(ImageLoader, nullptr);
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    ASSERT_TRUE(static_cast<bool>(ImageOrErr))
        << toString(ImageOrErr.takeError());
    ASSERT_EQ(ImageOrErr->Format, BinaryFormat::COFF);
    ASSERT_EQ(ImageOrErr->Arch, Arch::AArch64);

    size_t ParsedCatchAllRecords = 0;
    size_t IREligibleCatchAllRecords = 0;
    size_t OutputEligibleCatchAllRecords = 0;
    std::string RecordDiagnostics;
    for (const ExceptionFunction &Function :
         ImageOrErr->ExceptionMetadata.Functions) {
      if ((Function.PersonalityVA != 0 || Function.HandlerDataVA != 0) &&
          RecordDiagnostics.size() < 4096) {
        size_t CatchAllScopes = 0;
        size_t FilterScopes = 0;
        size_t FinallyScopes = 0;
        std::string CallbackVAs;
        if (Function.SEH)
          for (const SEHScopeRecord &Scope : Function.SEH->Scopes) {
            CatchAllScopes += Scope.Kind == SEHScopeKind::CatchAll;
            FilterScopes += Scope.Kind == SEHScopeKind::Filter;
            FinallyScopes += Scope.Kind == SEHScopeKind::Finally;
            if (Scope.FilterOrFinallyVA != 0 && CallbackVAs.size() < 160)
              CallbackVAs += " 0x" + llvm::utohexstr(Scope.FilterOrFinallyVA);
          }
        RecordDiagnostics +=
            ("range=0x" + llvm::utohexstr(Function.CodeRange.Begin) +
             " encoding=" + getExceptionEncodingName(Function.Encoding) +
             " status=" + getExceptionParseStatusName(Function.ParseStatus) +
             " personality=" +
             getExceptionPersonalityName(Function.Personality) +
             " name=" + Function.PersonalityName + " handler=0x" +
             llvm::utohexstr(Function.HandlerDataVA) + " scopes=" +
             std::to_string(Function.SEH ? Function.SEH->Scopes.size() : 0) +
             " catch=" + std::to_string(CatchAllScopes) +
             " filter=" + std::to_string(FilterScopes) +
             " finally=" + std::to_string(FinallyScopes) +
             " callbacks=" + CallbackVAs + "\n");
      }
      if (Function.Kind != RuntimeFunctionKind::Primary ||
          Function.Personality != ExceptionPersonality::CSpecificHandler ||
          !Function.SEH || Function.SEH->Scopes.empty() ||
          !llvm::all_of(Function.SEH->Scopes, [](const SEHScopeRecord &Scope) {
            return Scope.Kind == SEHScopeKind::CatchAll;
          }))
        continue;
      ++ParsedCatchAllRecords;

      const WindowsEHNativeSourceClassification IRSource =
          classifyWindowsEHNativeSource(Function, ImageOrErr->Arch,
                                        ImageOrErr->Format,
                                        WindowsEHNativeCapability::IRLowering);
      if (!IRSource.canLowerNativeIR())
        continue;
      ++IREligibleCatchAllRecords;
      EXPECT_EQ(IRSource.Model, WindowsEHNativeSourceModel::SEH);
      EXPECT_EQ(IRSource.Reason, WindowsEHNativeSourceReason::Eligible);
      EXPECT_FALSE(Function.GSCookie.has_value());
      EXPECT_TRUE(
          llvm::all_of(Function.SEH->Scopes, [](const SEHScopeRecord &Scope) {
            return Scope.Kind == SEHScopeKind::CatchAll;
          }));

      const WindowsEHNativeSourceClassification PatchSource =
          classifyWindowsEHNativeSource(Function, ImageOrErr->Arch,
                                        ImageOrErr->Format,
                                        WindowsEHNativeCapability::OutputPatch);
      if (PatchSource.canPatchOutput())
        ++OutputEligibleCatchAllRecords;
      EXPECT_EQ(PatchSource.canPatchOutput(), TestCase.OutputEligible)
          << RecordDiagnostics;
      EXPECT_EQ(PatchSource.Reason, TestCase.OutputReason)
          << "output classification was "
          << getWindowsEHNativeSourceReasonName(PatchSource.Reason) << '\n'
          << RecordDiagnostics;

      if (!TestCase.OutputEligible)
        continue;
      ASSERT_EQ(Function.SEH->Scopes.size(), 1u);
      const SEHScopeRecord &Scope = Function.SEH->Scopes.front();
      ASSERT_EQ(Scope.GuardedRange.End & 3u, 1u)
          << "the pinned clang-cl artifact no longer exercises the legacy "
             "ARM64 end spelling";
      const std::optional<ExceptionAddressRange> SemanticRange =
          getSemanticSEHGuardedRange(Scope, ImageOrErr->Arch,
                                     Function.CodeRange);
      ASSERT_TRUE(SemanticRange.has_value());
      ASSERT_EQ(SemanticRange->End, Scope.GuardedRange.End + 3);

      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::AArch64));
      CFGBuilder Builder;
      LowFunc Low = Builder.build(
          *ImageOrErr, Dec, Function.CodeRange.Begin,
          "corpus_aarch64_seh_" + llvm::utohexstr(Function.CodeRange.Begin));
      ASSERT_TRUE(Low.ExceptionMetadata.has_value());
      bool SawSemanticEnd = false;
      bool SawUnprotectedSuccessor = false;
      for (const LowBlock &Block : Low.Blocks) {
        const ExceptionAddressRange BlockRange{Block.StartAddr, Block.EndAddr};
        if (!BlockRange.isValid())
          continue;
        if (SemanticRange->overlaps(BlockRange)) {
          EXPECT_TRUE(SemanticRange->contains(BlockRange));
          EXPECT_TRUE(llvm::any_of(
              Block.ExceptionalSuccs, [&](const ExceptionalEdge &Edge) {
                return Edge.singleContext() &&
                       Edge.singleContext()->RegionIndex == 0 &&
                       Edge.Kind == ExceptionalEdgeKind::SEHHandler &&
                       Edge.TargetVA == Scope.HandlerVA;
              }));
        }
        SawSemanticEnd |= Block.EndAddr == SemanticRange->End;
        if (Block.StartAddr == SemanticRange->End) {
          SawUnprotectedSuccessor = true;
          EXPECT_FALSE(llvm::any_of(
              Block.ExceptionalSuccs, [](const ExceptionalEdge &Edge) {
                return Edge.singleContext() &&
                       Edge.singleContext()->RegionIndex == 0 &&
                       Edge.Kind == ExceptionalEdgeKind::SEHHandler;
              }));
        }
      }
      EXPECT_TRUE(SawSemanticEnd);
      EXPECT_TRUE(SawUnprotectedSuccessor);

      LowToMedConverter Converter;
      Converter.setBinaryImage(&*ImageOrErr);
      MedFunc Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::COFF);
      EXPECT_TRUE(llvm::any_of(Med.Blocks, [&](const MedBlock &Block) {
        return Block.EndAddr == SemanticRange->End;
      }));

      std::vector<std::pair<va_t, std::string>> Imports;
      for (const auto &[Address, Name] : ImageOrErr->getImportAddressNames())
        Imports.emplace_back(Address, Name);
      llvm::LLVMContext Context;
      // The focused module owns only this one real function.  Supplying the
      // complete image here would also request whole-image data-pointer
      // relocation proofs for unrelated functions that the focused module
      // deliberately does not declare.  Low/Med recovery above still used the
      // real image, and the patch plan below authenticates the resulting IR
      // against that complete source image.
      auto Module = MedLLVMEmitter().emit(
          {Med}, Context, "real-legacy-aarch64-seh", Arch::AArch64, Imports,
          nullptr, BinaryFormat::COFF);
      ASSERT_NE(Module, nullptr);
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      llvm::Function *Lifted = Module->getFunction(Med.Name);
      ASSERT_NE(Lifted, nullptr);
      EXPECT_NE(Lifted->getMetadata(windows_eh_md::NativeAttachment), nullptr);
      EXPECT_TRUE(llvm::any_of(*Lifted, [](const llvm::BasicBlock &Block) {
        return llvm::any_of(Block, [](const llvm::Instruction &Instruction) {
          return llvm::isa<llvm::InvokeInst>(Instruction);
        });
      }));
      auto Plan = planCOFFExceptionPatch(*Module, *ImageOrErr, Arch::AArch64);
      ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
    }
    EXPECT_EQ(ParsedCatchAllRecords, 1u)
        << "loader found no ARM64 catch-all SEH scope\n"
        << RecordDiagnostics;
    EXPECT_EQ(IREligibleCatchAllRecords, 1u)
        << "parsed ARM64 catch-all record did not pass the IR gate\n"
        << RecordDiagnostics;
    EXPECT_EQ(OutputEligibleCatchAllRecords, TestCase.OutputEligible ? 1u : 0u)
        << "ARM64 output boundary did not match the pinned source shape\n"
        << RecordDiagnostics;
  }
}

/// The definition of the function at \p Entry in \p Output, from its
/// `neverd.entry` comment through its closing brace, or empty.
std::string definitionOf(StringRef Output, StringRef Entry) {
  const size_t Start = Output.find(("/* neverd.entry: " + Entry + " */").str());
  if (Start == StringRef::npos)
    return {};
  const size_t Open = Output.find(") {\n", Start);
  if (Open == StringRef::npos)
    return {};
  int Depth = 0;
  for (size_t I = Open + 2; I < Output.size(); ++I) {
    if (Output[I] == '{')
      ++Depth;
    else if (Output[I] == '}' && --Depth == 0)
      return Output.slice(Start, I + 1).str();
  }
  return {};
}

size_t occurrencesIn(StringRef Text, StringRef Needle) {
  size_t Count = 0;
  for (size_t At = Text.find(Needle); At != StringRef::npos;
       At = Text.find(Needle, At + 1))
    ++Count;
  return Count;
}

// xcpt4u.c's eret:
//   __try { __try { foo1(Status); } __except (Status filter) {
//       *Counter += 1; return; } } __finally { *Counter += 1; }
// The return calls _local_unwind, which runs the finally handler once and
// resumes at the epilogue; MSVC also copies the finally after the try for the
// normal path. Each path must add one per source increment it passes: the
// arm's own, then the finally's, which only the clause may run on that path.
TEST(WindowsEHModesCorpus, LocalUnwindReturnRunsTheFinallyOnceInTheClause) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  auto ExpectationOrErr = selectUnique(
      *ExpectationsOrErr, {"xcpt4", "msvc", "x86_64", "fh4", true, "o0"});
  ASSERT_TRUE(static_cast<bool>(ExpectationOrErr))
      << toString(ExpectationOrErr.takeError());
  auto OutputOrErr =
      decompileToSource(std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                        (*ExpectationOrErr)->Path);
  ASSERT_TRUE(static_cast<bool>(OutputOrErr))
      << toString(OutputOrErr.takeError());
  const std::string Eret = definitionOf(*OutputOrErr, "0x1400011E0");
  ASSERT_FALSE(Eret.empty());
  // No runtime call or code address stands for the jump.
  EXPECT_EQ(Eret.find("_local_unwind"), std::string::npos) << Eret;
  EXPECT_EQ(Eret.find("0x140001230"), std::string::npos) << Eret;
  // Nor does the handler run as an explicit call: the clause runs it.
  EXPECT_EQ(Eret.find("sub_1400069E9("), std::string::npos) << Eret;
  const size_t Except = Eret.find("} __except (");
  const size_t Finally = Eret.find("} __finally {");
  const size_t Guard = Eret.find("if (AbnormalTermination())", Finally);
  ASSERT_NE(Except, std::string::npos) << Eret;
  ASSERT_NE(Finally, std::string::npos) << Eret;
  ASSERT_NE(Guard, std::string::npos) << Eret;
  const size_t After = Eret.find("\n    }\n", Guard);
  ASSERT_NE(After, std::string::npos) << Eret;
  // The arm leaves both tries by a jump, an abnormal termination in C.
  const StringRef Arm = StringRef(Eret).slice(Except, Finally);
  EXPECT_TRUE(Arm.contains("goto ") || Arm.contains("return")) << Eret;
  // Three increments, one each: the arm's, the clause's for the arm's
  // return, and the normal path's copy after the try.
  const StringRef Clause = StringRef(Eret).slice(Finally, After);
  const StringRef Normal = StringRef(Eret).substr(After);
  EXPECT_EQ(occurrencesIn(Arm, " + 1;"), 1u) << Eret;
  EXPECT_EQ(occurrencesIn(Clause, " + 1;"), 1u) << Eret;
  EXPECT_EQ(occurrencesIn(Normal, " + 1;"), 1u) << Eret;
}

// A single-function view gathers the handlers the protocol reads, so the
// exit reads the same there as in the whole unit.
TEST(WindowsEHModesCorpus, LocalUnwindExitAgreesAloneAndInTheWholeUnit) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  auto ExpectationOrErr = selectUnique(
      *ExpectationsOrErr, {"xcpt4", "msvc", "x86_64", "fh4", true, "o0"});
  ASSERT_TRUE(static_cast<bool>(ExpectationOrErr))
      << toString(ExpectationOrErr.takeError());
  const std::filesystem::path Input =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
      (*ExpectationOrErr)->Path;
  auto WholeOrErr = decompileToSource(Input);
  ASSERT_TRUE(static_cast<bool>(WholeOrErr))
      << toString(WholeOrErr.takeError());
  const std::array<StringRef, 1> Alone{"--func=0x1400011E0"};
  auto AloneOrErr = decompileToSource(Input, 120, Alone);
  ASSERT_TRUE(static_cast<bool>(AloneOrErr))
      << toString(AloneOrErr.takeError());
  const std::string Whole = definitionOf(*WholeOrErr, "0x1400011E0");
  ASSERT_FALSE(Whole.empty());
  EXPECT_EQ(Whole.find("_local_unwind"), std::string::npos) << Whole;
  EXPECT_EQ(definitionOf(*AloneOrErr, "0x1400011E0"), Whole);
}

// xcpt4's O2 loop exits: `return` out of two nested `__try/__finally` and
// `continue` out of the inner one, both local unwinds. The inner and outer
// finally scopes each have two ranges, and the outer one's first range holds
// the loop head. With every exit a jump, the copied `return 1` tail the
// first exit jumped to runs no more; left in place until dead-code
// elimination, its addresses past both ranges split the outer region.
TEST(WindowsEHModesCorpus, LocalUnwindExitsKeepTheirLoopRegionsStructured) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  auto ExpectationOrErr = selectUnique(
      *ExpectationsOrErr, {"xcpt4", "msvc", "x86_64", "fh4", true, "o2"});
  ASSERT_TRUE(static_cast<bool>(ExpectationOrErr))
      << toString(ExpectationOrErr.takeError());
  auto OutputOrErr =
      decompileToSource(std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                        (*ExpectationOrErr)->Path);
  ASSERT_TRUE(static_cast<bool>(OutputOrErr))
      << toString(OutputOrErr.takeError());
  for (const auto &[Entry, Regions] :
       {std::pair<StringRef, StringRef>{"0x1400037D0",
                                        "=4, fallback_regions=0"},
        {"0x140003870", "=6, fallback_regions=0"}}) {
    const size_t Start =
        OutputOrErr->find(("/* neverd.entry: " + Entry + " */").str());
    ASSERT_NE(Start, std::string::npos) << Entry.str();
    const StringRef Function =
        StringRef(*OutputOrErr)
            .slice(Start, OutputOrErr->find("/* neverd.entry: ", Start + 1));
    EXPECT_TRUE(
        Function.contains(("highir.structured_regions" + Regions).str()))
        << Function.str();
    EXPECT_FALSE(Function.contains("_local_unwind")) << Function.str();
  }
}

} // namespace
