//===- ProcessReplay.h - Process replay plan-only contract ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the independent `process-replay-v1` plan-candidate vocabulary and
/// its pure, fail-closed validator.  A successful validation means only that
/// the manifest is a bounded, typed plan candidate.  It is not a canonical
/// serialized manifest (this slice has no manifest self-digest), evidence that
/// a process was executed, evidence that an input occurrence was observed at
/// runtime, or evidence that a finding is process-replayable.
///
/// This contract deliberately does not reuse the legacy ReplayPlan version 1
/// / `process-input-v1` evidence adapter.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_PROCESSREPLAY_H
#define NEVERD_SAFETY_PROCESSREPLAY_H

#include "neverd/Common.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neverd::safety::process_replay {

inline constexpr std::string_view kProcessReplayAdapter = "process-replay-v1";
inline constexpr uint32_t kProcessReplaySchemaVersion = 1;

inline constexpr uint32_t kProcessReplayMaxArguments = 4096;
inline constexpr uint32_t kProcessReplayMaxEnvironmentEntries = 4096;
inline constexpr uint32_t kProcessReplayMaxResources = 16;
inline constexpr uint32_t kProcessReplayMaxEvents = 4096;
inline constexpr uint64_t kProcessReplayMaxLiteralBytes = uint64_t(1) << 20;
inline constexpr uint64_t kProcessReplayMaxReadRequestBytes = uint64_t(1) << 24;
inline constexpr uint64_t kProcessReplayMaxTotalReadRequestBytes = uint64_t(1)
                                                                   << 28;

enum class ProcessReplayEndianness : uint8_t {
  Little = 1,
  Big = 2,
};

/// Complete identity of the executable for which the plan was derived.
/// Optional fields preserve the distinction between a legitimate numeric zero
/// and identity data that was never established.
struct ProcessReplayTargetIdentity {
  std::optional<BinaryFormat> Format;
  std::optional<Arch> Architecture;
  std::optional<Bitness> Bits;
  std::optional<InstructionMode> Mode;
  std::optional<ProcessReplayEndianness> Endianness;
  std::optional<bool> Relocatable;
  std::optional<uint64_t> Base;
  std::optional<uint64_t> Entry;
  std::optional<std::array<uint8_t, 32>> SHA256;
};

/// Static and dynamic identity of one exact native call occurrence.
struct ProcessReplayOccurrence {
  std::optional<uint64_t> FuncEntry;
  std::optional<uint64_t> CallVA;
  std::optional<uint32_t> BlockId;
  std::optional<uint32_t> OpIdx;
  std::optional<uint32_t> OriginSeq;
  std::optional<uint32_t> CallSiteId;
  std::optional<uint64_t> Invocation;
};

/// One exact argv element.  Bytes excludes the implicit trailing NUL.  These
/// are candidate bytes for a future target-side argv adapter; their presence
/// does not prove that Target.Entry has a C `main(argc, argv)` ABI or that the
/// candidate can be launched directly.
struct ProcessReplayArgument {
  uint32_t Index = 0;
  std::vector<uint8_t> Bytes;
};

/// Capabilities an eventual trace-controlled runner must provide before this
/// candidate could be execution-verified.  The plan-only validator proves only
/// that this list is canonical and complete for the typed inputs below.
enum class ProcessReplayRequiredCapability : uint8_t {
  ArgumentVectorInjection = 1,
  EnvironmentIsolation = 2,
  EnvironmentLookupInterposition = 3,
  StandardInputInterposition = 4,
  TargetIdentityAuthentication = 5,
  TargetOccurrenceAttestation = 6,
};

/// One exact environment state assertion.  Present entries become NAME=VALUE;
/// absent entries are negative lookup facts.  Names and values exclude their
/// implicit C terminators.
struct ProcessReplayEnvironmentEntry {
  uint32_t Id = 0;
  std::vector<uint8_t> Name;
  bool Present = false;
  std::vector<uint8_t> Value;
};

enum class ProcessReplayResourceKind : uint8_t {
  StandardInput = 1,
};

/// Logical resources are independent of unstable host descriptor numbers.
/// The execution adapter will bind StandardInput to the intercepted target
/// API; this plan-only layer never opens or reads a host resource.
struct ProcessReplayResource {
  uint32_t Id = 0;
  ProcessReplayResourceKind Kind = ProcessReplayResourceKind::StandardInput;
};

enum class ProcessReplayReadOutcome : uint8_t {
  Data = 1,
  EndOfFile = 2,
  ZeroLength = 3,
};

struct ProcessReplayEnvironmentLookupEvent {
  ProcessReplayOccurrence At;
  uint32_t EnvironmentId = 0;
};

/// One exact standard-input call result.  Offset is the logical cursor before
/// the call.  RequestedBytes and ReturnedBytes are retained separately so a
/// future interposition adapter can reproduce short-read boundaries exactly;
/// concatenating Bytes into an ordinary pipe is not equivalent.
struct ProcessReplayStdinReadEvent {
  ProcessReplayOccurrence At;
  uint32_t ResourceId = 0;
  uint64_t Offset = 0;
  uint64_t RequestedBytes = 0;
  uint64_t ReturnedBytes = 0;
  std::vector<uint8_t> Bytes;
  ProcessReplayReadOutcome Outcome = ProcessReplayReadOutcome::Data;
  bool EOFAfter = false;
};

using ProcessReplayEventPayload =
    std::variant<std::monostate, ProcessReplayEnvironmentLookupEvent,
                 ProcessReplayStdinReadEvent>;

struct ProcessReplayEvent {
  uint32_t Id = 0;
  ProcessReplayEventPayload Payload;
};

struct ProcessReplayPlanCandidateV1 {
  uint32_t Version = kProcessReplaySchemaVersion;
  ProcessReplayTargetIdentity Target;
  ProcessReplayOccurrence TargetOccurrence;
  std::vector<ProcessReplayRequiredCapability> RequiredCapabilities;
  std::vector<ProcessReplayArgument> Arguments;
  std::vector<ProcessReplayEnvironmentEntry> Environment;
  std::vector<ProcessReplayResource> Resources;
  std::vector<ProcessReplayEvent> Events;
};

/// Caller-selected validation budgets.  Zero is an actual zero limit, not a
/// request for a default.  Values above the corresponding hard ceiling are
/// rejected instead of silently clamped.
struct ProcessReplayValidationLimits {
  uint32_t MaxArguments = kProcessReplayMaxArguments;
  uint32_t MaxEnvironmentEntries = kProcessReplayMaxEnvironmentEntries;
  uint32_t MaxResources = kProcessReplayMaxResources;
  uint32_t MaxEvents = kProcessReplayMaxEvents;
  uint64_t MaxLiteralBytes = kProcessReplayMaxLiteralBytes;
  uint64_t MaxReadRequestBytes = kProcessReplayMaxReadRequestBytes;
  uint64_t MaxTotalReadRequestBytes = kProcessReplayMaxTotalReadRequestBytes;
};

enum class ProcessReplayValidationReason : uint16_t {
  None = 0,
  UnsupportedVersion = 1,
  IncompleteTargetIdentity = 2,
  UnsupportedTargetFormat = 3,
  UnsupportedTargetArchitecture = 4,
  UnsupportedTargetBitness = 5,
  UnsupportedTargetMode = 6,
  UnsupportedTargetEndianness = 7,
  RelocatableTarget = 8,
  IncompleteOccurrence = 9,
  InvalidOccurrence = 10,
  InvalidLimits = 11,
  ArgumentLimitExceeded = 12,
  NonCanonicalArgumentOrder = 13,
  InvalidArgument = 14,
  LiteralByteBudgetExceeded = 15,
  ArithmeticOverflow = 16,
  InvalidTargetAddress = 17,
  EnvironmentLimitExceeded = 18,
  NonCanonicalEnvironmentOrder = 19,
  InvalidEnvironmentName = 20,
  InvalidEnvironmentValue = 21,
  DuplicateEnvironmentName = 22,
  ResourceLimitExceeded = 23,
  EventLimitExceeded = 24,
  ReadRequestLimitExceeded = 25,
  TotalReadRequestBudgetExceeded = 26,
  NonCanonicalResourceOrder = 27,
  UnsupportedResourceKind = 28,
  DuplicateResource = 29,
  UnreferencedResource = 30,
  NonCanonicalEventOrder = 31,
  UnsupportedEventKind = 32,
  DuplicateEventOccurrence = 33,
  InvocationOutOfOrder = 34,
  DanglingEnvironment = 35,
  DanglingResource = 36,
  InvalidReadOutcome = 37,
  InvalidReadResult = 38,
  StdinOffsetMismatch = 39,
  InvalidEOFTransition = 40,
  MissingArgumentVector = 41,
  UnsupportedRequiredCapability = 42,
  NonCanonicalCapabilityOrder = 43,
  RequiredCapabilityMismatch = 44,
  TargetOccurrenceCollision = 45,
};
static_assert(static_cast<uint16_t>(ProcessReplayValidationReason::None) == 0 &&
              static_cast<uint16_t>(
                  ProcessReplayValidationReason::TargetOccurrenceCollision) ==
                  45);

const char *toString(ProcessReplayValidationReason Reason);

struct ProcessReplayValidation {
  ProcessReplayValidationReason Reason = ProcessReplayValidationReason::None;
  std::string Detail;
  uint64_t LiteralBytes = 0;
  uint32_t EventCount = 0;

  bool candidateReady() const {
    return Reason == ProcessReplayValidationReason::None;
  }
};

ProcessReplayValidation
validate(const ProcessReplayPlanCandidateV1 &Plan,
         const ProcessReplayValidationLimits &Limits = {});

} // namespace neverd::safety::process_replay

#endif // NEVERD_SAFETY_PROCESSREPLAY_H
