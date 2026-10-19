//===- KernelModelReservedMappings.cpp - Reserved MDL system views --------===//
// NeverD Decompiler
//===----------------------------------------------------------------------===//
///
/// \file
/// Reserved MDL system views.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error reservationError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
uint64_t pageBase(uint64_t Address) {
  return Address & ~(profile::PageSize - 1);
}
} // namespace

uint64_t KernelModel::findSystemMappingAddress(uint64_t Size) const {
  const uint64_t First =
      GuestPageTables ? systemva::SystemMappingBase : profile::UserAliasBase;
  const uint64_t End = First + (GuestPageTables ? systemva::SystemMappingSize
                                                : profile::UserAliasSize);
  if (!Size || Size > End - First)
    return 0;
  std::vector<std::pair<uint64_t, uint64_t>> Used;
  for (const auto &[Address, State] : MDLs)
    if (State.Mapped && State.OwnsSystemMapping)
      Used.emplace_back(State.systemMappingBase(), State.AllocationSize);
  for (const auto &[Address, Reservation] : ReservedMappings)
    Used.emplace_back(Address, Reservation.Size);
  std::sort(Used.begin(), Used.end());
  uint64_t Candidate = First;
  for (const auto &[Base, Bytes] : Used) {
    if (Candidate <= Base && Size <= Base - Candidate)
      break;
    Candidate = std::max(Candidate, Base + Bytes);
  }
  return Candidate <= End && Size <= End - Candidate ? Candidate : 0;
}

llvm::Expected<uint64_t> KernelModel::allocateMappingAddress(uint64_t Size,
                                                             uint32_t Tag) {
  if (!Size)
    return reservationError("mapping reservation requires a nonzero size");
  if (!Tag || Size > (GuestPageTables ? systemva::SystemMappingSize
                                      : profile::UserAliasSize))
    return uint64_t(0);
  const uint64_t Bytes =
      (Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
  const uint64_t Address = findSystemMappingAddress(Bytes);
  if (Address)
    ReservedMappings.emplace(Address, ReservedMapping{Bytes, Tag});
  // No RAM/PFN or CPU mapping is published by reserving a virtual interval.
  return Address;
}

llvm::Expected<uint64_t> KernelModel::freeMappingAddress(uint64_t Address,
                                                         uint32_t Tag) {
  const auto It = ReservedMappings.find(Address);
  if (It == ReservedMappings.end() || It->second.Tag != Tag)
    return reservationError(
        "mapping reservation free requires its exact base and tag");
  if (It->second.MDL)
    return reservationError("mapping reservation must be unmapped before free");
  if (auto E = canRevokeVirtualRange(Address, It->second.Size))
    return E;
  ReservedMappings.erase(It);
  return uint64_t(0);
}

llvm::Expected<uint64_t>
KernelModel::mapReservedMapping(llvm::ArrayRef<uint64_t> A) {
  const auto Reservation = ReservedMappings.find(A[0]);
  if (Reservation == ReservedMappings.end() ||
      Reservation->second.Tag != uint32_t(A[1]))
    return reservationError(
        "reserved mapping requires its exact reservation base and tag");
  if (Reservation->second.MDL)
    return reservationError("reservation already has a live mapping");
  const auto It = MDLs.find(A[2]);
  if (It == MDLs.end())
    return reservationError("reserved mapping requires a live locked MDL");
  auto &State = It->second;
  if (!(State.isProbeLocked() ||
        State.Owner == LockedMdl::Ownership::AllocatedPages ||
        (State.Owner == LockedMdl::Ownership::Partial && State.LockOwnerMDL)))
    return reservationError("reserved mapping supports probe-locked, "
                            "allocated-page and locked partial MDLs");
  if (State.Mapped)
    return reservationError(
        "reserved mapping cannot create a second system mapping");
  const uint64_t Offset = State.BackingAddress & (profile::PageSize - 1);
  const uint64_t Bytes = (Offset + State.ByteCount + profile::PageSize - 1) &
                         ~(profile::PageSize - 1);
  if (Bytes > Reservation->second.Size)
    return uint64_t(0);
  auto Requested = memoryCacheType(uint32_t(A[3]));
  if (!Requested)
    return Requested.takeError();
  auto Cache = Physical.cacheTypeForMapping(State.BackingAddress,
                                            State.ByteCount, *Requested);
  if (!Cache)
    return Cache.takeError();
  auto Writable = Memory.canAccess(State.Address, State.Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return reservationError("reserved mapping requires writable MDL metadata");
  const uint64_t Alias = A[0] + Offset;
  const unsigned Permissions = Read | (State.Writable ? Write : 0);
  // Backend resource limits are model errors, not a fabricated NT low-resource
  // NULL result for an otherwise valid reserved mapping.
  if (auto E = Memory.mapAlias(A[0], pageBase(State.BackingAddress), Bytes,
                               Permissions))
    return E;
  if (auto E = Physical.commitMappingCache(State.BackingAddress,
                                           State.ByteCount, *Cache))
    return E;
  const uint16_t Flags =
      MDLMappedToSystemVA | (State.Owner == LockedMdl::Ownership::Partial
                                 ? MDLPartial | MDLPartialHasBeenMapped
                                 : MDLPagesLocked);
  // The matched 19041 body stores the offset-inclusive VA in this field.
  if (auto E = Memory.writeInteger(State.Address + MDLMappedSystemVAOffset,
                                   Alias, 8))
    return E;
  if (auto E = Memory.writeInteger(State.Address + MDLFlagsOffset, Flags, 2))
    return E;
  State.Buffer = Alias;
  State.AllocationSize = Bytes;
  State.Mapped = true;
  State.OwnsSystemMapping = true;
  State.SystemMappingOwnerMDL = State.Address;
  State.MappingReservation = A[0];
  Reservation->second.MDL = State.Address;
  return Alias;
}

llvm::Expected<uint64_t>
KernelModel::unmapReservedMapping(llvm::ArrayRef<uint64_t> A) {
  const auto It = MDLs.find(A[2]);
  if (It == MDLs.end() || !It->second.MappingReservation || !It->second.Mapped)
    return reservationError("reserved unmap requires its live mapped MDL");
  auto &State = It->second;
  const auto Reservation = ReservedMappings.find(State.MappingReservation);
  if (Reservation == ReservedMappings.end() ||
      Reservation->second.MDL != A[2] ||
      Reservation->second.Tag != uint32_t(A[1]) ||
      (A[0] != State.Buffer && A[0] != State.MappingReservation))
    return reservationError(
        "reserved unmap address, tag or MDL does not match");
  if (auto E = unmapLockedPages(State.Buffer, A[2], true))
    return E;
  Reservation->second.MDL = 0;
  State.MappingReservation = 0;
  return uint64_t(0);
}
} // namespace neverd::emulation
