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
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

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
         Expectation.Optimization == Selector.Optimization;
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

Expected<std::string> decompileToHighC(const std::filesystem::path &Input) {
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
      NEVERD_BINARY,  "decompile", "--no-debug", "--no-opt",
      "--language=c", "-o",        OutputString, InputString,
  };
  std::optional<StringRef> Redirects[] = {std::nullopt, StdoutPath.str(),
                                          StderrPath.str()};
  std::string ProgramError;
  const int Exit = sys::ExecuteAndWait(NEVERD_BINARY, Args, std::nullopt,
                                       Redirects, 120, 0, &ProgramError);
  if (Exit != 0) {
    std::string Diagnostic = ProgramError;
    if (auto StderrOrErr = MemoryBuffer::getFile(StderrPath.str()))
      Diagnostic += (*StderrOrErr)->getBuffer().str();
    return createStringError(inconvertibleErrorCode(),
                             "HighC decompile failed with exit %d: %s", Exit,
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
    const size_t Trap = Output.find("__builtin_trap();", Begin);
    if (Trap == StringRef::npos || (Next != StringRef::npos && Trap >= Next)) {
      Cursor = Next == StringRef::npos ? Output.size() : Next;
      continue;
    }
    const size_t ClosingBrace = Output.find("\n}\n", Trap);
    const size_t End = ClosingBrace == StringRef::npos
                           ? Output.size()
                           : ClosingBrace + StringRef("\n}\n").size();
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
    auto OutputOrErr = decompileToHighC(CorpusRoot / (*ExpectationOrErr)->Path);
    ASSERT_TRUE(static_cast<bool>(OutputOrErr))
        << toString(OutputOrErr.takeError());
    StringRef Output(*OutputOrErr);
    const std::vector<StringRef> GuardedBlocks = guardedAnalysisBlocks(Output);
    ASSERT_FALSE(GuardedBlocks.empty());
    for (const std::vector<StringRef> &RequiredTokens :
         TestCase.RequiredGuardedTokenGroups) {
      const bool Found = llvm::any_of(GuardedBlocks, [&](StringRef Block) {
        return Block.contains("neverd.exception") &&
               Block.contains("highir.structured_regions=") &&
               Block.contains("fallback_regions=") &&
               Block.contains("__builtin_trap();") &&
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
       {WindowsEHNativeSourceReason::UnsupportedArchitecture}},
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
          << "target record was rejected as "
          << getWindowsEHNativeSourceReasonName(Classification.Reason);
      if (Expected != TestCase.ExpectedReasons.end())
        SawExpectedReason[std::distance(TestCase.ExpectedReasons.begin(),
                                        Expected)] = true;
      EXPECT_FALSE(Classification.canRegenerateLanguageMetadata());
    }
    EXPECT_GT(TargetRecords, 0u) << "no target language record was found";
    for (size_t I = 0; I < TestCase.ExpectedReasons.size(); ++I)
      EXPECT_TRUE(SawExpectedReason[I])
          << "no target record was rejected as "
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
      {{"xcpt4", "msvc", "aarch64", "native", false, "o0"}, false,
       WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph},
      {{"xcpt4", "msvc", "aarch64", "native", false, "o2"}, false,
       WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph},
      {{"xcpt4", "clang-cl", "aarch64", "native", false, "o0"}, true,
       WindowsEHNativeSourceReason::Eligible},
      {{"xcpt4", "clang-cl", "aarch64", "native", false, "o2"}, true,
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
          !llvm::all_of(Function.SEH->Scopes,
                        [](const SEHScopeRecord &Scope) {
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
          "corpus_aarch64_seh_" +
              llvm::utohexstr(Function.CodeRange.Begin));
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
                return Edge.RegionIndex == 0 &&
                       Edge.Kind == ExceptionalEdgeKind::SEHHandler &&
                       Edge.TargetVA == Scope.HandlerVA;
              }));
        }
        SawSemanticEnd |= Block.EndAddr == SemanticRange->End;
        if (Block.StartAddr == SemanticRange->End) {
          SawUnprotectedSuccessor = true;
          EXPECT_FALSE(llvm::any_of(
              Block.ExceptionalSuccs, [](const ExceptionalEdge &Edge) {
                return Edge.RegionIndex == 0 &&
                       Edge.Kind == ExceptionalEdgeKind::SEHHandler;
              }));
        }
      }
      EXPECT_TRUE(SawSemanticEnd);
      EXPECT_TRUE(SawUnprotectedSuccessor);

      LowToMedConverter Converter;
      Converter.setBinaryImage(&*ImageOrErr);
      MedFunc Med =
          Converter.convert(Low, Arch::AArch64, BinaryFormat::COFF);
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
      auto Plan =
          planCOFFExceptionPatch(*Module, *ImageOrErr, Arch::AArch64);
      ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
    }
    EXPECT_EQ(ParsedCatchAllRecords, 1u)
        << "loader found no ARM64 catch-all SEH scope\n"
        << RecordDiagnostics;
    EXPECT_EQ(IREligibleCatchAllRecords, 1u)
        << "parsed ARM64 catch-all record did not pass the IR gate\n"
        << RecordDiagnostics;
    EXPECT_EQ(OutputEligibleCatchAllRecords,
              TestCase.OutputEligible ? 1u : 0u)
        << "ARM64 output boundary did not match the pinned source shape\n"
        << RecordDiagnostics;
  }
}

} // namespace
