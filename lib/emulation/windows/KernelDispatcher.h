//===- KernelDispatcher.h - Guest dispatcher objects ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded Windows dispatcher object semantics over a concrete scheduler.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELDISPATCHER_H
#define NEVERD_EMULATION_WINDOWS_KERNELDISPATCHER_H

#include "KernelScheduler.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace neverd::emulation {
class GuestMemory;

namespace dispatcher {
#define NEVERD_KERNEL_DISPATCHER_VALUE(Name, Value)                            \
  constexpr uint64_t Name = Value;
#include "KernelDispatcherValues.def"
#undef NEVERD_KERNEL_DISPATCHER_VALUE
} // namespace dispatcher

/// The caller owns wait registrations, guest callback execution and IRQL
/// changes. This object owns initialization, opaque extents, signal state and
/// producer lifetimes. Wait references must prevent reinitialization or storage
/// release until the caller has ended the corresponding wait.
class KernelDispatcher {
public:
  using AccessValidator = std::function<llvm::Error(uint64_t, uint32_t, bool)>;

  /// Validate must check other model lifetimes and residency, excluding this
  /// dispatcher's own opaque extents. It is retained for the model lifetime.
  KernelDispatcher(GuestMemory &Memory, KernelScheduler &Scheduler,
                   AccessValidator Validate)
      : Memory(Memory), Scheduler(Scheduler), Validate(std::move(Validate)) {}

  llvm::Error configure(uint64_t Owner, uint64_t Thread);
  static std::optional<unsigned> argumentCount(llvm::StringRef Name);
  llvm::Expected<uint64_t> call(llvm::StringRef Name,
                                llvm::ArrayRef<uint64_t> Arguments,
                                uint8_t CurrentIRQL);

  /// API operations use model state; direct guest structure access is denied.
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;
  /// Atomically validate all objects before unregistering any. The caller must
  /// separately reject outstanding wait references and then release storage.
  llvm::Error prepareReleaseRange(uint64_t Address, uint64_t Size);
  /// Read-only preflight for callers retiring multiple allocations together.
  llvm::Error canReleaseRange(uint64_t Address, uint64_t Size) const;
  bool isWaitable(uint64_t Object) const;
  /// Process timers due now, without advancing time or dispatching callbacks.
  /// Successful synchronization-object acquisition consumes its signal.
  llvm::Expected<bool> tryAcquire(uint64_t Object);

private:
  enum class Kind { DPC, Timer, Event };
  struct Object {
    Object(Kind Type, uint32_t Size) : Type(Type), Size(Size) {}
    Kind Type;
    uint32_t Size;
    bool Synchronization = false;
    bool Signaled = false;
    bool HasSchedule = false;
    uint64_t TimerDPC = 0;
    KernelScheduler::DpcCallback DPC;
  };

  GuestMemory &Memory;
  KernelScheduler &Scheduler;
  AccessValidator Validate;
  uint64_t Owner = 0;
  uint64_t Thread = 0;
  std::map<uint64_t, Object> Objects;

  llvm::Error initialize(uint64_t Address, Object State);
  llvm::Expected<Object *> object(uint64_t Address, Kind Type, bool IsWrite);
  llvm::Error canRelease(uint64_t Address, const Object &State) const;
  bool hasArmedTimerReference(uint64_t DPC) const;
  llvm::Expected<bool> signaled(uint64_t Address, const Object &State) const;
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_WINDOWS_KERNELDISPATCHER_H
