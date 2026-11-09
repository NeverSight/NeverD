//===- LinuxServices.h - Per-workload Linux service state -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXSERVICES_H
#define NEVERD_EMULATION_OS_LINUX_LINUXSERVICES_H

#include "LinuxFiles.h"
#include "LinuxMemory.h"
#include "LinuxPriority.h"
#include "LinuxSignals.h"
#include "LinuxTime.h"

namespace neverd::emulation::linux_model {

/// One workload's kernel state, shared by raw traps and libc wrappers.
/// Thread identity is supplied separately at each service boundary.
class LinuxServices {
public:
  LinuxServices(ExecutionBackend &CPU, const MemoryLayout &Layout,
                uint64_t InitialBreak, const ProcessOptions &Options,
                ProcessResult &Result)
      : CPU(CPU), Layout(Layout), Options(Options), Result(Result),
        Memory(*CPU.addressSpace(), Layout, InitialBreak, Options),
        Clock(Options.LinuxTime),
        Files(CPU, Layout, Options.LinuxFiles, &Memory, &Clock),
        Signals(Options.LinuxSignals), Priority(Options.LinuxPriority) {
    Result.IoctlTraceEnabled =
        Options.LinuxFiles && Options.LinuxFiles->IoctlTrace;
  }

  llvm::Expected<std::optional<uint64_t>>
  handle(const ProcessServiceEvent &Event, ThreadContext *Thread = nullptr);
  llvm::Expected<std::optional<uint64_t>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ThreadContext *Thread = nullptr);
  /// A libc model has already constructed these writev buffers. Descriptor
  /// identity, captured output, errno and unsupported providers stay shared.
  llvm::Expected<std::optional<uint64_t>>
  writeBuffers(uint32_t FD, llvm::ArrayRef<llvm::StringRef> Buffers,
               ThreadContext *Thread = nullptr);
  llvm::Error initializeProc() { return Memory.initializeProc(); }
  void setTaskProvider(std::function<std::vector<ProcTask>()> Provider) {
    Signals.setTaskProvider(Provider);
    Files.setTaskProvider(std::move(Provider));
  }
  LinuxSignals &signals() { return Signals; }
  llvm::Expected<bool> deliverSignals(uint64_t TID = ThreadID) {
    return Signals.deliver(CPU, Layout, Memory, TID, Result);
  }
  /// rt_sigreturn restores a continuation; it has no ordinary call result.
  bool takeSignalRestore() { return std::exchange(SignalRestored, false); }
  auto descriptorStatus(uint32_t FD) const {
    return Files.descriptorStatus(FD, Result);
  }
  auto binderConnection(uint32_t FD) const {
    return Files.binderConnection(FD, Result);
  }
  auto binderBuffer(const LinuxDeviceState &Device, uint64_t Address,
                    uint64_t Size) const {
    return Files.binderBuffer(Device, Address, Size);
  }
  LinuxClock &clock() { return Clock; }
  /// Libc's ppoll copies the timespec; the raw syscall updates guest memory.
  llvm::Expected<std::optional<uint64_t>> poll(ServiceKind Kind,
                                               const ProcessServiceEvent &Event,
                                               ThreadContext *Thread,
                                               bool PreserveTimeout) {
    Files.setCurrentTask(Thread ? Thread->ID : ThreadID);
    if (auto E = synchronizeDevices())
      return std::move(E);
    return Files.poll(Kind, Event, Result, Thread, PreserveTimeout);
  }
  LinuxPriority &priority() { return Priority; }
  /// Run while the physical owner is stopped, after idle scheduling and before
  /// guest execution or another service observes device memory.
  llvm::Error synchronizeDevices() {
    return Memory.synchronizeDevices(Clock.elapsed(), Options.LinuxFiles);
  }

  std::optional<SignalActionResult>
  signalAction(int32_t Signal, const LinuxSignalAction *NewAction,
               bool ReadOld) {
    return Signals.action(Signal, NewAction, ReadOld, Result);
  }

private:
  llvm::Expected<std::optional<uint64_t>>
  write(ServiceKind Kind, const ProcessServiceEvent &Event,
        std::optional<llvm::ArrayRef<llvm::StringRef>> Buffers = std::nullopt);
  ExecutionBackend &CPU;
  const MemoryLayout &Layout;
  const ProcessOptions &Options;
  ProcessResult &Result;
  LinuxMemory Memory;
  LinuxClock Clock;
  LinuxFiles Files;
  LinuxSignals Signals;
  LinuxPriority Priority;
  bool SignalRestored = false;
};

} // namespace neverd::emulation::linux_model

#endif
