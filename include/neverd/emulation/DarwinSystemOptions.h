//===- DarwinSystemOptions.h - Explicit system observations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H
#define NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
/// One fixed raw getrlimit observation, not a resource enforcement policy.
/// Both values are at most Darwin RLIM_INFINITY (INT64_MAX); Current <=
/// Maximum.
struct DarwinResourceLimit {
  uint64_t Current = 0;
  uint64_t Maximum = 0;
};
/// One fixed LP64 getrusage observation, never host/guest runtime accounting.
/// Microseconds must be below 1000000; seconds and counters preserve signed
/// 64-bit values. Counters use Darwin's raw units, without Linux conversions.
struct DarwinResourceUsage {
  static constexpr std::size_t CounterCount = 14;
  int64_t UserSeconds = 0;
  uint32_t UserMicroseconds = 0;
  int64_t SystemSeconds = 0;
  uint32_t SystemMicroseconds = 0;
  /// Indices 0..13: ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt,
  /// ru_majflt, ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv,
  /// ru_nsignals, ru_nvcsw, ru_nivcsw. Meanings are implementation-defined.
  std::array<int64_t, CounterCount> Counters{};
};
/// Fixed credential observations; alone these do not authorize permissions.
/// Supported IDs are 0..INT32_MAX. A supplied GroupAccessList has 1..16
/// entries, preserves order/duplicates and starts with EffectiveGID.
/// This is the in-credential list, not the SDK's extended resolver list.
/// Missing entries alone do not prove negative external group membership.
/// Omitted groups are unknown; constructing this record explicitly declares
/// its zero/root IDs. Omitting Credentials retains the profile's legacy IDs.
struct DarwinCredentials {
  uint32_t RealUID = 0;
  uint32_t EffectiveUID = 0;
  uint32_t RealGID = 0;
  uint32_t EffectiveGID = 0;
  std::optional<std::vector<uint32_t>> GroupAccessList;
  /// Original credential cr_gmuid, independent of scalar UID observations.
  /// Values 0..INT32_MAX or KAUTH_UID_NONE (4294967195) are supported.
  /// NONE makes an explicit complete group list authoritative for ordinary
  /// queries; an omitted list still cannot prove negative membership.
  /// Omission leaves the external membership context unknown.
  std::optional<uint32_t> GroupMembershipUID = std::nullopt;
};
/// Fixed system observations, independent of host hardware and OS identity.
/// Missing is unknown; an empty string is an explicit value. Strings contain
/// no NUL and at most 1023 bytes, except HostName (255 bytes). Page size comes
/// from the guest memory policy.
struct DarwinSystemOptions {
  std::optional<std::string> OSType;
  std::optional<std::string> OSRelease;
  std::optional<std::string> OSVersion;
  std::optional<std::string> KernelVersion;
  std::optional<std::string> Machine;
  std::optional<std::string> Model;
  std::optional<int32_t> OSRevision;
  /// Positive, at most INT32_MAX. This does not add thread scheduling.
  std::optional<uint32_t> CPUCount;
  /// Reported memory, independent of the emulation allocation budget.
  std::optional<uint64_t> MemorySize;
  /// Canonical resource keys 0..8. Missing is unknown, and zero is explicit.
  /// Supplies getrlimit observations and explicit NOFILE count admission for
  /// immediate poll. This never queries the host, changes execution budgets
  /// or enables setrlimit/signal delivery.
  std::map<uint32_t, DarwinResourceLimit> ResourceLimits;
  /// Independent fixed observations: one query never requires the other.
  /// Missing children remain unknown even when fork/wait are unsupported.
  /// No clock advancement, budget changes or guest performance is inferred.
  std::optional<DarwinResourceUsage> ResourceUsageSelf;
  std::optional<DarwinResourceUsage> ResourceUsageChildren;
  /// Scalar queries and new-file UID share this immutable observation.
  /// Absence retains UID/GID 1000 for observations; this fallback never
  /// supplies authorization knowledge. StaticOwnerQueries uses only explicitly
  /// supplied real/effective UID. Groups, credential mutation and privileged
  /// sysctl writes remain independent and are not enabled by these
  /// observations.
  std::optional<DarwinCredentials> Credentials;
  /// Nonnegative int observation (0..INT32_MAX) for kern.maxfilesperproc.
  /// With ResourceLimits[8].Current this supplies getdtablesize; neither
  /// observation enforces the descriptor budget or queries the host.
  std::optional<uint32_t> MaxFilesPerProcess;
  /// Caller-visible kern.hostname bytes, independent of host identity or
  /// mobile entitlements. Missing is unknown; empty is an explicit value.
  /// Does not authorize hostname writes, even with explicit root credentials.
  std::optional<std::string> HostName;
  /// Independent self-process observations. IDs are positive pid_t values
  /// (1..INT32_MAX); neither is inferred from PID, credentials or the host.
  std::optional<uint32_t> ProcessGroupID;
  std::optional<uint32_t> SessionID;
  /// Fixed issetugid/P_SUGID observation. Explicit false is known zero;
  /// absence is unknown. Does not change credentials or authorize privileges.
  std::optional<bool> ProcessTainted;
  /// Complete immutable session login buffer: exactly 255 raw bytes, including
  /// NULs and bytes after a terminator. Missing is unknown; all-zero is
  /// explicit. Not inferred from credentials, group/session IDs or the host.
  /// Supplies read-only getlogin, without login authorization or setlogin
  /// support.
  std::optional<std::vector<uint8_t>> LoginNameBytes;
  /// Fixed current-process nice observation (-20..20). Missing is unknown;
  /// explicit zero and -1 are known. No host lookup, scheduling, priority
  /// mutation or aggregate/peer observation is inferred.
  std::optional<int32_t> ProcessNice;
  /// Ordered exact getentropy observations, not a host RNG or cryptographic
  /// guarantee. At most 256 nonempty records of at most 256 bytes each.
  /// Missing is unknown; an empty queue is explicitly exhausted. Each run
  /// owns a fresh cursor; admitted success/whole EFAULT consumes one record.
  std::optional<std::vector<std::vector<uint8_t>>> EntropyReads;
  /// Fixed opaque current-thread observation for raw thread_selfid. Missing
  /// is unknown; every supplied uint64 value, including zero, is known. Does
  /// not infer host/PID/Mach identity, allocation or thread scheduling.
  std::optional<uint64_t> ThreadID;
  /// Independent raw Mach self-port names, not live rights or IPC authority.
  /// Missing is unknown; every uint32 pattern, including zero, is explicit.
  /// Raw trap returns sign-extend the name through the native int32 carrier.
  /// No host/PID/thread identity, reference allocation or lifetime is inferred.
  std::optional<uint32_t> ThreadSelfPort;
  std::optional<uint32_t> TaskSelfPort;
  std::optional<uint32_t> HostSelfPort;
};
} // namespace neverd::emulation
#endif
