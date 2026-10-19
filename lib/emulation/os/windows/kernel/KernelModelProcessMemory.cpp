//===- KernelModelProcessMemory.cpp - Process-owned committed RAM views
//----===//
// NeverD Decompiler
///
/// \file
/// Process-owned committed RAM views.
///

#include "../driver/DriverProcessImages.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/BackendFault.h"

#include <algorithm>
#include <array>
#include <iterator>
namespace neverd::emulation {
unsigned
KernelModel::processMemoryPermissions(const ProcessMemoryView &V) const {
  // Initial COW mappings must fault on writes until this process gets its
  // own page. Logical write admission is separate from adapter permissions.
  return V.Image && V.Image->PrivateBacking && !V.Image->Private
             ? V.Permissions & ~Write
             : V.Permissions;
}
llvm::Error KernelModel::initializeProcessMemory() {
  if (!ProcessInventory)
    return llvm::Error::success();
  struct SourceRecord {
    const DriverProcessImageMapping *Input;
    std::shared_ptr<ImageSection> Section;
  };
  std::vector<SourceRecord> Sources;
  for (const auto &P : *ProcessInventory) {
    if (!P.Memory && P.Images.empty())
      continue;
    auto &Views = ProcessMemoryViews[P.ProcessID];
    if (P.Memory)
      for (const auto &R : *P.Memory) {
        auto Backing =
            allocatePhysicalBuffer(R.Size, profile::PageSize, 0, R.Size);
        if (!Backing)
          return Backing.takeError();
        if (!R.Bytes.empty())
          if (auto E = Memory.write(*Backing, R.Bytes))
            return E;
        auto Storage = Memory.pinBacking(*Backing, R.Size);
        if (!Storage)
          return Storage.takeError();
        unsigned Permissions = 0;
        switch (R.Access) {
        case DriverUserPageAccess::ReadOnly:
          Permissions = Read;
          break;
        case DriverUserPageAccess::ReadWrite:
          Permissions = Read | Write;
          break;
        case DriverUserPageAccess::NoAccess:
          break;
        }
        if (auto E = Memory.protect(*Backing, R.Size, 0))
          return E;
        Views.push_back(
            {R.Base, R.Size, *Backing, Permissions, std::move(*Storage)});
      }
    for (const auto &Input : P.Images) {
      auto Existing =
          std::find_if(Sources.begin(), Sources.end(), [&](const auto &S) {
            return S.Input->MappedFile == Input.MappedFile &&
                   S.Input->DeviceName == Input.DeviceName &&
                   S.Input->FileBytes == Input.FileBytes;
          });
      if (Existing == Sources.end()) {
        std::shared_ptr<FileNode> File;
        auto Identity = driverProcessImageFileIdentity(Input, FiniteFileSystem);
        if (!Identity)
          return Identity.takeError();
        if (*Identity)
          File = FileNodes.at((*Identity)->Volume).at((*Identity)->Key);
        auto Section = imageSection(Input, std::move(File));
        if (!Section)
          return Section.takeError();
        Sources.push_back({&Input, std::move(*Section)});
        Existing = std::prev(Sources.end());
      }
      const auto &Section = Existing->Section;
      auto Pages = imagePages(Section, Section->Plan->Bytes.size());
      if (!Pages)
        return Pages.takeError();
      Views.insert(Views.end(), std::make_move_iterator(Pages->begin()),
                   std::make_move_iterator(Pages->end()));
      ImageMappings.emplace(
          std::pair{P.ProcessID, Section->Plan->Base},
          ImageMapping{Section, Section->Plan->Bytes.size(), false});
    }
  }
  return llvm::Error::success();
}
const KernelModel::ProcessMemoryView *
KernelModel::processMemoryView(uint32_t PID, uint64_t Address,
                               uint64_t Size) const {
  auto It = ProcessMemoryViews.find(PID);
  if (It == ProcessMemoryViews.end() || ExitedUserProcesses.contains(PID))
    return nullptr;
  for (const auto &V : It->second)
    if (Address >= V.Base && Address - V.Base < V.Size &&
        Size <= V.Size - (Address - V.Base))
      return &V;
  return nullptr;
}
llvm::Error KernelModel::validateProcessMemoryAccess(uint64_t Address,
                                                     uint32_t Size,
                                                     bool IsWrite) const {
  const uint32_t PID = UserRequestContext ? CurrentUserProcessID
                                          : uint32_t(windows::SystemProcessID);
  auto It = ProcessMemoryViews.find(PID);
  if (It == ProcessMemoryViews.end())
    return llvm::Error::success();
  for (const auto &V : It->second) {
    if (Address >= V.Base + V.Size || Address + Size <= V.Base)
      continue;
    const auto Context = executionProcessContext();
    if (!Context || Context->ProcessID != PID ||
        !canCatchUserAccess(Address, Size))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "process memory requires its active "
                                     "process context at IRQL <= APC_LEVEL");
    auto Space = Memory.addressSpace();
    if (!Space)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "process memory lost its address space");
    auto Source = Space->pinBacking(V.Backing, V.Size);
    if (!Source)
      return Source.takeError();
    if (!Source->describesSameBytes(V.Storage))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "process memory backing changed allocation identity");
  }
  if (IsWrite)
    return prepareProcessImageWrite(Address, Size);
  return llvm::Error::success();
}

llvm::Error KernelModel::prepareProcessImageWrite(uint64_t Address,
                                                  uint64_t Size) const {
  const uint32_t PID = UserRequestContext ? CurrentUserProcessID
                                          : uint32_t(windows::SystemProcessID);
  auto It = ProcessMemoryViews.find(PID);
  if (It == ProcessMemoryViews.end() || ExitedUserProcesses.contains(PID))
    return llvm::Error::success();
  std::vector<const ProcessMemoryView *> Pages;
  std::vector<GuestAliasRange> Remove;
  std::vector<GuestAliasMapping> Add;
  for (const auto &V : It->second) {
    if (!V.Image || !V.Image->PrivateBacking || V.Image->Private ||
        Address >= V.Base + V.Size ||
        Size <= V.Base - std::min(Address, V.Base))
      continue;
    if (!canCatchUserAccess(Address, Size))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "image COW requires its active process context at IRQL <= APC_LEVEL");
    if (auto E = canRevokeVirtualRange(V.Base, V.Size))
      return E;
    std::array<uint8_t, profile::PageSize> Bytes;
    if (auto E = Memory.readPinned(V.Storage, 0, Bytes))
      return E;
    if (auto E = Memory.writePinned(V.Image->PrivateStorage, 0, Bytes))
      return E;
    Pages.push_back(&V);
    Remove.push_back({V.Base, V.Size});
    Add.push_back({V.Base, V.Image->PrivateBacking, V.Size, V.Permissions});
  }
  if (Pages.empty())
    return llvm::Error::success();
  if (auto E = Memory.replaceAliases(Remove, Add))
    return E;
  for (const auto *V : Pages) {
    V->Backing = V->Image->PrivateBacking;
    V->Storage = V->Image->PrivateStorage;
    V->Image->Private = true;
    V->Image->Protection = V->Image->Protection == 0x80 ? 0x40 : 4;
  }
  return llvm::Error::success();
}
llvm::Expected<bool>
KernelModel::repairProcessImageWriteFault(const BackendFault &Fault) const {
  if (Fault.Kind != BackendFaultKind::Protection ||
      Fault.Access != BackendAccessKind::Write || !Fault.Address ||
      !Fault.Size || !*Fault.Size || *Fault.Size > UINT32_MAX ||
      *Fault.Size > UINT64_MAX - *Fault.Address)
    return false;
  const uint32_t PID = UserRequestContext ? CurrentUserProcessID
                                          : uint32_t(windows::SystemProcessID);
  const auto *V = processMemoryView(PID, *Fault.Address, 1);
  if (!V || !V->Image || !V->Image->PrivateBacking || V->Image->Private)
    return false;
  if (auto E = validateProcessMemoryAccess(*Fault.Address,
                                           uint32_t(*Fault.Size), true))
    return std::move(E);
  return V->Image->Private;
}
} // namespace neverd::emulation
