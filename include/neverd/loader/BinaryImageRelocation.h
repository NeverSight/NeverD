//===- BinaryImageRelocation.h - Parsed relocation records --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The two relocation records a loader reports: the symbolic entry codegen
/// needs in order to re-resolve a reference, and the bare ASLR/PIC base
/// relocation that only says an address has to be rebased.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGERELOCATION_H
#define NEVERD_LOADER_BINARYIMAGERELOCATION_H

#include "neverd/Common.h"

#include <cstdint>
#include <string>

namespace neverd {

// ===--------------------------------------------------------------------===//
// RelocationEntry — parsed relocation for codegen
// ===--------------------------------------------------------------------===//

struct RelocationEntry {
  va_t Address = 0;
  int64_t Addend = 0;
  bool HasExplicitAddend = false;
  uint32_t Type = 0;
  std::string SymbolName;
  uint32_t SymbolIndex = 0;
  std::string SectionName;
};

// ===--------------------------------------------------------------------===//
// BaseRelocation — ASLR/PIC base relocation entry
// ===--------------------------------------------------------------------===//

struct BaseRelocation {
  va_t Address = 0;
  uint8_t Type = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGERELOCATION_H
