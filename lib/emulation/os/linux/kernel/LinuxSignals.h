//===- LinuxSignals.h - Workload-owned signal dispositions ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXSIGNALS_H
#define NEVERD_EMULATION_OS_LINUX_LINUXSIGNALS_H

#include "LinuxKernel.h"

namespace neverd::emulation::linux_model {
class LinuxMemory;
llvm::Error validateSignalOptions(const LinuxSignalOptions &Options);

struct SignalActionResult {
  uint64_t Status = 0;
  LinuxSignalAction Previous;
};

/// Raw syscalls and libc adapters share the same process-wide action table.
/// Standard process/thread pending signals and per-thread blocked sets.
/// AArch64 handlers use guest signal frames, never host callbacks.
class LinuxSignals {
public:
  explicit LinuxSignals(const std::optional<LinuxSignalOptions> &Options)
      : Enabled(Options.has_value()),
        State(Options.value_or(LinuxSignalOptions{})) {
    Threads[ThreadID].Mask = State.InitialMask;
  }

  std::optional<SignalActionResult> action(int32_t Signal,
                                           const LinuxSignalAction *NewAction,
                                           bool ReadOld, ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  handle(ExecutionBackend &CPU, const MemoryLayout &Layout,
         const ProcessServiceEvent &Event, ProcessResult &Result);
  /// Linux pid_t/int arguments have already been narrowed by the service layer.
  std::optional<uint64_t> send(int32_t PID, int32_t Signal, uint64_t TID,
                               ProcessResult &Result);
  /// Queue a standard signal to the current, registered guest thread.
  /// Unlike kill, this cannot be consumed by a different runnable thread.
  std::optional<uint64_t> sendThread(int32_t Signal, uint64_t TID,
                                     ProcessResult &Result);
  struct MaskResult {
    uint64_t Status = 0, Previous = 0;
  };
  std::optional<MaskResult> mask(uint64_t TID, int32_t How,
                                 std::optional<uint64_t> Set, bool ReadOld,
                                 ProcessResult &Result);
  std::optional<uint64_t> pending(uint64_t TID, ProcessResult &Result) const;
  llvm::Expected<std::optional<uint64_t>>
  maskService(ExecutionBackend &CPU, const MemoryLayout &Layout,
              ServiceKind Kind, const ProcessServiceEvent &Event, uint64_t TID,
              ProcessResult &Result);
  llvm::Expected<bool> deliver(ExecutionBackend &CPU,
                               const MemoryLayout &Layout, LinuxMemory &Memory,
                               uint64_t TID, ProcessResult &Result);
  llvm::Expected<bool> restore(ExecutionBackend &CPU,
                               const MemoryLayout &Layout, uint64_t TID,
                               ProcessResult &Result);
  bool ready(uint64_t TID) const;
  struct Delivery {
    int32_t Signal = 0;
    LinuxSignalAction Action;
    bool ThreadDirected = false;
  };
  /// Discard ignored signals and resolve defaults before interrupting a wait.
  /// Signal zero means no handler; an absent result is a classified stop.
  std::optional<Delivery> prepare(uint64_t TID, ProcessResult &Result);
  size_t frameDepth(uint64_t TID) const {
    return Threads.at(TID).Frames.size();
  }
  void inherit(uint64_t Parent, uint64_t Child) {
    Threads[Child].Mask = Threads.at(Parent).Mask;
  }
  void retire(uint64_t TID) { Threads.erase(TID); }
  void setTaskProvider(std::function<std::vector<ProcTask>()> Provider) {
    Tasks = std::move(Provider);
  }

private:
  bool Enabled;
  LinuxSignalOptions State;
  struct ThreadState {
    std::optional<uint64_t> Mask;
    std::vector<uint64_t> Frames;
    uint64_t Pending = 0;
  };
  std::map<uint64_t, ThreadState> Threads;
  std::function<std::vector<ProcTask>()> Tasks;
  uint64_t Pending = 0, Trampoline = 0;
  std::optional<uint64_t> queue(int32_t Signal, uint64_t TID,
                                bool ThreadDirected, ProcessResult &Result);
  llvm::Expected<bool> enter(ExecutionBackend &CPU, const MemoryLayout &Layout,
                             LinuxMemory &Memory, uint64_t TID, int32_t Signal,
                             const LinuxSignalAction &Action,
                             bool ThreadDirected, ProcessResult &Result);
};
} // namespace neverd::emulation::linux_model
#endif
