//===- CxxItaniumEHCorpusTests.cpp - C++ Itanium EH corpus tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CxxItaniumEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
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

TEST(CxxItaniumEHCorpus, RewritesAndRunsTheHostMachOProbe) {
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
  auto Artifact = llvm::find_if(
      *ExpectationsOrErr, [&](const CxxItaniumEHArtifactExpectation &E) {
        return E.Toolchain == "clang" && StringRef(E.Target) == HostTarget &&
               E.Program == "cxx_eh_probe" && E.ArtifactKind == "exe" &&
               E.Exceptions == "on" && E.Optimization == "o0" && !E.Stripped;
      });
  ASSERT_NE(Artifact, ExpectationsOrErr->end());

  const std::filesystem::path Input =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / Artifact->Path;
  ASSERT_TRUE(std::filesystem::exists(Input));

  SmallString<128> Output;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("neverd-cxx-itanium-eh", "patched", Output));
  FileRemover RemoveOutput(Output);
  ASSERT_FALSE(sys::fs::remove(Output));

  const std::string InputString = Input.string();
  const std::string OutputString = Output.str().str();
  SmallVector<StringRef, 6> PatchArgs{
      NEVERD_BINARY, "patch", InputString, "-o", OutputString};
  std::string Error;
  ASSERT_EQ(sys::ExecuteAndWait(NEVERD_BINARY, PatchArgs, std::nullopt, {}, 0, 0,
                                &Error),
            0)
      << Error;

  SmallVector<StringRef, 1> RunArgs{OutputString};
  EXPECT_EQ(sys::ExecuteAndWait(OutputString, RunArgs, std::nullopt, {}, 0, 0,
                                &Error),
            0)
      << Error;
#endif
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
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
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
  EXPECT_EQ(Containers, (std::set<std::string>{"elf", "macho", "pe"}));
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
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ValidationLevel != CxxItaniumCorpusValidationLevel::CfiOnly)
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

// The same graph, out of a container that keeps it somewhere else entirely.
//
// ARM EHABI gives a C++ frame's language data no section of its own: the LSDA
// is appended to the frame's `.ARM.extab` entry, immediately after the unwind
// opcodes, and is reachable only by decoding the index that names the entry.
// An image here therefore carries a complete call-site table and no
// `.gcc_except_table` anywhere in it, which is what makes this the one cell
// where finding nothing looks exactly like there being nothing to find.
TEST(CxxItaniumEHCorpus, RecoversTheCallSiteGraphFromArmEhabiTables) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  std::set<std::string> Toolchains;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ValidationLevel != CxxItaniumCorpusValidationLevel::Ehabi)
      continue;
    ASSERT_TRUE(Expectation.ExpectArmEHABI);
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    Toolchains.insert(Expectation.Toolchain);
    SCOPED_TRACE(Expectation.Path);

    EXPECT_EQ(Image->Arch, Arch::ARM);
    EXPECT_TRUE(Image->hasSection(".ARM.exidx"));
    EXPECT_FALSE(Image->hasSection(".gcc_except_table"));

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed) << Diagnostics;
    EXPECT_TRUE(Info.hasModel(ExceptionModel::ARMEHABI)) << Diagnostics;

    // The index covers every function the linker placed, not only the ones
    // that can be unwound through, so it is also the most complete function
    // table a stripped image of this target has.
    uint64_t IndexEntries = 0;
    std::set<std::string> Personalities;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.ARMEHABI)
        continue;
      ++IndexEntries;
      Personalities.insert(getExceptionPersonalityName(Function.Personality));
      EXPECT_TRUE(Function.CodeRange.isValid())
          << "an index entry describes no code";
      if (!Function.Itanium)
        continue;
      // Language data reached through the index has to belong to the frame the
      // index named.  Both go wrong together when the entry's own extent was
      // taken from the wrong neighbour, which is the failure that otherwise
      // yields a plausible table attached to the function beside it.
      for (const ItaniumCallSite &Site : Function.Itanium->CallSites) {
        EXPECT_TRUE(Site.GuardedRange.isValid());
        EXPECT_TRUE(Function.CodeRange.contains(Site.GuardedRange))
            << "call site at 0x" << utohexstr(Site.GuardedRange.Begin)
            << " lies outside the frame its index entry named";
        if (Site.LandingPadVA == 0)
          continue;
        const Segment *Seg = Image->getSegmentFor(Site.LandingPadVA);
        EXPECT_TRUE(Seg != nullptr && Seg->isExecutable())
            << "landing pad 0x" << utohexstr(Site.LandingPadVA)
            << " does not name code";
      }
      for (const ItaniumAction &Action : Function.Itanium->Actions)
        if (Action.isCatch())
          EXPECT_LE(static_cast<uint64_t>(Action.TypeFilter),
                    Function.Itanium->TypeTable.size())
              << "catch filter names a slot the type table does not have";
    }
    EXPECT_GE(IndexEntries, Expectation.MinArmExidxEntries) << Diagnostics;

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

  // Two ARM cells of eight variants, less each cell's control: an
  // exception-free build has no language data on any target, so it is the
  // control's level that names it and not the container's.
  EXPECT_EQ(Images, 14u);
  // Both producers, because they do not spell this table the same way: GCC
  // writes the platform's type-table convention into the LSDA header where
  // Clang leaves the byte bare, and GCC reaches for the ARM-defined compact
  // personalities at `-O0` where Clang never does.
  EXPECT_EQ(Toolchains, (std::set<std::string>{"clang", "gcc"}));
}

// What a catch names, out of a table whose pointer encoding the header does
// not describe.
//
// EHABI hands a type-table slot to `_Unwind_decode_typeinfo_ptr`, which
// applies the platform's `R_ARM_TARGET2` convention and never reads the
// encoding byte.  Both producers here emit the same relocation and disagree
// about what to write in that byte, so a decoder that believes the header gets
// the Clang half of this cell wrong -- and gets it wrong quietly, reporting a
// displacement as though it were an address.
TEST(CxxItaniumEHCorpus, NamesTheTypesArmEhabiCatchesDispatchOn) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ValidationLevel != CxxItaniumCorpusValidationLevel::Ehabi ||
        Expectation.SourceLanguage != "cxx")
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);
    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);

    // The manifest requires the probe's own exception type as a string in the
    // image, because that is the one identity that survives stripping.  What
    // the type table has to do is arrive at it.
    std::set<std::string> Named;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Itanium)
        continue;
      for (const ItaniumTypeEntry &Entry : Function.Itanium->TypeTable)
        if (!Entry.TypeName.empty())
          Named.insert(Entry.TypeName);
    }
    for (const std::string &Required : Expectation.RequiredStrings) {
      const bool Reached = llvm::any_of(Named, [&](const std::string &Name) {
        return llvm::StringRef(Name).contains(Required);
      });
      EXPECT_TRUE(Reached)
          << "no catch reached the type `" << Required
          << "` the manifest requires this image to throw; " << Diagnostics;
    }
  }
  // The C probe carries no type table, so it is not among these.
  EXPECT_EQ(Images, 12u);
}

} // namespace
