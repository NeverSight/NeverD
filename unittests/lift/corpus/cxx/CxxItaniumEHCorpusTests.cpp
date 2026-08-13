//===- CxxItaniumEHCorpusTests.cpp - corpus matrix and container tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "CxxItaniumEHCorpusTestsDetail.h"

namespace {

using namespace llvm;
using namespace neverd;
using namespace neverd::test;
using namespace neverd::cxx_corpus_test;

TEST(CxxItaniumEHCorpus, DeclaresCompleteBuildMatrix) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  EXPECT_EQ(ExpectationsOrErr->size(), 72u);

  std::set<std::string> Cells;
  std::set<BinaryFormat> Formats;
  std::set<Arch> Architectures;
  std::set<std::string> Levels;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    Cells.insert(Expectation.Toolchain + "-" + Expectation.Target);
    Formats.insert(Expectation.ExpectedFormat);
    Architectures.insert(Expectation.ExpectedArch);
    Levels.insert(
        getCxxItaniumValidationLevelName(Expectation.ValidationLevel));
  }
  EXPECT_EQ(Cells.size(), 9u);
  EXPECT_EQ(Formats,
            (std::set<BinaryFormat>{BinaryFormat::ELF, BinaryFormat::COFF,
                                    BinaryFormat::MachO}));
  EXPECT_EQ(Architectures,
            (std::set<Arch>{Arch::X64, Arch::AArch64, Arch::ARM}));
  // One C++ ABI, three ways to reach the unwinder, and a control that reaches
  // it with no language data at all.
  EXPECT_EQ(Levels, (std::set<std::string>{"lsda-graph", "ehabi", "cfi-only"}));
}

TEST(CxxItaniumEHCorpus, MatchesDeclaredBytesAndContainer) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    SCOPED_TRACE(Expectation.Path);
    const std::filesystem::path ArtifactPath = CorpusRoot / Expectation.Path;
    auto BufferOrErr = MemoryBuffer::getFile(ArtifactPath.string());
    if (!BufferOrErr) {
      ADD_FAILURE() << "cannot read artifact: "
                    << BufferOrErr.getError().message();
      continue;
    }
    StringRef Bytes = (*BufferOrErr)->getBuffer();
    if (Bytes.size() != Expectation.Size) {
      ADD_FAILURE() << "artifact size does not match the manifest";
      continue;
    }
    std::array<uint8_t, 32> Digest = SHA256::hash(arrayRefFromStringRef(Bytes));
    if (toHex(ArrayRef<uint8_t>(Digest), true) != Expectation.SHA256) {
      ADD_FAILURE() << "artifact SHA-256 does not match the manifest";
      continue;
    }

    std::optional<BinaryImage> Image = loadArtifact(ArtifactPath);
    if (!Image)
      continue;
    EXPECT_EQ(Image->Format, Expectation.ExpectedFormat);
    EXPECT_EQ(Image->Arch, Expectation.ExpectedArch);
    for (const std::string &Name : Expectation.RequiredSections)
      EXPECT_TRUE(hasDeclaredSection(*Image, Name))
          << "the manifest requires section " << Name;
    for (const std::string &Name : Expectation.ForbiddenSections)
      EXPECT_FALSE(hasDeclaredSection(*Image, Name))
          << "the manifest forbids section " << Name;

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed)
        << diagnosticsFor(Info);
    for (const ExceptionFunction &Function : Info.Functions)
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
          << diagnosticsFor(Info);
  }
}

TEST(CxxItaniumEHCorpus, ResolvesMachOPersonalityPointerSlots) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  unsigned PersonalityImports = 0;
  unsigned VirtualTableTables = 0;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ExpectedFormat != BinaryFormat::MachO ||
        Expectation.ValidationLevel !=
            CxxItaniumCorpusValidationLevel::LsdaGraph)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    for (const Symbol &Sym : Image->Symbols)
      if (!Sym.IsFunc && StringRef(Sym.Name).contains("ZTT")) {
        ++VirtualTableTables;
        EXPECT_TRUE(Image->DataPtrRelocSlots.contains(Sym.Addr))
            << "Mach-O __DATA_CONST VTT slot was not classified as a "
               "relocated data pointer: "
            << Sym.Name;
      }

    auto Personality =
        llvm::find_if(Image->Imports, [](const Import &Candidate) {
          return Candidate.Name == "___gxx_personality_v0";
        });
    if (Personality == Image->Imports.end())
      continue;
    ++PersonalityImports;
    EXPECT_NE(Personality->IATAddr, 0u)
        << "an indirect personality encoding needs the concrete GOT slot";
  }
  EXPECT_GT(Images, 0u);
  EXPECT_GT(PersonalityImports, 0u);
  EXPECT_GT(VirtualTableTables, 0u);
}

TEST(CxxItaniumEHCorpus, RewritesAndRunsEveryHostMachOProbeVariant) {
#if !defined(__APPLE__) ||                                                   \
    (!defined(__aarch64__) && !defined(__x86_64__))
  GTEST_SKIP() << "the committed Mach-O probes only run on their host ISA";
#else
#if defined(__aarch64__)
  constexpr StringLiteral HostTarget = "arm64-apple-darwin";
#else
  constexpr StringLiteral HostTarget = "x86_64-apple-darwin";
#endif

  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  unsigned Rewritten = 0;
  for (const CxxItaniumEHArtifactExpectation &Artifact : *ExpectationsOrErr) {
    if (Artifact.Toolchain != "clang" ||
        StringRef(Artifact.Target) != HostTarget ||
        Artifact.Program != "cxx_eh_probe" ||
        Artifact.ArtifactKind != "exe" || Artifact.Exceptions != "on" ||
        Artifact.Execution != "passed")
      continue;

    ++Rewritten;
    SCOPED_TRACE(Artifact.Path);
    const std::filesystem::path Input =
        std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / Artifact.Path;
    ASSERT_TRUE(std::filesystem::exists(Input));

    SmallString<128> Output;
    ASSERT_FALSE(sys::fs::createTemporaryFile("neverd-cxx-itanium-eh",
                                               "patched", Output));
    FileRemover RemoveOutput(Output);
    ASSERT_FALSE(sys::fs::remove(Output));

    const std::string InputString = Input.string();
    const std::string OutputString = Output.str().str();
    SmallVector<StringRef, 6> PatchArgs{
        NEVERD_BINARY, "patch", InputString, "-o", OutputString};
    std::string Error;
    ASSERT_EQ(sys::ExecuteAndWait(NEVERD_BINARY, PatchArgs, std::nullopt, {}, 0,
                                  0, &Error),
              0)
        << Error;

    SmallVector<StringRef, 1> RunArgs{OutputString};
    EXPECT_EQ(sys::ExecuteAndWait(OutputString, RunArgs, std::nullopt, {}, 0, 0,
                                  &Error),
              0)
        << Error;
  }
  EXPECT_EQ(Rewritten, 4u);
#endif
}

} // namespace
