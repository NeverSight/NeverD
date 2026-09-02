//===- ProcessReplayExecutor.cpp - Exact replay coordinator -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ProcessReplayExecutor.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace neverd::safety::process_replay {
namespace {

using StaticOccurrenceKey =
    std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>;

StaticOccurrenceKey staticKey(const ProcessReplayOccurrence &Occurrence) {
  return {*Occurrence.FuncEntry, *Occurrence.CallVA,    *Occurrence.BlockId,
          *Occurrence.OpIdx,     *Occurrence.OriginSeq, *Occurrence.CallSiteId};
}

bool hasCompleteOccurrence(const ProcessReplayOccurrence &Occurrence) {
  return Occurrence.FuncEntry && Occurrence.CallVA && Occurrence.BlockId &&
         Occurrence.OpIdx && Occurrence.OriginSeq && Occurrence.CallSiteId &&
         Occurrence.Invocation;
}

bool sameOccurrence(const ProcessReplayOccurrence &Left,
                    const ProcessReplayOccurrence &Right) {
  return Left.FuncEntry == Right.FuncEntry && Left.CallVA == Right.CallVA &&
         Left.BlockId == Right.BlockId && Left.OpIdx == Right.OpIdx &&
         Left.OriginSeq == Right.OriginSeq &&
         Left.CallSiteId == Right.CallSiteId &&
         Left.Invocation == Right.Invocation;
}

bool sameTargetIdentity(const ProcessReplayTargetIdentity &Left,
                        const ProcessReplayTargetIdentity &Right) {
  return Left.Format == Right.Format &&
         Left.Architecture == Right.Architecture && Left.Bits == Right.Bits &&
         Left.Mode == Right.Mode && Left.Endianness == Right.Endianness &&
         Left.Relocatable == Right.Relocatable && Left.Base == Right.Base &&
         Left.Entry == Right.Entry && Left.SHA256 == Right.SHA256;
}

bool satisfiesCapabilities(
    const ProcessReplayExecutorCapabilitiesV1 &Capabilities) {
  if (!Capabilities.NativeFormatAndArchitecture ||
      !Capabilities.NoTranslatedExecution ||
      !Capabilities.SelfContainedStaticExecutable ||
      !Capabilities.PinnedTargetObject ||
      !Capabilities.LoadedImageReauthentication ||
      !Capabilities.DirectArgumentVector ||
      !Capabilities.ExactEnvironmentBlock ||
      !Capabilities.PrivateWorkingDirectory ||
      !Capabilities.ExactDescriptorWhitelist ||
      !Capabilities.SandboxBeforeTargetCode ||
      !Capabilities.PrivilegeGainDenied || !Capabilities.NetworkDenied ||
      !Capabilities.HostFilesystemReadIsolated ||
      !Capabilities.FilesystemWritesDenied ||
      !Capabilities.ProcessTreeContained ||
      !Capabilities.ProcessTreeKillAndReap || !Capabilities.WallTimeLimit ||
      !Capabilities.CpuTimeLimit || !Capabilities.MemoryLimit ||
      !Capabilities.OutputLimit ||
      !Capabilities.EnvironmentLookupInterposition ||
      !Capabilities.StandardInputInterposition ||
      !Capabilities.UniquePreCallOccurrenceAttestation)
    return false;
  return true;
}

bool limitsWithinHardCeilings(const ProcessReplayExecutionLimitsV1 &Limits) {
  return Limits.WallTimeMs <= kProcessReplayExecutionMaxWallTimeMs &&
         Limits.CpuTimeUs <= kProcessReplayExecutionMaxCpuTimeUs &&
         Limits.AddressSpaceBytes <=
             kProcessReplayExecutionMaxAddressSpaceBytes &&
         Limits.ResidentMemoryBytes <=
             kProcessReplayExecutionMaxResidentMemoryBytes &&
         Limits.MaxProcesses == kProcessReplayExecutionMaxProcesses &&
         Limits.MaxTargetSiteVisits <=
             kProcessReplayExecutionMaxTargetSiteVisits &&
         Limits.MaxControlEvents <= kProcessReplayExecutionMaxControlEvents &&
         Limits.MaxProtocolBytes <= kProcessReplayExecutionMaxProtocolBytes &&
         Limits.MaxStdoutBytes <= kProcessReplayExecutionMaxStreamBytes &&
         Limits.MaxStderrBytes <= kProcessReplayExecutionMaxStreamBytes &&
         Limits.MaxAggregateOutputBytes <=
             kProcessReplayExecutionMaxAggregateOutputBytes &&
         Limits.PreviewBytesPerStream <=
             kProcessReplayExecutionMaxPreviewBytes &&
         Limits.MaxBackendChunkBytes <=
             kProcessReplayExecutionMaxBackendChunkBytes;
}

ProcessReplayExecutionValidationV1
executionFailure(ProcessReplayExecutionReason Reason, const char *Detail,
                 ProcessReplayValidationReason PlanReason =
                     ProcessReplayValidationReason::None) {
  return {PlanReason, Reason, Detail};
}

ProcessReplayExecutionValidationV1
validateOccurrenceMap(const ProcessReplayPlanCandidateV1 &Plan) {
  const StaticOccurrenceKey Target = staticKey(Plan.TargetOccurrence);
  std::map<StaticOccurrenceKey, ProcessReplayProbeKindV1> InputSites;
  for (const ProcessReplayEvent &Event : Plan.Events) {
    const ProcessReplayOccurrence *At = nullptr;
    ProcessReplayProbeKindV1 Kind = ProcessReplayProbeKindV1::EnvironmentLookup;
    if (const auto *Lookup =
            std::get_if<ProcessReplayEnvironmentLookupEvent>(&Event.Payload)) {
      At = &Lookup->At;
      Kind = ProcessReplayProbeKindV1::EnvironmentLookup;
    } else if (const auto *Read =
                   std::get_if<ProcessReplayStdinReadEvent>(&Event.Payload)) {
      At = &Read->At;
      Kind = ProcessReplayProbeKindV1::StandardInputRead;
    }
    if (!At)
      return executionFailure(
          ProcessReplayExecutionReason::UnattestableOccurrenceMap,
          "input event has no execution-attestable occurrence kind");

    const StaticOccurrenceKey Key = staticKey(*At);
    if (Key == Target)
      return executionFailure(
          ProcessReplayExecutionReason::UnattestableOccurrenceMap,
          "target and input events share one static occurrence");
    const auto [It, Inserted] = InputSites.try_emplace(Key, Kind);
    if (!Inserted && It->second != Kind)
      return executionFailure(
          ProcessReplayExecutionReason::UnattestableOccurrenceMap,
          "one static input occurrence has multiple interposition kinds");
  }
  return {};
}

bool hasAllOperations(const ProcessReplayExecutorOperationsV1 &Operations) {
  return Operations.Begin && Operations.AuthenticatePinnedTarget &&
         Operations.PrepareLaunch && Operations.LaunchStopped &&
         Operations.ObserveAppliedIsolation &&
         Operations.AuthenticateLoadedTarget && Operations.Resume &&
         Operations.NextEvent && Operations.ReplyAndResume &&
         Operations.TerminateTree && Operations.ReapTree &&
         Operations.DrainOutput && Operations.CleanupAfterReap &&
         Operations.RetainContainmentForUnreaped;
}

ProcessReplayLaunchRequestV1
makeLaunchRequest(const ProcessReplayPlanCandidateV1 &Plan,
                  const ProcessReplayExecutionLimitsV1 &Limits) {
  ProcessReplayLaunchRequestV1 Request;
  Request.Target = Plan.Target;
  Request.TargetOccurrence = Plan.TargetOccurrence;
  Request.Limits = Limits;
  Request.Arguments.reserve(Plan.Arguments.size());
  for (const ProcessReplayArgument &Argument : Plan.Arguments)
    Request.Arguments.push_back(Argument.Bytes);
  for (const ProcessReplayEnvironmentEntry &Entry : Plan.Environment)
    if (Entry.Present)
      Request.Environment.push_back({Entry.Name, Entry.Value});
  Request.InputProbes.reserve(Plan.Events.size());
  for (const ProcessReplayEvent &Event : Plan.Events) {
    if (const auto *Lookup =
            std::get_if<ProcessReplayEnvironmentLookupEvent>(&Event.Payload))
      Request.InputProbes.push_back(
          {ProcessReplayProbeKindV1::EnvironmentLookup, Lookup->At});
    else if (const auto *Read =
                 std::get_if<ProcessReplayStdinReadEvent>(&Event.Payload))
      Request.InputProbes.push_back(
          {ProcessReplayProbeKindV1::StandardInputRead, Read->At});
  }
  return Request;
}

struct OutputState {
  llvm::SHA256 Hash;
  uint64_t ObservedBytes = 0;
  uint64_t DigestedBytes = 0;
  std::vector<uint8_t> Preview;
  bool Truncated = false;
};

static_assert(
    std::is_nothrow_move_constructible_v<ProcessReplayExecutionResultV1>,
    "the no-throw execution boundary requires a no-throw result handoff");

bool checkedAdd(uint64_t &Value, uint64_t Amount) {
  if (Amount > std::numeric_limits<uint64_t>::max() - Value)
    return false;
  Value += Amount;
  return true;
}

bool checkedMultiply(uint64_t Left, uint64_t Right, uint64_t &Product) {
  if (Left != 0 && Right > std::numeric_limits<uint64_t>::max() / Left)
    return false;
  Product = Left * Right;
  return true;
}

ProcessReplayExecutionValidationV1
validateMinimumExecutionBudgets(const ProcessReplayPlanCandidateV1 &Plan,
                                const ProcessReplayExecutionLimitsV1 &Limits) {
  uint64_t TargetVisits = *Plan.TargetOccurrence.Invocation;
  if (!checkedAdd(TargetVisits, 1))
    return executionFailure(ProcessReplayExecutionReason::ArithmeticOverflow,
                            "target visit minimum overflowed");

  uint64_t MinimumControlEvents = static_cast<uint64_t>(Plan.Events.size());
  if (!checkedAdd(MinimumControlEvents, TargetVisits))
    return executionFailure(ProcessReplayExecutionReason::ArithmeticOverflow,
                            "minimum control-event count overflowed");
  if (MinimumControlEvents > Limits.MaxControlEvents)
    return executionFailure(
        ProcessReplayExecutionReason::InsufficientExecutionBudget,
        "control-event budget is below the exact transcript minimum");

  uint64_t MinimumProtocolBytes = 0;
  auto Charge = [&](uint64_t Amount) {
    return checkedAdd(MinimumProtocolBytes, Amount);
  };
  for (const ProcessReplayEvent &Event : Plan.Events) {
    if (const auto *Lookup =
            std::get_if<ProcessReplayEnvironmentLookupEvent>(&Event.Payload)) {
      const ProcessReplayEnvironmentEntry &Entry =
          Plan.Environment[Lookup->EnvironmentId];
      if (!Charge(kProcessReplayBackendEventProtocolBytes) ||
          !Charge(static_cast<uint64_t>(Entry.Name.size())) ||
          !Charge(kProcessReplayEnvironmentReplyProtocolBytes) ||
          !Charge(static_cast<uint64_t>(Entry.Value.size())))
        return executionFailure(
            ProcessReplayExecutionReason::ArithmeticOverflow,
            "minimum environment protocol bytes overflowed");
      continue;
    }

    const auto &Read = std::get<ProcessReplayStdinReadEvent>(Event.Payload);
    if (!Charge(kProcessReplayBackendEventProtocolBytes) ||
        !Charge(kProcessReplayStdinRequestProtocolBytes) ||
        !Charge(kProcessReplayStdinReplyProtocolBytes) ||
        !Charge(static_cast<uint64_t>(Read.Bytes.size())))
      return executionFailure(ProcessReplayExecutionReason::ArithmeticOverflow,
                              "minimum stdin protocol bytes overflowed");
  }

  uint64_t TargetProtocolBytes = 0;
  if (!checkedMultiply(kProcessReplayBackendEventProtocolBytes +
                           kProcessReplayTargetHitProtocolBytes,
                       TargetVisits, TargetProtocolBytes) ||
      !Charge(TargetProtocolBytes))
    return executionFailure(ProcessReplayExecutionReason::ArithmeticOverflow,
                            "minimum target protocol bytes overflowed");
  if (MinimumProtocolBytes > Limits.MaxProtocolBytes)
    return executionFailure(
        ProcessReplayExecutionReason::InsufficientExecutionBudget,
        "protocol-byte budget is below the exact transcript minimum");
  return {};
}

class Coordinator {
  class EmergencyFinalizer {
  public:
    explicit EmergencyFinalizer(Coordinator &Owner) noexcept : Owner(Owner) {}
    ~EmergencyFinalizer() noexcept { Owner.teardown(); }

  private:
    Coordinator &Owner;
  };

  static_assert(
      std::is_nothrow_constructible_v<EmergencyFinalizer, Coordinator &> &&
          std::is_nothrow_destructible_v<EmergencyFinalizer>,
      "the post-Begin emergency finalizer must be statically no-throw");

public:
  Coordinator(ProcessReplayPlanCandidateV1 Plan,
              ProcessReplayExecutionLimitsV1 Limits,
              const ProcessReplayExecutorOperationsV1 &Operations,
              const detail::ProcessReplayPreBeginHooksForTestingV1 *Hooks)
      : Plan(std::move(Plan)), Limits(Limits), Operations(Operations),
        Hooks(Hooks), Deadline(std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(Limits.WallTimeMs)),
        ResourceOffsets(this->Plan.Resources.size(), 0),
        ResourceEOF(this->Plan.Resources.size(), false) {
    Result.Receipt.ExpectedTargetSHA256 = this->Plan.Target.SHA256;
  }

  ProcessReplayExecutionResultV1 run() noexcept {
    FinalizationArmed = true;
    // This stack guard is the last-resort owner of Begin's finalization
    // obligation.  Normal returns consume the guards in finish(); any future
    // exceptional escape still enters the same no-throw, exactly-once state
    // machine before leaving run().
    [[maybe_unused]] EmergencyFinalizer Finalizer(*this);
    try {
      if (llvm::Error Error = Operations.Begin())
        return failWithError(ProcessReplayExecutionReason::BeginFailed,
                             std::move(Error));

      llvm::Expected<ProcessReplayTargetAuthenticationV1> Source =
          Operations.AuthenticatePinnedTarget();
      if (!Source)
        return failWithError(
            ProcessReplayExecutionReason::SourceIdentityAuthenticationFailed,
            Source.takeError());
      SourceAuthentication = std::move(*Source);
      if (ProcessReplayExecutionReason Reason =
              validateSourceAuthentication(*SourceAuthentication);
          Reason != ProcessReplayExecutionReason::None)
        return fail(Reason, authenticationDetail(Reason, false));

      const ProcessReplayLaunchRequestV1 Request =
          makeLaunchRequest(Plan, Limits);
      if (llvm::Error Error = Operations.PrepareLaunch(Request))
        return failWithError(ProcessReplayExecutionReason::PrepareLaunchFailed,
                             std::move(Error));

      // LaunchStopped may have created only part of the target tree when it
      // reports failure, so tree teardown is armed before invoking it.
      LaunchArmed = true;
      if (llvm::Error Error = Operations.LaunchStopped())
        return failWithError(ProcessReplayExecutionReason::LaunchFailed,
                             std::move(Error));

      llvm::Expected<ProcessReplayExecutorCapabilitiesV1> Applied =
          Operations.ObserveAppliedIsolation();
      if (!Applied)
        return failWithError(
            ProcessReplayExecutionReason::IsolationObservationFailed,
            Applied.takeError());
      Result.Receipt.AppliedCapabilities = *Applied;
      if (!satisfiesCapabilities(*Applied))
        return fail(ProcessReplayExecutionReason::IsolationReceiptIncomplete,
                    "applied execution isolation is incomplete");

      llvm::Expected<ProcessReplayTargetAuthenticationV1> Loaded =
          Operations.AuthenticateLoadedTarget();
      if (!Loaded)
        return failWithError(
            ProcessReplayExecutionReason::LoadedIdentityAuthenticationFailed,
            Loaded.takeError());
      if (ProcessReplayExecutionReason Reason =
              validateLoadedAuthentication(*Loaded);
          Reason != ProcessReplayExecutionReason::None)
        return fail(Reason, authenticationDetail(Reason, true));

      if (llvm::Error Error = Operations.Resume())
        return failWithError(ProcessReplayExecutionReason::ResumeFailed,
                             std::move(Error));

      while (!Result.Receipt.TargetAttested) {
        llvm::Expected<ProcessReplayBackendEventV1> Event =
            Operations.NextEvent(Deadline);
        if (!Event)
          return failWithError(ProcessReplayExecutionReason::BackendEventFailed,
                               Event.takeError());
        if (ProcessReplayExecutionReason Reason = chargeEvent(*Event);
            Reason != ProcessReplayExecutionReason::None)
          return fail(Reason, eventBudgetDetail(Reason));
        if (ProcessReplayExecutionReason Reason = processEvent(*Event);
            Reason != ProcessReplayExecutionReason::None)
          return fail(Reason, eventDetail(Reason));
      }

      Result.Receipt.Termination.Kind =
          ProcessReplayTerminationKindV1::StoppedBeforeTargetCall;
      return finish();
    } catch (const std::exception &Exception) {
      return failWithException(ProcessReplayExecutionReason::CallbackException,
                               "execution callback threw: ", Exception.what());
    } catch (...) {
      return fail(ProcessReplayExecutionReason::CallbackException,
                  "execution callback threw a non-standard exception");
    }
  }

private:
  ProcessReplayExecutionResultV1 fail(ProcessReplayExecutionReason Reason,
                                      const char *Detail) noexcept {
    if (setPrimaryFailure(Reason))
      writeDiagnostic(Result.Detail, Detail);
    return finish();
  }

  ProcessReplayExecutionResultV1
  failWithError(ProcessReplayExecutionReason Reason,
                llvm::Error Error) noexcept {
    recordPrimaryError(Reason, std::move(Error));
    return finish();
  }

  void recordPrimaryError(ProcessReplayExecutionReason Reason,
                          llvm::Error Error) noexcept {
    const bool FirstFailure = setPrimaryFailure(Reason);
    std::string Detail = takeErrorDetailNoThrow(std::move(Error));
    if (FirstFailure)
      writeDiagnostic(Result.Detail, Detail);
  }

  ProcessReplayExecutionResultV1
  failWithException(ProcessReplayExecutionReason Reason, const char *Prefix,
                    const char *What) noexcept {
    if (setPrimaryFailure(Reason))
      writeDiagnostic(Result.Detail, Prefix, What);
    return finish();
  }

  bool setPrimaryFailure(ProcessReplayExecutionReason Reason) noexcept {
    if (Result.Reason != ProcessReplayExecutionReason::None)
      return false;
    // Set all typed, allocation-free state before attempting diagnostics.
    Result.Reason = Reason;
    if (LaunchArmed &&
        Result.Receipt.Termination.Kind == ProcessReplayTerminationKindV1::None)
      Result.Receipt.Termination.Kind = reasonTerminationKind(Reason);
    return true;
  }

  std::string takeErrorDetailNoThrow(llvm::Error Error) noexcept {
    try {
      if (Hooks && Hooks->BeforeErrorDetailMaterialization)
        Hooks->BeforeErrorDetailMaterialization();
      return llvm::toString(std::move(Error));
    } catch (...) {
      // Error's move constructor leaves the source checked.  This consumes the
      // still-owned payload when the test hook threw before the move, and is a
      // harmless checked success when llvm::toString had already consumed it.
      llvm::consumeError(std::move(Error));
      return {};
    }
  }

  void writeDiagnostic(std::string &Destination, std::string_view First,
                       std::string_view Second = {}) noexcept {
    try {
      if (Hooks && Hooks->BeforeDiagnosticWrite)
        Hooks->BeforeDiagnosticWrite();
      if (First.empty())
        Destination.clear();
      else
        Destination.assign(First.data(), First.size());
      if (!Second.empty())
        Destination.append(Second.data(), Second.size());
    } catch (...) {
      // Diagnostics are explicitly non-attested and best effort.  The typed
      // reason was committed by the caller before this allocation boundary.
    }
  }

  void appendDiagnostic(std::string &Destination, std::string_view First,
                        std::string_view Second = {}) noexcept {
    try {
      if (Hooks && Hooks->BeforeDiagnosticWrite)
        Hooks->BeforeDiagnosticWrite();
      if (!Destination.empty())
        Destination.append("; ");
      if (!First.empty())
        Destination.append(First.data(), First.size());
      if (!Second.empty())
        Destination.append(Second.data(), Second.size());
    } catch (...) {
      // Preserve the first typed reason even if supplementary text cannot be
      // allocated.
    }
  }

  ProcessReplayExecutionResultV1 finish() noexcept {
    teardown();
    finalizeOutput(StandardOutput, Result.Receipt.StandardOutput);
    finalizeOutput(StandardError, Result.Receipt.StandardError);
    Result.Receipt.TargetCallExecuted = false;
    Result.Receipt.Complete =
        Result.Reason == ProcessReplayExecutionReason::None &&
        Result.TeardownReason == ProcessReplayExecutionReason::None &&
        Result.ContainmentReason == ProcessReplayExecutionReason::None &&
        Result.Receipt.TargetAttested &&
        Result.Receipt.Termination.Kind ==
            ProcessReplayTerminationKindV1::StoppedBeforeTargetCall &&
        Result.Receipt.Termination.TreeFullyReaped && DrainComplete;
    return std::move(Result);
  }

  ProcessReplayExecutionReason validateSourceAuthentication(
      const ProcessReplayTargetAuthenticationV1 &Authentication) {
    Result.Receipt.SourceTargetSHA256 = Authentication.Identity.SHA256;
    if (!Authentication.RegularFile || !Authentication.Executable)
      return ProcessReplayExecutionReason::TargetNotRegularExecutable;
    if (Authentication.PrivilegeBearing)
      return ProcessReplayExecutionReason::PrivilegeBearingTarget;
    if (!Authentication.NativeFormatAndArchitecture)
      return ProcessReplayExecutionReason::UnsupportedHostTarget;
    if (Authentication.TranslatedExecution)
      return ProcessReplayExecutionReason::TranslatedExecution;
    if (!Authentication.SelfContainedStaticExecutable)
      return ProcessReplayExecutionReason::DynamicLoaderUnsupported;
    if (!Authentication.ByteSize || *Authentication.ByteSize == 0 ||
        !Authentication.ObjectIdentitySHA256 ||
        !sameTargetIdentity(Authentication.Identity, Plan.Target))
      return ProcessReplayExecutionReason::SourceIdentityMismatch;
    Result.Receipt.SourceIdentityMatched = true;
    return ProcessReplayExecutionReason::None;
  }

  ProcessReplayExecutionReason validateLoadedAuthentication(
      const ProcessReplayTargetAuthenticationV1 &Authentication) {
    Result.Receipt.LoadedTargetSHA256 = Authentication.Identity.SHA256;
    if (!Authentication.SelfContainedStaticExecutable)
      return ProcessReplayExecutionReason::DynamicLoaderUnsupported;
    if (!Authentication.RegularFile || !Authentication.Executable ||
        Authentication.PrivilegeBearing ||
        !Authentication.NativeFormatAndArchitecture ||
        Authentication.TranslatedExecution || !Authentication.ByteSize ||
        !Authentication.ObjectIdentitySHA256 || !SourceAuthentication ||
        !sameTargetIdentity(Authentication.Identity, Plan.Target) ||
        Authentication.ByteSize != SourceAuthentication->ByteSize ||
        Authentication.ObjectIdentitySHA256 !=
            SourceAuthentication->ObjectIdentitySHA256)
      return ProcessReplayExecutionReason::LoadedIdentityMismatch;
    Result.Receipt.LoadedIdentityMatched = true;
    Result.Receipt.StableObjectIdentityMatched = true;
    return ProcessReplayExecutionReason::None;
  }

  static const char *authenticationDetail(ProcessReplayExecutionReason Reason,
                                          bool Loaded) {
    switch (Reason) {
    case ProcessReplayExecutionReason::TargetNotRegularExecutable:
      return "pinned target is not a regular executable file";
    case ProcessReplayExecutionReason::PrivilegeBearingTarget:
      return "pinned target may gain privileges";
    case ProcessReplayExecutionReason::UnsupportedHostTarget:
      return "target format or architecture is not native to the host";
    case ProcessReplayExecutionReason::TranslatedExecution:
      return "translated or emulated execution cannot attest v1 occurrences";
    case ProcessReplayExecutionReason::DynamicLoaderUnsupported:
      return "v1 requires a self-contained static executable";
    case ProcessReplayExecutionReason::SourceIdentityMismatch:
      return "pinned target identity does not match the replay plan";
    case ProcessReplayExecutionReason::LoadedIdentityMismatch:
      return Loaded ? "loaded target identity does not match the pinned object"
                    : "target identity does not match the replay plan";
    default:
      return "target authentication failed";
    }
  }

  ProcessReplayExecutionReason
  chargeEvent(const ProcessReplayBackendEventV1 &Event) {
    // A backend that reports its own output-limit enforcement may already have
    // securely discarded bytes.  Preserve that fact before any accounting
    // rejection prevents the event from reaching processTermination().
    if (const auto *Termination =
            std::get_if<ProcessReplayTerminationEventV1>(&Event.Payload);
        Termination && Termination->Kind ==
                           ProcessReplayTerminationKindV1::OutputLimitExceeded)
      OutputCaptureTruncated = true;

    if (!checkedAdd(ControlEvents, 1)) {
      markRejectedOutput(Event);
      return ProcessReplayExecutionReason::ArithmeticOverflow;
    }
    if (ControlEvents > Limits.MaxControlEvents) {
      markRejectedOutput(Event);
      return ProcessReplayExecutionReason::ControlEventLimitExceeded;
    }

    uint64_t Bytes = kProcessReplayBackendEventProtocolBytes;
    if (const auto *Lookup =
            std::get_if<ProcessReplayEnvironmentLookupRequestV1>(
                &Event.Payload)) {
      if (!checkedAdd(Bytes, static_cast<uint64_t>(Lookup->Name.size())))
        return ProcessReplayExecutionReason::ArithmeticOverflow;
    } else if (const auto *Output =
                   std::get_if<ProcessReplayOutputChunkV1>(&Event.Payload)) {
      if (Output->Bytes.size() > Limits.MaxBackendChunkBytes) {
        markRejectedOutput(Event);
        return ProcessReplayExecutionReason::BackendProtocolViolation;
      }
      if (!checkedAdd(Bytes, static_cast<uint64_t>(Output->Bytes.size()))) {
        markRejectedOutput(Event);
        return ProcessReplayExecutionReason::ArithmeticOverflow;
      }
    } else if (std::holds_alternative<ProcessReplayStdinReadRequestV1>(
                   Event.Payload)) {
      if (!checkedAdd(Bytes, kProcessReplayStdinRequestProtocolBytes))
        return ProcessReplayExecutionReason::ArithmeticOverflow;
    } else if (std::holds_alternative<ProcessReplayTargetSiteHitV1>(
                   Event.Payload)) {
      if (!checkedAdd(Bytes, kProcessReplayTargetHitProtocolBytes))
        return ProcessReplayExecutionReason::ArithmeticOverflow;
    } else if (std::holds_alternative<ProcessReplayTerminationEventV1>(
                   Event.Payload)) {
      if (!checkedAdd(Bytes, kProcessReplayTerminationProtocolBytes))
        return ProcessReplayExecutionReason::ArithmeticOverflow;
    }

    if (!checkedAdd(ProtocolBytes, Bytes)) {
      markRejectedOutput(Event);
      return ProcessReplayExecutionReason::ArithmeticOverflow;
    }
    if (ProtocolBytes > Limits.MaxProtocolBytes) {
      markRejectedOutput(Event);
      return ProcessReplayExecutionReason::ProtocolByteLimitExceeded;
    }
    return ProcessReplayExecutionReason::None;
  }

  void markRejectedOutput(const ProcessReplayBackendEventV1 &Event) {
    if (std::holds_alternative<ProcessReplayOutputChunkV1>(Event.Payload))
      OutputCaptureTruncated = true;
  }

  ProcessReplayExecutionReason
  processEvent(const ProcessReplayBackendEventV1 &Event) {
    if (Event.Payload.valueless_by_exception() ||
        std::holds_alternative<std::monostate>(Event.Payload))
      return ProcessReplayExecutionReason::BackendProtocolViolation;
    if (const auto *Lookup =
            std::get_if<ProcessReplayEnvironmentLookupRequestV1>(
                &Event.Payload))
      return processEnvironmentLookup(*Lookup);
    if (const auto *Read =
            std::get_if<ProcessReplayStdinReadRequestV1>(&Event.Payload))
      return processStdinRead(*Read);
    if (const auto *Hit =
            std::get_if<ProcessReplayTargetSiteHitV1>(&Event.Payload))
      return processTargetHit(*Hit);
    if (const auto *Output =
            std::get_if<ProcessReplayOutputChunkV1>(&Event.Payload))
      return processOutput(*Output);
    if (const auto *Termination =
            std::get_if<ProcessReplayTerminationEventV1>(&Event.Payload))
      return processTermination(*Termination);
    return ProcessReplayExecutionReason::BackendProtocolViolation;
  }

  ProcessReplayExecutionReason processEnvironmentLookup(
      const ProcessReplayEnvironmentLookupRequestV1 &Request) {
    if (NextInputEvent >= Plan.Events.size())
      return ProcessReplayExecutionReason::UnexpectedInputEvent;
    const auto *Expected = std::get_if<ProcessReplayEnvironmentLookupEvent>(
        &Plan.Events[NextInputEvent].Payload);
    if (!Expected)
      return ProcessReplayExecutionReason::UnexpectedInputEvent;
    if (!sameOccurrence(Request.At, Expected->At))
      return ProcessReplayExecutionReason::EventOccurrenceMismatch;
    const ProcessReplayEnvironmentEntry &Entry =
        Plan.Environment[Expected->EnvironmentId];
    if (Request.Name != Entry.Name)
      return ProcessReplayExecutionReason::EnvironmentNameMismatch;

    ProcessReplayInputReplyV1 Reply;
    Reply.EventId = Plan.Events[NextInputEvent].Id;
    Reply.Payload =
        ProcessReplayEnvironmentLookupReplyV1{Entry.Present, Entry.Value};
    if (ProcessReplayExecutionReason Reason = chargeReply(Reply);
        Reason != ProcessReplayExecutionReason::None)
      return Reason;
    if (llvm::Error Error = Operations.ReplyAndResume(Reply)) {
      recordPrimaryError(ProcessReplayExecutionReason::InputResponseFailed,
                         std::move(Error));
      return ProcessReplayExecutionReason::InputResponseFailed;
    }
    ++NextInputEvent;
    Result.Receipt.MatchedInputEventCount =
        static_cast<uint32_t>(NextInputEvent);
    return ProcessReplayExecutionReason::None;
  }

  ProcessReplayExecutionReason
  processStdinRead(const ProcessReplayStdinReadRequestV1 &Request) {
    if (NextInputEvent >= Plan.Events.size())
      return ProcessReplayExecutionReason::UnexpectedInputEvent;
    const auto *Expected = std::get_if<ProcessReplayStdinReadEvent>(
        &Plan.Events[NextInputEvent].Payload);
    if (!Expected)
      return ProcessReplayExecutionReason::UnexpectedInputEvent;
    if (!sameOccurrence(Request.At, Expected->At))
      return ProcessReplayExecutionReason::EventOccurrenceMismatch;
    if (Request.Descriptor != 0 || Request.ResourceId != Expected->ResourceId ||
        Request.ResourceId >= Plan.Resources.size() ||
        Plan.Resources[Request.ResourceId].Kind !=
            ProcessReplayResourceKind::StandardInput)
      return ProcessReplayExecutionReason::StdinResourceMismatch;
    if (Request.RequestedBytes != Expected->RequestedBytes)
      return ProcessReplayExecutionReason::StdinRequestMismatch;
    if (Expected->Offset != ResourceOffsets[Request.ResourceId] ||
        (ResourceEOF[Request.ResourceId] &&
         Expected->Outcome == ProcessReplayReadOutcome::Data))
      return ProcessReplayExecutionReason::BackendProtocolViolation;

    ProcessReplayInputReplyV1 Reply;
    Reply.EventId = Plan.Events[NextInputEvent].Id;
    Reply.Payload = ProcessReplayStdinReadReplyV1{
        Expected->Outcome, Expected->ReturnedBytes, Expected->Bytes,
        Expected->EOFAfter};
    if (ProcessReplayExecutionReason Reason = chargeReply(Reply);
        Reason != ProcessReplayExecutionReason::None)
      return Reason;
    if (llvm::Error Error = Operations.ReplyAndResume(Reply)) {
      recordPrimaryError(ProcessReplayExecutionReason::InputResponseFailed,
                         std::move(Error));
      return ProcessReplayExecutionReason::InputResponseFailed;
    }
    ResourceOffsets[Request.ResourceId] += Expected->ReturnedBytes;
    ResourceEOF[Request.ResourceId] = Expected->EOFAfter;
    ++NextInputEvent;
    Result.Receipt.MatchedInputEventCount =
        static_cast<uint32_t>(NextInputEvent);
    return ProcessReplayExecutionReason::None;
  }

  ProcessReplayExecutionReason
  chargeReply(const ProcessReplayInputReplyV1 &Reply) {
    uint64_t Bytes = 0;
    if (const auto *Environment =
            std::get_if<ProcessReplayEnvironmentLookupReplyV1>(
                &Reply.Payload)) {
      Bytes = kProcessReplayEnvironmentReplyProtocolBytes;
      if (!checkedAdd(Bytes, static_cast<uint64_t>(Environment->Value.size())))
        return ProcessReplayExecutionReason::ArithmeticOverflow;
    } else if (const auto *Read =
                   std::get_if<ProcessReplayStdinReadReplyV1>(&Reply.Payload)) {
      Bytes = kProcessReplayStdinReplyProtocolBytes;
      if (!checkedAdd(Bytes, static_cast<uint64_t>(Read->Bytes.size())))
        return ProcessReplayExecutionReason::ArithmeticOverflow;
    } else {
      return ProcessReplayExecutionReason::BackendProtocolViolation;
    }
    if (!checkedAdd(ProtocolBytes, Bytes))
      return ProcessReplayExecutionReason::ArithmeticOverflow;
    if (ProtocolBytes > Limits.MaxProtocolBytes)
      return ProcessReplayExecutionReason::ProtocolByteLimitExceeded;
    return ProcessReplayExecutionReason::None;
  }

  ProcessReplayExecutionReason
  processTargetHit(const ProcessReplayTargetSiteHitV1 &Hit) {
    if (!Hit.BeforeCall)
      return ProcessReplayExecutionReason::TargetNotPreCall;
    if (!hasCompleteOccurrence(Hit.At) ||
        staticKey(Hit.At) != staticKey(Plan.TargetOccurrence))
      return ProcessReplayExecutionReason::TargetOccurrenceMismatch;
    if (!checkedAdd(Result.Receipt.TargetSiteVisitsObserved, 1))
      return ProcessReplayExecutionReason::ArithmeticOverflow;
    if (Result.Receipt.TargetSiteVisitsObserved > Limits.MaxTargetSiteVisits)
      return ProcessReplayExecutionReason::TargetVisitLimitExceeded;

    const uint64_t ObservedInvocation =
        Result.Receipt.TargetSiteVisitsObserved - 1;
    if (*Hit.At.Invocation != ObservedInvocation)
      return ProcessReplayExecutionReason::TargetOccurrenceMismatch;
    if (*Hit.At.Invocation < *Plan.TargetOccurrence.Invocation) {
      if (llvm::Error Error = Operations.Resume()) {
        recordPrimaryError(ProcessReplayExecutionReason::ResumeFailed,
                           std::move(Error));
        return ProcessReplayExecutionReason::ResumeFailed;
      }
      return ProcessReplayExecutionReason::None;
    }
    if (*Hit.At.Invocation != *Plan.TargetOccurrence.Invocation)
      return ProcessReplayExecutionReason::TargetOccurrenceMismatch;
    if (NextInputEvent != Plan.Events.size())
      return ProcessReplayExecutionReason::TargetBeforeTranscriptComplete;
    if (!sameOccurrence(Hit.At, Plan.TargetOccurrence))
      return ProcessReplayExecutionReason::TargetOccurrenceMismatch;
    Result.Receipt.TargetAttested = true;
    return ProcessReplayExecutionReason::None;
  }

  ProcessReplayExecutionReason
  processOutput(const ProcessReplayOutputChunkV1 &Chunk) {
    auto Reject = [this](ProcessReplayExecutionReason Reason) {
      OutputCaptureTruncated = true;
      return Reason;
    };
    OutputState *State = nullptr;
    uint64_t StreamLimit = 0;
    switch (Chunk.Stream) {
    case ProcessReplayOutputStreamV1::StandardOutput:
      State = &StandardOutput;
      StreamLimit = Limits.MaxStdoutBytes;
      break;
    case ProcessReplayOutputStreamV1::StandardError:
      State = &StandardError;
      StreamLimit = Limits.MaxStderrBytes;
      break;
    default:
      return Reject(ProcessReplayExecutionReason::BackendProtocolViolation);
    }

    const uint64_t Amount = static_cast<uint64_t>(Chunk.Bytes.size());
    if (!checkedAdd(State->ObservedBytes, Amount) ||
        !checkedAdd(AggregateObservedBytes, Amount))
      return Reject(ProcessReplayExecutionReason::ArithmeticOverflow);

    const uint64_t StreamAvailable = State->DigestedBytes < StreamLimit
                                         ? StreamLimit - State->DigestedBytes
                                         : 0;
    const uint64_t AggregateAvailable =
        AggregateDigestedBytes < Limits.MaxAggregateOutputBytes
            ? Limits.MaxAggregateOutputBytes - AggregateDigestedBytes
            : 0;
    const uint64_t Accepted = std::min<uint64_t>(
        Amount, std::min(StreamAvailable, AggregateAvailable));
    const llvm::ArrayRef<uint8_t> AcceptedBytes(Chunk.Bytes.data(),
                                                static_cast<size_t>(Accepted));
    State->Hash.update(AcceptedBytes);
    State->DigestedBytes += Accepted;
    AggregateDigestedBytes += Accepted;

    const uint64_t PreviewAvailable =
        State->Preview.size() < Limits.PreviewBytesPerStream
            ? Limits.PreviewBytesPerStream - State->Preview.size()
            : 0;
    const size_t PreviewCount =
        static_cast<size_t>(std::min<uint64_t>(Accepted, PreviewAvailable));
    State->Preview.insert(State->Preview.end(), AcceptedBytes.begin(),
                          AcceptedBytes.begin() + PreviewCount);

    if (Accepted != Amount || State->ObservedBytes > StreamLimit ||
        AggregateObservedBytes > Limits.MaxAggregateOutputBytes) {
      State->Truncated = true;
      return Reject(ProcessReplayExecutionReason::OutputLimitExceeded);
    }
    return ProcessReplayExecutionReason::None;
  }

  ProcessReplayExecutionReason
  processTermination(const ProcessReplayTerminationEventV1 &Termination) {
    if (Termination.Kind == ProcessReplayTerminationKindV1::OutputLimitExceeded)
      // Commit the conservative output scope before validating the rest of the
      // event.  Malformed auxiliary fields do not make discarded bytes whole.
      OutputCaptureTruncated = true;
    if (Termination.Kind == ProcessReplayTerminationKindV1::None ||
        Termination.Kind ==
            ProcessReplayTerminationKindV1::StoppedBeforeTargetCall)
      return ProcessReplayExecutionReason::BackendProtocolViolation;
    if ((Termination.Kind == ProcessReplayTerminationKindV1::Exited) !=
            Termination.ExitCode.has_value() ||
        (Termination.Kind == ProcessReplayTerminationKindV1::Signaled) !=
            Termination.Signal.has_value() ||
        (Termination.Kind != ProcessReplayTerminationKindV1::Signaled &&
         Termination.CoreDumped))
      return ProcessReplayExecutionReason::BackendProtocolViolation;

    Result.Receipt.Termination.Kind = Termination.Kind;
    Result.Receipt.Termination.ExitCode = Termination.ExitCode;
    Result.Receipt.Termination.Signal = Termination.Signal;
    Result.Receipt.Termination.CoreDumped = Termination.CoreDumped;
    Result.Receipt.Termination.WallTimeMs = Termination.WallTimeMs;
    Result.Receipt.Termination.CpuTimeUs = Termination.CpuTimeUs;
    Result.Receipt.Termination.PeakMemoryBytes = Termination.PeakMemoryBytes;
    Result.Receipt.Termination.PeakProcessCount = Termination.PeakProcessCount;

    if (Termination.WallTimeMs && *Termination.WallTimeMs > Limits.WallTimeMs)
      return ProcessReplayExecutionReason::WallTimeExceeded;
    if (Termination.CpuTimeUs && *Termination.CpuTimeUs > Limits.CpuTimeUs)
      return ProcessReplayExecutionReason::CpuTimeExceeded;
    if (Termination.PeakMemoryBytes &&
        (*Termination.PeakMemoryBytes > Limits.ResidentMemoryBytes ||
         *Termination.PeakMemoryBytes > Limits.AddressSpaceBytes))
      return ProcessReplayExecutionReason::MemoryLimitExceeded;
    if (Termination.PeakProcessCount &&
        *Termination.PeakProcessCount > Limits.MaxProcesses)
      return ProcessReplayExecutionReason::ProcessLimitExceeded;

    switch (Termination.Kind) {
    case ProcessReplayTerminationKindV1::Exited:
      return ProcessReplayExecutionReason::ExitedBeforeTarget;
    case ProcessReplayTerminationKindV1::Signaled:
      return ProcessReplayExecutionReason::SignaledBeforeTarget;
    case ProcessReplayTerminationKindV1::WallTimeExceeded:
      return ProcessReplayExecutionReason::WallTimeExceeded;
    case ProcessReplayTerminationKindV1::CpuTimeExceeded:
      return ProcessReplayExecutionReason::CpuTimeExceeded;
    case ProcessReplayTerminationKindV1::MemoryLimitExceeded:
      return ProcessReplayExecutionReason::MemoryLimitExceeded;
    case ProcessReplayTerminationKindV1::ProcessLimitExceeded:
      return ProcessReplayExecutionReason::ProcessLimitExceeded;
    case ProcessReplayTerminationKindV1::OutputLimitExceeded:
      return ProcessReplayExecutionReason::OutputLimitExceeded;
    case ProcessReplayTerminationKindV1::SandboxViolation:
      return ProcessReplayExecutionReason::SandboxViolation;
    case ProcessReplayTerminationKindV1::BackendFailure:
      return ProcessReplayExecutionReason::BackendEventFailed;
    case ProcessReplayTerminationKindV1::None:
    case ProcessReplayTerminationKindV1::StoppedBeforeTargetCall:
      break;
    }
    return ProcessReplayExecutionReason::BackendProtocolViolation;
  }

  void teardown() noexcept {
    if (TeardownStarted)
      return;
    TeardownStarted = true;

    // Consume both ownership guards before invoking any untrusted callback.
    // Teardown is therefore non-reentrant even when a callback throws, while
    // the local disposition below retains the fail-closed choice.
    const bool MustFinalize = std::exchange(FinalizationArmed, false);
    const bool HadPossibleLaunch = std::exchange(LaunchArmed, false);
    if (!MustFinalize)
      return;

    bool MustRetainContainment = HadPossibleLaunch;
    if (HadPossibleLaunch) {
      try {
        if (llvm::Error Error = Operations.TerminateTree())
          setTeardownError(ProcessReplayExecutionReason::TerminateFailed,
                           std::move(Error));
      } catch (const std::exception &Exception) {
        setTeardownException(ProcessReplayExecutionReason::TerminateFailed,
                             "terminate callback threw: ", Exception.what());
      } catch (...) {
        setTeardownFailure(ProcessReplayExecutionReason::TerminateFailed,
                           "terminate callback threw a non-standard exception");
      }

      bool Reaped = false;
      try {
        if (llvm::Error Error = Operations.ReapTree())
          setTeardownError(ProcessReplayExecutionReason::ReapFailed,
                           std::move(Error));
        else {
          Reaped = true;
          Result.Receipt.Termination.TreeFullyReaped = true;
        }
      } catch (const std::exception &Exception) {
        setTeardownException(ProcessReplayExecutionReason::ReapFailed,
                             "reap callback threw: ", Exception.what());
      } catch (...) {
        setTeardownFailure(ProcessReplayExecutionReason::ReapFailed,
                           "reap callback threw a non-standard exception");
      }

      MustRetainContainment = !Reaped;
      if (Reaped)
        drainOutput();
    }

    if (MustRetainContainment) {
      try {
        if (llvm::Error Error = Operations.RetainContainmentForUnreaped())
          setContainmentError(
              ProcessReplayExecutionReason::ContainmentRetentionFailed,
              std::move(Error));
        else
          Result.Receipt.ContainmentRetained = true;
      } catch (const std::exception &Exception) {
        setContainmentException(
            ProcessReplayExecutionReason::ContainmentRetentionFailed,
            "containment-retention callback threw: ", Exception.what());
      } catch (...) {
        setContainmentFailure(
            ProcessReplayExecutionReason::ContainmentRetentionFailed,
            "containment-retention callback threw a non-standard exception");
      }
      return;
    }

    try {
      if (llvm::Error Error = Operations.CleanupAfterReap())
        setTeardownError(ProcessReplayExecutionReason::CleanupFailed,
                         std::move(Error));
    } catch (const std::exception &Exception) {
      setTeardownException(ProcessReplayExecutionReason::CleanupFailed,
                           "cleanup callback threw: ", Exception.what());
    } catch (...) {
      setTeardownFailure(ProcessReplayExecutionReason::CleanupFailed,
                         "cleanup callback threw a non-standard exception");
    }
  }

  void drainOutput() noexcept {
    while (true) {
      try {
        llvm::Expected<ProcessReplayOutputDrainResultV1> Next =
            Operations.DrainOutput(Deadline);
        if (!Next) {
          setTeardownError(ProcessReplayExecutionReason::OutputDrainFailed,
                           Next.takeError());
          return;
        }
        if (Next->valueless_by_exception()) {
          noteDrainFailure(
              ProcessReplayExecutionReason::BackendProtocolViolation,
              "output drain returned a valueless terminal state");
          return;
        }
        if (std::holds_alternative<ProcessReplayOutputEndOfFileV1>(*Next)) {
          DrainComplete = true;
          return;
        }
        if (std::holds_alternative<ProcessReplayOutputSecurelyDiscardedV1>(
                *Next)) {
          OutputCaptureTruncated = true;
          DrainComplete = true;
          noteDrainFailure(ProcessReplayExecutionReason::OutputLimitExceeded,
                           "bounded output was securely discarded during "
                           "drain");
          return;
        }
        ProcessReplayBackendEventV1 Event;
        Event.Payload = std::move(std::get<ProcessReplayOutputChunkV1>(*Next));
        if (ProcessReplayExecutionReason Reason = chargeEvent(Event);
            Reason != ProcessReplayExecutionReason::None) {
          noteDrainFailure(Reason, eventBudgetDetail(Reason));
          return;
        }
        const auto &Chunk = std::get<ProcessReplayOutputChunkV1>(Event.Payload);
        if (ProcessReplayExecutionReason Reason = processOutput(Chunk);
            Reason != ProcessReplayExecutionReason::None) {
          noteDrainFailure(Reason, eventDetail(Reason));
          return;
        }
      } catch (const std::exception &Exception) {
        setTeardownException(ProcessReplayExecutionReason::OutputDrainFailed,
                             "output-drain callback threw: ", Exception.what());
        return;
      } catch (...) {
        setTeardownFailure(
            ProcessReplayExecutionReason::OutputDrainFailed,
            "output-drain callback threw a non-standard exception");
        return;
      }
    }
  }

  void noteDrainFailure(ProcessReplayExecutionReason Reason,
                        const char *Detail) noexcept {
    if (setPrimaryFailure(Reason)) {
      if (Result.Receipt.Termination.Kind ==
          ProcessReplayTerminationKindV1::StoppedBeforeTargetCall)
        Result.Receipt.Termination.Kind = reasonTerminationKind(Reason);
      writeDiagnostic(Result.Detail, Detail);
      return;
    }
    setTeardownFailure(ProcessReplayExecutionReason::OutputDrainFailed, Detail);
  }

  void setTeardownFailure(ProcessReplayExecutionReason Reason,
                          std::string_view Detail) noexcept {
    const bool First =
        Result.TeardownReason == ProcessReplayExecutionReason::None;
    if (First)
      Result.TeardownReason = Reason;
    if (First)
      writeDiagnostic(Result.TeardownDetail, Detail);
    else
      appendDiagnostic(Result.TeardownDetail, Detail);
  }

  void setTeardownError(ProcessReplayExecutionReason Reason,
                        llvm::Error Error) noexcept {
    const bool First =
        Result.TeardownReason == ProcessReplayExecutionReason::None;
    if (First)
      Result.TeardownReason = Reason;
    std::string Detail = takeErrorDetailNoThrow(std::move(Error));
    if (First)
      writeDiagnostic(Result.TeardownDetail, Detail);
    else
      appendDiagnostic(Result.TeardownDetail, Detail);
  }

  void setTeardownException(ProcessReplayExecutionReason Reason,
                            const char *Prefix, const char *What) noexcept {
    const bool First =
        Result.TeardownReason == ProcessReplayExecutionReason::None;
    if (First)
      Result.TeardownReason = Reason;
    if (First)
      writeDiagnostic(Result.TeardownDetail, Prefix, What);
    else
      appendDiagnostic(Result.TeardownDetail, Prefix, What);
  }

  void setContainmentFailure(ProcessReplayExecutionReason Reason,
                             std::string_view Detail) noexcept {
    const bool First =
        Result.ContainmentReason == ProcessReplayExecutionReason::None;
    if (First)
      Result.ContainmentReason = Reason;
    if (First)
      writeDiagnostic(Result.ContainmentDetail, Detail);
    else
      appendDiagnostic(Result.ContainmentDetail, Detail);
  }

  void setContainmentError(ProcessReplayExecutionReason Reason,
                           llvm::Error Error) noexcept {
    const bool First =
        Result.ContainmentReason == ProcessReplayExecutionReason::None;
    if (First)
      Result.ContainmentReason = Reason;
    std::string Detail = takeErrorDetailNoThrow(std::move(Error));
    if (First)
      writeDiagnostic(Result.ContainmentDetail, Detail);
    else
      appendDiagnostic(Result.ContainmentDetail, Detail);
  }

  void setContainmentException(ProcessReplayExecutionReason Reason,
                               const char *Prefix, const char *What) noexcept {
    const bool First =
        Result.ContainmentReason == ProcessReplayExecutionReason::None;
    if (First)
      Result.ContainmentReason = Reason;
    if (First)
      writeDiagnostic(Result.ContainmentDetail, Prefix, What);
    else
      appendDiagnostic(Result.ContainmentDetail, Prefix, What);
  }

  void finalizeOutput(OutputState &State,
                      ProcessReplayOutputReceiptV1 &Receipt) noexcept {
    Receipt.ObservedBytes = State.ObservedBytes;
    Receipt.DigestedBytes = State.DigestedBytes;
    Receipt.SHA256 = State.Hash.final();
    Receipt.Preview = std::move(State.Preview);
    Receipt.PreviewTruncated =
        State.ObservedBytes > static_cast<uint64_t>(Receipt.Preview.size());
    Receipt.StreamTruncated = State.Truncated || OutputCaptureTruncated ||
                              !DrainComplete ||
                              State.DigestedBytes != State.ObservedBytes;
    Receipt.DigestScope = Receipt.StreamTruncated
                              ? ProcessReplayDigestScopeV1::ObservedPrefix
                              : ProcessReplayDigestScopeV1::CompleteStream;
  }

  static ProcessReplayTerminationKindV1
  reasonTerminationKind(ProcessReplayExecutionReason Reason) {
    switch (Reason) {
    case ProcessReplayExecutionReason::ExitedBeforeTarget:
      return ProcessReplayTerminationKindV1::Exited;
    case ProcessReplayExecutionReason::SignaledBeforeTarget:
      return ProcessReplayTerminationKindV1::Signaled;
    case ProcessReplayExecutionReason::WallTimeExceeded:
      return ProcessReplayTerminationKindV1::WallTimeExceeded;
    case ProcessReplayExecutionReason::CpuTimeExceeded:
      return ProcessReplayTerminationKindV1::CpuTimeExceeded;
    case ProcessReplayExecutionReason::MemoryLimitExceeded:
      return ProcessReplayTerminationKindV1::MemoryLimitExceeded;
    case ProcessReplayExecutionReason::ProcessLimitExceeded:
      return ProcessReplayTerminationKindV1::ProcessLimitExceeded;
    case ProcessReplayExecutionReason::OutputLimitExceeded:
      return ProcessReplayTerminationKindV1::OutputLimitExceeded;
    case ProcessReplayExecutionReason::SandboxViolation:
      return ProcessReplayTerminationKindV1::SandboxViolation;
    default:
      return ProcessReplayTerminationKindV1::BackendFailure;
    }
  }

  const char *eventDetail(ProcessReplayExecutionReason Reason) {
    switch (Reason) {
    case ProcessReplayExecutionReason::BackendProtocolViolation:
      return "backend emitted an invalid process-replay event";
    case ProcessReplayExecutionReason::UnexpectedInputEvent:
      return "input event kind does not match the next transcript event";
    case ProcessReplayExecutionReason::EventOccurrenceMismatch:
      return "input event occurrence does not match the transcript";
    case ProcessReplayExecutionReason::EnvironmentNameMismatch:
      return "environment lookup name does not match the transcript";
    case ProcessReplayExecutionReason::StdinResourceMismatch:
      return "stdin request is not bound to the planned standard input";
    case ProcessReplayExecutionReason::StdinRequestMismatch:
      return "stdin request length does not match the transcript";
    case ProcessReplayExecutionReason::InputResponseFailed:
      return "backend rejected an exact input response";
    case ProcessReplayExecutionReason::ProtocolByteLimitExceeded:
      return "input response would exceed the protocol-byte budget";
    case ProcessReplayExecutionReason::ArithmeticOverflow:
      return "input response accounting overflowed";
    case ProcessReplayExecutionReason::TargetBeforeTranscriptComplete:
      return "target occurrence arrived before all input events were consumed";
    case ProcessReplayExecutionReason::TargetOccurrenceMismatch:
      return "target occurrence is skipped, duplicated, or otherwise "
             "mismatched";
    case ProcessReplayExecutionReason::TargetNotPreCall:
      return "target occurrence was not observed before its call executed";
    case ProcessReplayExecutionReason::TargetVisitLimitExceeded:
      return "target static-site visit budget was exceeded";
    case ProcessReplayExecutionReason::ResumeFailed:
      return "backend could not resume the stopped target";
    case ProcessReplayExecutionReason::ExitedBeforeTarget:
      return "target exited before the terminal occurrence";
    case ProcessReplayExecutionReason::SignaledBeforeTarget:
      return "target was signaled before the terminal occurrence";
    case ProcessReplayExecutionReason::WallTimeExceeded:
      return "wall-time execution budget was exceeded";
    case ProcessReplayExecutionReason::CpuTimeExceeded:
      return "CPU-time execution budget was exceeded";
    case ProcessReplayExecutionReason::MemoryLimitExceeded:
      return "memory execution budget was exceeded";
    case ProcessReplayExecutionReason::ProcessLimitExceeded:
      return "process-count execution budget was exceeded";
    case ProcessReplayExecutionReason::OutputLimitExceeded:
      return "bounded output budget was exceeded";
    case ProcessReplayExecutionReason::SandboxViolation:
      return "target violated an applied sandbox policy";
    case ProcessReplayExecutionReason::BackendEventFailed:
      return "backend could not produce the next attested event";
    default:
      return "process replay execution failed";
    }
  }

  static const char *eventBudgetDetail(ProcessReplayExecutionReason Reason) {
    switch (Reason) {
    case ProcessReplayExecutionReason::ControlEventLimitExceeded:
      return "backend control-event budget was exceeded";
    case ProcessReplayExecutionReason::ProtocolByteLimitExceeded:
      return "backend protocol-byte budget was exceeded";
    case ProcessReplayExecutionReason::BackendProtocolViolation:
      return "backend output chunk exceeds its v1 bound";
    case ProcessReplayExecutionReason::ArithmeticOverflow:
      return "backend event accounting overflowed";
    default:
      return "backend event budget was exceeded";
    }
  }

  ProcessReplayPlanCandidateV1 Plan;
  ProcessReplayExecutionLimitsV1 Limits;
  const ProcessReplayExecutorOperationsV1 &Operations;
  const detail::ProcessReplayPreBeginHooksForTestingV1 *Hooks = nullptr;
  std::chrono::steady_clock::time_point Deadline;
  ProcessReplayExecutionResultV1 Result;
  std::optional<ProcessReplayTargetAuthenticationV1> SourceAuthentication;
  std::vector<uint64_t> ResourceOffsets;
  std::vector<bool> ResourceEOF;
  OutputState StandardOutput;
  OutputState StandardError;
  uint64_t NextInputEvent = 0;
  uint64_t ControlEvents = 0;
  uint64_t ProtocolBytes = 0;
  uint64_t AggregateObservedBytes = 0;
  uint64_t AggregateDigestedBytes = 0;
  bool FinalizationArmed = false;
  bool LaunchArmed = false;
  bool TeardownStarted = false;
  bool DrainComplete = false;
  bool OutputCaptureTruncated = false;
};

} // namespace

const char *toString(ProcessReplayExecutionReason Reason) {
  switch (Reason) {
  case ProcessReplayExecutionReason::None:
    return "none";
  case ProcessReplayExecutionReason::PlanNotReady:
    return "plan_not_ready";
  case ProcessReplayExecutionReason::InvalidExecutionLimits:
    return "invalid_execution_limits";
  case ProcessReplayExecutionReason::InvocationBudgetExceeded:
    return "invocation_budget_exceeded";
  case ProcessReplayExecutionReason::UnattestableOccurrenceMap:
    return "unattestable_occurrence_map";
  case ProcessReplayExecutionReason::MissingOperation:
    return "missing_operation";
  case ProcessReplayExecutionReason::InsufficientCapabilities:
    return "insufficient_capabilities";
  case ProcessReplayExecutionReason::BeginFailed:
    return "begin_failed";
  case ProcessReplayExecutionReason::SourceIdentityAuthenticationFailed:
    return "source_identity_authentication_failed";
  case ProcessReplayExecutionReason::SourceIdentityMismatch:
    return "source_identity_mismatch";
  case ProcessReplayExecutionReason::TargetNotRegularExecutable:
    return "target_not_regular_executable";
  case ProcessReplayExecutionReason::PrivilegeBearingTarget:
    return "privilege_bearing_target";
  case ProcessReplayExecutionReason::UnsupportedHostTarget:
    return "unsupported_host_target";
  case ProcessReplayExecutionReason::TranslatedExecution:
    return "translated_execution";
  case ProcessReplayExecutionReason::PrepareLaunchFailed:
    return "prepare_launch_failed";
  case ProcessReplayExecutionReason::LaunchFailed:
    return "launch_failed";
  case ProcessReplayExecutionReason::IsolationObservationFailed:
    return "isolation_observation_failed";
  case ProcessReplayExecutionReason::IsolationReceiptIncomplete:
    return "isolation_receipt_incomplete";
  case ProcessReplayExecutionReason::LoadedIdentityAuthenticationFailed:
    return "loaded_identity_authentication_failed";
  case ProcessReplayExecutionReason::LoadedIdentityMismatch:
    return "loaded_identity_mismatch";
  case ProcessReplayExecutionReason::ResumeFailed:
    return "resume_failed";
  case ProcessReplayExecutionReason::BackendEventFailed:
    return "backend_event_failed";
  case ProcessReplayExecutionReason::BackendProtocolViolation:
    return "backend_protocol_violation";
  case ProcessReplayExecutionReason::ControlEventLimitExceeded:
    return "control_event_limit_exceeded";
  case ProcessReplayExecutionReason::ProtocolByteLimitExceeded:
    return "protocol_byte_limit_exceeded";
  case ProcessReplayExecutionReason::UnexpectedInputEvent:
    return "unexpected_input_event";
  case ProcessReplayExecutionReason::EventOccurrenceMismatch:
    return "event_occurrence_mismatch";
  case ProcessReplayExecutionReason::EnvironmentNameMismatch:
    return "environment_name_mismatch";
  case ProcessReplayExecutionReason::StdinResourceMismatch:
    return "stdin_resource_mismatch";
  case ProcessReplayExecutionReason::StdinRequestMismatch:
    return "stdin_request_mismatch";
  case ProcessReplayExecutionReason::InputResponseFailed:
    return "input_response_failed";
  case ProcessReplayExecutionReason::TargetBeforeTranscriptComplete:
    return "target_before_transcript_complete";
  case ProcessReplayExecutionReason::TargetOccurrenceMismatch:
    return "target_occurrence_mismatch";
  case ProcessReplayExecutionReason::TargetNotPreCall:
    return "target_not_precall";
  case ProcessReplayExecutionReason::TargetVisitLimitExceeded:
    return "target_visit_limit_exceeded";
  case ProcessReplayExecutionReason::TranscriptIncomplete:
    return "transcript_incomplete";
  case ProcessReplayExecutionReason::ExitedBeforeTarget:
    return "exited_before_target";
  case ProcessReplayExecutionReason::SignaledBeforeTarget:
    return "signaled_before_target";
  case ProcessReplayExecutionReason::WallTimeExceeded:
    return "wall_time_exceeded";
  case ProcessReplayExecutionReason::CpuTimeExceeded:
    return "cpu_time_exceeded";
  case ProcessReplayExecutionReason::MemoryLimitExceeded:
    return "memory_limit_exceeded";
  case ProcessReplayExecutionReason::ProcessLimitExceeded:
    return "process_limit_exceeded";
  case ProcessReplayExecutionReason::OutputLimitExceeded:
    return "output_limit_exceeded";
  case ProcessReplayExecutionReason::SandboxViolation:
    return "sandbox_violation";
  case ProcessReplayExecutionReason::TerminateFailed:
    return "terminate_failed";
  case ProcessReplayExecutionReason::ReapFailed:
    return "reap_failed";
  case ProcessReplayExecutionReason::OutputDrainFailed:
    return "output_drain_failed";
  case ProcessReplayExecutionReason::CleanupFailed:
    return "cleanup_failed";
  case ProcessReplayExecutionReason::CallbackException:
    return "callback_exception";
  case ProcessReplayExecutionReason::ArithmeticOverflow:
    return "arithmetic_overflow";
  case ProcessReplayExecutionReason::DynamicLoaderUnsupported:
    return "dynamic_loader_unsupported";
  case ProcessReplayExecutionReason::SnapshotCopyFailed:
    return "snapshot_copy_failed";
  case ProcessReplayExecutionReason::ContainmentRetentionFailed:
    return "containment_retention_failed";
  case ProcessReplayExecutionReason::InsufficientExecutionBudget:
    return "insufficient_execution_budget";
  }
  return "unknown";
}

ProcessReplayExecutionValidationV1 validateProcessReplayExecutionRequest(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits) {
  const ProcessReplayValidation PlanValidation = validate(Plan);
  if (!PlanValidation.candidateReady())
    return executionFailure(ProcessReplayExecutionReason::PlanNotReady,
                            PlanValidation.Detail.c_str(),
                            PlanValidation.Reason);
  if (!limitsWithinHardCeilings(Limits))
    return executionFailure(
        ProcessReplayExecutionReason::InvalidExecutionLimits,
        "execution limit exceeds a v1 hard ceiling or process count is not "
        "one");
  if (*Plan.TargetOccurrence.Invocation >= Limits.MaxTargetSiteVisits)
    return executionFailure(
        ProcessReplayExecutionReason::InvocationBudgetExceeded,
        "target invocation cannot fit the target-site visit budget");
  if (ProcessReplayExecutionValidationV1 Occurrences =
          validateOccurrenceMap(Plan);
      !Occurrences.ready())
    return Occurrences;
  if (ProcessReplayExecutionValidationV1 MinimumBudgets =
          validateMinimumExecutionBudgets(Plan, Limits);
      !MinimumBudgets.ready())
    return MinimumBudgets;
  return {};
}

ProcessReplayExecutionValidationV1
validateForExecution(const ProcessReplayPlanCandidateV1 &Plan,
                     const ProcessReplayExecutionLimitsV1 &Limits,
                     const ProcessReplayExecutorOperationsV1 &Operations) {
  if (ProcessReplayExecutionValidationV1 Request =
          validateProcessReplayExecutionRequest(Plan, Limits);
      !Request.ready())
    return Request;
  if (!hasAllOperations(Operations))
    return executionFailure(ProcessReplayExecutionReason::MissingOperation,
                            "one or more executor callbacks are unavailable");
  if (!satisfiesCapabilities(Operations.Capabilities))
    return executionFailure(
        ProcessReplayExecutionReason::InsufficientCapabilities,
        "executor cannot provide every required v1 confinement guarantee");
  return {};
}

namespace {

ProcessReplayExecutionResultV1
preBeginExceptionResult(const ProcessReplayPlanCandidateV1 &Plan) noexcept {
  ProcessReplayExecutionResultV1 Result;
  Result.Receipt.ExpectedTargetSHA256 = Plan.Target.SHA256;
  Result.Reason = ProcessReplayExecutionReason::SnapshotCopyFailed;
  return Result;
}

ProcessReplayExecutionResultV1 executeProcessReplayImpl(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits,
    const ProcessReplayExecutorOperationsV1 &Operations,
    const detail::ProcessReplayPreBeginHooksForTestingV1 *Hooks) noexcept {
  // Validate the caller-owned graph before copying it.  In particular, count
  // and literal-byte ceilings must reject an oversized untrusted candidate
  // without first duplicating its allocations.  All work through coordinator
  // construction remains inside one exception boundary, before Begin or any
  // other executor operation can run.
  std::optional<ProcessReplayPlanCandidateV1> Snapshot;
  std::optional<Coordinator> Execution;
  try {
    if (Hooks && Hooks->BeforeSourceValidation)
      Hooks->BeforeSourceValidation();
    const ProcessReplayExecutionValidationV1 SourceValidation =
        validateForExecution(Plan, Limits, Operations);
    if (!SourceValidation.ready()) {
      ProcessReplayExecutionResultV1 Result;
      Result.Receipt.PlanValidationReason = SourceValidation.PlanReason;
      Result.Receipt.ExpectedTargetSHA256 = Plan.Target.SHA256;
      Result.Reason = SourceValidation.Reason;
      Result.Detail = SourceValidation.Detail;
      return Result;
    }

    if (Hooks && Hooks->BeforeSnapshotCopy)
      Hooks->BeforeSnapshotCopy();
    Snapshot.emplace(Plan);
    const ProcessReplayExecutionValidationV1 SnapshotValidation =
        validateForExecution(*Snapshot, Limits, Operations);
    if (!SnapshotValidation.ready()) {
      ProcessReplayExecutionResultV1 Result;
      Result.Receipt.PlanValidationReason = SnapshotValidation.PlanReason;
      Result.Receipt.ExpectedTargetSHA256 = Snapshot->Target.SHA256;
      Result.Reason = SnapshotValidation.Reason;
      Result.Detail = SnapshotValidation.Detail;
      return Result;
    }
    Execution.emplace(std::move(*Snapshot), Limits, Operations, Hooks);
  } catch (const std::bad_alloc &) {
    return preBeginExceptionResult(Plan);
  } catch (const std::exception &) {
    return preBeginExceptionResult(Plan);
  } catch (...) {
    return preBeginExceptionResult(Plan);
  }
  return Execution->run();
}

} // namespace

ProcessReplayExecutionResultV1 executeProcessReplay(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits,
    const ProcessReplayExecutorOperationsV1 &Operations) noexcept {
  return executeProcessReplayImpl(Plan, Limits, Operations, nullptr);
}

ProcessReplayExecutionResultV1 detail::executeProcessReplayForTesting(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits,
    const ProcessReplayExecutorOperationsV1 &Operations,
    const ProcessReplayPreBeginHooksForTestingV1 &Hooks) noexcept {
  return executeProcessReplayImpl(Plan, Limits, Operations, &Hooks);
}

} // namespace neverd::safety::process_replay
