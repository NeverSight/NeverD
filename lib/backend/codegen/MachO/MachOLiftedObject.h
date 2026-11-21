//===- MachOLiftedObject.h - Verified lifted Mach-O objects ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOLIFTEDOBJECT_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOLIFTEDOBJECT_H

#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/loader/MachO/DataInCode.h"

#include <vector>

namespace neverd {
struct MachOLiftedSourceAddress;

/// A one-word address site could not reach its dyld pointer cell.
class MachOLiteralPoolOutOfRange
    : public llvm::ErrorInfo<MachOLiteralPoolOutOfRange> {
public:
  static char ID;
  explicit MachOLiteralPoolOutOfRange(llvm::ArrayRef<va_t> Instructions)
      : Instructions(Instructions.begin(), Instructions.end()) {}
  std::vector<va_t> Instructions;
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;
};

/// Verify lifted code and language metadata in the installed private candidate
/// before atomic publication. AddressPool is the installer's own section, if
/// it added one to place distant source addresses.
using MachOLiftedObjectVerifier = llvm::function_ref<llvm::Error(
    const CompiledImage &, const BinaryImage &Installed,
    va_t OriginalSectionSlide, llvm::ArrayRef<NativeMachOCompactRange>,
    const CompiledSection *AddressPool)>;

llvm::Expected<PatchResult>
installMachOLiftedObject(llvm::StringRef Object, const BinaryImage &Original,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         llvm::ArrayRef<SourcePatchBinding> Bindings,
                         const std::filesystem::path &Output,
                         llvm::ArrayRef<MachODataInCodeRange> RecoveredData,
                         llvm::ArrayRef<MachOLiftedSourceAddress> Addresses,
                         MachOLiftedObjectVerifier Verify);
} // namespace neverd

#endif
