//===- ProcessSession.h - Explicit guest process workloads -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSSESSION_H
#define NEVERD_EMULATION_PROCESSSESSION_H

#include "neverd/emulation/AndroidNative.h"
#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionBudget.h"
#include "neverd/emulation/ExecutionExit.h"
#include "neverd/emulation/WindowsProcessOptions.h"

#include <array>
#include <filesystem>
#include <vector>

namespace neverd::emulation {
enum class ProcessProfile {
#define NEVERD_PROCESS_PROFILE(Name, Text) Name,
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_PROFILE
};
enum class ProcessStopReason {
#define NEVERD_PROCESS_STOP(Name, Text) Name,
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_STOP
};
namespace process_defaults {
#define NEVERD_PROCESS_LIMIT(Name, Value)                                      \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_LIMIT
} // namespace process_defaults

struct ProcessOptions {
  ExecutionBackendKind Backend = ExecutionBackendKind::Auto;
  ExecutionLimits Limits{process_defaults::Instructions,
                         process_defaults::Events,
                         process_defaults::TimeoutMicroseconds};
  uint64_t MemoryLimit = process_defaults::Memory;
  uint64_t StackSize = process_defaults::Stack;
  uint64_t OutputLimit = process_defaults::Output;
  uint64_t InstructionQuantum = process_defaults::Quantum;
  /// Complete argv, including argv[0]. Empty selects the input's filename.
  std::vector<std::string> Arguments;
  /// Explicit guest environment; no host environment is inherited.
  std::vector<std::string> Environment;
  std::optional<AndroidNativeOptions> Android;
  std::optional<WindowsProcessOptions> Windows;
};

struct ProcessServiceEvent {
  uint64_t PC, Number;
  std::array<uint64_t, process_defaults::ServiceArguments> Arguments;
  /// Raw guest return bits; a nonreturning service has no result.
  std::optional<uint64_t> Result;
};

struct ProcessResult {
  ProcessProfile Profile;
  GuestArchitecture Architecture;
  ExecutionBackendKind SelectedBackend;
  std::string BackendSelectionReason;
  ProcessStopReason Stop = ProcessStopReason::RuntimeFailure;
  std::optional<uint32_t> ExitStatus;
  std::string Diagnostic;
  uint64_t Entry = 0, PC = 0, Instructions = 0, Events = 0;
  /// Exact captured bytes, including NUL and non-UTF8 output.
  std::string StandardOutput, StandardError;
  std::vector<ProcessServiceEvent> Services;
  std::optional<ExecutionExit> LastCPUExit;
  std::optional<uint64_t> ReturnValue;
  bool InitializersEnabled = false, TraceTruncated = false;
  std::vector<NativeCallEvent> NativeCalls;
  std::vector<uint64_t> Trace;
  std::vector<NativeMemorySnapshot> MemorySnapshots;
};

llvm::Expected<ProcessProfile> parseProcessProfile(llvm::StringRef Name);
const char *processProfileName(ProcessProfile Profile);
const char *processStopReasonName(ProcessStopReason Reason);
/// Profile selection is explicit and independent of the host. Invalid input
/// or unavailable transport returns Error; guest termination/failure is typed.
llvm::Expected<ProcessResult>
emulateProcess(const std::filesystem::path &Path, ProcessProfile Profile,
               const ProcessOptions &Options = {});
} // namespace neverd::emulation
#endif
