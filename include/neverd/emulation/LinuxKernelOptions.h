//===- LinuxKernelOptions.h - Explicit Linux kernel contracts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXKERNELOPTIONS_H
#define NEVERD_EMULATION_LINUXKERNELOPTIONS_H
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>
namespace neverd::emulation {
enum class AndroidGKIKernel {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  Name,
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
};
enum class LinuxUnavailableSyscall {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count) Name,
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
};
/// A live task in the guest PID namespace, observed at workload start.
struct LinuxKernelTask {
  bool GroupLeader = true;
};
/// The observed import order for getxattr/lgetxattr/fgetxattr requests.
enum class LinuxXAttrQueryImport { TargetBeforeName, NameBeforeTarget };
/// Legacy Android VMA names borrow a guest pointer until proc reads the name.
enum class LinuxAnonymousVMANames { AndroidUserPointer };
/// Query-time states for an exact rounded mincore request, consumed once.
/// Each byte is zero or one; these are not initial RAM contents.
struct LinuxResidencyObservation {
  uint64_t Address = 0;
  std::vector<uint8_t> Pages;
};
/// Explicit Linux kernel contracts and observed absent kernel interfaces.
/// Unmodeled interfaces retain their unsupported execution boundary.
struct LinuxKernelOptions {
  std::set<LinuxUnavailableSyscall> UnavailableSyscalls;
  /// Explicit released Android common kernel contract. Android API levels
  /// alone do not select a kernel. Only implemented services use this value.
  std::optional<AndroidGKIKernel> GKI;
  /// Independent xattr query import order, mutually exclusive with GKI.
  /// It supplies no attribute values, namespace policy or other kernel rules.
  std::optional<LinuxXAttrQueryImport> XAttrQueryImport;
  /// Optional closed catalogue of additional live tasks. The running process
  /// is always a live group leader. An absent catalogue leaves foreign task
  /// lookup unsupported; an explicitly empty one observes no additional tasks.
  /// Independent of GKI: this supplies CPU-clock task identities, not pidfd
  /// release rules or observations for other services.
  std::optional<std::map<uint32_t, LinuxKernelTask>> Tasks;
  /// Independent, opt-in anonymous VMA naming contract. Neither an Android
  /// API level nor GKI selects this legacy user-pointer representation.
  std::optional<LinuxAnonymousVMANames> AnonymousVMANames;
  /// Missing or exhausted observations do not imply residency. Only live
  /// anonymous allocations made through the Linux memory owner are admitted.
  std::optional<std::vector<LinuxResidencyObservation>> ResidencyObservations;
};
} // namespace neverd::emulation
#endif
