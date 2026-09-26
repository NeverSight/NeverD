//===- KernelModelMDLUserMappings.cpp - Process-owned MDL views -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// User mappings alias existing RAM and retain independent process permissions.
/// Their lifetime never changes an MDL's system mapping or physical identity.
///
//===----------------------------------------------------------------------===//

#include "KernelException.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error userMappingError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

llvm::Error accessViolation() {
  return llvm::make_error<KernelGuestException>(
      exceptions::StatusAccessViolation);
}

llvm::Error mappingShortage() {
  return llvm::make_error<KernelGuestException>(StatusInsufficientResources);
}

uint64_t pageBase(uint64_t Address) {
  return Address & ~(profile::PageSize - 1);
}
} // namespace

llvm::Expected<KernelModel::UserMemoryRange>
KernelModel::resolveUserMemoryRange(uint64_t Address, uint64_t Length,
                                    bool ForWrite) const {
  if (!canCatchUserAccess(Address, Length) ||
      ExitedUserProcesses.contains(CurrentUserProcessID))
    return accessViolation();
  uint64_t Backing = 0;
  auto View = UserMdlViews.upper_bound({CurrentUserProcessID, Address});
  if (View != UserMdlViews.begin()) {
    --View;
    const auto &V = View->second;
    if (V.ProcessID == CurrentUserProcessID && Address >= V.Address &&
        Address - V.Address < V.Length &&
        Length <= V.Length - (Address - V.Address)) {
      if (ForWrite && !(V.Permissions & Write))
        return accessViolation();
      Backing = V.BackingAddress + (Address - V.Address);
    }
  }
  if (!Backing) {
    auto Allocation = UserAllocations.upper_bound(Address);
    if (Allocation == UserAllocations.begin())
      return accessViolation();
    --Allocation;
    const uint64_t Offset = Address - Allocation->first;
    if (Allocation->second.ProcessID != CurrentUserProcessID ||
        RevokedUserAllocations.contains(Allocation->first) ||
        Offset >= Allocation->second.Size ||
        Length > Allocation->second.Size - Offset)
      return accessViolation();
    Backing = Address;
  }
  auto Allowed =
      Memory.canAccess(Address, Length, ForWrite ? Read | Write : Read);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return accessViolation();
  auto Owner = Physical.ownerForRange(Backing, Length);
  if (!Owner)
    return Owner.takeError();
  return UserMemoryRange{*Owner, Backing - Physical.find(*Owner)->Backing,
                         Backing};
}

llvm::Expected<uint64_t>
KernelModel::mapUserMDL(uint64_t MDL, uint64_t RequestedAddress,
                        uint32_t Priority,
                        KernelPhysicalMemory::CacheType RequestedCache) {
  if (CurrentIRQL > APCLevel || !UserRequestContext ||
      !canCatchUserAccess(profile::UserMappedAliasBase, 1) ||
      ExitedUserProcesses.contains(CurrentUserProcessID))
    return userMappingError("user MDL mapping requires a live process context "
                            "at IRQL <= APC_LEVEL");
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || It->second.Owner == LockedMdl::Ownership::Driver ||
      It->second.Owner == LockedMdl::Ownership::ReleasedPages)
    return userMappingError("user mapping requires a built, live MDL");
  const auto &State = It->second;
  const uint32_t BasePriority =
      Priority & ~(MdlMappingNoWrite | MdlMappingNoExecute);
  if (BasePriority != LowPagePriority && BasePriority != NormalPagePriority &&
      BasePriority != HighPagePriority)
    return userMappingError("unsupported user MDL mapping priority or flags");
  auto Owner = Physical.ownerForRange(State.BackingAddress, State.ByteCount);
  if (!Owner)
    return Owner.takeError();
  auto Cache = Physical.cacheTypeForMapping(State.BackingAddress,
                                            State.ByteCount, RequestedCache);
  if (!Cache)
    return Cache.takeError();
  const auto *Backing = Physical.find(*Owner);
  const bool PrivateRequestPages =
      std::any_of(MDLs.begin(), MDLs.end(), [&](const auto &Entry) {
        const auto &Root = Entry.second;
        return Root.Owner == LockedMdl::Ownership::Request &&
               pageBase(Root.BackingAddress) == *Owner;
      });
  if (!UserAllocations.contains(*Owner) && !PrivateRequestPages &&
      ((Backing->Backing & (profile::PageSize - 1)) ||
       (Backing->Size & (profile::PageSize - 1))))
    return userMappingError("user MDL mapping requires private backing pages");
  if (auto Pool = Allocations.find(*Owner); Pool != Allocations.end()) {
    if ((!Pool->second.NonPaged && !State.Pin) ||
        (Backing->Backing & (profile::PageSize - 1)) ||
        (Backing->Size & (profile::PageSize - 1)))
      return userMappingError("user mapping of pool requires a nonpaged "
                              "allocation covering complete private pages");
  }
  const uint64_t Offset = State.BackingAddress & (profile::PageSize - 1);
  const uint64_t Span = (Offset + State.ByteCount + profile::PageSize - 1) &
                        ~(profile::PageSize - 1);
  const uint64_t End =
      profile::UserMappedAliasBase + profile::UserMappedAliasSize;
  uint64_t Base = profile::UserMappedAliasBase;
  if (RequestedAddress) {
    if ((RequestedAddress & (profile::PageSize - 1)) != Offset)
      return userMappingError("requested user MDL address must preserve the "
                              "MDL page offset");
    Base = pageBase(RequestedAddress);
  } else {
    for (const auto &[Key, View] : UserMdlViews) {
      if (View.ProcessID != CurrentUserProcessID)
        continue;
      if (Base <= View.PageBase && Span <= View.PageBase - Base)
        break;
      Base = std::max(Base, View.PageBase + View.MappedSize);
    }
  }
  if (Base < profile::UserMappedAliasBase || Base > End || Span > End - Base)
    return mappingShortage();
  for (const auto &[Key, View] : UserMdlViews)
    if (View.ProcessID == CurrentUserProcessID &&
        Base < View.PageBase + View.MappedSize && View.PageBase < Base + Span)
      return mappingShortage();
  if (auto E = Memory.validateBacking(pageBase(State.BackingAddress), Span))
    return E;
  const unsigned Permissions =
      Read | (State.Writable && !(Priority & MdlMappingNoWrite) ? Write : 0);
  if (auto E = Memory.mapAlias(Base, pageBase(State.BackingAddress), Span,
                               Permissions)) {
    if (E.isA<GuestMemoryLimitError>()) {
      llvm::consumeError(std::move(E));
      return mappingShortage();
    }
    return E;
  }
  if (auto E = Physical.commitMappingCache(State.BackingAddress,
                                           State.ByteCount, *Cache))
    return E;
  const uint64_t Address = Base + Offset;
  UserMdlViews.emplace(UserMdlViewKey{CurrentUserProcessID, Address},
                       UserMdlView{MDL, Address, Base, Span,
                                   State.BackingAddress, CurrentUserProcessID,
                                   State.ByteCount, Permissions});
  return Address;
}

llvm::Error KernelModel::unmapUserMDL(uint64_t Address, uint64_t MDL) {
  auto It = UserMdlViews.find({CurrentUserProcessID, Address});
  if (It == UserMdlViews.end() || It->second.MDL != MDL)
    return userMappingError(
        "user MDL unmapping requires its live mapping address");
  auto &View = It->second;
  if (!UserRequestContext || CurrentUserProcessID != View.ProcessID ||
      !canCatchUserAccess(Address, View.Length))
    return userMappingError("user MDL unmapping requires its creating process "
                            "at IRQL <= APC_LEVEL");
  if (auto E = Memory.validateBacking(View.PageBase, View.MappedSize))
    return E;
  if (auto E = prepareRevokeVirtualRange(View.PageBase, View.MappedSize))
    return E;
  if (auto E = Memory.unmapAlias(View.PageBase, View.MappedSize))
    return E;
  UserMdlViews.erase(It);
  return llvm::Error::success();
}

llvm::Error KernelModel::canReleaseMdlUserViews(uint64_t MDL) const {
  for (const auto &[Address, View] : UserMdlViews)
    if (View.MDL == MDL)
      return userMappingError("MDL still owns a live user mapping");
  return llvm::Error::success();
}

llvm::Error KernelModel::canReleaseUserViewsForBacking(uint64_t Address,
                                                       uint64_t Size) const {
  if (Size > UINT64_MAX - Address)
    return userMappingError("overflowing user MDL backing release");
  if (!Size)
    return llvm::Error::success();
  for (const auto &[Base, View] : UserMdlViews)
    if (Address < View.BackingAddress + View.Length &&
        View.BackingAddress < Address + Size)
      return userMappingError("backing storage still has a live user mapping");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateUserMdlViewAccess(uint64_t Address,
                                                   uint64_t Size,
                                                   bool IsWrite) const {
  for (const auto &[Key, View] : UserMdlViews) {
    if (!UserRequestContext || View.ProcessID != CurrentUserProcessID)
      continue;
    if (Address >= View.PageBase + View.MappedSize ||
        Address + Size <= View.PageBase)
      continue;
    if (!canCatchUserAccess(Address, Size))
      return userMappingError(
          "user MDL mapping belongs to another process context");
    // Page protection raises the guest access violation in a valid process
    // context, just as it does for revoked or read-only scenario buffers.
    if (IsWrite && !(View.Permissions & Write))
      continue;
    if (Address < View.Address || Address - View.Address >= View.Length ||
        Size > View.Length - (Address - View.Address))
      return userMappingError("guest access exceeds the user MDL byte range");
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
