//===- ProcessSession.h - Explicit guest process workloads -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSSESSION_H
#define NEVERD_EMULATION_PROCESSSESSION_H

#include "neverd/emulation/AndroidNative.h"
#include "neverd/emulation/DarwinFileOptions.h"
#include "neverd/emulation/DarwinProcessOptions.h"
#include "neverd/emulation/DarwinSystemOptions.h"
#include "neverd/emulation/DarwinTimeOptions.h"
#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionBudget.h"
#include "neverd/emulation/ExecutionExit.h"
#include "neverd/emulation/LinuxFileOptions.h"
#include "neverd/emulation/LinuxKernelOptions.h"
#include "neverd/emulation/LinuxNetworkOptions.h"
#include "neverd/emulation/LinuxPriorityOptions.h"
#include "neverd/emulation/LinuxSignalOptions.h"
#include "neverd/emulation/LinuxTimeOptions.h"
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
  /// The user execution contract. Absent selects the profile's checked
  /// contract for the guest instruction set. A profile rejects a contract it
  /// cannot honor; a direct contract has a time budget but no instruction
  /// budget, quantum or instruction count.
  std::optional<ExecutionContract> Contract;
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
  std::optional<LinuxTimeOptions> LinuxTime;
  std::optional<LinuxFileOptions> LinuxFiles;
  std::optional<LinuxNetworkOptions> LinuxNetwork;
  std::optional<LinuxSignalOptions> LinuxSignals;
  std::optional<LinuxPriorityOptions> LinuxPriority;
  std::optional<LinuxKernelOptions> LinuxKernel;
  std::optional<DarwinFileOptions> DarwinFiles;
  std::optional<DarwinTimeOptions> DarwinTime;
  std::optional<DarwinSystemOptions> DarwinSystem;
  std::optional<DarwinProcessOptions> Darwin;
};

struct ProcessServiceEvent {
  uint64_t PC, Number;
  std::array<uint64_t, process_defaults::ServiceArguments> Arguments;
  /// Raw guest return bits; a nonreturning service has no result.
  std::optional<uint64_t> Result;
  /// Darwin BSD carry/error outcome. Absent when no return was modeled or
  /// when the selected OS uses a different error convention.
  std::optional<bool> Error;
  std::optional<uint64_t> ThreadID;
  /// Present when a native wrapper supplies its actual guest return address.
  std::optional<uint64_t> ReturnAddress;
};

struct ProcessIoctlMemory {
  std::optional<uint64_t> Address;
  uint64_t Size = 0;
  std::vector<uint8_t> Before, After;
  std::string BeforeStatus, AfterStatus;
};
struct ProcessIoctlEvent {
  uint64_t PC = 0, Argument = 0;
  uint32_t FD = 0, Request = 0, ThreadID = 0;
  std::optional<uint64_t> ReturnAddress, Result;
  std::vector<ProcessIoctlMemory> Memory;
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
  /// Optional independent observation stream. Result bits use Linux's raw
  /// errno convention even when a Bionic wrapper subsequently returns -1.
  std::vector<ProcessIoctlEvent> IoctlTrace;
  bool IoctlTraceEnabled = false, IoctlTraceTruncated = false;
  std::optional<ExecutionExit> LastCPUExit;
  std::optional<uint64_t> ReturnValue;
  bool InitializersEnabled = false, TraceTruncated = false;
  std::vector<NativeCallEvent> NativeCalls;
  std::optional<uint64_t> EventLogWritesConsumed;
  std::vector<NativeJNIRegistration> JNIRegistrations;
  std::optional<mobile::dalvik::RuntimeSnapshot> JavaState;
  std::vector<uint64_t> Trace;
  std::vector<NativeMemorySnapshot> MemorySnapshots;
  std::vector<NativeThreadSnapshot> NativeThreads;
  /// Runs in Trace: each row owns PCs from Index to the next row's Index.
  std::vector<NativeTraceThread> TraceThreads;
  std::vector<NativeExecutionObservation> ExecutionObservations;
  std::optional<NativeCPUState> CPUState;
  std::optional<std::vector<NativeThreadCPUState>> ThreadCPUStates;
  bool ExecutionObservationsEnabled = false;
  bool ExecutionObservationsTruncated = false;
  /// Guest signal termination, distinct from a normal exit status. No host
  /// signal is sent. A previously completed entry ReturnValue may be retained.
  std::optional<uint32_t> TerminationSignal;
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
