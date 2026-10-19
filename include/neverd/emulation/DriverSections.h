//===- DriverSections.h - Explicit named shared-memory sections -----------===//
// NeverD Decompiler
///
/// \file
/// Explicit named shared-memory sections.
///

#ifndef NEVERD_EMULATION_DRIVERSECTIONS_H
#define NEVERD_EMULATION_DRIVERSECTIONS_H
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace neverd::emulation {
struct DriverNamedSection {
  std::string Name;
  uint64_t Size = 0;
  std::vector<uint8_t> Bytes;
  // PAGE_READONLY or PAGE_READWRITE, with a cached SEC_COMMIT backing.
  uint32_t Protection = 4;
};
struct DriverSectionNamespace {
  // Each directory declares a complete subtree of section objects and
  // directories. Names outside these subtrees remain unknown.
  std::vector<std::string> Directories;
  std::vector<DriverNamedSection> Sections;
  // Provider-owned directory links, e.g. BaseNamedObjects\\Global. Targets
  // must be declared directories; these links cannot form cycles.
  std::map<std::string, std::string> Aliases;
};
llvm::Error validateDriverSectionNamespace(
    const std::optional<DriverSectionNamespace> &Namespace);
} // namespace neverd::emulation
#endif
