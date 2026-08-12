//===- CxxItaniumEHCorpusTests.cpp - C++ Itanium EH corpus tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CxxItaniumEHCorpusManifest.h"
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

Expected<std::vector<CxxItaniumEHArtifactExpectation>> loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "cxx-itanium-eh.json";
  return loadCxxItaniumEHCorpusManifest(ManifestPath.string(), true);
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

/// Whether \p Image carries the section the manifest calls \p Name.
///
/// Mach-O names a section by the segment it sits in, and the manifest spells
/// that the way the container does -- `__TEXT,__text`.  NeverD keeps the two
/// apart, so the pair has to be matched as a pair: `__text` alone appears in
/// more than one segment, and asking for it by section name would accept a
/// section that is not the one the manifest meant.
bool hasDeclaredSection(const BinaryImage &Image, StringRef Name) {
  const auto [Segment, Section] = Name.split(',');
  if (Section.empty())
    return Image.hasSection(Name);
  return llvm::any_of(Image.Sections, [&](const neverd::Section &Candidate) {
    return Candidate.Name == Section && Candidate.SegmentName == Segment;
  });
}

/// Everything an image's language-specific data areas add up to.  The manifest
/// states floors rather than counts, so what a reading has to survive is one
/// number per shape and not a fingerprint of one build.
struct GraphCensus {
  uint64_t Records = 0;
  uint64_t CallSites = 0;
  uint64_t LandingPads = 0;
  uint64_t CatchClauses = 0;
  uint64_t CleanupPads = 0;
  uint64_t TypeTableEntries = 0;
  uint64_t ExceptionSpecs = 0;
};

/// True when the chain \p Site names reaches a catch or an exception
/// specification.  A chain that names neither is destructor cleanup: the pad
/// runs it and resumes, so it stops nothing.
bool dispatchesOnAType(const ItaniumEHInfo &LSDA, const ItaniumCallSite &Site) {
  std::optional<uint64_t> Offset = Site.FirstActionOffset;
  for (size_t Step = 0; Offset && Step <= LSDA.Actions.size(); ++Step) {
    const ItaniumAction *Action = nullptr;
    for (const ItaniumAction &Candidate : LSDA.Actions)
      if (Candidate.TableOffset == *Offset) {
        Action = &Candidate;
        break;
      }
    if (!Action)
      break;
    if (!Action->isCleanup())
      return true;
    Offset = Action->NextActionOffset;
  }
  return false;
}

GraphCensus censusOf(const ExceptionInfo &Info) {
  GraphCensus Census;
  for (const ExceptionFunction &Function : Info.Functions) {
    if (!Function.Itanium)
      continue;
    const ItaniumEHInfo &LSDA = *Function.Itanium;
    ++Census.Records;
    Census.CallSites += LSDA.CallSites.size();
    Census.TypeTableEntries += LSDA.TypeTable.size();
    Census.ExceptionSpecs += LSDA.ExceptionSpecs.size();
    for (const ItaniumAction &Action : LSDA.Actions)
      if (Action.isCatch())
        ++Census.CatchClauses;

    // A pad is counted once per frame that reaches it: two call sites of one
    // frame sharing a pad are one place the unwinder can land, and the same
    // address in another frame would be a different frame's pad.
    std::set<va_t> Pads;
    std::set<va_t> CleanupOnly;
    for (const ItaniumCallSite &Site : LSDA.CallSites) {
      if (Site.LandingPadVA == 0)
        continue;
      Pads.insert(Site.LandingPadVA);
      if (!dispatchesOnAType(LSDA, Site))
        CleanupOnly.insert(Site.LandingPadVA);
    }
    Census.LandingPads += Pads.size();
    Census.CleanupPads += CleanupOnly.size();
  }
  return Census;
}

TEST(CxxItaniumEHCorpus, DeclaresCompleteBuildMatrix) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  EXPECT_EQ(ExpectationsOrErr->size(), 72u);

  std::set<std::string> Cells;
  std::set<BinaryFormat> Formats;
  std::set<Arch> Architectures;
  std::set<std::string> Levels;
  for (const CxxItaniumEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    Cells.insert(Expectation.Toolchain + "-" + Expectation.Target);
    Formats.insert(Expectation.ExpectedFormat);
    Architectures.insert(Expectation.ExpectedArch);
    Levels.insert(getCxxItaniumValidationLevelName(Expectation.ValidationLevel));
  }
  EXPECT_EQ(Cells.size(), 9u);
  EXPECT_EQ(Formats, (std::set<BinaryFormat>{BinaryFormat::ELF,
                                             BinaryFormat::COFF,
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

  for (const CxxItaniumEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
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

// The payoff of the whole product line.  An Itanium call-site table is what
// most C++ in the world dispatches through, and a reader that stops at "this
// frame has an LSDA" recovers no handler at all: the type a catch names lives
// in a table beside the call sites, reached through an action chain, and every
// step of that walk is a place a decoder can silently produce nothing.
TEST(CxxItaniumEHCorpus, RecoversTheCallSiteGraphOnEveryItaniumTarget) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  std::set<std::string> Containers;
  for (const CxxItaniumEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.ValidationLevel !=
        CxxItaniumCorpusValidationLevel::LsdaGraph)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    Containers.insert(Expectation.ObjectFormat);
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);

    std::set<std::string> Personalities;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Itanium)
        continue;
      Personalities.insert(getExceptionPersonalityName(Function.Personality));
      for (const ItaniumCallSite &Site : Function.Itanium->CallSites) {
        EXPECT_TRUE(Site.GuardedRange.isValid());
        // A guarded range and the pad it names both belong to the frame that
        // declared them.  Both go wrong together when the landing-pad base was
        // resolved against the wrong anchor, which is the failure that
        // otherwise yields plausible-looking but unrelated addresses.
        EXPECT_TRUE(Function.CodeRange.contains(Site.GuardedRange))
            << "call site at 0x" << utohexstr(Site.GuardedRange.Begin)
            << " lies outside its frame";
        if (Site.LandingPadVA == 0)
          continue;
        const Segment *Seg = Image->getSegmentFor(Site.LandingPadVA);
        EXPECT_TRUE(Seg != nullptr && Seg->isExecutable())
            << "landing pad 0x" << utohexstr(Site.LandingPadVA)
            << " does not name code";
      }
      // A catch names a slot, and the slot is what carries the type.  A filter
      // pointing past the table is the shape a misread type-table base takes.
      for (const ItaniumAction &Action : Function.Itanium->Actions)
        if (Action.isCatch())
          EXPECT_LE(static_cast<uint64_t>(Action.TypeFilter),
                    Function.Itanium->TypeTable.size())
              << "catch filter names a slot the type table does not have";
    }
    const bool PersonalityMatched =
        llvm::any_of(Expectation.Personalities, [&](const std::string &Name) {
          return Personalities.contains(Name);
        });
    EXPECT_TRUE(PersonalityMatched)
        << "parsed personalities did not include any the manifest allows; "
        << Diagnostics;

    const GraphCensus Census = censusOf(Info);
    EXPECT_GT(Census.Records, 0u) << Diagnostics;
    EXPECT_GE(Census.CallSites, Expectation.MinCallSites) << Diagnostics;
    EXPECT_GE(Census.LandingPads, Expectation.MinLandingPads) << Diagnostics;
    EXPECT_GE(Census.CatchClauses, Expectation.MinCatchClauses) << Diagnostics;
    EXPECT_GE(Census.CleanupPads, Expectation.MinCleanupPads) << Diagnostics;
    EXPECT_GE(Census.TypeTableEntries, Expectation.MinTypeTableEntries)
        << Diagnostics;
  }

  // Seven cells of eight variants, less each cell's control.  The two ARM
  // cells are not among them: EHABI carries its language data somewhere this
  // graph is not, which is what its own validation level says.
  EXPECT_EQ(Images, 49u);
  // The same table read out of three containers, which is the thing a single
  // target could never show.
  EXPECT_EQ(Containers,
            (std::set<std::string>{"elf", "macho", "pe"}));
}

// `-fno-exceptions` is the control, and what it decides is narrower than "no
// unwind information": the build keeps `-fasynchronous-unwind-tables`, so the
// frames are still described.  What it removes is the language data, and a
// frame with no LSDA cannot name a handler however the tables around it look.
TEST(CxxItaniumEHCorpus, KeepsTheExceptionFreeControlFreeOfLanguageData) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const CxxItaniumEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.ValidationLevel !=
        CxxItaniumCorpusValidationLevel::CfiOnly)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    // The control links the same C++ runtime as the probe beside it, so what
    // must carry no language data is the code the producer compiled -- named
    // here by the frames that lie inside this image's own text and not by the
    // whole image.  A record anywhere is still counted, because the claim is
    // about the probe's frames and a runtime that brought its own is exactly
    // what makes an image-wide claim untestable.
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Itanium)
        continue;
      EXPECT_FALSE(Function.Itanium->isCleanupOnly() &&
                   Function.Itanium->CallSites.empty())
          << "an empty record is a decode that produced nothing; "
          << Diagnostics;
    }
    const GraphCensus Census = censusOf(Info);
    EXPECT_EQ(Census.CatchClauses, 0u)
        << "a build without exceptions cannot name a catch; " << Diagnostics;
  }

  // One control per cell.
  EXPECT_EQ(Images, 9u);
}

// ARM EHABI is the one container in this corpus NeverD does not yet read: the
// language data lives inline in `.ARM.extab` rather than in a
// `.gcc_except_table` section, so nothing here has an LSDA to find.  What the
// corpus can hold today is that the gap stays a gap -- the image loads, the
// decode reports no malformed record, and no Itanium record is invented from
// bytes that do not encode one.
//
// This is the test that changes when EHABI support lands: the artifacts are
// already built, already carry their floors, and are already declared at their
// own validation level, so what is left is to ask them for the graph.
TEST(CxxItaniumEHCorpus, ReportsArmEhabiAsAGapRatherThanAMisread) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const CxxItaniumEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.ValidationLevel != CxxItaniumCorpusValidationLevel::Ehabi)
      continue;
    ASSERT_TRUE(Expectation.ExpectArmEHABI);
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    EXPECT_EQ(Image->Arch, Arch::ARM);
    EXPECT_TRUE(Image->hasSection(".ARM.exidx"));
    EXPECT_FALSE(Image->hasSection(".gcc_except_table"));

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed) << Diagnostics;
    const GraphCensus Census = censusOf(Info);
    EXPECT_EQ(Census.Records, 0u)
        << "an EHABI image has no `.gcc_except_table`, so a record here was "
           "read out of something else; "
        << Diagnostics;
  }

  // Two ARM cells of eight variants, less each cell's control: an
  // exception-free build has no language data on any target, so it is the
  // control's level that names it and not the container's.
  EXPECT_EQ(Images, 14u);
}

} // namespace
