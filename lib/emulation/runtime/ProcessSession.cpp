//===- ProcessSession.cpp - Explicit process-profile dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ProcessSession.h"

#include "../core/ExecutionDiagnostics.h"
#include "../os/darwin/kernel/DarwinFiles.h"
#include "../os/darwin/kernel/DarwinSystem.h"
#include "../os/darwin/kernel/DarwinTime.h"
#include "../os/darwin/process/DarwinProcess.h"
#include "../os/linux/android/AndroidInternal.h"
#include "../os/linux/kernel/LinuxKernelAvailability.h"
#include "../os/linux/kernel/LinuxPriority.h"
#include "../os/linux/kernel/LinuxSignals.h"
#include "../os/linux/kernel/LinuxTime.h"
#include "../os/linux/process/LinuxProcess.h"
#include "../os/windows/process/WindowsProcess.h"
#include "RuntimeValues.h"

#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::emulation {
llvm::Expected<ProcessProfile> parseProcessProfile(llvm::StringRef Name) {
#define NEVERD_PROCESS_PROFILE(ID, Text)                                       \
  if (Name == Text)                                                            \
    return ProcessProfile::ID;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_PROFILE
  return diagnostic::error(runtime::ProcessProfile);
}
const char *processProfileName(ProcessProfile Profile) {
  switch (Profile) {
#define NEVERD_PROCESS_PROFILE(ID, Text)                                       \
  case ProcessProfile::ID:                                                     \
    return Text;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_PROFILE
  }
  llvm_unreachable(runtime::ProcessProfile);
}
const char *processStopReasonName(ProcessStopReason Reason) {
  switch (Reason) {
#define NEVERD_PROCESS_STOP(ID, Text)                                          \
  case ProcessStopReason::ID:                                                  \
    return Text;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_STOP
  }
  llvm_unreachable(runtime::ProcessOutcome);
}

namespace {
llvm::Expected<ProcessResult> runProfile(const std::filesystem::path &Path,
                                         ProcessProfile Profile,
                                         const ProcessOptions &Options,
                                         ProcessObserver *Observer) {
  if (Options.Windows && Profile != ProcessProfile::WindowsPE64)
    return diagnostic::error(process_report::WindowsProfile);
  if (Options.DarwinSystem) {
    if (Profile != ProcessProfile::MacOSMachO64 &&
        Profile != ProcessProfile::IOSMachO64 &&
        Profile != ProcessProfile::IOSSimulatorMachO64)
      return diagnostic::error(process_report::DarwinSystemProfile);
    if (auto E = darwin_model::validateSystemOptions(*Options.DarwinSystem))
      return std::move(E);
  }
  if (Options.DarwinTime) {
    if (Profile != ProcessProfile::MacOSMachO64 &&
        Profile != ProcessProfile::IOSMachO64 &&
        Profile != ProcessProfile::IOSSimulatorMachO64)
      return diagnostic::error(process_report::DarwinTimeProfile);
    if (auto E = darwin_model::validateTimeOptions(*Options.DarwinTime))
      return std::move(E);
  }
  if (Options.DarwinFiles) {
    if (Profile != ProcessProfile::MacOSMachO64 &&
        Profile != ProcessProfile::IOSMachO64 &&
        Profile != ProcessProfile::IOSSimulatorMachO64)
      return diagnostic::error(process_report::DarwinFilesProfile);
    if (auto E = darwin_model::validateFileOptions(*Options.DarwinFiles))
      return std::move(E);
  }
  if (Options.LinuxKernel) {
    if (Profile != ProcessProfile::LinuxELF64 &&
        Profile != ProcessProfile::AndroidNativeAArch64)
      return diagnostic::error(process_report::LinuxKernelProfile);
    if (auto E = linux_model::validateKernelOptions(*Options.LinuxKernel))
      return std::move(E);
    if (auto E = linux_model::validateKernelTaskInputs(Options))
      return std::move(E);
  }
  if (Options.LinuxPriority) {
    if (Profile != ProcessProfile::LinuxELF64 &&
        Profile != ProcessProfile::AndroidNativeAArch64)
      return diagnostic::error(process_report::LinuxPriorityProfile);
    if (auto E = linux_model::validatePriorityOptions(*Options.LinuxPriority))
      return std::move(E);
  }
  if (Options.LinuxSignals) {
    if (Profile != ProcessProfile::LinuxELF64 &&
        Profile != ProcessProfile::AndroidNativeAArch64)
      return diagnostic::error(process_report::LinuxSignalsProfile);
    if (auto E = linux_model::validateSignalOptions(*Options.LinuxSignals))
      return std::move(E);
  }
  // An ignored observer would report an unobserved run as an observed one.
  if (Observer && Profile != ProcessProfile::WindowsPE64)
    return diagnostic::error(process_report::ObserverProfile);
  if (Options.LinuxFiles) {
    if (Profile != ProcessProfile::LinuxELF64 &&
        Profile != ProcessProfile::AndroidNativeAArch64)
      return diagnostic::error(process_report::LinuxFilesProfile);
    if (auto E = linux_model::validateFileOptions(*Options.LinuxFiles))
      return std::move(E);
  }
  if (Options.LinuxTime) {
    if (Profile != ProcessProfile::LinuxELF64 &&
        Profile != ProcessProfile::AndroidNativeAArch64)
      return diagnostic::error(process_report::LinuxTimeProfile);
    if (auto E = linux_model::validateTimeOptions(*Options.LinuxTime))
      return std::move(E);
    if (auto E = linux_model::validateCPUClockInputs(Options))
      return std::move(E);
  }
  switch (Profile) {
  case ProcessProfile::LinuxELF64:
    return linux_model::runProcess(Path, Options);
  case ProcessProfile::AndroidNativeAArch64:
    return android_model::runNative(Path, Options);
  case ProcessProfile::WindowsPE64:
    return windows_process::runProcess(Path, Options, Observer);
  case ProcessProfile::MacOSMachO64:
    return darwin_model::runProcess(Path, darwin_model::macOSProfile(),
                                    Options);
  case ProcessProfile::IOSMachO64:
    return darwin_model::runProcess(Path, darwin_model::iOSProfile(false),
                                    Options);
  case ProcessProfile::IOSSimulatorMachO64:
    return darwin_model::runProcess(Path, darwin_model::iOSProfile(true),
                                    Options);
  }
  return diagnostic::error(runtime::ProcessProfile);
}
} // namespace

llvm::Expected<ProcessResult> emulateProcess(const std::filesystem::path &Path,
                                             ProcessProfile Profile,
                                             const ProcessOptions &Options) {
  return runProfile(Path, Profile, Options, nullptr);
}

llvm::Expected<ProcessResult> observeProcess(const std::filesystem::path &Path,
                                             ProcessProfile Profile,
                                             const ProcessOptions &Options,
                                             ProcessObserver &Observer) {
  return runProfile(Path, Profile, Options, &Observer);
}
} // namespace neverd::emulation
