//===- COFFReloc.h - COFF/PE relocation handling -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE import and relocation resolution for patched binaries.
/// Handles both PE32 and PE32+.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFRELOC_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFRELOC_H

#include "neverd/backend/codegen/RelocResolver.h"

namespace neverd {

class COFFRelocResolver : public RelocResolver {
public:
  bool parse(const std::vector<uint8_t> &Binary, Arch TargetArch) override;

  bool populateFromImage(const BinaryImage &Image, Arch TargetArch) override;
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_COFF_COFFRELOC_H
