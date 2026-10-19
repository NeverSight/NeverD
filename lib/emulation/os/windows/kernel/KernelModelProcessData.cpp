//===- KernelModelProcessData.cpp - Guest System process pointer slot
//------===//
// NeverD Decompiler
///
/// \file
/// Guest System process pointer slot.
///

#include "KernelModel.h"
#include "KernelProcessData.h"
#include "WindowsKernelLayout.h"
namespace neverd::emulation {
llvm::Error KernelModel::initializeProcessData() {
  uint64_t Slot = process_data::InitialSystemProcessSlot;
  if (Exports) {
    auto Address = Exports->resolve(process_data::InitialSystemProcess);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return llvm::Error::success();
    const auto *Export = Exports->lookup(*Address);
    if (Export &&
        Export->Kind == KernelExportRegistry::ExportKind::ModuleData) {
      InitialSystemProcessDataSlot = *Address;
      return llvm::Error::success();
    }
    if (!Export ||
        Export->Kind != KernelExportRegistry::ExportKind::ProcessData)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "System process export is not typed data");
    Slot = *Address;
  }
  auto System = processObject(uint32_t(windows::SystemProcessID));
  if (!System)
    return System.takeError();
  // The native image is an address/extent observation. Its saved pointer value
  // is never copied; the session already owns the corresponding process.
  if (auto E = Memory.writeInteger(Slot, *System, profile::PointerSize))
    return E;
  InitialSystemProcessDataSlot = Slot;
  return llvm::Error::success();
}
bool KernelModel::isProcessDataRead(uint64_t Address, uint32_t Size) const {
  return InitialSystemProcessDataSlot && Size &&
         Address >= *InitialSystemProcessDataSlot &&
         Size <= profile::PointerSize &&
         Address - *InitialSystemProcessDataSlot <= profile::PointerSize - Size;
}
} // namespace neverd::emulation
