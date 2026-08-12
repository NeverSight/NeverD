//===- GoEHCorpusTests.cpp - Go runtime metadata corpus tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

Expected<std::vector<GoEHArtifactExpectation>> loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "go-eh.json";
  return loadGoEHCorpusManifest(ManifestPath.string(), true);
}

std::string diagnosticsFor(const ExceptionInfo &Info) {
  std::string Result;
  for (const std::string &Diagnostic : Info.Diagnostics) {
    if (!Result.empty())
      Result += "; ";
    Result += Diagnostic;
  }
  for (const ExceptionFunction &Function : Info.Functions)
    for (const std::string &Diagnostic : Function.Diagnostics) {
      if (!Result.empty())
        Result += "; ";
      Result += Diagnostic;
    }
  return Result;
}

bool containsString(ArrayRef<std::string> Values, StringRef Needle) {
  return llvm::any_of(
      Values, [&](const std::string &Value) { return Value == Needle; });
}

/// Load one artifact.  A file the loader does not recognize is reported rather
/// than skipped: the manifest already committed to the container, so failing
/// to recognize it is a regression and not a gap in the corpus.
std::optional<BinaryImage> loadArtifact(const std::filesystem::path &Path) {
  std::unique_ptr<Loader> ImageLoader = Loader::create(Path);
  if (!ImageLoader) {
    ADD_FAILURE() << "NeverD did not recognize " << Path.string();
    return std::nullopt;
  }
  auto ImageOrErr = ImageLoader->load(Path);
  if (!ImageOrErr) {
    ADD_FAILURE() << "cannot load " << Path.string() << ": "
                  << toString(ImageOrErr.takeError());
    return std::nullopt;
  }
  return std::move(*ImageOrErr);
}

/// What one image's Go frame records add up to.
struct GoCensus {
  uint64_t Functions = 0;
  uint64_t DeferSites = 0;
  uint64_t RecoverSites = 0;
  uint64_t PanicSites = 0;
  uint64_t OpenCodedDeferFunctions = 0;
  /// Frames whose `FUNCDATA_OpenCodedDeferInfo` was not just declared but
  /// read.  The two counts part company exactly when the record's shape was
  /// misjudged, which is the failure the count above cannot see.
  uint64_t OpenCodedDeferRecords = 0;
  uint64_t DeferReturnFunctions = 0;
};

GoCensus censusOf(const ExceptionInfo &Info) {
  GoCensus Census;
  for (const ExceptionFunction &Function : Info.Functions) {
    if (!Function.Go)
      continue;
    ++Census.Functions;
    Census.DeferSites += Function.Go->Defers.size();
    Census.RecoverSites += Function.Go->Recovers.size();
    Census.PanicSites += Function.Go->Panics.size();
    Census.OpenCodedDeferFunctions += Function.Go->UsesOpenCodedDefers;
    Census.OpenCodedDeferRecords += Function.Go->OpenCodedDeferInfo.has_value();
    Census.DeferReturnFunctions += Function.Go->DeferReturnOffset.has_value();
  }
  return Census;
}

TEST(GoEHCorpus, DeclaresEveryPclnTabGeneration) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  EXPECT_FALSE(ExpectationsOrErr->empty());

  std::set<std::string> Generations;
  std::set<BinaryFormat> Formats;
  std::set<Arch> Architectures;
  std::set<std::string> BuildModes;
  for (const GoEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    Generations.insert(Expectation.ExpectedPclnTabVersion);
    Formats.insert(Expectation.ExpectedFormat);
    Architectures.insert(Expectation.ExpectedArch);
    BuildModes.insert(Expectation.BuildMode);
  }
  EXPECT_EQ(Generations, (std::set<std::string>{"go1.2", "go1.16", "go1.18",
                                                "go1.20"}));
  EXPECT_EQ(Formats, (std::set<BinaryFormat>{BinaryFormat::ELF,
                                             BinaryFormat::COFF,
                                             BinaryFormat::MachO}));
  EXPECT_EQ(Architectures, (std::set<Arch>{Arch::X64, Arch::AArch64}));
  EXPECT_EQ(BuildModes,
            (std::set<std::string>{"exe", "pie", "c-shared"}));
}

// A Go image publishes nothing else: it carries no exception directory for its
// own code, no DWARF frame information, and — once stripped — no names either,
// so every later fact hangs off finding one table.  Where that table sits is a
// property of the container and the link rather than of the language: an ELF
// executable gets a section of its own, a position-independent one has it
// renamed into the relro segment, Mach-O spells it differently again, and PE
// gives it no section at all and leaves it somewhere inside `.rdata`.  Checking
// the recovered header against the placement the producer read out of the file
// is what separates a located table from a scan that stopped at the first
// plausible magic.
TEST(GoEHCorpus, LocatesPclnTabWhereTheLinkerLeftIt) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const GoEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    if (!Info.GoModule) {
      ADD_FAILURE() << "no pclntab was located; " << diagnosticsFor(Info);
      continue;
    }
    const GoModuleInfo &Module = *Info.GoModule;
    EXPECT_EQ(Module.PclnTabMagic, Expectation.PclnTabMagic);
    EXPECT_EQ(Module.PclnTabVersion, Expectation.ExpectedPclnTabVersion);
    EXPECT_EQ(Module.MinLC, Expectation.PclnTabMinLC);
    EXPECT_EQ(Module.PtrSize, Expectation.PclnTabPointerSize);
    EXPECT_EQ(Module.FunctionCount, Expectation.PclnTabFunctionCount);

    const Section *Table =
        Image->getSectionByName(Expectation.PclnTabSection);
    if (!Table) {
      ADD_FAILURE() << "no section named " << Expectation.PclnTabSection;
      continue;
    }
    EXPECT_TRUE(Table->contains(Module.PcHeaderVA))
        << "pcHeader 0x" << llvm::utohexstr(Module.PcHeaderVA)
        << " is outside " << Expectation.PclnTabSection;
    if (Expectation.PclnTabAtSectionStart)
      EXPECT_EQ(Module.PcHeaderVA, Table->VA);
    else
      EXPECT_GT(Module.PcHeaderVA, Table->VA);

    // From Go 1.18 the funcdata base lives in `moduledata` rather than in the
    // table, so a release that needs the module and did not find it can read
    // the function records but nothing behind them.
    if (Expectation.RequiresModuleData) {
      EXPECT_NE(Module.ModuleDataVA, 0u) << diagnosticsFor(Info);
      EXPECT_NE(Module.GoFuncBase, 0u) << diagnosticsFor(Info);
    }
  }

  EXPECT_GT(Images, 0u);
}

// Go's defer, panic, and recover are not spelled in any table the platform
// knows about, and since Go 1.14 the compiler open-codes a deferred call
// outright: there is no `deferproc` for a disassembler to find, only a
// `FUNCDATA_OpenCodedDeferInfo` record behind the function's funcdata pointer
// and a `deferreturn` offset the runtime re-enters the frame at.  A decoder
// that reads the function records but not the funcdata behind them recovers
// every Go function and no deferred call at all, which reads downstream as a
// program that defers nothing rather than as metadata that went unread.
TEST(GoEHCorpus, RecoversDeferPanicAndRecoverSites) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  unsigned OpenCodedImages = 0;
  for (const GoEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    // A `table-only` release predates the tables the defer state comes from,
    // so its contract stops at the pclntab and asking for more would fail an
    // artifact that is exactly what it claims to be.
    if (Expectation.ValidationLevel != GoCorpusValidationLevel::RuntimeGraph)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Go)
        continue;
      EXPECT_EQ(Function.Encoding, ExceptionEncoding::GoFuncTable);
      // The runtime resumes a panicking frame by jumping to this offset from
      // the function's entry, so an offset past the end of the body is a
      // misread record rather than an unusual function.
      if (Function.Go->DeferReturnOffset)
        EXPECT_LT(*Function.Go->DeferReturnOffset, Function.CodeRange.size())
            << "deferreturn lies outside "
            << (Function.Go->Name.empty() ? "the frame" : Function.Go->Name);
      for (const GoDeferSite &Defer : Function.Go->Defers)
        EXPECT_TRUE(Function.CodeRange.contains(Defer.CallVA));
      for (const GoRecoverSite &Recover : Function.Go->Recovers)
        EXPECT_TRUE(Function.CodeRange.contains(Recover.CallVA));
      for (const GoPanicSite &Panic : Function.Go->Panics)
        EXPECT_TRUE(Function.CodeRange.contains(Panic.CallVA));
    }

    const GoCensus Census = censusOf(Info);
    EXPECT_GE(Census.DeferSites, Expectation.MinDeferSites) << Diagnostics;
    EXPECT_GE(Census.RecoverSites, Expectation.MinRecoverSites) << Diagnostics;
    EXPECT_GE(Census.PanicSites, Expectation.MinPanicSites) << Diagnostics;
    EXPECT_GE(Census.OpenCodedDeferFunctions,
              Expectation.MinOpenCodedDeferFuncs)
        << Diagnostics;
    // A frame that defers must have a `deferreturn` for the runtime to resume
    // it at, so the two counts cannot be recovered independently.
    EXPECT_GT(Census.DeferReturnFunctions, 0u) << Diagnostics;
    OpenCodedImages += Expectation.MinOpenCodedDeferFuncs != 0;
  }

  EXPECT_GT(Images, 0u);
  // `-gcflags=all=-N -l` is the only thing that turns open coding off, so a
  // corpus that never exercises it leaves the funcdata path untested.
  EXPECT_LT(OpenCodedImages, Images);
}

// `FUNCDATA_OpenCodedDeferInfo` has been respelled twice since open-coded
// defers arrived in Go 1.14, and the pclntab magic moved at neither boundary:
// the per-defer argument fields went away inside the span of `Go118Magic`, and
// the closure slots became one run inside the span of `Go120Magic`.  A decoder
// that takes the record's shape from the header is therefore wrong on one side
// of each change, and wrong quietly -- the record does not parse, which costs
// every frame in the image its open-coded defer state while the function
// records around it keep reading, so the image looks like a program that
// defers nothing.  Reaching the shape the release actually wrote, from the
// bytes, is what this asserts.
TEST(GoEHCorpus, ReadsTheOpenCodedDeferRecordEachReleaseWrote) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  std::set<GoOpenCodedDeferLayout> Shapes;
  for (const GoEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    // `-N` clears `ssagen.hasOpenDefers`, so an unoptimized image has no
    // record to read and nothing to say about its shape.
    if (Expectation.MinOpenCodedDeferFuncs == 0)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    if (!Info.GoModule) {
      ADD_FAILURE() << "no pclntab was located; " << Diagnostics;
      continue;
    }
    EXPECT_EQ(Info.GoModule->OpenCodedDeferLayout,
              Expectation.OpenCodedDeferLayout)
        << Diagnostics;
    Shapes.insert(Expectation.OpenCodedDeferLayout);

    // Declaring the funcdata and reading it are separate things, and only the
    // second one needs the shape to be right.
    const GoCensus Census = censusOf(Info);
    EXPECT_GE(Census.OpenCodedDeferRecords, Expectation.MinOpenCodedDeferFuncs)
        << Diagnostics;
    EXPECT_EQ(Census.OpenCodedDeferRecords, Census.OpenCodedDeferFunctions)
        << "a frame declared open-coded defer info that could not be read; "
        << Diagnostics;
  }

  EXPECT_GT(Images, 0u);
  // Two of the three shapes belong to releases no magic separates, so a corpus
  // that reached only one of them would leave the reading this test is about
  // unexercised while still looking complete.
  EXPECT_EQ(Shapes.size(), 3u);
}

TEST(GoEHCorpus, ParsesDeclaredRuntimeMetadata) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  for (const GoEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    SCOPED_TRACE(Expectation.Path + " [go" + Expectation.GoVersion + ", " +
                 Expectation.GOOS + "-" + Expectation.GOARCH + ", " +
                 Expectation.BuildMode + ", " +
                 (Expectation.Stripped ? "stripped" : "symtab") + "]");

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
      EXPECT_TRUE(Image->hasSection(Name))
          << "the manifest requires section " << Name;
    // Only a C object contributes one of these, so their presence is the one
    // visible difference cgo makes to an image's unwind machinery.
    for (const std::string &Name : Expectation.NativeUnwindSections)
      EXPECT_TRUE(Image->hasSection(Name))
          << "the manifest requires cgo's " << Name;

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    EXPECT_TRUE(Info.hasModel(ExceptionModel::GoRuntime)) << Diagnostics;
    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed) << Diagnostics;
    EXPECT_TRUE(containsString(Expectation.AllowedParseStatuses,
                               getExceptionParseStatusName(Info.ParseStatus)))
        << "unexpected image exception parse status: "
        << getExceptionParseStatusName(Info.ParseStatus) << "; " << Diagnostics;

    const GoCensus Census = censusOf(Info);
    EXPECT_GE(Census.Functions, Expectation.MinGoFunctions) << Diagnostics;
    for (const ExceptionFunction &Function : Info.Functions)
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
          << Diagnostics;
  }
}

} // namespace
