//===- NativeProcessReplayAdapter.cpp - Native replay availability -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/NativeProcessReplayAdapter.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Path.h"

#include <map>
#include <tuple>
#include <utility>

namespace neverd::safety::process_replay {
namespace {

NativeProcessReplayAvailabilityV1
unavailable(NativeProcessReplayAvailabilityReasonV1 Reason,
            llvm::StringRef Detail) {
  NativeProcessReplayAvailabilityV1 Result;
  Result.Reason = Reason;
  Result.Detail = Detail.str();
  return Result;
}

bool hasDotPathComponent(llvm::StringRef Path) {
  for (auto It = llvm::sys::path::begin(Path), End = llvm::sys::path::end(Path);
       It != End; ++It)
    if (*It == "." || *It == "..")
      return true;
  return false;
}

const ProcessReplayOccurrence *
eventOccurrence(const ProcessReplayEvent &Event) {
  if (const auto *Lookup =
          std::get_if<ProcessReplayEnvironmentLookupEvent>(&Event.Payload))
    return &Lookup->At;
  if (const auto *Read =
          std::get_if<ProcessReplayStdinReadEvent>(&Event.Payload))
    return &Read->At;
  return nullptr;
}

enum class PhysicalProbeRole : uint8_t {
  Target = 1,
  EnvironmentLookup = 2,
  StandardInputRead = 3,
};

using PhysicalProbeDescriptor =
    std::tuple<PhysicalProbeRole, uint64_t, uint64_t, uint32_t, uint32_t,
               uint32_t, uint32_t>;

PhysicalProbeDescriptor
physicalProbeDescriptor(PhysicalProbeRole Role,
                        const ProcessReplayOccurrence &Occurrence) {
  return {Role,
          *Occurrence.FuncEntry,
          *Occurrence.CallVA,
          *Occurrence.BlockId,
          *Occurrence.OpIdx,
          *Occurrence.OriginSeq,
          *Occurrence.CallSiteId};
}

NativeProcessReplayAvailabilityV1 executionRequestUnavailable(
    const ProcessReplayExecutionValidationV1 &Validation) {
  NativeProcessReplayAvailabilityV1 Result = unavailable(
      NativeProcessReplayAvailabilityReasonV1::ExecutionRequestNotReady,
      Validation.Detail);
  Result.PlanReason = Validation.PlanReason;
  Result.ExecutionReason = Validation.Reason;
  return Result;
}

} // namespace

const char *toString(NativeProcessReplayAvailabilityReasonV1 Reason) {
  switch (Reason) {
  case NativeProcessReplayAvailabilityReasonV1::None:
    return "none";
  case NativeProcessReplayAvailabilityReasonV1::InvalidOptions:
    return "invalid_options";
  case NativeProcessReplayAvailabilityReasonV1::ExecutionRequestNotReady:
    return "execution_request_not_ready";
  case NativeProcessReplayAvailabilityReasonV1::UnsupportedHost:
    return "unsupported_host";
  case NativeProcessReplayAvailabilityReasonV1::BackendIncomplete:
    return "backend_incomplete";
  }
  return "unknown";
}

NativeProcessReplayAvailabilityV1 queryNativeProcessReplayAvailabilityV1(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits,
    const NativeProcessReplayAdapterOptionsV1 &Options) {
  if (Options.Version != kNativeProcessReplayAdapterVersion)
    return unavailable(NativeProcessReplayAvailabilityReasonV1::InvalidOptions,
                       "native process replay options version is unsupported");
  const llvm::StringRef ExecutablePath(Options.ExecutablePath);
  if (ExecutablePath.empty() || ExecutablePath.contains('\0') ||
      !llvm::sys::path::is_absolute(ExecutablePath) ||
      hasDotPathComponent(ExecutablePath))
    return unavailable(
        NativeProcessReplayAvailabilityReasonV1::InvalidOptions,
        "native process replay executable locator must be an absolute path "
        "without embedded NUL or dot components");

  const ProcessReplayExecutionValidationV1 Validation =
      validateProcessReplayExecutionRequest(Plan, Limits);
  if (!Validation.ready())
    return executionRequestUnavailable(Validation);

  // A software breakpoint identifies one physical instruction address.  The
  // richer analysis labels cannot disambiguate different probe roles or
  // different static sites mapped to the same CallVA.  Repeated invocations of
  // one static input site are intentionally allowed: Invocation is dynamic
  // identity and the backend distinguishes it with its per-site hit counter.
  std::map<uint64_t, PhysicalProbeDescriptor> PhysicalCallSites;
  PhysicalCallSites.emplace(*Plan.TargetOccurrence.CallVA,
                            physicalProbeDescriptor(PhysicalProbeRole::Target,
                                                    Plan.TargetOccurrence));
  for (const ProcessReplayEvent &Event : Plan.Events) {
    const ProcessReplayOccurrence *At = eventOccurrence(Event);
    PhysicalProbeRole Role = PhysicalProbeRole::EnvironmentLookup;
    if (std::holds_alternative<ProcessReplayStdinReadEvent>(Event.Payload))
      Role = PhysicalProbeRole::StandardInputRead;
    if (!At || !At->CallVA) {
      ProcessReplayExecutionValidationV1 Collision;
      Collision.Reason =
          ProcessReplayExecutionReason::UnattestableOccurrenceMap;
      Collision.Detail =
          "native process replay probes do not have unique physical call "
          "addresses";
      return executionRequestUnavailable(Collision);
    }
    const PhysicalProbeDescriptor Descriptor =
        physicalProbeDescriptor(Role, *At);
    const auto [It, Inserted] =
        PhysicalCallSites.try_emplace(*At->CallVA, Descriptor);
    if (!Inserted && It->second != Descriptor) {
      ProcessReplayExecutionValidationV1 Collision;
      Collision.Reason =
          ProcessReplayExecutionReason::UnattestableOccurrenceMap;
      Collision.Detail =
          "native process replay probes do not have unique physical call "
          "addresses";
      return executionRequestUnavailable(Collision);
    }
  }

#if defined(__linux__)
  return unavailable(
      NativeProcessReplayAvailabilityReasonV1::BackendIncomplete,
      "Linux native process replay is unavailable until a trusted static-ELF "
      "instrumentation manifest, persistent supervisor, complete containment, "
      "and all runtime attestation/limit primitives are implemented");
#elif defined(__APPLE__)
  return unavailable(
      NativeProcessReplayAvailabilityReasonV1::UnsupportedHost,
      "macOS native process replay is unavailable because supported public "
      "primitives cannot provide held-object execution, an arbitrary-target "
      "pre-code sandbox, and race-free process-tree containment");
#else
  return unavailable(
      NativeProcessReplayAvailabilityReasonV1::UnsupportedHost,
      "native process replay is unavailable on this host platform");
#endif
}

llvm::Expected<ProcessReplayExecutorOperationsV1>
createNativeProcessReplayOperationsV1(
    ProcessReplayPlanCandidateV1 Plan, ProcessReplayExecutionLimitsV1 Limits,
    NativeProcessReplayAdapterOptionsV1 Options) {
  const NativeProcessReplayAvailabilityV1 Availability =
      queryNativeProcessReplayAvailabilityV1(Plan, Limits, Options);
  if (!Availability.Available)
    return llvm::createStringError(llvm::errc::function_not_supported,
                                   "native process replay unavailable (%s): %s",
                                   toString(Availability.Reason),
                                   Availability.Detail.c_str());
  return llvm::createStringError(
      llvm::errc::function_not_supported,
      "native process replay availability has no operations implementation");
}

} // namespace neverd::safety::process_replay
