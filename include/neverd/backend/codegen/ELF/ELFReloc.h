//===- ELFReloc.h - ELF relocation handling ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF PLT/GOT and relocation resolution for patched binaries.
/// Handles both ELF32 and ELF64.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFRELOC_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFRELOC_H

#include "neverd/backend/codegen/RelocResolver.h"

namespace neverd {

class ELFRelocResolver : public RelocResolver {
public:
  bool parse(const std::vector<uint8_t> &Binary, Arch TargetArch) override;

  /// ELF populates PLT entries from loaded import data. Falls back to
  /// full binary parsing if the image has insufficient PLT information.
  bool populateFromImage(const BinaryImage &Image, Arch TargetArch) override;
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_ELF_ELFRELOC_H
