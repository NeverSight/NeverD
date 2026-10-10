//===- Platform.cpp - Instruction set and guest system selection ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Platform.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::unpack {
using emulation::GuestArchitecture;

llvm::Expected<ArchitectureTraits>
architectureTraits(GuestArchitecture Architecture) {
#define NEVERD_UNPACK_ARCHITECTURE(Name, StackPointer, Window)                 \
  if (Architecture == GuestArchitecture::Name)                                 \
    return ArchitectureTraits{emulation::CPURegister::StackPointer, Window};
#include "Observation.def"
#undef NEVERD_UNPACK_ARCHITECTURE
  return failure(llvm::Twine(text::NoArchitecture) +
                 architectureName(Architecture));
}

llvm::Expected<emulation::ProcessProfile>
processProfile(const InputImage &Image) {
  if (Image.domain() != ExecutionDomain::User)
    return failure("a kernel image requires the driver execution profile");
#define NEVERD_UNPACK_PLATFORM(Format, Architecture, Profile)                  \
  if (Image.format() == FormatKind::Format &&                                  \
      Image.architecture() == GuestArchitecture::Architecture)                 \
    return emulation::ProcessProfile::Profile;
#include "Observation.def"
#undef NEVERD_UNPACK_PLATFORM
  return failure(llvm::Twine(text::NoPlatform) +
                 formatKindName(Image.format()) + text::PlatformSeparator +
                 architectureName(Image.architecture()));
}

llvm::Expected<ObservedExecution>
observeImage(const std::filesystem::path &Path, const InputImage &Image,
             const UnpackOptions &Options,
             emulation::ProcessObserver &Observer) {
  using namespace emulation;
  ObservedExecution Out;
  if (Image.domain() == ExecutionDomain::Kernel) {
#ifdef NEVERD_UNPACK_DRIVER_EXECUTION
    if (Image.format() != FormatKind::PE64 ||
        Image.architecture() != GuestArchitecture::X64)
      return failure("driver unpacking requires a Windows x64 native PE");
    const auto &P = Options.Process;
    if (!P.Arguments.empty() || !P.Environment.empty() || P.Android ||
        P.LinuxTime || P.LinuxFiles || P.LinuxSignals || P.LinuxPriority ||
        P.LinuxKernel || P.DarwinFiles || P.DarwinTime || P.DarwinSystem ||
        (P.Windows && (!P.Windows->Modules.empty() || P.Windows->PEBVersion)) ||
        P.StackSize != process_defaults::Stack ||
        P.OutputLimit != process_defaults::Output ||
        P.InstructionQuantum != defaults::Quantum)
      return failure("user-process inputs do not apply to driver unpacking");
    DriverOptions Driver = Options.Driver.value_or(DriverOptions{});
    Driver.Backend = P.Backend;
    Driver.Contract = P.Contract.value_or(ExecutionContract::CheckedX64);
    Driver.InstructionLimit = P.Limits.Instructions;
    Driver.MemoryLimit = P.MemoryLimit;
    Driver.EventLimit = P.Limits.Events;
    Driver.TimeoutMilliseconds = P.Limits.TimeoutMicroseconds / 1000 +
                                 (P.Limits.TimeoutMicroseconds % 1000 != 0);
    Driver.ReportBackendSelection = true;
    auto Run = observeDriver(Path, Driver, Observer);
    if (!Run)
      return Run.takeError();
    Out.Profile = profile::ReportProfile;
    Out.Stop = driverStopReasonName(Run->Stop);
    Out.Diagnostic = std::move(Run->Diagnostic);
    Out.BackendSelectionReason = std::move(Run->BackendSelectionReason);
    Out.Backend = Run->SelectedBackend;
    Out.PC = Run->PC;
    Out.Instructions = Run->Instructions;
    Out.Events = Run->Calls.size() + Run->Writes.size();
    return Out;
#else
    return failure("driver unpacking requires NEVERD_ENABLE_DRIVER_EMULATION");
#endif
  }
  if (Options.Driver)
    return failure("driver inputs require a native-subsystem kernel image");
  auto Profile = processProfile(Image);
  if (!Profile)
    return Profile.takeError();
  auto Run = observeProcess(Path, *Profile, Options.Process, Observer);
  if (!Run)
    return Run.takeError();
  Out.Profile = processProfileName(Run->Profile);
  Out.Stop = processStopReasonName(Run->Stop);
  Out.Diagnostic = std::move(Run->Diagnostic);
  Out.BackendSelectionReason = std::move(Run->BackendSelectionReason);
  Out.Backend = Run->SelectedBackend;
  Out.PC = Run->PC;
  Out.Instructions = Run->Instructions;
  Out.Events = Run->Events;
  Out.DirectServiceCalls = llvm::count_if(Run->NativeCalls, [](const auto &C) {
    return C.DirectServiceNumber.has_value();
  });
  return Out;
}
} // namespace neverd::unpack
