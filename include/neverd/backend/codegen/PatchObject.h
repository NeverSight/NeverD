//===- PatchObject.h - Native object replacement input ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_PATCHOBJECT_H
#define NEVERD_BACKEND_CODEGEN_PATCHOBJECT_H

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/SourcePatchBindings.h"

namespace neverd {
struct SourcePatchDependency;

/// Append a native arm64e object's storage, authenticated fixups and supported
/// native unwind records to a linked image. Does not change original entries,
/// sign the file, or grant source-publication/call ABI qualification. Explicit
/// dependencies name providers for the object's authenticated undefined
/// symbols. The binary is changed only after the entire transaction is
/// replayed.
llvm::Expected<CompiledImage>
appendMachOObjectImage(std::vector<uint8_t> &Binary, llvm::StringRef Object,
                       llvm::ArrayRef<SourcePatchDependency> Dependencies = {});

/// NeverC hosted DynCode carries a flat, verified payload and the native object
/// that owns its function extents/unwind records. The installer must compare
/// the linked code and entry layout with this audit before publishing.
struct DynCodePatchEvidence {
  std::string Payload;
  std::string Audit;
};

llvm::Error
validateDynCodePatchImage(const DynCodePatchEvidence &Evidence,
                          llvm::StringRef Object, const BinaryImage &Original,
                          const CompiledImage &Compiled,
                          llvm::ArrayRef<SourcePatchMapping> Mappings,
                          llvm::ArrayRef<SourcePatchBinding> Bindings);

/// Install one native ELF, COFF or Mach-O relocatable object, retaining code
/// and compiler-generated unwind bytes. Only declared relocations and decoded
/// unwind-address fields change. Symbol/section/relocation evidence belongs to
/// this producer; no LLVM MC owner or private function-range identity is
/// synthesized from object names.
llvm::Expected<PatchResult> installObjectPatch(
    llvm::StringRef Object, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> Bindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt = {},
    const DynCodePatchEvidence *DynCode = nullptr,
    const SourcePatchPreparation *Preparation = nullptr);

/// Format-owned native COFF parser and installer. Uses COFF auxiliary/section
/// extents and native relocations, independently of MC rewrite provenance.
llvm::Expected<PatchResult> installCOFFObjectPatch(
    llvm::StringRef Object, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> Bindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const DynCodePatchEvidence *DynCode = nullptr,
    const SourcePatchPreparation *Preparation = nullptr);

llvm::Expected<PatchResult> installMachOObjectPatch(
    llvm::StringRef Object, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> Bindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const DynCodePatchEvidence *DynCode = nullptr,
    const SourcePatchPreparation *Preparation = nullptr);

} // namespace neverd

#endif
