//===- MachOLiftedObject.h - Verified lifted Mach-O objects ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOLIFTEDOBJECT_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOLIFTEDOBJECT_H

#include "neverd/backend/codegen/PatchObject.h"

namespace neverd {
/// Verify lifted code and language metadata in the installed private candidate
/// before atomic publication. The image includes newly generated LSDA storage.
using MachOLiftedObjectVerifier = llvm::function_ref<llvm::Error(
    const CompiledImage &, const BinaryImage &Installed,
    va_t OriginalSectionSlide, llvm::ArrayRef<NativeMachOCompactRange>)>;

llvm::Expected<PatchResult>
installMachOLiftedObject(llvm::StringRef Object, const BinaryImage &Original,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         llvm::ArrayRef<SourcePatchBinding> Bindings,
                         const std::filesystem::path &Output,
                         MachOLiftedObjectVerifier Verify);
} // namespace neverd

#endif
