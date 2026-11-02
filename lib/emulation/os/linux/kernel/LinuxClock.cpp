//===- LinuxClock.cpp - Explicit workload time and idle advancement ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTime.h"

#include "llvm/Support/FormatVariadic.h"

#include <set>

namespace neverd::emulation::linux_model {
namespace {
constexpr uint64_t NanosecondsPerSecond = 1000000000;

bool advancingClock(int32_t ID) {
  return ID == ClockRealtime || ID == ClockMonotonic ||
         ID == ClockMonotonicRaw || ID == ClockBootTime;
}

std::optional<LinuxTimespec> shifted(LinuxTimespec Value, uint64_t Elapsed) {
  const uint64_t Nanoseconds =
      Value.Nanoseconds + Elapsed % NanosecondsPerSecond;
  const int64_t Seconds =
      Elapsed / NanosecondsPerSecond + Nanoseconds / NanosecondsPerSecond;
  if (Value.Seconds > INT64_MAX - Seconds)
    return std::nullopt;
  return LinuxTimespec{Value.Seconds + Seconds,
                       int64_t(Nanoseconds % NanosecondsPerSecond)};
}

void unsupported(ProcessResult &Result, llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason.str();
}
} // namespace

bool isKnownClock(int32_t ID) {
  switch (ID) {
#define NEVERD_LINUX_CLOCK(Name, Number)                                       \
  case Number:                                                                 \
    return true;
#include "../LinuxValues.def"
#undef NEVERD_LINUX_CLOCK
  default:
    return false;
  }
}

bool isNormalizedTimespec(const LinuxTimespec &Value) {
  return Value.Nanoseconds >= 0 &&
         uint64_t(Value.Nanoseconds) < NanosecondsPerSecond;
}

llvm::Error validateTimeOptions(const LinuxTimeOptions &Options) {
  if (Options.Clocks.size() > TimeClockLimit)
    return failure(TimeClockOption);
  std::set<int32_t> Identities;
  for (const auto &[ID, Value] : Options.Clocks) {
    const auto CPU = processCPUClock(ID);
    if ((!isKnownClock(ID) && !CPU) ||
        !Identities.insert(canonicalClockID(ID)).second)
      return failure(TimeClockOption);
    if (!isNormalizedTimespec(Value))
      return failure(TimeNanoseconds);
    if ((CPU || ID == ClockProcessCPU || ID == ClockThreadCPU) &&
        Value.Seconds < 0)
      return failure(TimeCPUObservation);
    if ((Options.AdvanceOnIdle || Options.NanosecondsPerInstruction) &&
        !advancingClock(ID) && ID != ClockProcessCPU && !CPU)
      return failure(TimeAdvanceClock);
  }
  return llvm::Error::success();
}

std::optional<LinuxTimespec> LinuxClock::read(int32_t ID,
                                              ProcessResult &Result) const {
  if (Options) {
    ID = canonicalClockID(ID);
    auto I = Options->Clocks.find(ID);
    if (I == Options->Clocks.end()) {
      if (ID == ClockProcessCPU) {
        I = Options->Clocks.find(processCPUClockID(0, CPUClockScheduled));
        if (I == Options->Clocks.end())
          I = Options->Clocks.find(
              processCPUClockID(ProcessID, CPUClockScheduled));
      } else if (auto CPU = processCPUClock(ID); CPU && CPU->PID == ProcessID) {
        I = Options->Clocks.find(processCPUClockID(0, CPU->Kind));
      }
    }
    if (I != Options->Clocks.end()) {
      auto Value = advancingClock(ID) ? shifted(I->second, Elapsed)
                                      : std::optional<LinuxTimespec>(I->second);
      if (!Value)
        unsupported(Result, TimeAdvanceOverflow);
      return Value;
    }
  }
  unsupported(Result, llvm::formatv(TimeClockMissing, ID).str());
  return std::nullopt;
}

std::optional<uint64_t> LinuxClock::deadline(const LinuxTimespec &Duration,
                                             ProcessResult &Result) const {
  assert(advancesOnIdle() && Duration.Seconds >= 0 &&
         isNormalizedTimespec(Duration));
  // Saturated Linux ktime deadlines are outside this finite model. They do
  // not become EINVAL, an immediate success or a wrapped earlier deadline.
  if (uint64_t(Duration.Seconds) >=
      uint64_t(INT64_MAX) / NanosecondsPerSecond) {
    unsupported(Result, SleepRange);
    return std::nullopt;
  }
  const uint64_t DurationNS =
      uint64_t(Duration.Seconds) * NanosecondsPerSecond + Duration.Nanoseconds;
  if (DurationNS > uint64_t(INT64_MAX) - Elapsed) {
    unsupported(Result, SleepRange);
    return std::nullopt;
  }
  return Elapsed + DurationNS;
}

bool LinuxClock::advanceTo(uint64_t Deadline, ProcessResult &Result) {
  assert(Options && (advancesOnIdle() || Options->NanosecondsPerInstruction) &&
         Deadline >= Elapsed && Deadline <= INT64_MAX);
  for (const auto &[ID, Value] : Options->Clocks) {
    if (advancingClock(ID) && !shifted(Value, Deadline)) {
      unsupported(Result, TimeAdvanceOverflow);
      return false;
    }
  }
  Elapsed = Deadline;
  return true;
}

bool LinuxClock::accountInstructions(uint64_t Total, ProcessResult &Result) {
  if (!Options || !Options->NanosecondsPerInstruction)
    return true;
  if (Total < AccountedInstructions ||
      Total - AccountedInstructions > (uint64_t(INT64_MAX) - Elapsed) /
                                          Options->NanosecondsPerInstruction) {
    unsupported(Result, TimeAdvanceOverflow);
    return false;
  }
  if (!advanceTo(Elapsed + (Total - AccountedInstructions) *
                               Options->NanosecondsPerInstruction,
                 Result))
    return false;
  AccountedInstructions = Total;
  return true;
}
} // namespace neverd::emulation::linux_model
