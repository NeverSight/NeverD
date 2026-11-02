//===- LinuxTime.h - Shared explicit Linux clock inputs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXTIME_H
#define NEVERD_EMULATION_OS_LINUX_LINUXTIME_H
#include "LinuxKernel.h"

#include <variant>

namespace neverd::emulation::linux_model {
#define NEVERD_LINUX_CLOCK(Name, ID) inline constexpr int32_t Name = ID;
#include "../LinuxValues.def"
#undef NEVERD_LINUX_CLOCK
bool isNormalizedTimespec(const LinuxTimespec &Value);
llvm::Error validateTimeOptions(const LinuxTimeOptions &Options);
bool isKnownClock(int32_t ID);
struct ProcessCPUClock {
  uint32_t PID, Kind;
};
std::optional<ProcessCPUClock> processCPUClock(int32_t ID);
int32_t processCPUClockID(uint32_t PID, uint32_t Kind);
int32_t canonicalClockID(int32_t ID);
llvm::Error validateCPUClockInputs(const ProcessOptions &Options);
/// Select an observed process clock. Zero admits the query, an errno rejects
/// it, and an absent result preserves an unsupported observation boundary.
std::optional<uint64_t> selectProcessCPUClock(int32_t ID, uint64_t CurrentTask,
                                              const ProcessOptions &Options,
                                              ProcessResult &Result,
                                              int32_t &SelectedID);

/// One workload's clock observations and elapsed virtual nanoseconds. Sleep
/// admission and idle advancement share this owner across every service ABI.
class LinuxClock {
public:
  explicit LinuxClock(const std::optional<LinuxTimeOptions> &Options)
      : Options(Options) {}
  bool advancesOnIdle() const { return Options && Options->AdvanceOnIdle; }
  uint64_t elapsed() const { return Elapsed; }
  /// Missing input stops the workload without inventing a timestamp.
  std::optional<LinuxTimespec> read(int32_t ID, ProcessResult &Result) const;
  std::optional<uint64_t> deadline(const LinuxTimespec &Duration,
                                   ProcessResult &Result) const;
  /// Validate all supplied clocks before committing any elapsed time.
  bool advanceTo(uint64_t Deadline, ProcessResult &Result);
  /// A cumulative workload count, shared by every thread. Repeated counts
  /// cannot bill an instruction twice; failed advancement commits no time.
  bool accountInstructions(uint64_t Total, ProcessResult &Result);

private:
  const std::optional<LinuxTimeOptions> &Options;
  uint64_t Elapsed = 0;
  uint64_t AccountedInstructions = 0;
};
llvm::Expected<std::optional<uint64_t>>
timeService(ExecutionBackend &CPU, ServiceKind Kind,
            const ProcessServiceEvent &Event, const MemoryLayout &Layout,
            const ProcessOptions &Options, LinuxClock &Clock,
            ProcessResult &Result, uint64_t CurrentTask = ThreadID);
llvm::Expected<std::optional<uint64_t>>
sleepService(ExecutionBackend &CPU, const ProcessServiceEvent &Event,
             const MemoryLayout &Layout, LinuxClock &Clock,
             ThreadContext *Thread, ProcessResult &Result);
/// Admit a copied or scalar duration through the same virtual sleep policy.
/// A modeled libc automatic retains a value instead of inventing guest heap
/// storage. Both destinations receive the same kernel-owned remainder.
using SleepRemainder = std::variant<uint64_t, std::shared_ptr<LinuxTimespec>>;
std::optional<uint64_t> sleepRelative(ExecutionBackend &CPU,
                                      const MemoryLayout &Layout,
                                      const LinuxTimespec &Duration,
                                      LinuxClock &Clock, ThreadContext *Thread,
                                      ProcessResult &Result,
                                      SleepRemainder Remaining = uint64_t(0));
} // namespace neverd::emulation::linux_model
#endif
