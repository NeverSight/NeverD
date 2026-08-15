//===- AdaDEHCorpusTestsDetail.h - Ada/D corpus test harness --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CORPUS_ADA_D_EH_ADADEHCORPUSTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_CORPUS_ADA_D_EH_ADADEHCORPUSTESTSDETAIL_H

#include "AdaDEHCorpusManifest.h"
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

namespace neverd::ada_d_corpus_test {

inline llvm::Expected<std::vector<test::AdaDEHArtifactExpectation>>
loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "ada-d-eh.json";
  return test::loadAdaDEHCorpusManifest(ManifestPath.string(), true);
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

struct GraphCensus {
  uint64_t Records = 0;
  uint64_t CallSites = 0;
  uint64_t LandingPads = 0;
  uint64_t CatchClauses = 0;
  uint64_t TypeTableEntries = 0;
};

inline GraphCensus censusOf(const ExceptionInfo &Info) {
  GraphCensus Census;
  for (const ExceptionFunction &Function : Info.Functions) {
    if (!Function.Itanium)
      continue;
    const ItaniumEHInfo &LSDA = *Function.Itanium;
    ++Census.Records;
    Census.CallSites += LSDA.CallSites.size();
    Census.TypeTableEntries += LSDA.TypeTable.size();
    for (const ItaniumAction &Action : LSDA.Actions)
      if (Action.isCatch())
        ++Census.CatchClauses;

    std::set<va_t> Pads;
    for (const ItaniumCallSite &Site : LSDA.CallSites) {
      if (Site.LandingPadVA == 0)
        continue;
      Pads.insert(Site.LandingPadVA);
    }
    Census.LandingPads += Pads.size();
  }
  return Census;
}

} // namespace neverd::ada_d_corpus_test

#endif
