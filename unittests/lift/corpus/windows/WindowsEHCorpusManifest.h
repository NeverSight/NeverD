//===- WindowsEHCorpusManifest.h - Corpus manifest reader -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_WINDOWSEHCORPUSMANIFEST_H
#define NEVERD_UNITTESTS_LIFT_WINDOWSEHCORPUSMANIFEST_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::test {

enum class CorpusValidationLevel : uint8_t {
  ExceptionGraph,
  UnwindOnly,
  LoadOnly,
};

struct WindowsEHArtifactExpectation {
  std::string Path;
  std::string SHA256;
  uint64_t Size = 0;
  std::string Name;
  std::string Architecture;
  Arch ExpectedArch = Arch::Unknown;
  std::string Toolchain;
  std::string Optimization;
  std::string CxxFormat;
  std::string Execution;
  bool SecurityCookie = false;
  CorpusValidationLevel ValidationLevel = CorpusValidationLevel::LoadOnly;
  std::vector<std::string> AllowedParseStatuses;
  std::vector<std::string> Personalities;
  uint64_t MinExceptionFunctions = 0;
  uint64_t MinCxxFunctions = 0;
  uint64_t MinTryBlocks = 0;
  uint64_t MinSEHScopes = 0;
};

llvm::Expected<std::vector<WindowsEHArtifactExpectation>>
parseWindowsEHCorpusManifest(llvm::StringRef Contents,
                             bool RequireCompleteMatrix);

llvm::Expected<std::vector<WindowsEHArtifactExpectation>>
loadWindowsEHCorpusManifest(llvm::StringRef Path, bool RequireCompleteMatrix);

} // namespace neverd::test

#endif
