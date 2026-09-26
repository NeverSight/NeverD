//===- KernelRemoveLocks.h - Exact-device remove-lock ownership ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded remove-lock acquisitions and drain latches, independent of PnP
/// transactions, guest memory, callback execution and wait registration.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELREMOVELOCKS_H
#define NEVERD_EMULATION_WINDOWS_KERNELREMOVELOCKS_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>

namespace neverd::emulation {

namespace remove_lock {
#define NEVERD_KERNEL_REMOVE_LOCK_VALUE(Name, Value)                           \
  constexpr uint64_t Name = Value;
#include "KernelRemoveLockValues.def"
#undef NEVERD_KERNEL_REMOVE_LOCK_VALUE
} // namespace remove_lock

/// Sole authority for lock extents, exact device ownership and acquisitions.
/// The bridge validates that initialization lies inside a live guest device
/// extension and owns IRQL, PnP context, wait references and physical storage.
/// Tags are opaque values, including NULL and repeated values; never IRP
/// owners.
class KernelRemoveLocks {
public:
  struct Registration {
    uint64_t Address = 0;
    uint64_t OwnerDevice = 0;
    uint32_t Size = 0;
  };

  llvm::Error initialize(Registration Identity);
  /// False means admission is closed: map to STATUS_DELETE_PENDING. No new
  /// acquisition or release obligation is created on this path.
  llvm::Expected<bool> acquire(uint64_t Lock, uint32_t Size, uint64_t Tag);
  llvm::Error release(uint64_t Lock, uint32_t Size, uint64_t Tag);
  /// Atomically close admission and release one matching acquisition. False
  /// requires the caller to wait; true means all acquisitions have drained.
  llvm::Expected<bool> releaseAndWait(uint64_t Lock, uint32_t Size,
                                      uint64_t Tag);
  /// Readiness remains latched until the storage is explicitly unregistered.
  llvm::Expected<bool> drained(uint64_t Lock) const;
  llvm::Expected<Registration> registration(uint64_t Lock) const;

  /// Registered lock storage is opaque for both guest reads and writes.
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;
  /// Reject partial retirement and outstanding acquisitions without mutation.
  /// The bridge must additionally reject its outstanding wait references.
  llvm::Error canReleaseRange(uint64_t Base, uint64_t Size) const;
  /// Preflight the complete range, then unregister every covered lock. An
  /// initialized lock without acquisitions may retire without starting drain.
  llvm::Error forgetRange(uint64_t Base, uint64_t Size);

private:
  struct LockState {
    Registration Identity;
    bool Draining = false;
    std::map<uint64_t, uint64_t> Tags;
  };

  std::map<uint64_t, LockState> Locks;
  uint64_t TotalReferences = 0;

  llvm::Expected<LockState *> lookup(uint64_t Lock, uint32_t Size);
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_WINDOWS_KERNELREMOVELOCKS_H
