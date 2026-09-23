//===- KernelModelSpinLocks.cpp - Executive spin locks --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded single-processor executive spin locks and IRQL restoration.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

#include "neverd/emulation/DriverProfile.h"

namespace neverd::emulation {
namespace {
llvm::Error spinLockError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::callSpinLockAPI(llvm::StringRef Name,
                             llvm::ArrayRef<uint64_t> Arguments) {
  const uint64_t Address = Arguments[0];
  if (!Address || (Address & 7))
    return spinLockError("executive spin lock requires aligned kernel storage");
  if (Name == "KeReleaseSpinLock" || Name == "KeReleaseSpinLockFromDpcLevel") {
    auto Lock = ExecutiveSpinLocks.find(Address);
    if (Lock == ExecutiveSpinLocks.end() ||
        Lock->second.Execution != CurrentExecution)
      return spinLockError("executive spin lock is not owned by this thread");
    if (CurrentIRQL != scheduler::DispatchLevel)
      return spinLockError(
          "executive spin lock release requires DISPATCH_LEVEL");
    const bool RestoreIRQL = Name == "KeReleaseSpinLock";
    if (Lock->second.RaisedIRQL != RestoreIRQL)
      return spinLockError("executive spin lock release variant does not "
                           "match acquisition");
    if (RestoreIRQL && uint8_t(Arguments[1]) != Lock->second.OldIRQL)
      return spinLockError("executive spin lock release must restore the "
                           "saved IRQL");
    auto Value = Memory.readInteger(Address, 8);
    if (!Value)
      return Value.takeError();
    if (*Value != 1)
      return spinLockError("executive spin lock storage was modified");
    if (auto E = Memory.writeInteger(Address, 0, 8))
      return E;
    CurrentIRQL = RestoreIRQL ? Lock->second.OldIRQL : scheduler::DispatchLevel;
    ExecutiveSpinLocks.erase(Lock);
    return 0;
  }

  if (Address < profile::UserProbeLimit)
    return spinLockError("executive spin lock requires kernel storage");
  if (Name != "KeInitializeSpinLock" && Name != "KeAcquireSpinLockRaiseToDpc" &&
      Name != "KeAcquireSpinLockAtDpcLevel" &&
      Name != "KeTryToAcquireSpinLockAtDpcLevel")
    return spinLockError("unknown executive spin-lock operation");
  const bool AtDpc = Name == "KeAcquireSpinLockAtDpcLevel" ||
                     Name == "KeTryToAcquireSpinLockAtDpcLevel";
  if (AtDpc && CurrentIRQL != scheduler::DispatchLevel)
    return spinLockError("DPC-level spin-lock acquisition requires "
                         "DISPATCH_LEVEL");
  if (Name != "KeInitializeSpinLock" && !CurrentExecution)
    return spinLockError("executive spin lock requires an active guest thread");
  if (ExecutiveSpinLocks.count(Address)) {
    if (Name == "KeTryToAcquireSpinLockAtDpcLevel")
      return 0;
    if (Name == "KeInitializeSpinLock")
      return spinLockError("cannot initialize a held executive spin lock");
    return spinLockError("executive spin lock would deadlock on this "
                         "cooperative processor");
  }
  if (auto E = validateGuestAccess(Address, 8, true))
    return E;
  auto Writable = Memory.canAccess(Address, 8, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return spinLockError("executive spin lock requires writable storage");
  for (const auto &[Pool, Allocation] : Allocations)
    if (Address >= Pool && Address - Pool < Allocation.Size &&
        (!Allocation.NonPaged || 8 > Allocation.Size - (Address - Pool)))
      return spinLockError("executive spin lock requires nonpaged storage");

  if (Name == "KeInitializeSpinLock") {
    if (auto E = Memory.writeInteger(Address, 0, 8))
      return E;
    return 0;
  }
  auto Value = Memory.readInteger(Address, 8);
  if (!Value)
    return Value.takeError();
  if (*Value)
    return spinLockError("executive spin lock must be initialized and free");
  if (auto E = Memory.writeInteger(Address, 1, 8))
    return E;
  const uint8_t OldIRQL = CurrentIRQL;
  CurrentIRQL = scheduler::DispatchLevel;
  ExecutiveSpinLocks.emplace(
      Address, ExecutiveSpinLock{CurrentExecution, OldIRQL, !AtDpc});
  if (Name == "KeAcquireSpinLockRaiseToDpc")
    return OldIRQL;
  return Name == "KeTryToAcquireSpinLockAtDpcLevel" ? 1 : 0;
}
} // namespace neverd::emulation
