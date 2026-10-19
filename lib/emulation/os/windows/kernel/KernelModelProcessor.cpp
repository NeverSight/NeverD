//===- KernelModelProcessor.cpp - Explicit guest processor observations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit guest processor observations.
///

#include "../../../arch/x86_64/X64Machine.h"
#include "KernelModel.h"
#include "KernelProcessorData.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
llvm::Error KernelModel::initializeProcessorData() {
  uint64_t Slot = processor::CountSlot;
  if (Exports) {
    auto Address = Exports->resolve(processor::CountExport);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return llvm::Error::success();
    const auto *Export = Exports->lookup(*Address);
    if (Export &&
        Export->Kind == KernelExportRegistry::ExportKind::ModuleData) {
      ProcessorCountDataSlot = *Address;
      return llvm::Error::success();
    }
    if (!Export ||
        Export->Kind != KernelExportRegistry::ExportKind::ProcessorData)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "processor count export is not typed data");
    Slot = *Address;
  }
  if (auto E = Memory.writeInteger(Slot, processor::ActiveCount,
                                   processor::CountBytes))
    return E;
  ProcessorCountDataSlot = Slot;
  return llvm::Error::success();
}
bool KernelModel::isProcessorDataRead(uint64_t Address, uint32_t Size) const {
  return ProcessorCountDataSlot && Size && Size <= processor::CountBytes &&
         Address >= *ProcessorCountDataSlot &&
         Address - *ProcessorCountDataSlot <= processor::CountBytes - Size;
}
llvm::Expected<uint64_t>
KernelModel::processorNumber(uint64_t Output, bool FromIndex, uint32_t Index) {
  if (FromIndex && Index >= processor::ActiveCount)
    return uint64_t(windows::StatusInvalidParameter);
  if (Output || FromIndex) {
    if (auto E = objectBuffer(Output, 4, true))
      return E;
    // PROCESSOR_NUMBER: group USHORT, processor UCHAR, reserved UCHAR.
    const uint32_t Value =
        processor::CurrentGroup | (uint32_t(processor::CurrentNumber) << 16);
    if (auto E = Memory.writeInteger(Output, Value, 4))
      return E;
  }
  return FromIndex ? uint64_t(0) : uint64_t(processor::CurrentIndex);
}
llvm::Expected<uint64_t>
KernelModel::groupAffinity(uint64_t Input, uint64_t Previous, bool Revert) {
  if (!CurrentThreadKey)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "group affinity requires a modeled execution thread");
  const bool Active = SystemAffinityThreads.contains(CurrentThreadKey);
  // Native revert does not inspect PreviousAffinity if there is no system
  // affinity to undo, including when its pointer is NULL.
  if (Revert && !Active)
    return uint64_t(0);
  if (auto E = objectBuffer(Input, 16, false))
    return E;
  auto Mask = Memory.readInteger(Input, 8),
       Group = Memory.readInteger(Input + 8, 2),
       Reserved = Memory.readInteger(Input + 10, 2),
       Tail = Memory.readInteger(Input + 12, 4);
  if (!Mask)
    return Mask.takeError();
  if (!Group)
    return Group.takeError();
  if (!Reserved)
    return Reserved.takeError();
  if (!Tail)
    return Tail.takeError();
  if (*Reserved || *Tail || *Group >= processor::GroupCount ||
      ((!Revert || *Mask) && !(*Mask & processor::ActiveMask)))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "group affinity requires the single active "
                                   "guest processor and zero reserved fields");
  if (Revert && !*Mask) {
    SystemAffinityThreads.erase(CurrentThreadKey);
    return uint64_t(0);
  }
  // The matched native routine masks its input in place before publishing
  // PreviousAffinity. This ordering also supports aliased input/output.
  if (auto E = objectBuffer(Input, 8, true))
    return E;
  if (auto E = Memory.writeInteger(Input, *Mask & processor::ActiveMask, 8))
    return E;
  SystemAffinityThreads.insert(CurrentThreadKey);
  if (!Revert && Previous) {
    if (auto E = objectBuffer(Previous, 16, true))
      return E;
    if (auto E = Memory.writeInteger(Previous,
                                     Active ? processor::ActiveMask : 0, 8))
      return E;
    if (auto E = Memory.writeInteger(Previous + 8, processor::CurrentGroup, 8))
      return E;
  }
  return uint64_t(0);
}

uint64_t KernelModel::guestCR4() const { return x64::CR4; }
bool KernelModel::hasModelSpecificRegisters() const {
  // The fixed x64 profile owns supervisor CET even without a kernel image.
  // Other indices still require their own provider in the read dispatcher.
  static_assert(!(x64::CR4 & (uint64_t(1) << 23)) && !x64::SupervisorCET);
  return true;
}
llvm::Expected<uint64_t>
KernelModel::readModelSpecificRegister(uint32_t Index) const {
  if (Index == windows::IA32SupervisorCET)
    return x64::SupervisorCET;
  if (Index == windows::IA32LStar && DebuggerReference &&
      DebuggerReference->debugger() &&
      DebuggerReference->debugger()->UnifiedSystemCallEntry &&
      windows::KernelVAShadowInformationFlags == 0)
    return DebuggerReference->debugger()->UnifiedSystemCallEntry;
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "unmodeled guest MSR: 0x" +
                                     llvm::utohexstr(Index));
}
} // namespace neverd::emulation
