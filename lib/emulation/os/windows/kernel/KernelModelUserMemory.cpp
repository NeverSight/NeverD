//===- KernelModelUserMemory.cpp - Declared user memory ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Request user regions, explicit guest pointers, and diagnostic snapshots.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "KernelReferenceImage.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
llvm::Error userMemoryError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

unsigned userPermissions(DriverUserPageAccess Access) {
  switch (Access) {
  case DriverUserPageAccess::ReadWrite:
    return Read | Write;
  case DriverUserPageAccess::ReadOnly:
    return Read;
  case DriverUserPageAccess::NoAccess:
    return 0;
  }
  llvm_unreachable("invalid driver user page access");
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::allocateUserBuffer(uint32_t Size, llvm::ArrayRef<uint8_t> Initial,
                                DriverUserPageAccess Access,
                                uint32_t ProcessID) {
  if (!Size)
    return 0;
  if (Initial.size() > Size || Size > KernelMemoryBytes)
    return userMemoryError("invalid synthetic user buffer extent");
  const uint64_t Pages =
      (uint64_t(Size) + profile::PageSize - 1) & ~(profile::PageSize - 1);
  const uint64_t End = profile::UserArenaBase + profile::UserArenaSize;
  if (NextUserAddress > End || Pages > End - NextUserAddress)
    return userMemoryError("synthetic user address space exhausted");
  const uint64_t Address = NextUserAddress;
  if (auto E = Memory.map(Address, Pages, userPermissions(Access)))
    return std::move(E);
  if (auto E = Physical.registerRegion(Address, Address, Size))
    return std::move(E);
  if (auto E = Memory.writeBacking(Address, Initial))
    return std::move(E);
  UserAllocations.emplace(Address, UserAllocation{Size, ProcessID, Access});
  NextUserAddress += Pages;
  return Address;
}

llvm::Error KernelModel::setUserRequestContext(bool Active,
                                               uint32_t ProcessID) {
  const bool Switching = UserRequestContext != Active ||
                         (Active && CurrentUserProcessID != ProcessID);
  std::vector<GuestAliasRange> Removing;
  std::vector<GuestAliasMapping> Adding;
  if (Switching) {
    const uint32_t OldPID = UserRequestContext
                                ? CurrentUserProcessID
                                : uint32_t(windows::SystemProcessID);
    const uint32_t NewPID =
        Active ? ProcessID : uint32_t(windows::SystemProcessID);
    for (const auto &[PID, Views] : ProcessMemoryViews) {
      if (PID == OldPID)
        for (const auto &V : Views) {
          if (auto E = canRevokeVirtualRange(V.Base, V.Size))
            return E;
          Removing.push_back({V.Base, V.Size});
        }
      if (PID == NewPID && !ExitedUserProcesses.contains(PID))
        for (const auto &V : Views) {
          auto Source = Memory.pinBacking(V.Backing, V.Size);
          if (!Source)
            return Source.takeError();
          if (!Source->describesSameBytes(V.Storage))
            return userMemoryError(
                "process memory backing changed allocation identity");
          Adding.push_back(
              {V.Base, V.Backing, V.Size, processMemoryPermissions(V)});
        }
    }
    for (const auto &[Key, View] : SectionViews) {
      if (View.ProcessID == OldPID) {
        if (auto E = canRevokeVirtualRange(View.Base, View.Size))
          return E;
        if (auto E =
                Dispatcher.validateGuestAccess(View.Base, View.Size, false))
          return E;
        for (size_t I = 0; I < View.Pages.size(); ++I)
          Removing.push_back(
              {View.Base + I * profile::PageSize, profile::PageSize});
      }
      if (View.ProcessID == NewPID)
        for (size_t I = 0; I < View.Pages.size(); ++I) {
          const auto &Page = View.Pages[I];
          auto Source = Memory.pinBacking(Page.Backing, profile::PageSize);
          if (!Source)
            return Source.takeError();
          if (!Source->describesSameBytes(Page.Storage))
            return userMemoryError(
                "section view source changed allocation identity");
          Adding.push_back({View.Base + I * profile::PageSize, Page.Backing,
                            profile::PageSize, View.Permissions});
        }
    }
    for (const auto &[Key, View] : UserMdlViews) {
      if (UserRequestContext && View.ProcessID == CurrentUserProcessID) {
        if (auto E = canRevokeVirtualRange(View.PageBase, View.MappedSize))
          return E;
        // Dispatcher identities are currently kernel virtual addresses. Keep
        // their original binding until the driver explicitly unmaps the view.
        if (auto E = Dispatcher.validateGuestAccess(View.PageBase,
                                                    View.MappedSize, false))
          return userMemoryError("cannot switch process while a user MDL view "
                                 "contains a dispatcher object: " +
                                 llvm::toString(std::move(E)));
        Removing.push_back({View.PageBase, View.MappedSize});
      }
      if (!Active || View.ProcessID != ProcessID)
        continue;
      auto Owner = Physical.ownerForRange(View.BackingAddress, View.Length);
      if (!Owner)
        return Owner.takeError();
      const uint64_t BackingPage =
          View.BackingAddress & ~(profile::PageSize - 1);
      if (auto E = Memory.validateBacking(BackingPage, View.MappedSize))
        return E;
      Adding.push_back(
          {View.PageBase, BackingPage, View.MappedSize, View.Permissions});
    }
  }
  for (const auto &[Address, Allocation] : UserAllocations) {
    const uint64_t Pages =
        (Allocation.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
    if (auto E = Memory.validateBacking(Address, Pages))
      return E;
  }
  // The backend preflights the whole replacement, including the final page
  // budget. A failed switch leaves the old process and all its permissions.
  if (!Removing.empty() || !Adding.empty())
    if (auto E = Memory.replaceAliases(Removing, Adding))
      return E;
  for (const auto &[Address, Allocation] : UserAllocations) {
    const uint64_t Pages =
        (Allocation.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
    const unsigned Permissions =
        Active && Allocation.ProcessID == ProcessID &&
                !ExitedUserProcesses.contains(ProcessID) &&
                !RevokedUserAllocations.contains(Address)
            ? userPermissions(Allocation.Access)
            : 0;
    if (auto E = Memory.protect(Address, Pages, Permissions))
      return E;
  }
  UserRequestContext = Active;
  CurrentUserProcessID = Active ? ProcessID : 0;
  if (Active)
    if (auto E = admitProcess(ProcessID))
      return E;
  if (Active && DebuggerReference && !DebuggerData.empty() &&
      DebuggerReference->debugger()->ActiveProcessHead) {
    auto Object = processObject(ProcessID);
    if (!Object)
      return Object.takeError();
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::prepareUserRequestBuffers(ActiveRequest &Request,
                                                   const DriverRequest &Input) {
  auto Allocate = [&](DriverUserBufferKind Kind, llvm::StringRef ID,
                      uint32_t Size, llvm::ArrayRef<uint8_t> Initial,
                      DriverUserPageAccess Access) -> llvm::Expected<uint64_t> {
    auto Address = allocateUserBuffer(Size, Initial, Access, Request.ProcessID);
    if (!Address)
      return Address.takeError();
    if (*Address)
      Request.UserRegions.push_back({Kind, ID.str(), *Address, Size});
    return *Address;
  };
  const bool IsWrite = Input.Kind == DriverRequestKind::Write;
  if (Input.Kind == DriverRequestKind::DeviceControl) {
    auto Address = Allocate(
        DriverUserBufferKind::Input, {}, Input.Input.size(), Input.Input,
        Input.UserInputAccess.value_or(DriverUserPageAccess::ReadWrite));
    if (!Address)
      return Address.takeError();
    Request.UserInput = *Address;
  }
  auto Address = Allocate(
      IsWrite ? DriverUserBufferKind::Input : DriverUserBufferKind::Output, {},
      IsWrite ? Input.Input.size() : Input.OutputSize,
      IsWrite ? llvm::ArrayRef<uint8_t>(Input.Input)
              : llvm::ArrayRef<uint8_t>(),
      (IsWrite ? Input.UserInputAccess : Input.UserOutputAccess)
          .value_or(DriverUserPageAccess::ReadWrite));
  if (!Address)
    return Address.takeError();
  Request.UserBuffer = *Address;
  auto &Observation = Result.Requests[Request.ResultIndex];
  for (const auto &Buffer : Input.UserBuffers) {
    auto Address = Allocate(DriverUserBufferKind::Memory, Buffer.ID,
                            Buffer.Size, Buffer.Input, Buffer.Access);
    if (!Address)
      return Address.takeError();
    DriverUserBufferResult BufferResult;
    BufferResult.ID = Buffer.ID;
    BufferResult.Address = *Address;
    BufferResult.Size = Buffer.Size;
    BufferResult.Access = Buffer.Access;
    Observation.UserBuffers.push_back(std::move(BufferResult));
  }

  auto Resolve = [&](const DriverUserBufferRef &Ref,
                     uint64_t Size) -> llvm::Expected<uint64_t> {
    auto Region =
        std::find_if(Request.UserRegions.begin(), Request.UserRegions.end(),
                     [&](const auto &Region) {
                       return Region.Kind == Ref.Kind && Region.ID == Ref.ID;
                     });
    if (Region == Request.UserRegions.end() || Ref.Offset > Region->Size ||
        Size > Region->Size - Ref.Offset)
      return userMemoryError(
          "pointer reference is outside declared user memory");
    return Region->Address + Ref.Offset;
  };
  std::vector<std::pair<uint64_t, uint64_t>> Patches;
  for (const auto &Pointer : Input.UserPointers) {
    auto Source = Resolve(Pointer.Source, profile::PointerSize);
    if (!Source)
      return Source.takeError();
    auto Target = Resolve(Pointer.Target, 0);
    if (!Target)
      return Target.takeError();
    if (auto E = Memory.validateBacking(*Source, profile::PointerSize))
      return E;
    Patches.emplace_back(*Source, *Target);
  }
  for (const auto &[Source, Target] : Patches) {
    static_assert(profile::PointerSize == sizeof(uint64_t));
    std::array<uint8_t, profile::PointerSize> Bytes;
    llvm::support::endian::write64le(Bytes.data(), Target);
    if (auto E = Memory.writeBacking(Source, Bytes))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::revokeRequestUserBuffers(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || !Request->Neither || !Request->DispatchReturned)
    return userMemoryError(
        "user unmapping requires a dispatched neither-I/O IRP");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  for (const auto &Region : Request->UserRegions) {
    const uint64_t Address = Region.Address;
    auto It = UserAllocations.find(Address);
    if (It == UserAllocations.end() || RevokedUserAllocations.contains(Address))
      return userMemoryError("user unmapping requires a live declared buffer");
    const uint64_t Pages =
        (It->second.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
    if (auto E = Memory.validateBacking(Address, Pages))
      return E;
    Ranges.emplace_back(Address, Pages);
  }
  for (const auto &[Address, Pages] : Ranges) {
    if (auto E = Memory.protect(Address, Pages, 0))
      return E;
    RevokedUserAllocations.insert(Address);
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::exitRequestorProcess(uint64_t IRP) {
  const auto *Request = requestForIRP(IRP);
  if (!Request || !Request->Neither || !Request->DispatchReturned ||
      !Request->ProcessID || ExitedUserProcesses.contains(Request->ProcessID))
    return userMemoryError(
        "requestor exit requires a live dispatched neither-I/O "
        "request and process");
  if (!ProcessNotifiers.empty())
    return userMemoryError("requestor exit with process callbacks requires an "
                           "explicit process_events exit");
  return retireUserProcess(Request->ProcessID);
}

llvm::Error KernelModel::retireUserProcess(uint32_t ProcessID) {
  if (!ProcessID || ProcessID == windows::SystemProcessID ||
      ExitedUserProcesses.contains(ProcessID))
    return userMemoryError(
        "process retirement requires a live non-System process");
  for (const auto &A : ProcessAttachments)
    if (A.TargetProcessID == ProcessID)
      return userMemoryError("cannot retire a process with a live attachment");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  for (const auto &[Address, Allocation] : UserAllocations) {
    if (Allocation.ProcessID != ProcessID ||
        RevokedUserAllocations.contains(Address))
      continue;
    const uint64_t Pages =
        (Allocation.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
    if (auto E = Memory.validateBacking(Address, Pages))
      return E;
    Ranges.emplace_back(Address, Pages);
  }
  const bool ActiveProcess =
      UserRequestContext && CurrentUserProcessID == ProcessID;
  std::vector<UserMdlViewKey> Views;
  std::vector<GuestAliasRange> Removing;
  if (auto It = ProcessMemoryViews.find(ProcessID);
      It != ProcessMemoryViews.end())
    for (const auto &V : It->second) {
      if (auto E = canRevokeVirtualRange(V.Base, V.Size))
        return E;
      if (ActiveProcess)
        Removing.push_back({V.Base, V.Size});
    }
  for (const auto &[Key, View] : SectionViews)
    if (View.ProcessID == ProcessID) {
      if (auto E = canRevokeVirtualRange(View.Base, View.Size))
        return E;
      for (const auto &[Owner, Pin] : View.Pins)
        if (auto E = Physical.canUnpin(Pin))
          return E;
      if (ActiveProcess)
        for (size_t I = 0; I < View.Pages.size(); ++I)
          Removing.push_back(
              {View.Base + I * profile::PageSize, profile::PageSize});
    }
  for (const auto &[Key, View] : UserMdlViews) {
    if (View.ProcessID != ProcessID)
      continue;
    if (ActiveProcess) {
      if (auto E = Memory.validateBacking(View.PageBase, View.MappedSize))
        return E;
      if (auto E = canRevokeVirtualRange(View.PageBase, View.MappedSize))
        return E;
      Removing.push_back({View.PageBase, View.MappedSize});
    }
    Views.push_back(Key);
  }
  if (!Removing.empty())
    if (auto E = Memory.replaceAliases(Removing, {}))
      return E;
  if (auto E = retireProcessSections(ProcessID, true))
    return E;
  for (const auto &Key : Views) {
    const auto &View = UserMdlViews.at(Key);
    if (ActiveProcess)
      if (auto E = prepareRevokeVirtualRange(View.PageBase, View.MappedSize))
        return E;
    UserMdlViews.erase(Key);
  }
  for (const auto &[Address, Pages] : Ranges) {
    if (auto E = Memory.protect(Address, Pages, 0))
      return E;
    RevokedUserAllocations.insert(Address);
  }
  if (!ProcessExitStatuses.contains(ProcessID))
    if (auto E = observeProcessExitStatus(ProcessID, std::nullopt))
      return E;
  ExitedUserProcesses.insert(ProcessID);
  ProcessMemoryViews.erase(ProcessID);
  for (auto It = ImageMappings.begin(); It != ImageMappings.end();)
    if (It->first.first == ProcessID)
      It = ImageMappings.erase(It);
    else
      ++It;
  LifecycleLiveProcesses.erase(ProcessID);
  for (auto Thread = CurrentThreadObjects.begin();
       Thread != CurrentThreadObjects.end();) {
    const uint64_t Object = Thread->second;
    if (ThreadObjects.at(Object).ProcessID != ProcessID) {
      ++Thread;
      continue;
    }
    auto &State = ThreadObjects.at(Object);
    State.Exited = true;
    State.ExitPriority = Scheduler.threadPriority(Thread->first);
    Thread = CurrentThreadObjects.erase(Thread);
    retireObjectIfUnreferenced(Object);
  }
  return synchronizeProcessList();
}

llvm::Error KernelModel::snapshotUserBuffers() {
  for (auto &Request : Result.Requests) {
    for (auto &Buffer : Request.UserBuffers) {
      auto Allocation = UserAllocations.find(Buffer.Address);
      if (Allocation == UserAllocations.end() ||
          Allocation->second.Size != Buffer.Size ||
          Allocation->second.Access != Buffer.Access)
        return userMemoryError("user buffer snapshot lost its allocation");
      Buffer.Revoked = RevokedUserAllocations.contains(Buffer.Address);
      std::vector<uint8_t> Bytes(Buffer.Size);
      if (auto E = Memory.snapshotBacking(Buffer.Address, Bytes))
        return E;
      Buffer.Backing = std::move(Bytes);
    }
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
