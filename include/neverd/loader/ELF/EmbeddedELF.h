//===- EmbeddedELF.h - File-contained ELF metadata ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validated embedded ELF extents and symbolic relocation references. These
/// are file coordinates, never mappings or functions of the containing image.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_EMBEDDEDELF_H
#define NEVERD_LOADER_ELF_EMBEDDEDELF_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

struct EmbeddedELFSection {
  std::string Name;
  uint64_t Offset = 0, Size = 0;
};

struct EmbeddedELFReference {
  /// Offsets from the embedded ELF header, before any relocation is applied.
  uint64_t From = 0, To = 0, Record = 0;
  uint32_t FromSection = 0, ToSection = 0, Type = 0;
  int64_t Addend = 0;
  std::string Symbol;
};

struct EmbeddedELFObject {
  uint64_t Offset = 0, Size = 0;
  uint16_t Machine = 0, Type = 0;
  uint8_t Bits = 0;
  bool BigEndian = false;
  uint64_t Relocations = 0, UnsupportedRelocations = 0,
           UnresolvedRelocations = 0;
  std::vector<EmbeddedELFSection> Sections;
  std::vector<EmbeddedELFReference> References;
  /// Malformed candidates publish no references. Unsupported relocation forms
  /// remain counted separately from undefined, non-file-backed or external
  /// symbols. This inventory does not claim executable semantics or
  /// reachability.
  std::string Diagnostic;
};

/// Scan all ELF signatures after file offset zero, including nested objects.
/// Validate tables/ranges through LLVM's ELF reader. No decompression,
/// decryption, relocation application or modification of the outer image.
std::vector<EmbeddedELFObject> scanEmbeddedELF(llvm::ArrayRef<uint8_t> Bytes);

} // namespace neverd

#endif // NEVERD_LOADER_ELF_EMBEDDEDELF_H
