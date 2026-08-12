//===- RustEHCorpusTests.cpp - Rust panic corpus tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RustEHCorpusManifest.h"
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

Expected<std::vector<RustEHArtifactExpectation>> loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "rust-eh.json";
  return loadRustEHCorpusManifest(ManifestPath.string(), true);
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

/// Address of the probe \p Name, trying the Mach-O spelling as well: a Mach-O
/// symbol table keeps the leading underscore the C ABI adds, while the
/// manifest records source-level names.
std::optional<va_t> resolveProbe(const BinaryImage &Image, StringRef Name) {
  const std::string Underscored = ("_" + Name).str();
  for (StringRef Candidate : {Name, StringRef(Underscored)}) {
    if (const Symbol *Sym = Image.findSymbol(Candidate); Sym && Sym->Addr != 0)
      return Sym->Addr;
    if (const Export *Exp = Image.findExport(Candidate); Exp && Exp->Addr != 0)
      return Exp->Addr;
  }
  return std::nullopt;
}

/// Every Rust landing pad the image recovered, tallied by classification.
struct PadCensus {
  uint64_t Total = 0;
  uint64_t DropGlue = 0;
  uint64_t CatchUnwind = 0;
  uint64_t NoUnwindGuard = 0;
  uint64_t PanicSites = 0;
};

PadCensus censusOf(const ExceptionInfo &Info) {
  PadCensus Census;
  for (const ExceptionFunction &Function : Info.Functions) {
    if (!Function.Rust)
      continue;
    Census.PanicSites += Function.Rust->Panics.size();
    for (const RustLandingPad &Pad : Function.Rust->LandingPads) {
      ++Census.Total;
      switch (Pad.Kind) {
      case RustLandingPadKind::DropGlue:
        ++Census.DropGlue;
        break;
      case RustLandingPadKind::CatchUnwind:
        ++Census.CatchUnwind;
        break;
      case RustLandingPadKind::NoUnwindGuard:
        ++Census.NoUnwindGuard;
        break;
      }
    }
  }
  return Census;
}

TEST(RustEHCorpus, DeclaresCompleteTargetMatrix) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  EXPECT_EQ(ExpectationsOrErr->size(), 40u);

  std::set<std::string> Targets;
  std::set<BinaryFormat> Formats;
  std::set<std::string> Strategies;
  std::set<Arch> Architectures;
  for (const RustEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    Targets.insert(Expectation.TargetTriple);
    Formats.insert(Expectation.ExpectedFormat);
    Strategies.insert(Expectation.PanicStrategy);
    Architectures.insert(Expectation.ExpectedArch);
  }
  EXPECT_EQ(Targets.size(), 5u);
  EXPECT_EQ(Formats, (std::set<BinaryFormat>{BinaryFormat::ELF,
                                             BinaryFormat::COFF,
                                             BinaryFormat::MachO}));
  EXPECT_EQ(Strategies, (std::set<std::string>{"unwind", "abort"}));
  EXPECT_EQ(Architectures, (std::set<Arch>{Arch::X64, Arch::AArch64}));
}

// Rust has no `catch` in the C++ sense.  A panic unwinds, runs `Drop` glue on
// the way out, and is stopped in exactly one place -- the pad `catch_unwind`
// compiles to -- while an `extern "C"` boundary is a third thing again: a pad
// that must abort rather than let the panic past.  Nothing in the code tells
// the three apart; only the action chain a call site names does.  A reader
// that stops at "this call site has a landing pad" therefore reports every
// Rust frame as cleanup, which reads downstream as a program that can neither
// catch nor abort, so requiring all three classifications to appear is what
// pins the chain walk.
TEST(RustEHCorpus, ClassifiesEveryItaniumLandingPadKind) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  PadCensus Total;
  for (const RustEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.ObjectFormat == "pe" ||
        Expectation.PanicStrategy != "unwind")
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
      if (!Function.Rust)
        continue;
      EXPECT_EQ(Function.Personality, ExceptionPersonality::RustEhPersonality);
      EXPECT_FALSE(Function.Rust->UsesMSVCTables);
      for (const RustLandingPad &Pad : Function.Rust->LandingPads) {
        EXPECT_NE(Pad.PadVA, 0u) << Diagnostics;
        EXPECT_TRUE(Pad.GuardedRange.isValid());
        // A pad is code, and the region it serves belongs to the frame that
        // declared it.  Both fail together when the LSDA's landing-pad base
        // was resolved against the wrong anchor, which is the failure that
        // otherwise produces plausible-looking but unrelated addresses.
        EXPECT_TRUE(Function.CodeRange.contains(Pad.GuardedRange))
            << "call site at 0x" << llvm::utohexstr(Pad.GuardedRange.Begin)
            << " lies outside its frame";
        const Segment *Seg = Image->getSegmentFor(Pad.PadVA);
        EXPECT_TRUE(Seg != nullptr && Seg->isExecutable())
            << "landing pad 0x" << llvm::utohexstr(Pad.PadVA)
            << " does not name code";
      }
    }

    const PadCensus Census = censusOf(Info);
    EXPECT_GE(Census.Total, Expectation.MinLandingPads) << Diagnostics;
    EXPECT_GE(Census.DropGlue, Expectation.MinDropGluePads) << Diagnostics;
    EXPECT_GE(Census.CatchUnwind, Expectation.MinCatchUnwindPads)
        << Diagnostics;
    EXPECT_GE(Census.NoUnwindGuard, Expectation.MinNoUnwindGuardPads)
        << Diagnostics;
    EXPECT_GE(Census.PanicSites, Expectation.MinPanicSites) << Diagnostics;
    Total.DropGlue += Census.DropGlue;
    Total.CatchUnwind += Census.CatchUnwind;
    Total.NoUnwindGuard += Census.NoUnwindGuard;
  }

  // Four Itanium targets, two optimization levels, two crates.
  EXPECT_EQ(Images, 16u);
  EXPECT_GT(Total.DropGlue, 0u);
  EXPECT_GT(Total.CatchUnwind, 0u);
  EXPECT_GT(Total.NoUnwindGuard, 0u);
}

// `-C panic=abort` still links a prebuilt standard library that was compiled
// to unwind, so an aborting image keeps `.eh_frame`, an except table, and
// `rust_eh_personality` -- an image-wide "no landing pads" check would fail on
// a perfectly correct artifact.  What the flag actually decides is narrower:
// no frame the producer compiled carries a pad.  Anchoring on the probes the
// manifest names is what makes that checkable without the standard library's
// own frames either drowning it or contradicting it, and it keeps working for
// an unwinding cell, where the claim narrows to the one `extern "C"` probe
// whose body provably cannot panic.
TEST(RustEHCorpus, KeepsDeclaredProbeFramesFreeOfLandingPads) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  unsigned Frames = 0;
  for (const RustEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (!Expectation.SymbolNamesExpected)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    for (const std::string &Name : Expectation.LandingPadFreeSymbols) {
      std::optional<va_t> Addr = resolveProbe(*Image, Name);
      if (!Addr) {
        ADD_FAILURE() << "the manifest names " << Name
                      << ", which this image does not";
        continue;
      }
      const ExceptionFunction *Function = Info.findFunction(*Addr);
      if (!Function || !Function->Rust)
        continue;
      ++Frames;
      EXPECT_TRUE(Function->Rust->LandingPads.empty())
          << Name << " carries "
          << getRustLandingPadKindName(Function->Rust->LandingPads.front().Kind)
          << ", which its build cannot produce";
    }
  }

  // Every cell but the four PE executables names its probes.
  EXPECT_EQ(Images, 36u);
  EXPECT_GT(Frames, 0u) << "no named probe resolved to a classified frame";
}

// On `*-pc-windows-msvc` a Rust frame is spelled with the same
// `__CxxFrameHandler3` tables as a C++ one, because LLVM picks the unwind
// table format from the personality's name.  The only thing that separates
// them is a catch naming the unmangled `rust_panic` type descriptor, so a
// reader that does not follow the descriptor pointer and compare the name sees
// a Rust image with no Rust frames in it at all.
TEST(RustEHCorpus, IdentifiesMSVCRustFramesByPanicTypeDescriptor) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const RustEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.ObjectFormat != "pe" ||
        Expectation.PanicStrategy != "unwind")
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    ASSERT_TRUE(Info.RustRuntime.has_value())
        << "no Rust runtime was recognized; " << diagnosticsFor(Info);
    const RustRuntimeInfo &Runtime = *Info.RustRuntime;
    EXPECT_TRUE(Runtime.UsesMSVCUnwinding);
    EXPECT_NE(Runtime.PanicTypeDescriptorVA, 0u)
        << "the rust_panic descriptor is what separates a Rust frame from a "
           "C++ one on this target";
    EXPECT_GT(Runtime.CatchUnwindFrames, 0u);
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Rust)
        continue;
      EXPECT_TRUE(Function.Rust->UsesMSVCTables);
      EXPECT_TRUE(isCxxPersonality(Function.Personality))
          << "a Rust frame on MSVC dispatches through the C++ personality";
    }

    const PadCensus Census = censusOf(Info);
    EXPECT_GE(Census.Total, Expectation.MinLandingPads);
    EXPECT_GE(Census.CatchUnwind, Expectation.MinCatchUnwindPads);
  }

  EXPECT_EQ(Images, 4u);
}

TEST(RustEHCorpus, ParsesDeclaredPanicMetadata) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  for (const RustEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    SCOPED_TRACE(Expectation.Path + " [" + Expectation.TargetTriple + ", " +
                 Expectation.PanicStrategy + ", " + Expectation.Optimization +
                 ", " + Expectation.CrateType + "]");

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

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed) << Diagnostics;
    EXPECT_TRUE(containsString(Expectation.AllowedParseStatuses,
                               getExceptionParseStatusName(Info.ParseStatus)))
        << "unexpected image exception parse status: "
        << getExceptionParseStatusName(Info.ParseStatus) << "; " << Diagnostics;

    // An aborting cell asks nothing of Rust panic semantics: the tables it
    // keeps belong to a standard library the producer did not compile.
    if (Expectation.ValidationLevel != RustCorpusValidationLevel::PanicGraph) {
      for (const ExceptionFunction &Function : Info.Functions)
        EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
            << Diagnostics;
      continue;
    }

    std::set<std::string> Personalities;
    for (const ExceptionFunction &Function : Info.Functions) {
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
          << Diagnostics;
      if (Function.Personality != ExceptionPersonality::None &&
          Function.Personality != ExceptionPersonality::Unknown)
        Personalities.insert(getExceptionPersonalityName(Function.Personality));
    }
    const bool PersonalityMatched =
        llvm::any_of(Expectation.Personalities, [&](const std::string &Name) {
          return Personalities.contains(Name);
        });
    EXPECT_TRUE(PersonalityMatched)
        << "parsed personalities did not include any manifest alternative";

    const PadCensus Census = censusOf(Info);
    EXPECT_GE(Census.Total, Expectation.MinLandingPads) << Diagnostics;
    EXPECT_GE(Census.DropGlue, Expectation.MinDropGluePads) << Diagnostics;
    EXPECT_GE(Census.CatchUnwind, Expectation.MinCatchUnwindPads)
        << Diagnostics;
    EXPECT_GE(Census.NoUnwindGuard, Expectation.MinNoUnwindGuardPads)
        << Diagnostics;
    EXPECT_GE(Census.PanicSites, Expectation.MinPanicSites) << Diagnostics;

    ASSERT_TRUE(Info.RustRuntime.has_value()) << Diagnostics;
    // The image-wide counters are a second reading of the same records, so a
    // classification that reached a per-function record but not the summary
    // (or the reverse) shows up here rather than downstream.
    EXPECT_EQ(Info.RustRuntime->PanicSites, Census.PanicSites);
    EXPECT_EQ(Info.RustRuntime->CatchUnwindFrames != 0,
              Census.CatchUnwind != 0);
    EXPECT_EQ(Info.RustRuntime->CleanupFrames != 0, Census.DropGlue != 0);
    EXPECT_EQ(Info.RustRuntime->NoUnwindGuardFrames != 0,
              Census.NoUnwindGuard != 0);
  }
}

} // namespace
