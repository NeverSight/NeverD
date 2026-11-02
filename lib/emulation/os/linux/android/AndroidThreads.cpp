//===- AndroidThreads.cpp - Deterministic guest pthread scheduling ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidThreads.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
using namespace thread_attribute_abi;
std::array<uint8_t, ObjectBytes> GuestThreads::defaultAttributes() {
  std::array<uint8_t, ObjectBytes> Bytes{};
  llvm::support::endian::write64le(Bytes.data() + StackSizeOffset,
                                   DefaultStackSize);
  llvm::support::endian::write64le(Bytes.data() + GuardSizeOffset, PageSize);
  return Bytes;
}
void GuestThreads::stackAttributes(std::array<uint8_t, ObjectBytes> &Bytes,
                                   const NativeThreadSnapshot &Thread) {
  llvm::support::endian::write64le(Bytes.data() + StackBaseOffset,
                                   Thread.StackBase);
  llvm::support::endian::write64le(Bytes.data() + StackSizeOffset,
                                   Thread.StackSize);
  llvm::support::endian::write64le(Bytes.data() + GuardSizeOffset,
                                   Thread.GuardSize);
  auto Flags = llvm::support::endian::read32le(Bytes.data() + FlagsOffset);
  Flags = (Flags & ~DetachedFlag) | (Thread.Detached ? DetachedFlag : 0);
  llvm::support::endian::write32le(Bytes.data() + FlagsOffset, Flags);
}

llvm::Error GuestThreads::access(uint64_t Address, uint64_t Size,
                                 unsigned Permissions, unsigned Alignment) {
  if (Address % Alignment)
    return failure(diagnostic::PthreadAlignment);
  auto Allowed = CPU.canAccess(Address, Size, Permissions | UserAccessible);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return failure(diagnostic::GuestPointer);
  return llvm::Error::success();
}
llvm::Error GuestThreads::put64(uint64_t Address, uint64_t Value) {
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Value);
  return CPU.write(Address, Bytes);
}
llvm::Error GuestThreads::initialize(uint64_t StackBase) {
  Thread Main;
  Main.Report = {linux_model::ThreadID,
                 thread_model::ArenaBase - PageSize,
                 TLSAddress,
                 StackBase,
                 Options.StackSize,
                 0};
  Main.Kernel.ID = Main.Report.ID;
  Main.Attributes = defaultAttributes();
  stackAttributes(Main.Attributes, Main.Report);
  Threads.push_back(std::move(Main));
  if (enabled())
    return put64(TLSAddress + thread_model::IdentityOffset,
                 current().Report.Handle);
  return llvm::Error::success();
}
BionicResult GuestThreads::unsupported(llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason.str();
  return std::optional<BionicValue>();
}
llvm::Error GuestThreads::validateTLS() {
  auto Register = CPU.readRegister(CPURegister::AArch64TPIDR_EL0);
  if (!Register)
    return Register.takeError();
  if ((*Register)[0] != tls())
    return failure(diagnostic::ThreadTLS);
  if (auto E = access(tls(), 16, Read))
    return E;
  uint8_t Bytes[16];
  if (auto E = CPU.read(tls(), Bytes))
    return E;
  if (llvm::support::endian::read64le(Bytes) != tls() ||
      llvm::support::endian::read64le(Bytes + thread_model::IdentityOffset) !=
          current().Report.Handle)
    return failure(diagnostic::ThreadTLS);
  return llvm::Error::success();
}
llvm::Error GuestThreads::retire(size_t Index) {
  auto &T = Threads[Index];
  if (T.MappingSize) {
    auto Removed =
        Kernel.handle(linux_model::ServiceKind::Munmap,
                      {0, 0, {T.MappingBase, T.MappingSize}, {}}, &T.Kernel);
    if (!Removed)
      return Removed.takeError();
    if (!*Removed || **Removed != 0)
      return failure("guest thread mappings could not be retired");
    T.MappingSize = 0;
  }
  T.Context.reset();
  T.Report.Retired = true;
  return llvm::Error::success();
}
llvm::Error GuestThreads::finish(uint64_t Value, uint32_t ExitStatus) {
  LastExitStatus = ExitStatus;
  current().Report.Finished = true;
  current().Report.ReturnValue = Value;
  current().Kernel.Exit.reset();
  Signals.retire(id());
  // A joinable userspace handle can survive the nonleader kernel task.
  // Keep the entry/leader record consistent with the retained proc zombie.
  if (!isEntry())
    PriorityState.exit(id());
  if (current().Report.Detached)
    return retire(Current);
  return llvm::Error::success();
}
std::optional<size_t> GuestThreads::nextRunnable() const {
  for (size_t Offset = 1; Offset <= Threads.size(); ++Offset) {
    size_t Candidate = (Current + Offset) % Threads.size();
    const auto &T = Threads[Candidate];
    if (!T.Report.Finished &&
        (!T.Waiting || ready(*T.Waiting) || Signals.ready(T.Kernel.ID)))
      return Candidate;
  }
  return std::nullopt;
}

llvm::Error GuestThreads::switchTo(size_t Next) {
  if (Next == Current)
    return llvm::Error::success();
  if (!current().Report.Finished) {
    if (current().Context) {
      if (auto E = CPU.saveContext(*current().Context))
        return E;
    } else {
      auto Saved = CPU.saveContext();
      if (!Saved)
        return Saved.takeError();
      current().Context = std::move(*Saved);
    }
  }
  if (auto E = CPU.restoreContext(*Threads[Next].Context))
    return E;
  Current = Next;
  return llvm::Error::success();
}

llvm::Expected<bool> GuestThreads::schedule(Bionic &LibC) {
  for (;;) {
    auto Next = nextRunnable();
    if (!Next) {
      auto Deadline = nextWake();
      if (!Deadline)
        break;
      if (!Clock.advanceTo(*Deadline, Result))
        return false;
      Next = nextRunnable();
      assert(Next && "earliest sleep did not become runnable");
    }
    if (auto E = switchTo(*Next))
      return std::move(E);
    if (!current().Waiting)
      return true;
    if (Signals.ready(id())) {
      const auto Delivery = Signals.prepare(id(), Result);
      if (!Delivery)
        return false;
      if (!ready(*current().Waiting)) {
        if (Delivery->Signal)
          return interruptWait(Delivery->Action);
        // Ignored pending signals do not complete or interrupt this wait.
        continue;
      }
    }
    auto Resumed = resumeWait(LibC);
    if (!Resumed)
      return Resumed.takeError();
    if (!*Resumed)
      return false;
    if (std::holds_alternative<GuestThreadWait>(**Resumed))
      continue;
    if (auto E = completeWait(std::get<uint64_t>(**Resumed)))
      return std::move(E);
    return true;
  }
  for (const auto &T : Threads) {
    if (!T.Report.Finished) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::ThreadDeadlock;
      return false;
    }
  }
  if (Result.ReturnValue)
    Result.Stop = ProcessStopReason::Returned;
  else {
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = LastExitStatus;
  }
  return false;
}
std::vector<linux_model::ProcTask> GuestThreads::procTasks() const {
  std::vector<linux_model::ProcTask> Tasks;
  for (size_t I = 0; I < Threads.size(); ++I) {
    const auto &T = Threads[I];
    if (I && T.Report.Finished)
      continue;
    Tasks.push_back({uint32_t(T.Report.ID), T.Report.Finished ? 'Z'
                                            : T.Waiting && !ready(*T.Waiting) &&
                                                    !Signals.ready(T.Kernel.ID)
                                                ? 'S'
                                                : 'R'});
  }
  return Tasks;
}
std::vector<NativeThreadCPUState> GuestThreads::observeCPUStates() const {
  std::vector<NativeThreadCPUState> States;
  for (size_t I = 0; I < Threads.size(); ++I) {
    const auto &Thread = Threads[I];
    if (Thread.Report.Finished)
      continue;
    auto &Snapshot = States.emplace_back();
    Snapshot.Current = I == Current;
    auto &State = Snapshot.State;
    State.ThreadID = Thread.Kernel.ID;
    auto Registers = Snapshot.Current ? CPU.readRegisters()
                                      : CPU.readRegisters(*Thread.Context);
    if (Registers)
      State.Registers = std::move(*Registers);
    else
      State.Diagnostic = llvm::toString(Registers.takeError());
  }
  return States;
}
void GuestThreads::report() {
  if (!enabled())
    return;
  for (const auto &T : Threads) {
    auto Snapshot = T.Report;
    Snapshot.Waiting = T.Waiting.has_value();
    if (Result.Stop == ProcessStopReason::Signaled && !Snapshot.Finished) {
      Snapshot.TerminationSignal = Result.TerminationSignal;
      Snapshot.Waiting = false;
    }
    Result.NativeThreads.push_back(Snapshot);
  }
}
} // namespace neverd::emulation::android_model
