//===- KernelModelFirmware.cpp - Owned read-only guest ACPI observations
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owned read-only guest ACPI observations.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error firmwareError(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool contains(uint64_t Base, uint64_t Length, uint64_t Address, uint32_t Size) {
  return Base && Size && Address >= Base && Size <= Length &&
         Address - Base <= Length - Size;
}
} // namespace

llvm::Error
KernelModel::configurePciMcfg(const std::optional<DriverPciMcfg> &Facts,
                              bool RequireSymbols) {
  if (!Facts)
    return llvm::Error::success();
  auto Count = validateDriverPciMcfg(*Facts);
  if (!Count)
    return Count.takeError();
  const bool HasSymbols = DebuggerReference && DebuggerReference->debugger() &&
                          DebuggerReference->debugger()->PciMcfgCount &&
                          DebuggerReference->debugger()->PciMcfgPointer;
  // A complete ACPI snapshot also determines MCFG without a reference image.
  // Keep that observation for queries; native globals are only published when
  // their authenticated symbol pair exists. Legacy pci_mcfg retains its gate.
  if (!RequireSymbols && !HasSymbols) {
    PciMcfg = *Facts;
    PciMcfgCount = *Count;
    return llvm::Error::success();
  }
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return firmwareError("pci_mcfg requires Windows 10.0.19041");
  if (!HasSymbols)
    return firmwareError(
        "pci_mcfg requires matching kernel PE/PDB MCFG symbols");
  PciMcfg = *Facts;
  PciMcfgCount = *Count;
  return llvm::Error::success();
}

llvm::Error KernelModel::initializePciMcfg() {
  if (!PciMcfg || !DebuggerReference || !DebuggerReference->debugger() ||
      !DebuggerReference->debugger()->PciMcfgCount ||
      !DebuggerReference->debugger()->PciMcfgPointer)
    return llvm::Error::success();
  const auto &Facts = *DebuggerReference->debugger();
  for (auto [Address, Size] :
       {std::pair<uint64_t, uint32_t>{Facts.PciMcfgCount, 4},
        {Facts.PciMcfgPointer, 8}}) {
    auto Writable = Memory.canAccess(Address, Size, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return firmwareError("MCFG reference fields have no provider backing");
  }
  uint64_t TablePointer = 0;
  if (PciMcfg->Present) {
    const auto &Bytes = PciMcfg->Table;
    const uint64_t MappingSize =
        (Bytes.size() + profile::PageSize - 1) & ~(profile::PageSize - 1);
    if (MappingSize > windows::FirmwareMcfgWindowSize)
      return firmwareError("MCFG table exceeds the firmware VA reservation");
    if (auto E = Memory.map(windows::FirmwareMcfgBase, MappingSize, Read))
      return E;
    if (auto E = Memory.writeBacking(windows::FirmwareMcfgBase, Bytes))
      return E;
    if (auto E = Physical.registerRegion(
            windows::FirmwareMcfgBase, windows::FirmwareMcfgBase, Bytes.size()))
      return E;
    TablePointer = windows::FirmwareMcfgBase;
  }
  // Addresses come from the authenticated PDB, values from the sole guest
  // firmware owner. Never copy native PE initializers or forward host state.
  if (auto E = Memory.writeInteger(Facts.PciMcfgCount, PciMcfgCount, 4))
    return E;
  if (auto E = Memory.writeInteger(Facts.PciMcfgPointer, TablePointer, 8))
    return E;
  PciMcfgPublished = true;
  return llvm::Error::success();
}

bool KernelModel::permitsPciMcfgFieldRead(uint64_t Address,
                                          uint32_t Size) const {
  if (!PciMcfgPublished)
    return false;
  const auto &Facts = *DebuggerReference->debugger();
  return contains(Facts.PciMcfgCount, 4, Address, Size) ||
         contains(Facts.PciMcfgPointer, 8, Address, Size);
}

llvm::Error KernelModel::validatePciMcfgAccess(uint64_t Address, uint32_t Size,
                                               bool IsWrite) const {
  // The enclosing guest validator has already rejected wrapping ranges and
  // canonicalized physical-section aliases before checking these fields.
  const uint64_t End = Address + Size;
  if (Address < windows::FirmwareMcfgBase + windows::FirmwareMcfgWindowSize &&
      windows::FirmwareMcfgBase < End) {
    if (IsWrite || !PciMcfgPublished || !PciMcfg->Present ||
        !contains(windows::FirmwareMcfgBase, PciMcfg->Table.size(), Address,
                  Size))
      return firmwareError("guest access to opaque or read-only MCFG table");
  }
  if (DebuggerReference && DebuggerReference->debugger()) {
    const auto &Facts = *DebuggerReference->debugger();
    for (auto [Base, Length] :
         {std::pair<uint64_t, uint64_t>{Facts.PciMcfgCount, 4},
          {Facts.PciMcfgPointer, 8}})
      if (Base && Address < Base + Length && Base < End) {
        if (!PciMcfgPublished)
          return firmwareError("MCFG reference fields require pci_mcfg");
        if (IsWrite || !permitsPciMcfgFieldRead(Address, Size))
          return firmwareError(
              "guest access to opaque or read-only MCFG field");
      }
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
