//===- KernelModelRemoveLocks.cpp - Guest remove-lock contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind the WDK Ex exports to exact device-extension storage and resumable
/// drain waits. Tags remain opaque, including retired packet addresses.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error removeLockError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "remove lock: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::initializeRemoveLock(llvm::ArrayRef<uint64_t> Arguments) {
  const uint64_t Address = Arguments[0];
  const uint32_t Size = uint32_t(Arguments[4]);
  if (Size != remove_lock::RetailSize && Size != remove_lock::DebugSize)
    return removeLockError("unsupported retail or DBG IO_REMOVE_LOCK size");
  if (Address % remove_lock::ObjectAlignment || Size > UINT64_MAX - Address)
    return removeLockError("unaligned or overflowing lock storage");
  if (uint32_t(Arguments[3]) > INT32_MAX)
    return removeLockError("HighWatermark exceeds the documented ULONG range");
  uint64_t Owner = 0;
  for (const auto &[Device, Record] : Devices) {
    if (Record.OwnerKind != DeviceOwnerKind::Guest || Record.DeletePending ||
        Address < Record.Extension || Address >= Device + Record.Size ||
        Size > Device + Record.Size - Address)
      continue;
    if (Owner)
      return removeLockError("lock storage has multiple device owners");
    Owner = Device;
  }
  if (!Owner)
    return removeLockError("lock must fit in a live guest device extension");
  if (auto E = validateGuestAccess(Address, Size, true))
    return E;
  // These bytes are opaque after registration. AllocateTag, MaxLockedMinutes
  // and HighWatermark are verifier metadata, not simulated timeout producers.
  if (auto E = RemoveLocks.initialize({Address, Owner, Size}))
    return E;
  return 0;
}

llvm::Error KernelModel::validateRemoveLockOwner(uint64_t Lock) const {
  auto Registration = RemoveLocks.registration(Lock);
  if (!Registration)
    return Registration.takeError();
  const auto Owner = Devices.find(Registration->OwnerDevice);
  if (Owner == Devices.end() ||
      Owner->second.OwnerKind != DeviceOwnerKind::Guest)
    return removeLockError("registered lock lost its live device owner");
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::acquireRemoveLock(llvm::ArrayRef<uint64_t> Arguments) {
  if (auto E = validateRemoveLockOwner(Arguments[0]))
    return E;
  // File and Line are diagnostic metadata. In particular, do not dereference
  // a File string at DISPATCH_LEVEL or infer ownership from a pointer-like Tag.
  auto Acquired =
      RemoveLocks.acquire(Arguments[0], uint32_t(Arguments[4]), Arguments[1]);
  if (!Acquired)
    return Acquired.takeError();
  return *Acquired ? windows::StatusSuccess : windows::StatusDeletePending;
}

llvm::Expected<uint64_t>
KernelModel::releaseRemoveLock(llvm::ArrayRef<uint64_t> Arguments, bool Wait) {
  const uint64_t Lock = Arguments[0];
  if (auto E = validateRemoveLockOwner(Lock))
    return E;
  const uint32_t Size = uint32_t(Arguments[2]);
  if (!Wait) {
    if (auto E = RemoveLocks.release(Lock, Size, Arguments[1]))
      return E;
    return 0;
  }
  if (PendingWait)
    return removeLockError("a previous deferred wait was not consumed");
  auto Registration = RemoveLocks.registration(Lock);
  if (!Registration)
    return Registration.takeError();
  if (Registration->Size != Size)
    return removeLockError("lock size differs from its registration");
  const uint64_t Owner = Registration->OwnerDevice;
  const uint64_t PDO = Devices.at(Owner).PnpDevice;
  const bool ReceivedRemove =
      std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        const auto &Request = Entry.second;
        if (!PDO || Request.PnpDevice != PDO || !Request.PnpOperation ||
            Request.PnpOperation->Minor != DevicePnpRequest::Remove ||
            std::find(Request.DeviceRoute.begin(), Request.DeviceRoute.end(),
                      Owner) == Request.DeviceRoute.end())
          return false;
        const auto &Observation = Result.Requests[Request.ResultIndex].Pnp;
        return Observation && Observation->BusReceivedAt100ns.has_value();
      });
  // Provider receipt is this profile's conservative proof of forwarding. It
  // does not identify the calling dispatch frame or implement Driver Verifier.
  // Lower completion and drain readiness are independent, including time zero.
  if (!ReceivedRemove)
    return removeLockError("AndWait requires a retained REMOVE route after "
                           "provider receipt in this profile");
  auto Drained = RemoveLocks.releaseAndWait(Lock, Size, Arguments[1]);
  if (!Drained)
    return Drained.takeError();
  if (!*Drained) {
    KernelModel::Wait Pending;
    Pending.Type = KernelModel::Wait::Kind::RemoveLock;
    Pending.Object = Lock;
    ++RemoveLockWaitReferences[Lock];
    PendingWait = Pending;
  }
  return 0;
}

llvm::Error KernelModel::canReleaseRemoveLockStorage(uint64_t Base,
                                                     uint64_t Size) const {
  if (Size > UINT64_MAX - Base)
    return removeLockError("overflowing lock storage range");
  for (const auto &[Lock, References] : RemoveLockWaitReferences)
    if (References && Lock >= Base && Lock < Base + Size)
      return removeLockError(
          "cannot release storage with a pending drain wait");
  return RemoveLocks.canReleaseRange(Base, Size);
}
} // namespace neverd::emulation
