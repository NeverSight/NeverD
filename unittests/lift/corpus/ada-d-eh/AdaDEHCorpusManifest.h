//===- AdaDEHCorpusManifest.h - Corpus manifest reader --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_ADADEHCORPUSMANIFEST_H
#define NEVERD_UNITTESTS_LIFT_ADADEHCORPUSMANIFEST_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::test {

/// What a type-table slot names for this language runtime.
enum class AdaDDescriptorABI : uint8_t {
  GnatExceptionId,
  DClassInfo,
};

struct AdaDEHArtifactExpectation {
  std::string Path;
  std::string SHA256;
  uint64_t Size = 0;

  std::string Toolchain;
  std::string Target;
  std::string Architecture;
  Arch ExpectedArch = Arch::Unknown;
  std::string ObjectFormat;
  BinaryFormat ExpectedFormat = BinaryFormat::Unknown;
  std::string SourceLanguage;
  std::string Optimization;
  std::string Execution;

  std::vector<std::string> RequiredSections;
  std::vector<std::string> RequiredSymbols;
  std::vector<std::string> RequiredStrings;

  std::vector<std::string> Personalities;
  AdaDDescriptorABI DescriptorABI = AdaDDescriptorABI::GnatExceptionId;
  uint64_t MinCallSites = 0;
  uint64_t MinLandingPads = 0;
  uint64_t MinCatchClauses = 0;
  uint64_t MinCleanupPads = 0;
  uint64_t MinTypeTableEntries = 0;
};

const char *getAdaDDescriptorABIName(AdaDDescriptorABI ABI);

llvm::Expected<std::vector<AdaDEHArtifactExpectation>>
parseAdaDEHCorpusManifest(llvm::StringRef Contents, bool RequireCompleteMatrix);

llvm::Expected<std::vector<AdaDEHArtifactExpectation>>
loadAdaDEHCorpusManifest(llvm::StringRef Path, bool RequireCompleteMatrix);

} // namespace neverd::test

#endif
