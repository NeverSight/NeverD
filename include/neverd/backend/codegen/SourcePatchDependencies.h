//===- SourcePatchDependencies.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Plan and verify explicit native dependencies.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_SOURCEPATCHDEPENDENCIES_H
#define NEVERD_BACKEND_CODEGEN_SOURCEPATCHDEPENDENCIES_H

#include "neverd/loader/BinaryImage.h"

#include <array>
#include <optional>

namespace llvm {
class Module;
}

namespace neverd {
enum class SourcePatchDependencyKind : uint8_t { Function, Data };
struct SourcePatchDependency {
  std::string Library;
  std::string Symbol;
  SourcePatchDependencyKind Kind = SourcePatchDependencyKind::Function;
};
struct SourcePatchImport {
  SourcePatchDependency Request;
  uint64_t SlotVA = 0;
  std::optional<std::array<uint8_t, 32>> MachODyldDigest;
};
struct SourcePatchDependencies {
  BinaryImage Image;
  std::vector<SourcePatchImport> Imports;
};

/// Validate request shape, uniqueness and resource limits before a native
/// format admits providers or allocates loader-owned slots.
llvm::Error validateSourcePatchDependencyRequests(
    llvm::ArrayRef<SourcePatchDependency> Requests);

/// Native object emission retains symbolic initializers and declarations.
/// Validate providers without replacing them with ordinary indirect loads.
llvm::Error validateSourcePatchDependencyDeclarations(
    const llvm::Module &Module, llvm::ArrayRef<SourcePatchDependency> Requests);

/// Append real native loader metadata, preserving all original import slots.
/// Requests have exact symbol identities; TLS, ordinal and version guessing
/// are not part of this contract. No filesystem output is produced.
llvm::Expected<SourcePatchDependencies>
prepareSourcePatchDependencies(const BinaryImage &Original,
                               llvm::ArrayRef<SourcePatchDependency> Requests);
llvm::Error
verifySourcePatchDependencies(const BinaryImage &Image,
                              llvm::ArrayRef<SourcePatchImport> Imports);

/// Linker-owned, position-independent tail thunk and its exact DWARF frame.
/// The sequence preserves arguments, the caller return address and ABI state.
llvm::Expected<std::vector<uint8_t>>
buildSourceImportThunk(Arch A, InstructionMode Mode, uint64_t VA,
                       uint64_t Slot);
llvm::Expected<std::vector<uint8_t>>
buildSourceImportThunkFrame(Arch A, uint64_t VA, uint64_t Size,
                            uint64_t FrameVA);

/// Use loader-populated slots for IR declarations, without resolving their
/// eventual runtime addresses in the patching process.
llvm::Error
bindSourcePatchDependencies(llvm::Module &Module,
                            llvm::ArrayRef<SourcePatchImport> Imports,
                            bool RewriteDeclarations = true);
} // namespace neverd
#endif
