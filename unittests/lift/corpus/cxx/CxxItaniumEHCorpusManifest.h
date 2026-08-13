//===- CxxItaniumEHCorpusManifest.h - Corpus manifest reader --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CXXITANIUMEHCORPUSMANIFEST_H
#define NEVERD_UNITTESTS_LIFT_CXXITANIUMEHCORPUSMANIFEST_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::test {

/// How much of an artifact's exception machinery the manifest claims is
/// recoverable.  One C++ ABI reaches the unwinder three different ways, and
/// what a reader can be asked for differs by which one a target uses.
enum class CxxItaniumCorpusValidationLevel : uint8_t {
  /// A `.gcc_except_table` call-site table: the whole graph is recoverable,
  /// down to the type each catch names.
  LsdaGraph,
  /// ARM EHABI, where the language specific data area lives inline in
  /// `.ARM.extab` and there is no `.gcc_except_table` to read.
  Ehabi,
  /// The `-fno-exceptions` control, which keeps unwind tables and has no
  /// language data at all.
  CfiOnly,
};

const char *
getCxxItaniumValidationLevelName(CxxItaniumCorpusValidationLevel Level);

struct CxxItaniumEHArtifactExpectation {
  std::string Path;
  std::string SHA256;
  uint64_t Size = 0;

  std::string Toolchain;
  std::string Target;
  std::string Architecture;
  Arch ExpectedArch = Arch::Unknown;
  std::string ObjectFormat;
  BinaryFormat ExpectedFormat = BinaryFormat::Unknown;

  std::string Program;
  std::string ArtifactKind;
  std::string SourceLanguage;
  /// "on" or "off"; the one axis that decides whether language data exists.
  std::string Exceptions;
  std::string Optimization;
  bool Stripped = false;
  std::string Execution;

  std::vector<std::string> RequiredSections;
  std::vector<std::string> ForbiddenSections;
  std::vector<std::string> RequiredSymbols;
  std::vector<std::string> ForbiddenSymbols;
  /// Mangled type names the image must carry as data.  These survive stripping,
  /// which is what lets a stripped artifact still be identified by what it
  /// throws.
  std::vector<std::string> RequiredStrings;
  bool SymbolNamesExpected = false;
  bool EhFramePresent = false;
  bool ArmExidxPresent = false;
  uint64_t MinArmExidxEntries = 0;
  bool RequireUnwindTables = false;

  CxxItaniumCorpusValidationLevel ValidationLevel =
      CxxItaniumCorpusValidationLevel::CfiOnly;
  std::vector<std::string> Personalities;
  bool ExpectNoLSDA = false;
  bool ExpectArmEHABI = false;
  uint64_t MinCallSites = 0;
  uint64_t MinLandingPads = 0;
  uint64_t MinCatchClauses = 0;
  uint64_t MinCleanupPads = 0;
  uint64_t MinTypeTableEntries = 0;
};

llvm::Expected<std::vector<CxxItaniumEHArtifactExpectation>>
parseCxxItaniumEHCorpusManifest(llvm::StringRef Contents,
                                bool RequireCompleteMatrix);

llvm::Expected<std::vector<CxxItaniumEHArtifactExpectation>>
loadCxxItaniumEHCorpusManifest(llvm::StringRef Path,
                               bool RequireCompleteMatrix);

} // namespace neverd::test

#endif
