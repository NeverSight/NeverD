//===- KernelRemoveLocks.cpp - Exact-device remove-lock ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Track opaque acquisitions and close admission before draining. The public
/// contracts are IoInitializeRemoveLock, IoAcquireRemoveLock,
/// IoReleaseRemoveLock and IoReleaseRemoveLockAndWait in Microsoft Learn.
/// No private Windows counter, event, checked-kernel or verifier state is
/// serialized into guest memory.
///
//===----------------------------------------------------------------------===//

#include "KernelRemoveLocks.h"

#include <limits>
#include <utility>

namespace neverd::emulation {
namespace {

llvm::Error removeLockError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "remove lock: " + Message);
}

llvm::Error validateRange(uint64_t Base, uint64_t Size) {
  if (Size > std::numeric_limits<uint64_t>::max() - Base)
    return removeLockError("overflowing storage range");
  return llvm::Error::success();
}

bool overlaps(uint64_t Base, uint64_t End,
              const KernelRemoveLocks::Registration &Identity) {
  return Base < Identity.Address + Identity.Size && Identity.Address < End;
}

} // namespace

llvm::Error KernelRemoveLocks::initialize(Registration Identity) {
  if (!Identity.Address || !Identity.OwnerDevice)
    return removeLockError("initialization requires nonzero lock and device");
  if (Identity.Address % remove_lock::ObjectAlignment)
    return removeLockError("storage is not aligned");
  if (Identity.Size != remove_lock::RetailSize &&
      Identity.Size != remove_lock::DebugSize)
    return removeLockError("unsupported structure size");
  if (auto E = validateRange(Identity.Address, Identity.Size))
    return E;
  if (Locks.count(Identity.Address))
    return removeLockError("lock is already initialized");
  if (auto E = validateGuestAccess(Identity.Address, Identity.Size, true))
    return E;
  if (Locks.size() >= remove_lock::MaxLocks)
    return removeLockError("registration capacity exceeded");
  Locks.emplace(Identity.Address, LockState{Identity, false, {}});
  return llvm::Error::success();
}

llvm::Expected<KernelRemoveLocks::LockState *>
KernelRemoveLocks::lookup(uint64_t Lock, uint32_t Size) {
  auto It = Locks.find(Lock);
  if (It == Locks.end())
    return removeLockError("unknown lock identity");
  if (It->second.Identity.Size != Size)
    return removeLockError("structure size does not match initialization");
  return &It->second;
}

llvm::Expected<bool> KernelRemoveLocks::acquire(uint64_t Lock, uint32_t Size,
                                                uint64_t Tag) {
  auto Found = lookup(Lock, Size);
  if (!Found)
    return Found.takeError();
  auto &State = **Found;
  if (State.Draining)
    return false;
  if (TotalReferences >= remove_lock::MaxReferences)
    return removeLockError("acquisition capacity exceeded");
  ++State.Tags[Tag];
  ++TotalReferences;
  return true;
}

llvm::Error KernelRemoveLocks::release(uint64_t Lock, uint32_t Size,
                                       uint64_t Tag) {
  auto Found = lookup(Lock, Size);
  if (!Found)
    return Found.takeError();
  auto &State = **Found;
  auto Acquisition = State.Tags.find(Tag);
  if (Acquisition == State.Tags.end())
    return removeLockError("release has no matching acquisition tag");
  if (!--Acquisition->second)
    State.Tags.erase(Acquisition);
  --TotalReferences;
  return llvm::Error::success();
}

llvm::Expected<bool>
KernelRemoveLocks::releaseAndWait(uint64_t Lock, uint32_t Size, uint64_t Tag) {
  auto Found = lookup(Lock, Size);
  if (!Found)
    return Found.takeError();
  auto &State = **Found;
  if (State.Draining)
    return removeLockError("lock is already draining");
  if (auto E = release(Lock, Size, Tag))
    return E;
  State.Draining = true;
  return State.Tags.empty();
}

llvm::Expected<bool> KernelRemoveLocks::drained(uint64_t Lock) const {
  auto It = Locks.find(Lock);
  if (It == Locks.end())
    return removeLockError("unknown lock identity");
  return It->second.Draining && It->second.Tags.empty();
}

llvm::Expected<KernelRemoveLocks::Registration>
KernelRemoveLocks::registration(uint64_t Lock) const {
  auto It = Locks.find(Lock);
  if (It == Locks.end())
    return removeLockError("unknown lock identity");
  return It->second.Identity;
}

llvm::Error KernelRemoveLocks::validateGuestAccess(uint64_t Address,
                                                   uint32_t Size,
                                                   bool IsWrite) const {
  (void)IsWrite;
  if (auto E = validateRange(Address, Size))
    return E;
  if (!Size)
    return llvm::Error::success();
  for (const auto &[Lock, State] : Locks) {
    (void)Lock;
    if (overlaps(Address, Address + Size, State.Identity))
      return removeLockError("guest access overlaps opaque lock storage");
  }
  return llvm::Error::success();
}

llvm::Error KernelRemoveLocks::canReleaseRange(uint64_t Base,
                                               uint64_t Size) const {
  if (auto E = validateRange(Base, Size))
    return E;
  if (!Size)
    return llvm::Error::success();
  for (const auto &[Lock, State] : Locks) {
    (void)Lock;
    if (!overlaps(Base, Base + Size, State.Identity))
      continue;
    if (Base > State.Identity.Address ||
        Base + Size < State.Identity.Address + State.Identity.Size)
      return removeLockError("cannot release partial lock storage");
    if (!State.Tags.empty())
      return removeLockError("cannot release storage with acquisitions");
  }
  return llvm::Error::success();
}

llvm::Error KernelRemoveLocks::forgetRange(uint64_t Base, uint64_t Size) {
  if (auto E = canReleaseRange(Base, Size))
    return E;
  if (!Size)
    return llvm::Error::success();
  for (auto It = Locks.begin(); It != Locks.end();) {
    if (overlaps(Base, Base + Size, It->second.Identity))
      It = Locks.erase(It);
    else
      ++It;
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
