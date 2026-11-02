//===- LinuxKernelAvailability.cpp - Explicit kernel absence --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxKernelAvailability.h"

#include "neverd/emulation/Limits.h"

namespace neverd::emulation::linux_model {
std::optional<uint32_t> gkiPidFDFlags(AndroidGKIKernel Kernel) {
  switch (Kernel) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return Flags;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
std::optional<IOVectorImportKind> gkiIOVectorImport(AndroidGKIKernel Kernel) {
  switch (Kernel) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return IOVectorImportKind::Import;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
std::optional<uint32_t> gkiPidFDNonLeaderError(AndroidGKIKernel Kernel) {
  switch (Kernel) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return NonLeader;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
std::optional<LinuxXAttrQueryImport>
xattrQueryImport(const LinuxKernelOptions &Options) {
  if (Options.XAttrQueryImport) {
    if (Options.GKI)
      return std::nullopt;
    switch (*Options.XAttrQueryImport) {
    case LinuxXAttrQueryImport::TargetBeforeName:
    case LinuxXAttrQueryImport::NameBeforeTarget:
      return Options.XAttrQueryImport;
    }
    return std::nullopt;
  }
  if (!Options.GKI)
    return std::nullopt;
  switch (*Options.GKI) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return NameFirst ? LinuxXAttrQueryImport::NameBeforeTarget                 \
                     : LinuxXAttrQueryImport::TargetBeforeName;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
llvm::Error validateKernelOptions(const LinuxKernelOptions &Options) {
  using namespace neverd::limits::emulation;
  if (Options.ResidencyObservations) {
    if (Options.ResidencyObservations->size() > kMaxLinuxResidencyObservations)
      return failure(KernelOptions);
    uint64_t Total = 0;
    for (const auto &Observation : *Options.ResidencyObservations) {
      const uint64_t Size = Observation.Pages.size();
      if (!Size || Size > kMaxLinuxResidencyPages ||
          Observation.Address % ResidencyPageSize ||
          Observation.Address > UINT64_MAX - Size * ResidencyPageSize ||
          Size > kMaxLinuxResidencyObservationBytes - Total)
        return failure(KernelOptions);
      for (uint8_t State : Observation.Pages)
        if (State > 1)
          return failure(KernelOptions);
      Total += Size;
    }
  }
  if (Options.AnonymousVMANames &&
      *Options.AnonymousVMANames != LinuxAnonymousVMANames::AndroidUserPointer)
    return failure(KernelOptions);
  if (Options.XAttrQueryImport && !xattrQueryImport(Options))
    return failure(KernelOptions);
  if (Options.GKI &&
      (!gkiPidFDFlags(*Options.GKI) || Options.UnavailableSyscalls.contains(
                                           LinuxUnavailableSyscall::PidFDOpen)))
    return failure(KernelOptions);
  if (Options.Tasks) {
    if (Options.Tasks->size() > KernelTaskLimit)
      return failure(KernelOptions);
    for (const auto &[ID, Task] : *Options.Tasks)
      if (!ID || ID > INT32_MAX || (ID == ProcessID && !Task.GroupLeader))
        return failure(KernelOptions);
  }
  for (auto Call : Options.UnavailableSyscalls) {
    switch (Call) {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count)         \
  case LinuxUnavailableSyscall::Name:                                          \
    break;
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
    default:
      return failure(KernelOptions);
    }
  }
  return llvm::Error::success();
}
llvm::Error validateKernelTaskInputs(const ProcessOptions &Options) {
  if (!Options.LinuxKernel || !Options.LinuxKernel->Tasks)
    return llvm::Error::success();
  // Static foreign observations must not occupy identities the guest thread
  // owner can publish later. The running group leader remains authoritative.
  if (Options.Android && Options.Android->ThreadLimit > 1)
    for (const auto &[ID, Task] : *Options.LinuxKernel->Tasks)
      if (ID > ThreadID &&
          uint64_t(ID) - ThreadID < Options.Android->ThreadLimit)
        return failure(KernelOptions);
  if (Options.LinuxPriority)
    for (const auto &[ID, Nice] : Options.LinuxPriority->Tasks)
      if (ID != ProcessID && !Options.LinuxKernel->Tasks->contains(ID))
        return failure(KernelOptions);
  return llvm::Error::success();
}
bool unavailableKernelService(
    ServiceKind Kind, const std::optional<LinuxKernelOptions> &Options) {
  if (!Options)
    return false;
  switch (Kind) {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count)         \
  case ServiceKind::Name:                                                      \
    return Options->UnavailableSyscalls.contains(LinuxUnavailableSyscall::Name);
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
  default:
    return false;
  }
}
} // namespace neverd::emulation::linux_model
