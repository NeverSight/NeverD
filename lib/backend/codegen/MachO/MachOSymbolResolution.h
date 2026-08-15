//===- MachOSymbolResolution.h - Patch symbol resolution -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal, fail-closed classification of unresolved symbols emitted while
/// compiling a Mach-O patch image.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSYMBOLRESOLUTION_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSYMBOLRESOLUTION_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct BinaryImage;

namespace macho_patch_detail {

enum class MachOSymbolUse : uint8_t { Direct, Callable, ImportSlot };

enum class MachOSymbolTargetKind : uint8_t { Callable, ImportSlot, Data };

struct MachOSymbolTarget {
  uint64_t Address = 0;
  MachOSymbolTargetKind Kind = MachOSymbolTargetKind::Data;
};

llvm::Expected<MachOSymbolUse> classifyMachOSymbolUse(
    Arch TargetArch,
    const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request);

llvm::Expected<std::optional<MachOSymbolTarget>>
resolveUniqueMachOSymbol(const BinaryImage &Image, llvm::StringRef Requested,
                         MachOSymbolUse Use);

} // namespace macho_patch_detail
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSYMBOLRESOLUTION_H
