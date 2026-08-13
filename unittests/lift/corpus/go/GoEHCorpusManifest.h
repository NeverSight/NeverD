//===- GoEHCorpusManifest.h - Corpus manifest reader ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_GOEHCORPUSMANIFEST_H
#define NEVERD_UNITTESTS_LIFT_GOEHCORPUSMANIFEST_H

#include "neverd/Common.h"
#include "neverd/loader/LanguageEH.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::test {

/// How much of the Go runtime's frame metadata the manifest claims is
/// recoverable.  `table-only` marks a release whose `pclntab` predates the
/// funcdata tables the defer/panic state would come from, so only the header
/// and function records are expected of it.
enum class GoCorpusValidationLevel : uint8_t {
  RuntimeGraph,
  TableOnly,
};

struct GoEHArtifactExpectation {
  std::string Path;
  std::string SHA256;
  uint64_t Size = 0;
  std::string GoVersion;
  std::string GOOS;
  std::string GOARCH;
  Arch ExpectedArch = Arch::Unknown;
  std::string ObjectFormat;
  BinaryFormat ExpectedFormat = BinaryFormat::Unknown;
  std::string BuildMode;
  bool CgoEnabled = false;
  bool Stripped = false;
  std::string Optimization;

  std::vector<std::string> RequiredSections;
  std::string PclnTabSection;
  /// False only on PE, where the linker folds the table into `.rdata` behind
  /// other data instead of giving it a section of its own.
  bool PclnTabAtSectionStart = false;
  uint32_t PclnTabMagic = 0;
  uint8_t PclnTabMinLC = 0;
  uint8_t PclnTabPointerSize = 0;
  uint64_t PclnTabFunctionCount = 0;
  std::string SymbolTable;
  /// Empty when the manifest spells the funcdata base symbol as null, which a
  /// stripped ELF or PE image is.
  std::string GoFuncSymbol;
  /// Platform unwind tables cgo's C objects contribute beside the `pclntab`.
  std::vector<std::string> NativeUnwindSections;

  GoCorpusValidationLevel ValidationLevel = GoCorpusValidationLevel::TableOnly;
  std::vector<std::string> AllowedParseStatuses;
  std::string ExpectedPclnTabVersion;
  uint64_t MinGoFunctions = 0;
  uint64_t MinDeferSites = 0;
  uint64_t MinRecoverSites = 0;
  uint64_t MinPanicSites = 0;
  uint64_t MinOpenCodedDeferFuncs = 0;
  /// Shape of `FUNCDATA_OpenCodedDeferInfo` this release writes.  Its two
  /// boundaries are not the magic's, so this is the one claim in the contract
  /// a consumer cannot reach by reading the `pcHeader`.
  GoOpenCodedDeferLayout OpenCodedDeferLayout =
      GoOpenCodedDeferLayout::Contiguous;
  bool RequiresModuleData = false;
};

llvm::Expected<std::vector<GoEHArtifactExpectation>>
parseGoEHCorpusManifest(llvm::StringRef Contents, bool RequireCompleteMatrix);

llvm::Expected<std::vector<GoEHArtifactExpectation>>
loadGoEHCorpusManifest(llvm::StringRef Path, bool RequireCompleteMatrix);

} // namespace neverd::test

#endif
