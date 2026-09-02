//===- ProcessReplayExecutor.h - Exact replay coordinator -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the platform-neutral coordinator for attempting one authenticated
/// `process-replay-v1` execution.  The existing plan schema is unchanged: the
/// ordered Events array is the complete input transcript and TargetOccurrence
/// is an implicit terminal marker observed immediately before the target call.
///
/// Native adapter availability/factory policy lives in the separate
/// NativeProcessReplayAdapter boundary.  A backend must explicitly attest
/// every confinement and interposition capability before any target code is
/// resumed; unavailable capabilities fail closed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_PROCESSREPLAYEXECUTOR_H
#define NEVERD_SAFETY_PROCESSREPLAYEXECUTOR_H

#include "neverd/safety/ProcessReplay.h"

#include "llvm/Support/Error.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neverd::safety::process_replay {

inline constexpr std::string_view kProcessReplayExecutorAdapter =
    "process-replay-exec-v1";
inline constexpr uint32_t kProcessReplayExecutorSchemaVersion = 1;

inline constexpr uint64_t kProcessReplayExecutionMaxWallTimeMs = 10 * 60 * 1000;
inline constexpr uint64_t kProcessReplayExecutionMaxCpuTimeUs =
    10 * 60 * 1000 * 1000;
inline constexpr uint64_t kProcessReplayExecutionMaxAddressSpaceBytes =
    uint64_t{64} << 30;
inline constexpr uint64_t kProcessReplayExecutionMaxResidentMemoryBytes =
    uint64_t{64} << 30;
inline constexpr uint32_t kProcessReplayExecutionMaxProcesses = 1;
inline constexpr uint64_t kProcessReplayExecutionMaxTargetSiteVisits =
    uint64_t{1} << 20;
inline constexpr uint64_t kProcessReplayExecutionMaxControlEvents = uint64_t{1}
                                                                    << 20;
inline constexpr uint64_t kProcessReplayExecutionMaxProtocolBytes = uint64_t{1}
                                                                    << 28;
inline constexpr uint64_t kProcessReplayExecutionMaxStreamBytes = uint64_t{1}
                                                                  << 28;
inline constexpr uint64_t kProcessReplayExecutionMaxAggregateOutputBytes =
    uint64_t{1} << 29;
inline constexpr uint64_t kProcessReplayExecutionMaxPreviewBytes = uint64_t{1}
                                                                   << 16;
inline constexpr uint64_t kProcessReplayExecutionMaxBackendChunkBytes =
    uint64_t{1} << 20;
inline constexpr uint64_t kProcessReplayBackendEventProtocolBytes = 1;
inline constexpr uint64_t kProcessReplayStdinRequestProtocolBytes = 24;
inline constexpr uint64_t kProcessReplayTargetHitProtocolBytes = 8;
inline constexpr uint64_t kProcessReplayTerminationProtocolBytes = 32;
inline constexpr uint64_t kProcessReplayEnvironmentReplyProtocolBytes = 16;
inline constexpr uint64_t kProcessReplayStdinReplyProtocolBytes = 32;

/// Execution budgets are distinct from the plan validator's manifest budgets.
/// Zero is an actual zero limit, never a request for a default.  A value above
/// a hard ceiling is rejected before Begin rather than silently clamped.
struct ProcessReplayExecutionLimitsV1 {
  uint64_t WallTimeMs = 30 * 1000;
  uint64_t CpuTimeUs = 10 * 1000 * 1000;
  uint64_t AddressSpaceBytes = uint64_t{1} << 30;
  uint64_t ResidentMemoryBytes = uint64_t{512} << 20;
  uint32_t MaxProcesses = 1;
  uint64_t MaxTargetSiteVisits = 4096;
  uint64_t MaxControlEvents = 16384;
  uint64_t MaxProtocolBytes = uint64_t{64} << 20;
  uint64_t MaxStdoutBytes = uint64_t{8} << 20;
  uint64_t MaxStderrBytes = uint64_t{8} << 20;
  uint64_t MaxAggregateOutputBytes = uint64_t{16} << 20;
  uint64_t PreviewBytesPerStream = 4096;
  uint64_t MaxBackendChunkBytes = uint64_t{1} << 20;
};

enum class ProcessReplayExecutionReason : uint16_t {
  None = 0,
  PlanNotReady = 1,
  InvalidExecutionLimits = 2,
  InvocationBudgetExceeded = 3,
  UnattestableOccurrenceMap = 4,
  MissingOperation = 5,
  InsufficientCapabilities = 6,
  BeginFailed = 7,
  SourceIdentityAuthenticationFailed = 8,
  SourceIdentityMismatch = 9,
  TargetNotRegularExecutable = 10,
  PrivilegeBearingTarget = 11,
  UnsupportedHostTarget = 12,
  TranslatedExecution = 13,
  PrepareLaunchFailed = 14,
  LaunchFailed = 15,
  IsolationObservationFailed = 16,
  IsolationReceiptIncomplete = 17,
  LoadedIdentityAuthenticationFailed = 18,
  LoadedIdentityMismatch = 19,
  ResumeFailed = 20,
  BackendEventFailed = 21,
  BackendProtocolViolation = 22,
  ControlEventLimitExceeded = 23,
  ProtocolByteLimitExceeded = 24,
  UnexpectedInputEvent = 25,
  EventOccurrenceMismatch = 26,
  EnvironmentNameMismatch = 27,
  StdinResourceMismatch = 28,
  StdinRequestMismatch = 29,
  InputResponseFailed = 30,
  TargetBeforeTranscriptComplete = 31,
  TargetOccurrenceMismatch = 32,
  TargetNotPreCall = 33,
  TargetVisitLimitExceeded = 34,
  TranscriptIncomplete = 35,
  ExitedBeforeTarget = 36,
  SignaledBeforeTarget = 37,
  WallTimeExceeded = 38,
  CpuTimeExceeded = 39,
  MemoryLimitExceeded = 40,
  ProcessLimitExceeded = 41,
  OutputLimitExceeded = 42,
  SandboxViolation = 43,
  TerminateFailed = 44,
  ReapFailed = 45,
  OutputDrainFailed = 46,
  CleanupFailed = 47,
  CallbackException = 48,
  ArithmeticOverflow = 49,
  DynamicLoaderUnsupported = 50,
  SnapshotCopyFailed = 51,
  ContainmentRetentionFailed = 52,
  InsufficientExecutionBudget = 53,
};

static_assert(
    static_cast<uint16_t>(ProcessReplayExecutionReason::None) == 0 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::PlanNotReady) == 1 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::InvalidExecutionLimits) == 2 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::InvocationBudgetExceeded) == 3 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::UnattestableOccurrenceMap) == 4 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::MissingOperation) ==
        5 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::InsufficientCapabilities) == 6 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::BeginFailed) == 7 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::SourceIdentityAuthenticationFailed) ==
        8 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::SourceIdentityMismatch) == 9 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::TargetNotRegularExecutable) == 10 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::PrivilegeBearingTarget) == 11 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::UnsupportedHostTarget) == 12 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::TranslatedExecution) ==
        13 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::PrepareLaunchFailed) ==
        14 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::LaunchFailed) == 15 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::IsolationObservationFailed) == 16 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::IsolationReceiptIncomplete) == 17 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::LoadedIdentityAuthenticationFailed) ==
        18 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::LoadedIdentityMismatch) == 19 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::ResumeFailed) == 20 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::BackendEventFailed) ==
        21 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::BackendProtocolViolation) == 22 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::ControlEventLimitExceeded) == 23 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::ProtocolByteLimitExceeded) == 24 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::UnexpectedInputEvent) ==
        25 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::EventOccurrenceMismatch) == 26 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::EnvironmentNameMismatch) == 27 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::StdinResourceMismatch) == 28 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::StdinRequestMismatch) ==
        29 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::InputResponseFailed) ==
        30 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::TargetBeforeTranscriptComplete) == 31 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::TargetOccurrenceMismatch) == 32 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::TargetNotPreCall) ==
        33 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::TargetVisitLimitExceeded) == 34 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::TranscriptIncomplete) ==
        35 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::ExitedBeforeTarget) ==
        36 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::SignaledBeforeTarget) ==
        37 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::WallTimeExceeded) ==
        38 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::CpuTimeExceeded) ==
        39 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::MemoryLimitExceeded) ==
        40 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::ProcessLimitExceeded) ==
        41 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::OutputLimitExceeded) ==
        42 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::SandboxViolation) ==
        43 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::TerminateFailed) ==
        44 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::ReapFailed) == 45 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::OutputDrainFailed) ==
        46 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::CleanupFailed) == 47 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::CallbackException) ==
        48 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::ArithmeticOverflow) ==
        49 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::DynamicLoaderUnsupported) == 50 &&
    static_cast<uint16_t>(ProcessReplayExecutionReason::SnapshotCopyFailed) ==
        51 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::ContainmentRetentionFailed) == 52 &&
    static_cast<uint16_t>(
        ProcessReplayExecutionReason::InsufficientExecutionBudget) == 53);

const char *toString(ProcessReplayExecutionReason Reason);

/// Claims made by an adapter before Begin, and observed again before Resume.
/// The all-false default is intentional.  Unit-test fakes may opt in; a native
/// adapter must leave any unavailable kernel/OS guarantee false.
struct ProcessReplayExecutorCapabilitiesV1 {
  bool NativeFormatAndArchitecture = false;
  bool NoTranslatedExecution = false;
  bool SelfContainedStaticExecutable = false;
  bool PinnedTargetObject = false;
  bool LoadedImageReauthentication = false;
  bool DirectArgumentVector = false;
  bool ExactEnvironmentBlock = false;
  bool PrivateWorkingDirectory = false;
  bool ExactDescriptorWhitelist = false;
  bool SandboxBeforeTargetCode = false;
  bool PrivilegeGainDenied = false;
  bool NetworkDenied = false;
  bool HostFilesystemReadIsolated = false;
  bool FilesystemWritesDenied = false;
  bool ProcessTreeContained = false;
  bool ProcessTreeKillAndReap = false;
  bool WallTimeLimit = false;
  bool CpuTimeLimit = false;
  bool MemoryLimit = false;
  bool OutputLimit = false;
  /// Both input interpositions are mandatory in v1 even when the transcript
  /// contains no corresponding event: proving that no unexpected lookup or
  /// read occurred is part of authenticating a complete input transcript.
  bool EnvironmentLookupInterposition = false;
  bool StandardInputInterposition = false;
  bool UniquePreCallOccurrenceAttestation = false;
};

/// Authentication of the held executable object or its loaded image.  The
/// opaque object digest binds a stable platform identity without placing raw
/// device/inode/HANDLE material in the receipt.
struct ProcessReplayTargetAuthenticationV1 {
  ProcessReplayTargetIdentity Identity;
  std::optional<uint64_t> ByteSize;
  std::optional<std::array<uint8_t, 32>> ObjectIdentitySHA256;
  bool RegularFile = false;
  bool Executable = false;
  bool PrivilegeBearing = false;
  bool NativeFormatAndArchitecture = false;
  bool TranslatedExecution = false;
  bool SelfContainedStaticExecutable = false;
};

enum class ProcessReplayProbeKindV1 : uint8_t {
  EnvironmentLookup = 1,
  StandardInputRead = 2,
};

struct ProcessReplayLaunchEnvironmentV1 {
  std::vector<uint8_t> Name;
  std::vector<uint8_t> Value;
};

struct ProcessReplayLaunchProbeV1 {
  ProcessReplayProbeKindV1 Kind = ProcessReplayProbeKindV1::EnvironmentLookup;
  ProcessReplayOccurrence At;
};

/// Owned launch material.  It contains no command string or target pathname;
/// the backend must execute the object pinned by Begin directly.
struct ProcessReplayLaunchRequestV1 {
  ProcessReplayTargetIdentity Target;
  ProcessReplayOccurrence TargetOccurrence;
  ProcessReplayExecutionLimitsV1 Limits;
  std::vector<std::vector<uint8_t>> Arguments;
  /// Only present plan entries appear here, in plan order.
  std::vector<ProcessReplayLaunchEnvironmentV1> Environment;
  std::vector<ProcessReplayLaunchProbeV1> InputProbes;
  bool UseShell = false;
  bool SearchPath = false;
  bool ClearInheritedEnvironment = true;
  bool PrivateEmptyWorkingDirectory = true;
  bool DenyHostFilesystemReadsExceptPinnedTarget = true;
  bool InheritOnlyStandardDescriptors = true;
  bool BindStandardInputToInterposition = true;
  bool CaptureStandardOutput = true;
  bool CaptureStandardError = true;
  bool StartStoppedBeforeTargetCode = true;
  bool StopBeforeTargetCall = true;
  bool RequireSelfContainedStaticExecutable = true;
};

struct ProcessReplayEnvironmentLookupRequestV1 {
  ProcessReplayOccurrence At;
  std::vector<uint8_t> Name;
};

struct ProcessReplayStdinReadRequestV1 {
  ProcessReplayOccurrence At;
  uint32_t ResourceId = 0;
  int32_t Descriptor = 0;
  uint64_t RequestedBytes = 0;
};

struct ProcessReplayTargetSiteHitV1 {
  ProcessReplayOccurrence At;
  bool BeforeCall = false;
};

enum class ProcessReplayOutputStreamV1 : uint8_t {
  StandardOutput = 1,
  StandardError = 2,
};

struct ProcessReplayOutputChunkV1 {
  ProcessReplayOutputStreamV1 Stream =
      ProcessReplayOutputStreamV1::StandardOutput;
  std::vector<uint8_t> Bytes;
};

/// Explicit output-drain terminals.  EndOfFile proves that every byte from
/// both captured streams reached the coordinator.  SecurelyDiscarded proves
/// only that a backend-enforced output bound discarded the remainder without
/// allowing it to escape containment; both stream digests are then prefixes.
struct ProcessReplayOutputEndOfFileV1 {};
struct ProcessReplayOutputSecurelyDiscardedV1 {};

using ProcessReplayOutputDrainResultV1 =
    std::variant<ProcessReplayOutputChunkV1, ProcessReplayOutputEndOfFileV1,
                 ProcessReplayOutputSecurelyDiscardedV1>;

enum class ProcessReplayTerminationKindV1 : uint8_t {
  None = 0,
  StoppedBeforeTargetCall = 1,
  Exited = 2,
  Signaled = 3,
  WallTimeExceeded = 4,
  CpuTimeExceeded = 5,
  MemoryLimitExceeded = 6,
  ProcessLimitExceeded = 7,
  OutputLimitExceeded = 8,
  SandboxViolation = 9,
  BackendFailure = 10,
};

struct ProcessReplayTerminationEventV1 {
  ProcessReplayTerminationKindV1 Kind = ProcessReplayTerminationKindV1::None;
  std::optional<int32_t> ExitCode;
  std::optional<int32_t> Signal;
  bool CoreDumped = false;
  std::optional<uint64_t> WallTimeMs;
  std::optional<uint64_t> CpuTimeUs;
  std::optional<uint64_t> PeakMemoryBytes;
  std::optional<uint32_t> PeakProcessCount;
};

using ProcessReplayBackendEventPayloadV1 =
    std::variant<std::monostate, ProcessReplayEnvironmentLookupRequestV1,
                 ProcessReplayStdinReadRequestV1, ProcessReplayTargetSiteHitV1,
                 ProcessReplayOutputChunkV1, ProcessReplayTerminationEventV1>;

struct ProcessReplayBackendEventV1 {
  ProcessReplayBackendEventPayloadV1 Payload;
};

struct ProcessReplayEnvironmentLookupReplyV1 {
  bool Present = false;
  std::vector<uint8_t> Value;
};

struct ProcessReplayStdinReadReplyV1 {
  ProcessReplayReadOutcome Outcome = ProcessReplayReadOutcome::Data;
  uint64_t ReturnedBytes = 0;
  std::vector<uint8_t> Bytes;
  bool EOFAfter = false;
};

using ProcessReplayInputReplyPayloadV1 =
    std::variant<ProcessReplayEnvironmentLookupReplyV1,
                 ProcessReplayStdinReadReplyV1>;

struct ProcessReplayInputReplyV1 {
  uint32_t EventId = 0;
  ProcessReplayInputReplyPayloadV1 Payload;
};

/// Native boundary for one execution.  Finalization is armed before Begin.
/// CleanupAfterReap must tolerate partially initialized state but may only
/// release containment when no process was launched or the entire tree was
/// reaped.  If reaping cannot be proved, RetainContainmentForUnreaped instead
/// must transfer every possibly live execution object and its kill authority
/// to a persistent supervisor that continues containment and reap attempts
/// after this call returns.  It must not be a no-op, release containment, or
/// report success without completing that durable handoff; ordinary cleanup is
/// forbidden.  Both callbacks are idempotent handoff boundaries and exactly
/// one is invoked.  LaunchStopped, TerminateTree, and ReapTree must tolerate a
/// launch attempt that created only part of a process tree.  DrainOutput is
/// incremental and returns an explicit EndOfFile or SecurelyDiscarded terminal;
/// those states are deliberately not conflated because only EndOfFile can
/// authenticate complete-stream digests.
/// NextEvent and DrainOutput must honor their monotonic deadline; the
/// coordinator requires the corresponding applied capability before Resume.
struct ProcessReplayExecutorOperationsV1 {
  ProcessReplayExecutorCapabilitiesV1 Capabilities;

  std::function<llvm::Error()> Begin;
  std::function<llvm::Expected<ProcessReplayTargetAuthenticationV1>()>
      AuthenticatePinnedTarget;
  std::function<llvm::Error(const ProcessReplayLaunchRequestV1 &)>
      PrepareLaunch;
  std::function<llvm::Error()> LaunchStopped;
  std::function<llvm::Expected<ProcessReplayExecutorCapabilitiesV1>()>
      ObserveAppliedIsolation;
  std::function<llvm::Expected<ProcessReplayTargetAuthenticationV1>()>
      AuthenticateLoadedTarget;
  std::function<llvm::Error()> Resume;
  std::function<llvm::Expected<ProcessReplayBackendEventV1>(
      std::chrono::steady_clock::time_point)>
      NextEvent;
  std::function<llvm::Error(const ProcessReplayInputReplyV1 &)> ReplyAndResume;
  std::function<llvm::Error()> TerminateTree;
  std::function<llvm::Error()> ReapTree;
  std::function<llvm::Expected<ProcessReplayOutputDrainResultV1>(
      std::chrono::steady_clock::time_point)>
      DrainOutput;
  std::function<llvm::Error()> CleanupAfterReap;
  std::function<llvm::Error()> RetainContainmentForUnreaped;
};

enum class ProcessReplayDigestScopeV1 : uint8_t {
  CompleteStream = 1,
  ObservedPrefix = 2,
};

struct ProcessReplayOutputReceiptV1 {
  uint64_t ObservedBytes = 0;
  uint64_t DigestedBytes = 0;
  std::array<uint8_t, 32> SHA256{};
  std::vector<uint8_t> Preview;
  bool PreviewTruncated = false;
  bool StreamTruncated = true;
  ProcessReplayDigestScopeV1 DigestScope =
      ProcessReplayDigestScopeV1::ObservedPrefix;
};

struct ProcessReplayTerminationReceiptV1 {
  ProcessReplayTerminationKindV1 Kind = ProcessReplayTerminationKindV1::None;
  std::optional<int32_t> ExitCode;
  std::optional<int32_t> Signal;
  bool CoreDumped = false;
  bool TreeFullyReaped = false;
  std::optional<uint64_t> WallTimeMs;
  std::optional<uint64_t> CpuTimeUs;
  std::optional<uint64_t> PeakMemoryBytes;
  std::optional<uint32_t> PeakProcessCount;
};

struct ProcessReplayExecutionReceiptV1 {
  uint32_t Version = kProcessReplayExecutorSchemaVersion;
  bool Complete = false;
  ProcessReplayValidationReason PlanValidationReason =
      ProcessReplayValidationReason::None;
  std::optional<std::array<uint8_t, 32>> ExpectedTargetSHA256;
  std::optional<std::array<uint8_t, 32>> SourceTargetSHA256;
  std::optional<std::array<uint8_t, 32>> LoadedTargetSHA256;
  bool SourceIdentityMatched = false;
  bool LoadedIdentityMatched = false;
  bool StableObjectIdentityMatched = false;
  ProcessReplayExecutorCapabilitiesV1 AppliedCapabilities;
  uint32_t MatchedInputEventCount = 0;
  uint64_t TargetSiteVisitsObserved = 0;
  bool TargetAttested = false;
  /// The exact terminal occurrence is stopped before its call instruction.
  bool TargetCallExecuted = false;
  bool ContainmentRetained = false;
  ProcessReplayTerminationReceiptV1 Termination;
  ProcessReplayOutputReceiptV1 StandardOutput;
  ProcessReplayOutputReceiptV1 StandardError;
};

struct ProcessReplayExecutionResultV1 {
  ProcessReplayExecutionReceiptV1 Receipt;
  ProcessReplayExecutionReason Reason = ProcessReplayExecutionReason::None;
  ProcessReplayExecutionReason TeardownReason =
      ProcessReplayExecutionReason::None;
  ProcessReplayExecutionReason ContainmentReason =
      ProcessReplayExecutionReason::None;
  /// Diagnostic-only text; it is not attested receipt material.
  std::string Detail;
  std::string TeardownDetail;
  std::string ContainmentDetail;

  bool succeeded() const {
    return Receipt.Complete && Receipt.TargetAttested &&
           !Receipt.TargetCallExecuted && Receipt.Termination.TreeFullyReaped &&
           Reason == ProcessReplayExecutionReason::None &&
           TeardownReason == ProcessReplayExecutionReason::None &&
           ContainmentReason == ProcessReplayExecutionReason::None;
  }
};

struct ProcessReplayExecutionValidationV1 {
  ProcessReplayValidationReason PlanReason =
      ProcessReplayValidationReason::None;
  ProcessReplayExecutionReason Reason = ProcessReplayExecutionReason::None;
  std::string Detail;

  bool ready() const {
    return PlanReason == ProcessReplayValidationReason::None &&
           Reason == ProcessReplayExecutionReason::None;
  }
};

/// Pure execution-request preflight shared by native availability queries and
/// operation-aware validation.  It validates plan, limits, minimum budgets,
/// invocation bounds, and the platform-neutral occurrence map without
/// inspecting or invoking an operation.
ProcessReplayExecutionValidationV1 validateProcessReplayExecutionRequest(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits);

/// Pure runner preflight.  In addition to the immutable request above, it
/// requires every callback and every v1 capability.  It does not invoke an
/// operation or make a native sandbox claim.  In v1 it additionally rejects a
/// target static site that is also used by an input event, because the event
/// kind would not be uniquely attestable.
ProcessReplayExecutionValidationV1
validateForExecution(const ProcessReplayPlanCandidateV1 &Plan,
                     const ProcessReplayExecutionLimitsV1 &Limits,
                     const ProcessReplayExecutorOperationsV1 &Operations);

/// Executes an immutable copy of Plan.  Success means every input event was
/// answered exactly and the terminal TargetOccurrence was observed pre-call
/// under the applied capability set; it does not mean the target call ran or
/// that a finding manifested.  An exception during either pre-Begin validation,
/// snapshot creation, snapshot revalidation, or coordinator construction is
/// reported as SnapshotCopyFailed without invoking an executor operation.
/// Exceptions, including allocation failures, never escape this boundary.
ProcessReplayExecutionResultV1 executeProcessReplay(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits,
    const ProcessReplayExecutorOperationsV1 &Operations) noexcept;

namespace detail {

/// Fault-injection seams for proving both the pre-Begin snapshot boundary and
/// the post-Begin no-throw diagnostic boundary.  These hooks are not executor
/// operations and are never consulted by executeProcessReplay; production
/// adapters must not use this entry point.
struct ProcessReplayPreBeginHooksForTestingV1 {
  std::function<void()> BeforeSourceValidation;
  std::function<void()> BeforeSnapshotCopy;
  /// Fault-injection seams for the post-Begin no-throw boundary.  They run
  /// immediately before consuming an llvm::Error into text and before writing
  /// diagnostic text respectively.  Production execution never installs them.
  std::function<void()> BeforeErrorDetailMaterialization;
  std::function<void()> BeforeDiagnosticWrite;
};

ProcessReplayExecutionResultV1 executeProcessReplayForTesting(
    const ProcessReplayPlanCandidateV1 &Plan,
    const ProcessReplayExecutionLimitsV1 &Limits,
    const ProcessReplayExecutorOperationsV1 &Operations,
    const ProcessReplayPreBeginHooksForTestingV1 &Hooks) noexcept;

} // namespace detail

} // namespace neverd::safety::process_replay

#endif // NEVERD_SAFETY_PROCESSREPLAYEXECUTOR_H
