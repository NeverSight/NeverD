//===- RustEHCorpusManifest.h - Corpus manifest reader --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_RUSTEHCORPUSMANIFEST_H
#define NEVERD_UNITTESTS_LIFT_RUSTEHCORPUSMANIFEST_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::test {

/// How much of a Rust artifact's panic machinery the manifest claims is
/// recoverable.  `panic=abort` links a standard library that was still
/// compiled to unwind, so such an image keeps unwind tables that say nothing
/// about Rust panic semantics -- which is why an aborting cell asks only for
/// bounded, non-malformed records.
enum class RustCorpusValidationLevel : uint8_t {
  PanicGraph,
  UnwindOnly,
};

struct RustEHArtifactExpectation {
  std::string Path;
  std::string SHA256;
  uint64_t Size = 0;
  std::string TargetTriple;
  std::string Architecture;
  Arch ExpectedArch = Arch::Unknown;
  std::string ObjectFormat;
  BinaryFormat ExpectedFormat = BinaryFormat::Unknown;
  std::string CrateName;
  std::string CrateType;
  std::string PanicStrategy;
  std::string Optimization;
  std::string Execution;
  std::vector<std::string> RequiredSections;
  /// False for a linked PE executable, whose names live in a PDB the corpus
  /// does not ship; nothing keyed on a symbol can be asked of one.
  bool SymbolNamesExpected = false;
  RustCorpusValidationLevel ValidationLevel =
      RustCorpusValidationLevel::UnwindOnly;
  std::vector<std::string> AllowedParseStatuses;
  std::vector<std::string> Personalities;
  bool ExpectNoLandingPads = false;
  /// Frames the producer compiled that must carry no landing pad, named by
  /// symbol.  Under `panic=abort` that is every probe; under `panic=unwind` it
  /// is the one `extern "C"` probe whose body provably cannot panic.
  std::vector<std::string> LandingPadFreeSymbols;
  uint64_t MinLandingPads = 0;
  uint64_t MinDropGluePads = 0;
  uint64_t MinCatchUnwindPads = 0;
  uint64_t MinNoUnwindGuardPads = 0;
  uint64_t MinPanicSites = 0;
};

llvm::Expected<std::vector<RustEHArtifactExpectation>>
parseRustEHCorpusManifest(llvm::StringRef Contents, bool RequireCompleteMatrix);

llvm::Expected<std::vector<RustEHArtifactExpectation>>
loadRustEHCorpusManifest(llvm::StringRef Path, bool RequireCompleteMatrix);

} // namespace neverd::test

#endif
