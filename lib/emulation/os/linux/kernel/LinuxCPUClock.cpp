//===- LinuxCPUClock.cpp - Explicit process CPU clock identities ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTime.h"

#include <cassert>

namespace neverd::emulation::linux_model {
namespace {
bool hasProcessCPUClockContract(const ProcessOptions &Options) {
  return Options.LinuxKernel &&
         (Options.LinuxKernel->GKI || Options.LinuxKernel->Tasks);
}
} // namespace

std::optional<ProcessCPUClock> processCPUClock(int32_t ID) {
  const uint32_t Bits = static_cast<uint32_t>(ID);
  if (ID >= 0 || (Bits & CPUClockThread) ||
      (Bits & CPUClockKindMask) > CPUClockScheduled)
    return std::nullopt;
  return ProcessCPUClock{~Bits >> CPUClockShift,
                         uint32_t(Bits & CPUClockKindMask)};
}

int32_t processCPUClockID(uint32_t PID, uint32_t Kind) {
  return static_cast<int32_t>((~PID << CPUClockShift) | Kind);
}

int32_t canonicalClockID(int32_t ID) {
  auto CPU = processCPUClock(ID);
  if (!CPU)
    return ID;
  const uint32_t PID = CPU->PID ? CPU->PID : uint32_t(ProcessID);
  if (PID == ProcessID && CPU->Kind == CPUClockScheduled)
    return ClockProcessCPU;
  return processCPUClockID(PID, CPU->Kind);
}

llvm::Error validateCPUClockInputs(const ProcessOptions &Options) {
  if (!Options.LinuxTime)
    return llvm::Error::success();
  for (const auto &[ID, Value] : Options.LinuxTime->Clocks) {
    const auto CPU = processCPUClock(ID);
    if (!CPU)
      continue;
    if (!hasProcessCPUClockContract(Options))
      return failure(TimeCPUObservation);
    if (!CPU->PID || CPU->PID == ProcessID)
      continue;
    if (!Options.LinuxKernel->Tasks)
      return failure(TimeCPUObservation);
    const auto Task = Options.LinuxKernel->Tasks->find(CPU->PID);
    if (Task == Options.LinuxKernel->Tasks->end() || !Task->second.GroupLeader)
      return failure(TimeCPUObservation);
  }
  return llvm::Error::success();
}

std::optional<uint64_t> selectProcessCPUClock(int32_t ID, uint64_t CurrentTask,
                                              const ProcessOptions &Options,
                                              ProcessResult &Result,
                                              int32_t &SelectedID) {
  auto Unsupported = [&](const char *Reason) -> std::optional<uint64_t> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Reason;
    return std::nullopt;
  };
  if (!hasProcessCPUClockContract(Options))
    return Unsupported(TimeDynamicClock);
  const uint32_t Bits = static_cast<uint32_t>(ID);
  if ((Bits & CPUClockFDMask) == CPUClockFD)
    return Unsupported(TimeDynamicClock);
  if ((Bits & CPUClockKindMask) > CPUClockScheduled)
    return uint64_t(0) - InvalidArgument;
  if (Bits & CPUClockThread)
    return Unsupported(TimeThreadCPUClock);
  const auto CPU = processCPUClock(ID);
  assert(CPU);
  // clock_gettime permits the current task's PID to identify its group even
  // when the current task is not the group leader. Other tasks require TGID.
  if (!CPU->PID || CPU->PID == CurrentTask || CPU->PID == ProcessID) {
    SelectedID = canonicalClockID(processCPUClockID(ProcessID, CPU->Kind));
    return 0;
  }
  if (!Options.LinuxKernel->Tasks)
    return Unsupported(TimeCPUTargetMissing);
  const auto Task = Options.LinuxKernel->Tasks->find(CPU->PID);
  if (Task == Options.LinuxKernel->Tasks->end() || !Task->second.GroupLeader)
    return uint64_t(0) - InvalidArgument;
  SelectedID = ID;
  return 0;
}
} // namespace neverd::emulation::linux_model
