//===- CxxItaniumEHCorpusTestsDetail.h - C++ Itanium corpus test harness -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Manifest loading, artifact loading and the call-site census shared by
// the CxxItaniumEHCorpus* translation units.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CORPUS_CXX_CXXITANIUMEHCORPUSTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_CORPUS_CXX_CXXITANIUMEHCORPUSTESTSDETAIL_H

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

namespace neverd::cxx_corpus_test {

inline llvm::Expected<std::vector<test::CxxItaniumEHArtifactExpectation>>
loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "cxx-itanium-eh.json";
  return test::loadCxxItaniumEHCorpusManifest(ManifestPath.string(), true);
}

inline std::string diagnosticsFor(const ExceptionInfo &Info) {
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
inline std::optional<BinaryImage> loadArtifact(const std::filesystem::path &Path) {
  std::unique_ptr<Loader> ImageLoader = Loader::create(Path);
  if (!ImageLoader) {
    ADD_FAILURE() << "NeverD did not recognize " << Path.string();
    return std::nullopt;
  }
  auto ImageOrErr = ImageLoader->load(Path);
  if (!ImageOrErr) {
    ADD_FAILURE() << "cannot load " << Path.string() << ": "
                  << llvm::toString(ImageOrErr.takeError());
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
inline bool hasDeclaredSection(const BinaryImage &Image,
                               llvm::StringRef Name) {
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
inline bool dispatchesOnAType(const ItaniumEHInfo &LSDA, const ItaniumCallSite &Site) {
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

inline GraphCensus censusOf(const ExceptionInfo &Info) {
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

} // namespace neverd::cxx_corpus_test

#endif // NEVERD_UNITTESTS_LIFT_CORPUS_CXX_CXXITANIUMEHCORPUSTESTSDETAIL_H
