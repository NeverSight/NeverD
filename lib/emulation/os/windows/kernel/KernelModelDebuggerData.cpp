//===- KernelModelDebuggerData.cpp - Authenticated guest KD identities ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Authenticated guest KD identities.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
llvm::Error debuggerError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace
llvm::Error
KernelModel::setDebuggerReference(const KernelReferenceImage &Reference) {
  if (DriverObject)
    return debuggerError(
        "kernel debugger reference must precede model initialization");
  DebuggerReference = &Reference;
  if (!Reference.debugger())
    return llvm::Error::success();
  ProcessObjectSize = Reference.debugger()->ProcessSize;
  ThreadObjectSize = Reference.debugger()->ThreadSize;
  return llvm::Error::success();
}
llvm::Error KernelModel::initializeDebuggerState() {
  if (!DebuggerReference || !DebuggerReference->debugger())
    return llvm::Error::success();
  if (!DriverObject || !DebuggerData.empty())
    return debuggerError("invalid kernel debugger initialization state");
  const auto &Facts = *DebuggerReference->debugger();
  auto Bytes = Facts.Template;
  auto Pointer = [&](uint64_t Offset, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
  // One debugger-data block registered with the guest's debugger-data list.
  Pointer(0, Facts.ListHead);
  Pointer(8, Facts.ListHead);
  llvm::support::endian::write32le(Bytes.data() + 0x10, 0x4742444b); // KDBG
  llvm::support::endian::write32le(Bytes.data() + 0x14, Bytes.size());
  Pointer(0x18, DebuggerReference->base());
  Pointer(0x28, 0); // No prior bug check owns a saved context.
  Pointer(0x170, Facts.PagedPoolCommit);
  Pointer(0x2d8, Facts.LoaderBlock);
  if (GuestPageTables)
    Pointer(0x360, GuestPageTables->pteBase());
  // This guest profile has no L1TF PFN swizzling or host mitigation state.
  Bytes[0x2c2] = 0;
  Bytes[0x2c3] = 0;
  // No retpoline table is instantiated by the bounded CPU/kernel profile.
  Pointer(0x368, 0);
  for (uint64_t Offset : {0x370u, 0x374u, 0x378u})
    llvm::support::endian::write32le(Bytes.data() + Offset, 0);
  if (auto E = Memory.write(Facts.Address, Bytes))
    return E;
  if (auto E = Memory.writeInteger(Facts.ListHead, Facts.Address, 8))
    return E;
  if (auto E = Memory.writeInteger(Facts.ListHead + 8, Facts.Address, 8))
    return E;
  if (GuestPageTables && Facts.SystemVaAssignments) {
    // Reservations and allocators share one layout authority. Uninstantiated
    // subsystems have empty reservations, never copied host-kernel state.
    std::array<uint8_t, systemva::AssignmentBytes> Assignments{};
    auto Reserve = [&](systemva::Kind Kind, uint64_t Base, uint64_t Size) {
      auto *Entry = Assignments.data() + unsigned(Kind) * 16;
      llvm::support::endian::write64le(Entry, Base);
      llvm::support::endian::write64le(Entry + 8, Size);
    };
    Reserve(systemva::Kind::BootLoaded, DebuggerReference->base(),
            DebuggerReference->size());
    Reserve(systemva::Kind::NonPagedPool, profile::KernelArenaBase,
            KernelMemoryBytes);
    Reserve(systemva::Kind::PagedPool, systemva::PagedPoolBase,
            KernelMemoryBytes);
    Reserve(systemva::Kind::SystemPtes, systemva::SystemPteBase,
            systemva::SystemPteSize);
    for (const auto &Module : LoadedModules)
      if (Module.Guest)
        Reserve(systemva::Kind::DriverImages, Module.Base, Module.Size);
    if (auto E = Memory.write(Facts.SystemVaAssignments, Assignments))
      return E;
  }
  if (Facts.SecureCookie)
    if (auto E = Memory.writeInteger(Facts.SecureCookie, SecureMemoryCookie, 8))
      return E;
  if (Facts.ServiceDescriptor) {
    std::array<uint8_t, 32> Descriptor{};
    llvm::support::endian::write64le(Descriptor.data(), Facts.ServiceTable);
    llvm::support::endian::write32le(Descriptor.data() + 16,
                                     Facts.ServiceArgumentBytes.size());
    llvm::support::endian::write64le(Descriptor.data() + 24,
                                     Facts.ServiceArguments);
    if (auto E = Memory.write(Facts.ServiceDescriptor, Descriptor))
      return E;
    if (auto E = Memory.write(Facts.ServiceTable, Facts.CompactedServices))
      return E;
    if (auto E =
            Memory.write(Facts.ServiceArguments, Facts.ServiceArgumentBytes))
      return E;
  }
  if (Facts.GuardBitmapPointer) {
    auto Bitmap = std::make_unique<KernelGuardBitmap>(Memory);
    if (auto E = Bitmap->initialize(*DebuggerReference))
      return E;
    if (auto E =
            Memory.writeInteger(Facts.GuardBitmapPointer, Bitmap->pointer(), 8))
      return E;
    GuardBitmap = std::move(Bitmap);
  }
  if (Facts.ActiveProcessHead) {
    auto System = processObject(windows::SystemProcessID);
    if (!System)
      return System.takeError();
  }
  DebuggerData = std::move(Bytes);
  return synchronizeProcessList();
}

llvm::Error KernelModel::synchronizeProcessList() {
  if (!DebuggerReference || DebuggerData.empty())
    return llvm::Error::success();
  const auto &Facts = *DebuggerReference->debugger();
  std::vector<std::pair<uint64_t, uint64_t>> Writes;
  for (auto PID : LifecycleLiveProcesses)
    if (isLiveProcess(PID)) {
      auto Object = materializeProcessObject(PID);
      if (!Object)
        return Object.takeError();
    }
  for (auto [PID, Object] : ProcessObjects)
    if (Facts.ProcessIDOffset)
      Writes.emplace_back(Object + Facts.ProcessIDOffset, PID);
  if (Facts.ActiveProcessHead) {
    std::vector<uint64_t> Nodes{Facts.ActiveProcessHead};
    auto System = ProcessObjects.find(windows::SystemProcessID);
    if (System == ProcessObjects.end() ||
        ExitedUserProcesses.contains(windows::SystemProcessID))
      return debuggerError("active process list lost the System identity");
    Nodes.push_back(System->second + Facts.ActiveProcessLinksOffset);
    for (auto [PID, Object] : ProcessObjects)
      if (PID != windows::SystemProcessID && isLiveProcess(PID))
        Nodes.push_back(Object + Facts.ActiveProcessLinksOffset);
    for (size_t I = 0; I < Nodes.size(); ++I) {
      Writes.emplace_back(Nodes[I], Nodes[(I + 1) % Nodes.size()]);
      Writes.emplace_back(Nodes[I] + 8,
                          Nodes[(I + Nodes.size() - 1) % Nodes.size()]);
    }
  }
  for (auto [Address, Value] : Writes) {
    auto Allowed = Memory.canAccess(Address, 8, Write);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return debuggerError(
          "active process metadata is not writable by its owner");
  }
  for (auto [Address, Value] : Writes)
    if (auto E = Memory.writeInteger(Address, Value, 8))
      return E;
  return llvm::Error::success();
}

llvm::Error KernelModel::validateReferenceDataAccess(uint64_t Address,
                                                     uint32_t Size,
                                                     bool IsWrite) const {
  if (!DebuggerReference)
    return llvm::Error::success();
  const auto &Exports = DebuggerReference->exportData();
  if (!IsWrite && Exports.permitsRead(Address, Size))
    return llvm::Error::success();
  if (IsWrite && Exports.overlapsBytes(Address, Size))
    return debuggerError(
        "access to opaque or read-only kernel export metadata");
  if (!IsWrite && isObjectTypeDataRead(Address, Size))
    return llvm::Error::success();
  if (!IsWrite && isProcessDataRead(Address, Size))
    return llvm::Error::success();
  if (!IsWrite && isProcessorDataRead(Address, Size))
    return llvm::Error::success();
  if (!IsWrite && isNlsDataRead(Address, Size))
    return llvm::Error::success();
  if (!IsWrite && isHalDispatchRead(Address, Size))
    return llvm::Error::success();
  if (!IsWrite && permitsPciMcfgFieldRead(Address, Size))
    return llvm::Error::success();
  if (GuardBitmap)
    if (auto E = GuardBitmap->validateAccess(Address, Size, IsWrite))
      return E;
  for (const auto &Region : DebuggerReference->opaqueData()) {
    if (Address >= Region.Address + Region.Bytes.size() ||
        Address + Size <= Region.Address)
      continue;
    if (!DebuggerReference->debugger())
      return debuggerError(
          "access to an unmodeled reference kernel data field");
    const auto &Facts = *DebuggerReference->debugger();
    if (!IsWrite && !DebuggerData.empty())
      for (const auto &[Base, Length] :
           {std::pair<uint64_t, uint64_t>{Facts.Address, DebuggerData.size()},
            {Facts.ListHead, 16},
            {Facts.ServiceDescriptor, Facts.ServiceDescriptor ? 32u : 0u},
            {Facts.ServiceTable, Facts.CompactedServices.size()},
            {Facts.ServiceArguments, Facts.ServiceArgumentBytes.size()},
            {Facts.ActiveProcessHead, Facts.ActiveProcessHead ? 16u : 0u}})
        if (Address >= Base && Size <= Length &&
            Address - Base <= Length - Size)
          return llvm::Error::success();
    if (!IsWrite && !DebuggerData.empty() && GuestPageTables &&
        Facts.SystemVaAssignments && Address >= Facts.SystemVaAssignments &&
        Size <= systemva::AssignmentBytes &&
        Address - Facts.SystemVaAssignments <= systemva::AssignmentBytes - Size)
      return llvm::Error::success();
    if (!IsWrite && !DebuggerData.empty() && Facts.SecureCookie &&
        Address >= Facts.SecureCookie && Size <= 8 &&
        Address - Facts.SecureCookie <= 8 - Size)
      return llvm::Error::success();
    if (!IsWrite && !DebuggerData.empty() && GuardBitmap &&
        Address >= Facts.GuardBitmapPointer && Size <= 8 &&
        Address - Facts.GuardBitmapPointer <= 8 - Size)
      return llvm::Error::success();
    return debuggerError("access to an unmodeled or read-only reference kernel "
                         "data field at 0x" +
                         llvm::utohexstr(Address));
  }
  if (Exports.overlapsPage(Address, Size))
    return debuggerError("access to opaque kernel export metadata padding");
  return llvm::Error::success();
}
} // namespace neverd::emulation
