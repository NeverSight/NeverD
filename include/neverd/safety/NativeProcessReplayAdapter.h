//===- NativeProcessReplayAdapter.h - Native replay availability -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A fail-closed factory boundary for native process-replay-exec-v1 backends.
/// The current implementation intentionally returns no operations on any host:
/// macOS lacks several required public primitives, while the Linux backend is
/// not enabled until its trusted instrumentation ABI, persistent supervisor,
/// containment, limits, and runtime attestation can prove all capabilities.
///
/// Keeping this boundary concrete prevents callers from manufacturing a
/// partially capable operation table and makes native unavailability typed and
/// testable.  A future positive factory must return operations only when every
/// capability is available atomically; there is no partial execution mode.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_NATIVEPROCESSREPLAYADAPTER_H
#define NEVERD_SAFETY_NATIVEPROCESSREPLAYADAPTER_H

#include "neverd/safety/ProcessReplayExecutor.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::safety::process_replay {

inline constexpr std::string_view kNativeProcessReplayAdapter =
    "native-process-replay-v1";
inline constexpr uint32_t kNativeProcessReplayAdapterVersion = 1;

enum class NativeProcessReplayAvailabilityReasonV1 : uint16_t {
  None = 0,
  InvalidOptions = 1,
  ExecutionRequestNotReady = 2,
  UnsupportedHost = 3,
  BackendIncomplete = 4,
};
static_assert(
    static_cast<uint16_t>(NativeProcessReplayAvailabilityReasonV1::None) == 0 &&
    static_cast<uint16_t>(
        NativeProcessReplayAvailabilityReasonV1::BackendIncomplete) == 4);

const char *toString(NativeProcessReplayAvailabilityReasonV1 Reason);

/// Owned future-factory inputs.  ExecutablePath is only a locator used to open
/// and authenticate an object; a positive backend must never execute this
/// pathname directly after authentication.  v1 requires an absolute native
/// path without embedded NUL or dot/dot-dot components.
struct NativeProcessReplayAdapterOptionsV1 {
  uint32_t Version = kNativeProcessReplayAdapterVersion;
  std::string ExecutablePath;
};

struct NativeProcessReplayAvailabilityV1 {
  bool Available = false;
  NativeProcessReplayAvailabilityReasonV1 Reason =
      NativeProcessReplayAvailabilityReasonV1::BackendIncomplete;
  ProcessReplayValidationReason PlanReason =
      ProcessReplayValidationReason::None;
  ProcessReplayExecutionReason ExecutionReason =
      ProcessReplayExecutionReason::None;
  /// Always all-false while Available is false.  This is a pre-Begin host
  /// claim, not a partial feature inventory.
  ProcessReplayExecutorCapabilitiesV1 Capabilities;
  std::string Detail;
};

/// Pure availability query.  It validates the immutable execution request and
/// native-specific physical occurrence constraints, but performs no open,
/// process launch, callback, or other external mutation.
NativeProcessReplayAvailabilityV1 queryNativeProcessReplayAvailabilityV1(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits,
    const NativeProcessReplayAdapterOptionsV1 &Options);

/// Single-use future factory.  Plan, Limits, and Options are owned so every
/// callback can be bound to exactly the request that was preflighted.  While
/// availability is false this returns an Error and no callback table.
llvm::Expected<ProcessReplayExecutorOperationsV1>
createNativeProcessReplayOperationsV1(
    ProcessReplayPlanCandidateV1 Plan, ProcessReplayExecutionLimitsV1 Limits,
    NativeProcessReplayAdapterOptionsV1 Options);

} // namespace neverd::safety::process_replay

#endif // NEVERD_SAFETY_NATIVEPROCESSREPLAYADAPTER_H
