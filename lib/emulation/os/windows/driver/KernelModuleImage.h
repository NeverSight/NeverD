//===- KernelModuleImage.h - Native guest module mapping plans ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native guest module mapping plans.
///

#ifndef NEVERD_EMULATION_KERNELMODULEIMAGE_H
#define NEVERD_EMULATION_KERNELMODULEIMAGE_H
#include "DriverImage.h"

#include "neverd/emulation/ModuleBinding.h"
namespace neverd::emulation {
class KernelExportRegistry;
struct KernelModuleUnwindInput {
  std::shared_ptr<const ExceptionInfo> Metadata;
  uint64_t PreferredBase = 0, HeaderSize = 0, CookieAddress = 0;
};
struct KernelModuleRuntimeData final : ModuleRuntimeData {
  inline static char ID = 0;
  const void *typeID() const override { return &ID; }
  KernelModuleUnwindInput Unwind;
  std::string SourceDigest;
  uint64_t Base = 0, Size = 0;
  struct Witness {
    uint64_t Address;
    std::vector<uint8_t> Bytes;
  };
  std::vector<Witness> Witnesses;
  DriverGuardControlFlow Guard;
  std::vector<uint64_t> CompatibilityExports;
  std::vector<Witness> GuardWitnesses;
};
struct KernelModulePreparation final : ModuleMappingPreparation {
  inline static char ID = 0;
  const void *typeID() const override { return &ID; }
  std::vector<DriverImport> Imports;
  PEProgramExports Exports;
  KernelModuleUnwindInput Unwind;
  DriverGuardControlFlow Guard;
};
/// Pure mapping of an explicitly supplied native x64 guest image. This grants
/// no boot-initialized kernel state, entry initialization or unmodeled DDI.
llvm::Expected<ModuleMappingPlan>
planKernelGuestModule(const ModuleBinding &, const modules::ModuleImage &,
                      uint64_t MemoryLimit);
llvm::Error finalizeKernelGuestModules(llvm::ArrayRef<ModuleInstance>,
                                       llvm::ArrayRef<ModuleMappingCandidate>,
                                       const KernelExportRegistry &);
struct KernelImportPatch {
  uint64_t Slot, Address;
};
/// Resolve prevalidated IAT identities against the same provider/forwarder
/// policy as new modules. Pure metadata; returned writes require a RAM lease.
llvm::Expected<std::vector<KernelImportPatch>> planKernelGuestImports(
    llvm::ArrayRef<ModuleInstance>, llvm::ArrayRef<DriverImport>,
    const KernelExportRegistry &, const std::shared_ptr<AddressSpace> &);
} // namespace neverd::emulation
#endif
