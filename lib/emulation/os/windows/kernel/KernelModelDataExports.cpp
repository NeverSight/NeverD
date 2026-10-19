//===- KernelModelDataExports.cpp - Session-owned kernel data ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Initialize kernel data through the authoritative object and profile owners.
///
//===----------------------------------------------------------------------===//

#include "KernelDataExports.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <bit>

namespace neverd::emulation {
llvm::Error KernelModel::initializeDataExports(const DriverOptions &Options) {
  DebuggerStateDeclared = Options.DebuggerState.has_value();
  if (DebuggerStateDeclared)
    recordUnpackEnvironmentRead();
  if (!Exports)
    return llvm::Error::success();
  const bool HasKernel = std::any_of(
      LoadedModules.begin(), LoadedModules.end(), [](const auto &Module) {
        return Module.Base == profile::KernelModuleBase;
      });
  const bool HasData = std::any_of(
      Exports->entries().begin(), Exports->entries().end(), [](const auto &E) {
        return E.second.Kind == KernelExportRegistry::ExportKind::ModuleData;
      });
  if (!HasData || (!HasKernel && !DebuggerReference))
    return llvm::Error::success();
  const uint64_t Base =
      profile::KernelModuleBase + profile::KernelModuleDataRVA;
  // A reference image replaces the synthetic PE, while modeled data retains
  // its separate owner. Map only those slots, without inventing a second PE.
  if (!HasKernel)
    if (auto E = Memory.map(Base, profile::KernelModuleDataSize, Read | Write))
      return E;
  if (auto E =
          Memory.protect(Base, profile::KernelModuleDataSize, Read | Write))
    return E;
  for (const auto &[Address, Entry] : Exports->entries()) {
    if (Entry.Kind != KernelExportRegistry::ExportKind::ModuleData)
      continue;
    const auto *Data = kernel_data::lookup(Entry.Name);
    if (!Data || Address != Base + Data->Offset)
      return llvm::createStringError("invalid kernel data export identity");
    llvm::Expected<uint64_t> Value = [&]() -> llvm::Expected<uint64_t> {
#define NEVERD_KERNEL_DATA_EXPORT(Symbol, Offset, Size, Value)                 \
  if (Entry.Name == #Symbol)                                                   \
    return Value;
#include "KernelDataExports.def"
#undef NEVERD_KERNEL_DATA_EXPORT
      return llvm::createStringError("unmodeled kernel data export");
    }();
    if (!Value)
      return Value.takeError();
    if (auto E = Memory.writeInteger(Address, *Value, Data->Size))
      return E;
  }
  if (auto E = Memory.protect(Base, profile::KernelModuleDataSize, Read))
    return E;
  if (!DebuggerStateDeclared)
    return Memory.protect(Base + profile::PageSize, profile::PageSize, 0);
  return llvm::Error::success();
}

llvm::Error KernelModel::validateDataExportAccess(uint64_t Address,
                                                  uint32_t Size,
                                                  bool IsWrite) const {
  if (!Exports || !KernelExportRegistry::overlapsData(Address, Size))
    return llvm::Error::success();
  // Preflight is pure: neither a permission query nor validation alone records
  // an observed environment read. Model reads and CPU reads do so separately.
  if (IsWrite)
    return llvm::createStringError("kernel data exports are read-only");
  const uint64_t Debugger = profile::KernelModuleBase +
                            profile::KernelModuleDataRVA + profile::PageSize;
  if (!DebuggerStateDeclared && Address + Size > Debugger)
    return llvm::createStringError(
        "kernel debugger data requires debugger_state");
  return llvm::Error::success();
}
} // namespace neverd::emulation
