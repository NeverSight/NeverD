//===- KernelInterrupts.h - Assigned interrupts and callback ownership ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own opaque connections, explicit external pulses and the interrupt lock
/// shared by ISR, synchronization callbacks and manual critical sections.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELINTERRUPTS_H
#define NEVERD_EMULATION_KERNELINTERRUPTS_H

#include "KernelGuestCall.h"
#include "KernelResources.h"

#include "neverd/emulation/DriverSession.h"

#include <map>

namespace neverd::emulation {
namespace interrupts {
#define NEVERD_KERNEL_INTERRUPT_VALUE(Name, Value)                             \
  constexpr uint64_t Name = Value;
#include "KernelInterruptValues.def"
#undef NEVERD_KERNEL_INTERRUPT_VALUE
} // namespace interrupts
class KernelInterrupts {
public:
  KernelInterrupts(const KernelResources &Resources, DriverResult &Result)
      : Resources(Resources), Result(Result) {}
  struct Connection {
    uint64_t Object = 0;
    uint64_t PDO = 0;
    uint64_t Epoch = 0;
    size_t ResourceIndex = 0;
    uint64_t Routine = 0;
    uint64_t Context = 0;
    /// Exact guest DEVICE_OBJECT allocation whose extension contains the
    /// registration output slot, when proven by the API bridge. This is not
    /// inferred from the PDO association or pointers inside ServiceContext.
    uint64_t OutputDeviceBase = 0;
    uint64_t OutputDeviceSize = 0;
    uint8_t IRQL = 0;
    uint8_t SynchronizeIRQL = 0;
    uint64_t SpinLock = 0;
    /// Zero is the legacy API; otherwise the exact Ex connection version.
    uint32_t Version = 0;
    /// Resource-local assignment index; also set for fully specified connects.
    std::optional<uint32_t> ResourceMessage;
    /// PDO table index supplied only to a message-service routine.
    std::optional<uint32_t> MessageID;
    bool Passive = false;
    /// Framework ISR signatures differ from the WDM service routine. The
    /// execution still belongs to this connection and its interrupt lock.
    std::optional<std::vector<uint64_t>> ServiceArguments;
  };
  /// PDO zero selects the unique translated vector; LineBased selects the
  /// sole assigned line on an explicit PDO. Neither uses ServiceContext.
  llvm::Expected<Connection> match(uint64_t PDO, uint32_t Vector, uint8_t IRQL,
                                   uint64_t Affinity, bool LineBased = false,
                                   bool Passive = false) const;
  llvm::Expected<std::vector<Connection>> matchMessages(uint64_t PDO) const;
  /// Select one assigned line or message by resource-list order. An explicit
  /// resource index narrows the ordinal to messages in that descriptor.
  llvm::Expected<Connection>
  matchOrdinal(uint64_t PDO, size_t Ordinal,
               std::optional<size_t> ResourceIndex = {}) const;
  DriverInterruptMessage assignment(const Connection &Connection) const;
  llvm::Error canConnectMessages(uint64_t Table,
                                 llvm::ArrayRef<Connection> Candidates) const;
  llvm::Error connectMessages(uint64_t Table,
                              llvm::ArrayRef<Connection> Candidates);
  llvm::Error connect(Connection Candidate);
  llvm::Error canConnect(const Connection &Candidate) const;
  const Connection *connection(uint64_t Object) const;
  bool usesSpinLock(uint64_t Address) const;
  llvm::Error disconnect(uint64_t Object, uint32_t Version);
  /// Retire an individually owned connection. WDM message tables remain an
  /// atomic group and cannot be dismantled through this bridge.
  llvm::Error disconnectConnection(uint64_t Object);
  llvm::Error canRelease(uint64_t PDO) const;
  /// Validate known storage retirement without inferring a context extent or
  /// recursively examining pointers stored inside caller-owned context data.
  llvm::Error canReleaseRange(uint64_t Base, uint64_t Size) const;
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite = false) const;

  llvm::Error canArm(llvm::ArrayRef<DriverInterruptEvent> Events,
                     size_t SourceIndex, uint64_t Now) const;
  llvm::Error arm(llvm::ArrayRef<DriverInterruptEvent> Events,
                  size_t SourceIndex, uint64_t Now);
  std::optional<uint64_t> nextEventTime() const;
  bool hasPendingEvents() const {
    return !Events.empty() || !LevelLines.empty();
  }
  /// Pure callback/token capacity check; hardware availability is observed at
  /// the actual boundary after same-time provider hardware publications.
  llvm::Expected<uint64_t> dueCount(uint64_t Time) const;
  struct Delivery {
    uint64_t PDO = 0;
    uint8_t IRQL = 0;
    uint8_t Priority = 0;
    KernelGuestCall Call;
  };
  llvm::Expected<std::optional<Delivery>> queueNextDue(uint64_t Now);

  llvm::Expected<KernelGuestCall> synchronize(uint64_t Object, uint64_t Routine,
                                              uint64_t Context);
  llvm::Expected<bool> reserveSynchronization(uint64_t Token);
  llvm::Expected<uint8_t> beginCall(uint64_t Token, uint8_t CallerIRQL,
                                    uint64_t Now);
  struct CallbackReturn {
    uint8_t Value = 0;
    uint8_t RestoredIRQL = 0;
    std::optional<KernelGuestCall> Next;
  };
  llvm::Expected<CallbackReturn> finishCall(uint64_t Token, uint64_t Value,
                                            uint8_t CurrentIRQL, uint64_t Now);
  /// Framework passive locks are waitable; an empty result retains no hold.
  llvm::Expected<bool> tryAcquirePassive(uint64_t Object, uint64_t Execution);
  llvm::Expected<uint8_t> acquire(uint64_t Object, uint64_t Execution,
                                  uint8_t CurrentIRQL);
  llvm::Expected<uint8_t> release(uint64_t Object, uint64_t Execution,
                                  uint8_t OldIRQL, uint8_t CurrentIRQL);
  std::optional<uint8_t> manualHoldIRQL(uint64_t Execution) const;
  llvm::Error validateExecutionReturn(uint64_t Execution) const;

private:
  enum class HoldKind { Manual, Callback };
  struct Hold {
    uint64_t Object;
    uint64_t Owner;
    uint8_t OldIRQL;
    HoldKind Kind;
  };
  struct Call {
    uint64_t Object;
    std::optional<size_t> Observation;
    bool Begun = false;
    size_t Handler = 0;
    std::vector<uint64_t> Chain;
    std::optional<uint32_t> Line;
    uint32_t DeliveryIndex = 0;
    bool Reserved = false;
  };
  struct Event {
    uint64_t Object;
    uint64_t PDO;
    uint64_t Epoch;
    uint64_t Due;
    size_t Observation;
    bool Queued = false;
    std::vector<uint64_t> Chain;
    size_t ResourceIndex = 0;
    uint32_t Vector = 0;
    DriverInterruptAction Action = DriverInterruptAction::Pulse;
    uint64_t RetriggerAfter100ns = 0;
    bool Observed = false;
  };
  using Source = std::pair<uint64_t, size_t>;
  struct LevelLine {
    std::map<Source, Event> Sources;
    uint64_t Due = 0;
    bool Queued = false;
    uint32_t Deliveries = 0;
  };
  struct Boundary {
    std::vector<size_t> Transitions;
    std::map<uint32_t, LevelLine> Lines;
    std::vector<size_t> Pulses;
    std::vector<uint32_t> Levels;
  };
  const KernelResources &Resources;
  DriverResult &Result;
  std::map<uint64_t, Connection> Connections;
  struct MessageGroup {
    uint64_t Size;
    std::vector<uint64_t> Objects;
    bool Live = true;
  };
  std::map<uint64_t, MessageGroup> MessageGroups;
  /// Opaque addresses remain reserved even after a disconnect.
  std::vector<uint64_t> Tokens;
  std::map<uint64_t, Call> Calls;
  std::vector<Hold> Holds;
  std::map<uint64_t, uint64_t> PassiveLockWaiters;
  std::map<size_t, Event> Events;
  std::map<uint32_t, LevelLine> LevelLines;
  uint64_t NextCall = 1;
  uint64_t Deliveries = 0;
  llvm::Expected<Event> resolveEvent(const DriverInterruptEvent &Input,
                                     uint64_t Now) const;
  llvm::Error canHold(uint64_t Object, uint8_t CurrentIRQL) const;
  llvm::Error canPrepareCalls(uint64_t Count) const;
  uint64_t lockIdentity(uint64_t Object) const;
  llvm::Error canShare(const Connection &Left, const Connection &Right) const;
  llvm::Error canDisconnect(uint64_t Object, uint32_t Version) const;
  KernelGuestCall serviceCall(uint64_t Token,
                              const Connection &Connection) const;
  bool sameLine(const Connection &Left, const Connection &Right) const;
  bool passiveAvailable(uint64_t Object, uint64_t Token = 0) const;
  bool runnable(const Event &Event) const;
  Boundary planBoundary(uint64_t Time) const;
  llvm::Error validateEvent(const Event &Event, bool CheckPower) const;
  llvm::Error deliveryError(Event &Event, const llvm::Twine &Message,
                            uint64_t Now);
};
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_KERNELINTERRUPTS_H
