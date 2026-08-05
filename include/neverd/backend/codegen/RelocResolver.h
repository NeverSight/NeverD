//===- RelocResolver.h - Import/PLT/stub resolution ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Base class for format-specific import/PLT/stub resolution used by
/// the inplace rewriting pipeline.  Concrete implementations
/// (COFFRelocResolver, ELFRelocResolver, MachORelocResolver) provide
/// format-specific parsing.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_RELOCRESOLVER_H
#define NEVERD_BACKEND_CODEGEN_RELOCRESOLVER_H

#include "neverd/loader/BinaryImage.h"

#include <map>
#include <string>
#include <vector>

namespace neverd {

class RelocResolver {
public:
  virtual ~RelocResolver() = default;

  virtual bool parse(const std::vector<uint8_t> &Binary, Arch TargetArch) = 0;

  /// Populate resolver entries directly from an already-parsed BinaryImage,
  /// avoiding redundant re-parsing of the raw binary.  Subclasses that need
  /// format-specific information (e.g. ELF PLT addresses) can override.
  virtual bool populateFromImage(const BinaryImage &Image, Arch TargetArch);

  struct RelocEntry {
    std::string Name;
    va_t Addr = 0;
    uint32_t Size = 0;
    bool IsCode = false;
  };

  va_t findSymbol(const std::string &Symbol) const;
  const RelocEntry *findEntry(const std::string &Symbol) const;

  const std::vector<RelocEntry> &entries() const { return Entries; }

  /// Build a name-to-address map from resolved entries.
  std::map<std::string, uint64_t> buildAddrMap() const {
    std::map<std::string, uint64_t> Map;
    for (const auto &E : Entries)
      Map[E.Name] = E.Addr;
    return Map;
  }

protected:
  std::vector<RelocEntry> Entries;
  std::map<std::string, size_t> ByName;
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_RELOCRESOLVER_H
