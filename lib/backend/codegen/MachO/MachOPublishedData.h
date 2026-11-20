//===- MachOPublishedData.h - Installed Mach-O data ranges -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOPUBLISHEDDATA_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOPUBLISHEDDATA_H

#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/loader/MachO/DataInCode.h"

namespace neverd {
/// Merge section-relative MH_OBJECT records with the source image's DICE
/// table after assigning final VAs, before signing the private candidate.
llvm::Error installMachOPublishedData(
    std::vector<uint8_t> &Binary, const CompiledImage &Image,
    llvm::ArrayRef<std::pair<size_t, MachODataInCodeRange>> Records,
    llvm::ArrayRef<MachODataInCodeRange> Recovered = {}, va_t Slide = 0);
} // namespace neverd
#endif
