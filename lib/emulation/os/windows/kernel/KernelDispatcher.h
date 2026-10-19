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
#define NEVERD_KERNEL_DISPATCHER_TEXT(Name, Text) constexpr char Name[] = Text;
#include "KernelDispatcherValues.def"
#undef NEVERD_KERNEL_DISPATCHER_TEXT
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
                                uint8_t CurrentIRQL, uint64_t ThreadKey = 0);

  /// API operations use model state; direct guest structure access is denied.
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;
  /// Atomically validate all objects before unregistering any. The caller must
  /// separately reject outstanding wait references and then release storage.
  llvm::Error prepareReleaseRange(uint64_t Address, uint64_t Size);
  /// Read-only preflight for callers retiring multiple allocations together.
  llvm::Error canReleaseRange(uint64_t Address, uint64_t Size) const;
  bool isWaitable(uint64_t Object) const;
  bool isEvent(uint64_t Address) const {
    auto I = Objects.find(Address);
    return I != Objects.end() && I->second.Type == Kind::Event;
  }
  /// Process timers due now, without advancing time or dispatching callbacks.
  /// Successful synchronization-object acquisition consumes its signal.
  /// Mutex ownership uses the logical thread key, shared by all of its stacks.
  llvm::Expected<bool> tryAcquire(uint64_t Object, uint64_t ThreadKey = 0,
                                  uint8_t CurrentIRQL = 0);
  /// A thread's termination state is validated and supplied by its model owner.
  /// External conditions never identify or mutate a dispatcher-owned object.
  struct WaitCondition {
    enum class Kind { Dispatcher, Ready, Pending };
    Kind Type = Kind::Dispatcher;
    uint64_t Object = 0;
  };
  /// Validate the complete set, select the first ready condition for WaitAny,
  /// and commit every selected acquisition together. An unsatisfied WaitAll
  /// consumes no event/timer signal, semaphore count or mutex recursion.
  llvm::Expected<std::optional<uint32_t>>
  tryAcquireSet(llvm::ArrayRef<WaitCondition> Conditions, bool All,
                uint64_t ThreadKey, uint8_t CurrentIRQL);
  bool ownsMutex(uint64_t ThreadKey, uint64_t ReleasingAddress = 0) const;
  /// Embedded fast-mutex events share this owner's event identity. The
  /// guarded initializer also publishes its target-specific byte layout.
  llvm::Error initializeFastMutexEvent(uint64_t Address);
  llvm::Error validateFastMutexEvent(uint64_t Address) const;

  /// Framework wait locks share dispatcher ownership and opaque storage. An
  /// interrupt reservation exists before its guest thread begins execution.
  struct WaitLockOwner {
    enum class Kind { Thread, Interrupt };
    Kind Type = Kind::Thread;
    uint64_t ID = 0;
    bool operator==(const WaitLockOwner &) const = default;
  };
  llvm::Error initializeWaitLock(uint64_t Address, bool Recursive = false);
  llvm::Expected<bool> tryAcquireWaitLock(uint64_t Address,
                                          WaitLockOwner Owner);
  llvm::Error releaseWaitLock(uint64_t Address, WaitLockOwner Owner);
  bool waitLockAvailable(uint64_t Address,
                         std::optional<WaitLockOwner> Owner = {}) const;
  bool waitLockOwnedByThread(uint64_t Thread) const;

private:
  enum class Kind { DPC, Timer, Event, Semaphore, Mutex, WaitLock };
  struct Object {
    Object(Kind Type, uint32_t Size) : Type(Type), Size(Size) {}
    Kind Type;
    uint32_t Size;
    bool Synchronization = false;
    bool Signaled = false;
    int32_t Count = 0;
    int32_t Limit = 0;
    uint64_t MutexOwner = 0; // Logical thread key, never an execution stack.
    uint32_t MutexDepth = 0;
    bool MutexAcquiredAtDispatch = false;
    std::optional<WaitLockOwner> LockOwner;
    uint32_t LockDepth = 0;
    bool RecursiveLock = false;
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

  llvm::Error initialize(uint64_t Address, Object State,
                         llvm::ArrayRef<uint8_t> InitialBytes = {});
  llvm::Expected<Object *> object(uint64_t Address, Kind Type, bool IsWrite);
  llvm::Error canRelease(uint64_t Address, const Object &State) const;
  bool hasArmedTimerReference(uint64_t DPC) const;
  llvm::Expected<bool> signaled(uint64_t Address, const Object &State) const;
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_WINDOWS_KERNELDISPATCHER_H
