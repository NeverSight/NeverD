//===- MachOReloc.h - Mach-O relocation handling ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O stub and relocation resolution for patched binaries.
/// Handles both 32-bit and 64-bit Mach-O.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHORELOC_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHORELOC_H

#include "neverd/backend/codegen/RelocResolver.h"

namespace neverd {

class MachORelocResolver : public RelocResolver {
public:
  bool parse(const std::vector<uint8_t> &Binary, Arch TargetArch) override;

  bool populateFromImage(const BinaryImage &Image, Arch TargetArch) override;
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHORELOC_H
