//===- LinuxSleep.cpp - Relative nanosleep admission ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTime.h"
#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
namespace {
class RelativeSleep final : public KernelWait {
public:
  RelativeSleep(ExecutionBackend &CPU, const MemoryLayout &Layout,
                LinuxClock &Clock, uint64_t Deadline, SleepRemainder Remaining)
      : CPU(CPU), Layout(Layout), Clock(Clock), Deadline(Deadline),
        Remaining(std::move(Remaining)) {}
  bool ready() const override { return Deadline <= Clock.elapsed(); }
  std::optional<uint64_t> nextWake() const override { return Deadline; }
  llvm::Expected<std::optional<uint64_t>> resume(ProcessResult &) override {
    assert(ready() && "relative sleep resumed before its deadline");
    return std::optional<uint64_t>(0);
  }
  llvm::Expected<std::optional<uint64_t>>
  interrupt(ProcessResult &Result) override {
    assert(!ready() && "completed sleep cannot be interrupted");
    const uint64_t Left = Deadline - Clock.elapsed();
    const LinuxTimespec Value{int64_t(Left / 1000000000),
                              int64_t(Left % 1000000000)};
    if (auto *Output =
            std::get_if<std::shared_ptr<LinuxTimespec>>(&Remaining)) {
      assert(*Output && "modeled libc remainder has no storage");
      **Output = Value;
    } else if (const auto Address = std::get<uint64_t>(Remaining)) {
      uint8_t Bytes[16];
      llvm::support::endian::write64le(Bytes, Value.Seconds);
      llvm::support::endian::write64le(Bytes + 8, Value.Nanoseconds);
      auto Stored = writeUserMemory(CPU, Layout, Address, Bytes);
      if (!Stored)
        return Stored.takeError();
      if (*Stored == UserWriteResult::BadAddress)
        return std::optional<uint64_t>(0 - BadAddress);
      if (*Stored == UserWriteResult::MixedAccess) {
        Result.Stop = ProcessStopReason::UnsupportedService;
        Result.Diagnostic = SleepPartialOutput;
        return std::optional<uint64_t>();
      }
    }
    // Relative nanosleep is never restarted by SA_RESTART.
    return std::optional<uint64_t>(0 - InterruptedCall);
  }

private:
  ExecutionBackend &CPU;
  const MemoryLayout Layout;
  LinuxClock &Clock;
  uint64_t Deadline;
  SleepRemainder Remaining;
};
} // namespace
llvm::Expected<std::optional<uint64_t>>
sleepService(ExecutionBackend &CPU, const ProcessServiceEvent &Event,
             const MemoryLayout &Layout, LinuxClock &Clock,
             ThreadContext *Thread, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  if (!Clock.advancesOnIdle())
    return sleepRelative(CPU, Layout, {0, 0}, Clock, Thread, Result);
  // Linux copies both LP64 fields before validating either value. Failed
  // input copies have no observable partial output to the guest.
  uint8_t Bytes[16];
  const uint64_t Address = Event.Arguments[0];
  if (Address > Layout.UserLimit || sizeof(Bytes) > Layout.UserLimit - Address)
    return Reply(0 - BadAddress);
  auto Readable = CPU.canAccess(Address, sizeof(Bytes), Read | UserAccessible);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return Reply(0 - BadAddress);
  if (auto E = CPU.read(Address, Bytes))
    return std::move(E);
  const LinuxTimespec Duration{
      int64_t(llvm::support::endian::read64le(Bytes)),
      int64_t(llvm::support::endian::read64le(Bytes + 8))};
  return sleepRelative(CPU, Layout, Duration, Clock, Thread, Result,
                       Event.Arguments[1]);
}

std::optional<uint64_t> sleepRelative(ExecutionBackend &CPU,
                                      const MemoryLayout &Layout,
                                      const LinuxTimespec &Duration,
                                      LinuxClock &Clock, ThreadContext *Thread,
                                      ProcessResult &Result,
                                      SleepRemainder Remaining) {
  using Reply = std::optional<uint64_t>;
  if (!Clock.advancesOnIdle()) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = SleepPolicy;
    return Reply();
  }
  if (Duration.Seconds < 0 || !isNormalizedTimespec(Duration))
    return Reply(0 - InvalidArgument);
  auto Deadline = Clock.deadline(Duration, Result);
  if (!Deadline)
    return Reply();
  // A successful relative sleep never touches the remaining-time pointer,
  // even when it aliases the request or is inaccessible.
  if (*Deadline == Clock.elapsed())
    return Reply(0);
  if (Thread) {
    assert(!Thread->Pending && !Thread->Exit);
    Thread->Pending = std::make_unique<RelativeSleep>(
        CPU, Layout, Clock, *Deadline, std::move(Remaining));
    return Reply();
  }
  if (!Clock.advanceTo(*Deadline, Result))
    return Reply();
  return Reply(0);
}
} // namespace neverd::emulation::linux_model
