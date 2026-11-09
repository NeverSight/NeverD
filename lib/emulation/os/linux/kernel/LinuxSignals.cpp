//===- LinuxSignals.cpp - Explicit LP64 signal action state --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxSignals.h"

#include "LinuxUserMemory.h"

#include "llvm/ADT/bit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
namespace {
constexpr uint64_t Unblockable =
    (uint64_t(1) << (SignalKill - 1)) | (uint64_t(1) << (SignalStop - 1));
// Common LP64 Linux flags. Later flag-probing extensions need their own ABI.
constexpr uint64_t ActionFlags = 0xdc000007;
bool validSignal(int32_t Signal) {
  return Signal > 0 && uint32_t(Signal) <= SignalCount;
}
bool kernelOnly(int32_t Signal) {
  return Signal == SignalKill || Signal == SignalStop;
}
uint64_t bit(int32_t Signal) { return uint64_t(1) << (Signal - 1); }
bool defaultIgnored(int32_t Signal) {
  return Signal == 17 || Signal == 23 || Signal == 28;
}
bool defaultTerminates(int32_t Signal) {
  switch (Signal) {
  case SignalAbort:
  case 1:
  case 2:
  case 10:
  case 12:
  case 13:
  case 14:
  case 15:
  case 16:
  case 26:
  case 27:
  case 29:
  case 30:
    return true;
  default:
    return false;
  }
}
void unsupported(ProcessResult &Result, llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason.str();
}
bool supportedFlags(uint64_t Flags) {
  if (uint32_t(Flags) & ~ActionFlags)
    return false;
  // Bionic's signed int flags become unsigned long in the kernel structure.
  return Flags <= UINT32_MAX ||
         Flags == uint64_t(int64_t(int32_t(uint32_t(Flags))));
}
} // namespace

llvm::Error validateSignalOptions(const LinuxSignalOptions &Options) {
  if (Options.InitialMask && (*Options.InitialMask & Unblockable))
    return failure(SignalOptions);
  for (const auto &[Signal, Action] : Options.Actions) {
    if (!validSignal(Signal) || (Action.Mask & Unblockable))
      return failure(SignalOptions);
    if (kernelOnly(Signal) &&
        (Action.Handler || Action.Flags || Action.Restorer || Action.Mask))
      return failure(SignalOptions);
  }
  return llvm::Error::success();
}

std::optional<SignalActionResult>
LinuxSignals::action(int32_t Signal, const LinuxSignalAction *NewAction,
                     bool ReadOld, ProcessResult &Result) {
  if (!validSignal(Signal) || (NewAction && kernelOnly(Signal)))
    return SignalActionResult{uint64_t(0) - InvalidArgument, {}};
  if (!NewAction && !ReadOld)
    return SignalActionResult{};
  if (!Enabled) {
    unsupported(Result, SignalInputMissing);
    return std::nullopt;
  }
  if (NewAction && !supportedFlags(NewAction->Flags)) {
    unsupported(Result, SignalFlagsUnsupported);
    return std::nullopt;
  }
  SignalActionResult Reply;
  if (ReadOld) {
    auto I = State.Actions.find(Signal);
    if (I == State.Actions.end()) {
      unsupported(Result, llvm::formatv(SignalActionMissing, Signal).str());
      return std::nullopt;
    }
    Reply.Previous = I->second;
  }
  if (NewAction) {
    auto Stored = *NewAction;
    Stored.Mask &= ~Unblockable;
    State.Actions[Signal] = Stored;
    // Installing an ignored disposition discards already-pending instances,
    // even in threads which currently block this signal.
    if (Stored.Handler == 1 || (!Stored.Handler && defaultIgnored(Signal))) {
      Pending &= ~bit(Signal);
      for (auto &[TID, Thread] : Threads)
        Thread.Pending &= ~bit(Signal);
    }
  }
  return Reply;
}

std::optional<uint64_t> LinuxSignals::send(int32_t PID, int32_t Signal,
                                           uint64_t TID,
                                           ProcessResult &Result) {
  if (Signal < 0 || uint32_t(Signal) > SignalCount)
    return uint64_t(0) - InvalidArgument;
  // No host process lookup or signal delivery. Unknown PIDs are not evidence
  // of ESRCH, and pid 0/-1/negative group selectors are not self aliases.
  if (PID != ProcessID) {
    unsupported(Result, llvm::formatv(SignalTargetUnsupported, PID).str());
    return std::nullopt;
  }
  return queue(Signal, TID, false, Result);
}

std::optional<uint64_t> LinuxSignals::sendThread(int32_t Signal, uint64_t TID,
                                                 ProcessResult &Result) {
  if (Signal < 0 || uint32_t(Signal) > SignalCount)
    return uint64_t(0) - InvalidArgument;
  return queue(Signal, TID, true, Result);
}

std::optional<uint64_t> LinuxSignals::queue(int32_t Signal, uint64_t TID,
                                            bool ThreadDirected,
                                            ProcessResult &Result) {
  if (!Signal)
    return 0;
  // SIGKILL cannot be caught, blocked or ignored; it does not need a supplied
  // disposition table and must not return to any thread in the guest group.
  if (Signal == SignalKill) {
    Result.Stop = ProcessStopReason::Signaled;
    Result.TerminationSignal = Signal;
    Result.ExitStatus.reset();
    return std::nullopt;
  }
  // Job control, real-time queues and synchronous CPU faults are
  // separate contracts. Do not reinterpret them as standard user signals.
  if (Signal >= 32 || Signal == 18 || Signal == 19 ||
      (Signal >= 20 && Signal <= 22)) {
    unsupported(Result, llvm::formatv(SignalDeliveryUnsupported, Signal).str());
    return std::nullopt;
  }
  auto I = Threads.find(TID);
  if (I == Threads.end() || !I->second.Mask) {
    unsupported(Result, SignalMaskMissing);
    return std::nullopt;
  }
  if (!ThreadDirected && (*I->second.Mask & bit(Signal)) && Tasks) {
    for (const auto &Task : Tasks()) {
      if (Task.State == 'Z')
        continue;
      auto T = Threads.find(Task.ID);
      if (T == Threads.end() || !T->second.Mask) {
        unsupported(Result, SignalMaskMissing);
        return std::nullopt;
      }
    }
  }
  // Standard signals coalesce separately in process and thread queues.
  (ThreadDirected ? I->second.Pending : Pending) |= bit(Signal);
  return 0;
}

std::optional<LinuxSignals::MaskResult>
LinuxSignals::mask(uint64_t TID, int32_t How, std::optional<uint64_t> Set,
                   bool ReadOld, ProcessResult &Result) {
  if (Set && (How < 0 || How > 2))
    return MaskResult{uint64_t(0) - InvalidArgument, 0};
  auto I = Threads.find(TID);
  if (I == Threads.end() ||
      (!I->second.Mask && (ReadOld || (Set && How != 2)))) {
    unsupported(Result, SignalMaskMissing);
    return std::nullopt;
  }
  MaskResult Reply{0, I->second.Mask.value_or(0)};
  if (Set) {
    const uint64_t Value = *Set & ~Unblockable;
    I->second.Mask = How == 0   ? Reply.Previous | Value
                     : How == 1 ? Reply.Previous & ~Value
                                : Value;
  }
  return Reply;
}

std::optional<uint64_t> LinuxSignals::pending(uint64_t TID,
                                              ProcessResult &Result) const {
  auto I = Threads.find(TID);
  if (I == Threads.end() || !I->second.Mask) {
    unsupported(Result, SignalMaskMissing);
    return std::nullopt;
  }
  return (Pending | I->second.Pending) & *I->second.Mask;
}

llvm::Expected<std::optional<uint64_t>>
LinuxSignals::maskService(ExecutionBackend &CPU, const MemoryLayout &Layout,
                          ServiceKind Kind, const ProcessServiceEvent &Event,
                          uint64_t TID, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  uint64_t Output = A[2], Size = SignalMaskSize, Value = 0;
  if (Kind == ServiceKind::SignalPending) {
    Output = A[0];
    Size = A[1];
    if (Size > SignalMaskSize)
      return Reply(uint64_t(0) - InvalidArgument);
    if (!Size)
      return Reply(0);
    auto Observed = pending(TID, Result);
    if (!Observed)
      return Reply();
    Value = *Observed;
  } else {
    if (A[3] != SignalMaskSize)
      return Reply(uint64_t(0) - InvalidArgument);
    std::optional<uint64_t> Set;
    if (A[1]) {
      auto Allowed = CPU.canAccess(A[1], 8, Read | UserAccessible);
      if (!Allowed)
        return Allowed.takeError();
      if (!*Allowed || A[1] >= Layout.UserLimit || 8 > Layout.UserLimit - A[1])
        return Reply(uint64_t(0) - BadAddress);
      uint8_t Bytes[8];
      if (auto E = CPU.read(A[1], Bytes))
        return std::move(E);
      Set = llvm::support::endian::read64le(Bytes);
    }
    auto Applied = mask(TID, int32_t(uint32_t(A[0])), Set, A[2] != 0, Result);
    if (!Applied)
      return Reply();
    if (Applied->Status || !A[2])
      return Reply(Applied->Status);
    Value = Applied->Previous;
  }
  if (!Size)
    return Reply(0);
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Value);
  auto Stored = writeUserMemory(CPU, Layout, Output,
                                llvm::ArrayRef(Bytes).take_front(Size));
  if (!Stored)
    return Stored.takeError();
  if (*Stored == UserWriteResult::Stored)
    return Reply(0);
  if (*Stored == UserWriteResult::BadAddress)
    return Reply(uint64_t(0) - BadAddress);
  unsupported(Result, SignalPartialOutput);
  return Reply();
}

bool LinuxSignals::ready(uint64_t TID) const {
  auto I = Threads.find(TID);
  return I != Threads.end() && I->second.Mask &&
         ((Pending | I->second.Pending) & ~*I->second.Mask);
}

std::optional<LinuxSignals::Delivery>
LinuxSignals::prepare(uint64_t TID, ProcessResult &Result) {
  auto I = Threads.find(TID);
  if (!Pending && (I == Threads.end() || !I->second.Pending))
    return Delivery{};
  if (I == Threads.end() || !I->second.Mask) {
    unsupported(Result, SignalMaskMissing);
    return std::nullopt;
  }
  while (ready(TID)) {
    // Linux dequeues a private pending signal before a process-wide one.
    const bool ThreadDirected = I->second.Pending & ~*I->second.Mask;
    auto &Queue = ThreadDirected ? I->second.Pending : Pending;
    const uint64_t Ready = Queue & ~*I->second.Mask;
    const int32_t Signal = llvm::countr_zero(Ready) + 1;
    auto A = State.Actions.find(Signal);
    if (A == State.Actions.end()) {
      unsupported(Result, llvm::formatv(SignalActionMissing, Signal).str());
      return std::nullopt;
    }
    const auto Action = A->second;
    if (Action.Handler == 1 || (!Action.Handler && defaultIgnored(Signal))) {
      Queue &= ~bit(Signal);
      continue;
    }
    if (!Action.Handler) {
      if (!defaultTerminates(Signal)) {
        unsupported(Result,
                    llvm::formatv(SignalDeliveryUnsupported, Signal).str());
        return std::nullopt;
      }
      Queue &= ~bit(Signal);
      Result.Stop = ProcessStopReason::Signaled;
      Result.TerminationSignal = Signal;
      Result.ExitStatus.reset();
      return std::nullopt;
    }
    return Delivery{Signal, Action, ThreadDirected};
  }
  return Delivery{};
}

llvm::Expected<bool> LinuxSignals::deliver(ExecutionBackend &CPU,
                                           const MemoryLayout &Layout,
                                           LinuxMemory &Memory, uint64_t TID,
                                           ProcessResult &Result) {
  const auto Next = prepare(TID, Result);
  if (!Next)
    return false;
  if (!Next->Signal)
    return true;
  const auto &[Signal, Action, ThreadDirected] = *Next;
  auto Entered =
      enter(CPU, Layout, Memory, TID, Signal, Action, ThreadDirected, Result);
  if (!Entered || !*Entered)
    return Entered;
  (ThreadDirected ? Threads.at(TID).Pending : Pending) &= ~bit(Signal);
  if (uint32_t(Action.Flags) & 0x80000000u)
    State.Actions.at(Signal).Handler = 0; // SA_RESETHAND, before guest code.
  auto &Mask = *Threads.at(TID).Mask;
  Mask |= Action.Mask;
  if (!(Action.Flags & 0x40000000)) // SA_NODEFER
    Mask |= bit(Signal);
  Mask &= ~Unblockable;
  return true;
}

llvm::Expected<std::optional<uint64_t>>
LinuxSignals::handle(ExecutionBackend &CPU, const MemoryLayout &Layout,
                     const ProcessServiceEvent &Event, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  if (A[3] != SignalMaskSize)
    return Reply(uint64_t(0) - InvalidArgument);
  LinuxSignalAction NewAction;
  if (A[1]) {
    if (A[1] > Layout.UserLimit || SignalActionSize > Layout.UserLimit - A[1])
      return Reply(uint64_t(0) - BadAddress);
    auto Readable =
        CPU.canAccess(A[1], SignalActionSize, Read | UserAccessible);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return Reply(uint64_t(0) - BadAddress);
    uint8_t Bytes[SignalActionSize];
    if (auto E = CPU.read(A[1], Bytes))
      return std::move(E);
    NewAction = {llvm::support::endian::read64le(Bytes),
                 llvm::support::endian::read64le(Bytes + 8),
                 llvm::support::endian::read64le(Bytes + 16),
                 llvm::support::endian::read64le(Bytes + 24)};
  }
  auto Applied = action(int32_t(uint32_t(A[0])), A[1] ? &NewAction : nullptr,
                        A[2] != 0, Result);
  if (!Applied)
    return Reply();
  if (Applied->Status || !A[2])
    return Reply(Applied->Status);
  uint8_t Bytes[SignalActionSize];
  const auto &Old = Applied->Previous;
  llvm::support::endian::write64le(Bytes, Old.Handler);
  llvm::support::endian::write64le(Bytes + 8, Old.Flags);
  llvm::support::endian::write64le(Bytes + 16, Old.Restorer);
  llvm::support::endian::write64le(Bytes + 24, Old.Mask);
  // The new disposition is already installed when the old-action copy fails.
  auto Stored = writeUserMemory(CPU, Layout, A[2], Bytes);
  if (!Stored)
    return Stored.takeError();
  switch (*Stored) {
  case UserWriteResult::Stored:
    return Reply(0);
  case UserWriteResult::BadAddress:
    return Reply(uint64_t(0) - BadAddress);
  case UserWriteResult::MixedAccess:
    unsupported(Result, SignalPartialOutput);
    return Reply();
  }
  llvm_unreachable("unknown Linux user-copy outcome");
}
} // namespace neverd::emulation::linux_model
