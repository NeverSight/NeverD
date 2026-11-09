//===- LinuxKernel.h - Shared Linux kernel contracts -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXKERNEL_H
#define NEVERD_EMULATION_OS_LINUX_LINUXKERNEL_H

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringRef.h"

namespace neverd::emulation::linux_model {
#define NEVERD_LINUX_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#define NEVERD_LINUX_DIAGNOSTIC(Name, Text) inline constexpr char Name[] = Text;
#include "../LinuxValues.def"
#undef NEVERD_LINUX_DIAGNOSTIC
#undef NEVERD_LINUX_VALUE

inline std::string procProcessPath() {
  return "/proc/" + std::to_string(ProcessID);
}

inline llvm::Error failure(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
enum class ServiceKind {
#define NEVERD_LINUX_SERVICE(Name, X64Number, ARMNumber, Count) Name,
#define NEVERD_LINUX_X64_SERVICE(Name, Number, Count) Name,
#include "../LinuxValues.def"
#undef NEVERD_LINUX_X64_SERVICE
#undef NEVERD_LINUX_SERVICE
};
/// Address policy shared by ELF processes and Android native workloads.
struct MemoryLayout {
  uint64_t UserLimit, PageSize;
};
/// Explicit OS-owned thread identity. The caller consumes Exit after a
/// nonreturning SYS_exit; exit_group still terminates the whole process.
/// An absent context retains the Linux single-thread workload contract.
struct ProcTask {
  uint32_t ID;
  char State; // R: runnable, S: cooperative wait, Z: exited group leader.
};
/// A suspended kernel operation retains its copied inputs in the kernel owner.
/// The scheduler only asks when to run it and completes the original trap.
class KernelWait {
public:
  virtual ~KernelWait() = default;
  virtual bool ready() const = 0;
  virtual std::optional<uint64_t> nextWake() const = 0;
  virtual llvm::Expected<std::optional<uint64_t>>
  resume(ProcessResult &Result) = 0;
  /// A zero-progress operation may request re-entry at the original trap.
  /// The old wait is released before the handler; re-entry imports the restored
  /// registers and looks up descriptors again, rather than resuming stale
  /// state.
  virtual bool restartOnSignal(const LinuxSignalAction &) const {
    return false;
  }
  /// Only the operation owner can decide whether/how a caught signal ends a
  /// wait. Unsupported restart/copy-out contracts must not become EINTR.
  virtual llvm::Expected<std::optional<uint64_t>>
  interrupt(ProcessResult &Result) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = SignalWaitUnsupported;
    return std::optional<uint64_t>();
  }
};
struct ThreadContext {
  uint64_t ID = ThreadID;
  std::optional<uint64_t> Exit;
  /// Consumed by the scheduler before another service can run on this thread.
  std::unique_ptr<KernelWait> Pending;
};
struct ServiceABI {
  ServiceRequestKind Trap;
  CPURegister Number, Result, StackPointer, PC;
  std::array<CPURegister, process_defaults::ServiceArguments> Arguments;
};
const ServiceABI &serviceABI(GuestArchitecture Architecture);
llvm::Expected<std::optional<uint64_t>>
writeOutput(ExecutionBackend &CPU, ServiceKind Kind,
            const ProcessServiceEvent &Event, const MemoryLayout &Layout,
            const ProcessOptions &Options, ProcessResult &Result,
            std::optional<uint64_t> AfterVectorImportError = std::nullopt);
/// Captured output from already materialized model bytes. Uses the same
/// whole-write budget and sinks as guest-backed write/writev.
llvm::Expected<std::optional<uint64_t>>
writeOutputBuffers(ExecutionBackend &CPU, uint32_t FD,
                   llvm::ArrayRef<llvm::StringRef> Buffers,
                   const MemoryLayout &Layout, const ProcessOptions &Options,
                   ProcessResult &Result);
llvm::Expected<ProcessServiceEvent> readService(ExecutionBackend &CPU,
                                                const ServiceRequest &Request);
llvm::Error returnService(ExecutionBackend &CPU, const ServiceRequest &Request,
                          uint64_t Result);
llvm::Expected<std::optional<uint64_t>>
archPrctl(ExecutionBackend &CPU, const ProcessServiceEvent &Event,
          const MemoryLayout &Layout, ProcessResult &Result);
} // namespace neverd::emulation::linux_model
#endif
