//===- ProcessReplay.cpp - Process replay plan validation ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ProcessReplay.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <variant>

namespace neverd::safety::process_replay {
namespace {

ProcessReplayValidation failure(ProcessReplayValidationReason Reason,
                                const char *Detail, uint64_t LiteralBytes = 0,
                                uint32_t EventCount = 0) {
  return {Reason, Detail, LiteralBytes, EventCount};
}

ProcessReplayValidationReason
validateOccurrence(const ProcessReplayOccurrence &Occurrence) {
  if (!Occurrence.FuncEntry || !Occurrence.CallVA || !Occurrence.BlockId ||
      !Occurrence.OpIdx || !Occurrence.OriginSeq || !Occurrence.CallSiteId ||
      !Occurrence.Invocation)
    return ProcessReplayValidationReason::IncompleteOccurrence;

  constexpr uint32_t MaxSignedIndex =
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  if (*Occurrence.FuncEntry == std::numeric_limits<uint64_t>::max() ||
      *Occurrence.CallVA == std::numeric_limits<uint64_t>::max() ||
      *Occurrence.BlockId > MaxSignedIndex ||
      *Occurrence.OpIdx > MaxSignedIndex ||
      *Occurrence.OriginSeq > MaxSignedIndex || *Occurrence.CallSiteId == 0 ||
      *Occurrence.Invocation >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return ProcessReplayValidationReason::InvalidOccurrence;
  return ProcessReplayValidationReason::None;
}

bool containsNul(const std::vector<uint8_t> &Bytes) {
  return std::find(Bytes.begin(), Bytes.end(), uint8_t{0}) != Bytes.end();
}

bool checkedCharge(uint64_t &Used, uint64_t Amount, uint64_t Limit) {
  if (Amount > std::numeric_limits<uint64_t>::max() - Used)
    return false;
  Used += Amount;
  return Used <= Limit;
}

/// Trivial result for the allocation-free size preflight.  Diagnostic strings
/// are materialized only after this pass has rejected or bounded every
/// attacker-controlled container and literal.
struct ProcessReplaySizePreflight {
  ProcessReplayValidationReason Reason = ProcessReplayValidationReason::None;
  const char *Detail = "";
  uint64_t LiteralBytes = 0;
  uint32_t EventCount = 0;

  bool ready() const { return Reason == ProcessReplayValidationReason::None; }
};

ProcessReplaySizePreflight sizeFailure(ProcessReplayValidationReason Reason,
                                       const char *Detail,
                                       uint64_t LiteralBytes = 0,
                                       uint32_t EventCount = 0) {
  return {Reason, Detail, LiteralBytes, EventCount};
}

/// Rejects attacker-controlled byte/count budgets before validation creates an
/// owning set, map, vector, or diagnostic string from plan material.  The full
/// validator below deliberately repeats the accounting while checking semantic
/// order so its historical partial-count diagnostics remain unchanged.
ProcessReplaySizePreflight
preflightSizes(const ProcessReplayPlanCandidateV1 &Plan,
               const ProcessReplayValidationLimits &Limits) {
  if (Plan.Arguments.size() > Limits.MaxArguments)
    return sizeFailure(ProcessReplayValidationReason::ArgumentLimitExceeded,
                       "argument count exceeds the validation limit");
  if (Plan.Environment.size() > Limits.MaxEnvironmentEntries)
    return sizeFailure(ProcessReplayValidationReason::EnvironmentLimitExceeded,
                       "environment entry count exceeds the validation limit");
  if (Plan.Resources.size() > Limits.MaxResources)
    return sizeFailure(ProcessReplayValidationReason::ResourceLimitExceeded,
                       "resource count exceeds the validation limit");
  if (Plan.Events.size() > Limits.MaxEvents)
    return sizeFailure(ProcessReplayValidationReason::EventLimitExceeded,
                       "event count exceeds the validation limit");

  uint64_t LiteralBytes = 0;
  for (const ProcessReplayArgument &Argument : Plan.Arguments) {
    const uint64_t ByteCount = static_cast<uint64_t>(Argument.Bytes.size());
    if (ByteCount == std::numeric_limits<uint64_t>::max())
      return sizeFailure(ProcessReplayValidationReason::ArithmeticOverflow,
                         "argument terminator length overflows", LiteralBytes);
    if (!checkedCharge(LiteralBytes, ByteCount + 1, Limits.MaxLiteralBytes))
      return sizeFailure(
          ProcessReplayValidationReason::LiteralByteBudgetExceeded,
          "argument bytes exceed the validation limit", LiteralBytes);
  }

  for (const ProcessReplayEnvironmentEntry &Entry : Plan.Environment) {
    // An absent entry has no value bytes by contract.  Reject the
    // contradiction from vector metadata alone before charging the name or
    // walking attacker-controlled value storage looking for an embedded NUL.
    if (!Entry.Present && !Entry.Value.empty())
      return sizeFailure(ProcessReplayValidationReason::InvalidEnvironmentValue,
                         "environment value contradicts its presence state",
                         LiteralBytes);
    if (!checkedCharge(LiteralBytes, static_cast<uint64_t>(Entry.Name.size()),
                       Limits.MaxLiteralBytes) ||
        !checkedCharge(LiteralBytes, 1, Limits.MaxLiteralBytes) ||
        (Entry.Present &&
         (!checkedCharge(LiteralBytes, 1, Limits.MaxLiteralBytes) ||
          !checkedCharge(LiteralBytes,
                         static_cast<uint64_t>(Entry.Value.size()),
                         Limits.MaxLiteralBytes))))
      return sizeFailure(
          ProcessReplayValidationReason::LiteralByteBudgetExceeded,
          "environment bytes exceed the validation limit", LiteralBytes);
  }

  uint64_t TotalRequestedBytes = 0;
  for (size_t Index = 0; Index < Plan.Events.size(); ++Index) {
    const auto *Read =
        std::get_if<ProcessReplayStdinReadEvent>(&Plan.Events[Index].Payload);
    if (!Read)
      continue;
    const uint32_t EventCount = static_cast<uint32_t>(Index);
    if (Read->RequestedBytes > Limits.MaxReadRequestBytes)
      return sizeFailure(
          ProcessReplayValidationReason::ReadRequestLimitExceeded,
          "stdin request exceeds the per-event limit", LiteralBytes,
          EventCount);
    if (Read->RequestedBytes >
        std::numeric_limits<uint64_t>::max() - TotalRequestedBytes)
      return sizeFailure(ProcessReplayValidationReason::ArithmeticOverflow,
                         "aggregate stdin request bytes overflow", LiteralBytes,
                         EventCount);
    TotalRequestedBytes += Read->RequestedBytes;
    if (TotalRequestedBytes > Limits.MaxTotalReadRequestBytes)
      return sizeFailure(
          ProcessReplayValidationReason::TotalReadRequestBudgetExceeded,
          "aggregate stdin request bytes exceed the validation limit",
          LiteralBytes, EventCount);
    if (!checkedCharge(LiteralBytes, static_cast<uint64_t>(Read->Bytes.size()),
                       Limits.MaxLiteralBytes))
      return sizeFailure(
          ProcessReplayValidationReason::LiteralByteBudgetExceeded,
          "stdin bytes exceed the validation limit", LiteralBytes, EventCount);
  }
  return {ProcessReplayValidationReason::None, "", LiteralBytes,
          static_cast<uint32_t>(Plan.Events.size())};
}

using StaticOccurrenceKey =
    std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>;
using DynamicOccurrenceKey = std::tuple<uint64_t, uint64_t, uint32_t, uint32_t,
                                        uint32_t, uint32_t, uint64_t>;

StaticOccurrenceKey staticKey(const ProcessReplayOccurrence &Occurrence) {
  return {*Occurrence.FuncEntry, *Occurrence.CallVA,    *Occurrence.BlockId,
          *Occurrence.OpIdx,     *Occurrence.OriginSeq, *Occurrence.CallSiteId};
}

DynamicOccurrenceKey dynamicKey(const ProcessReplayOccurrence &Occurrence) {
  return std::tuple_cat(staticKey(Occurrence),
                        std::make_tuple(*Occurrence.Invocation));
}

} // namespace

const char *toString(ProcessReplayValidationReason Reason) {
  switch (Reason) {
  case ProcessReplayValidationReason::None:
    return "none";
  case ProcessReplayValidationReason::UnsupportedVersion:
    return "unsupported_version";
  case ProcessReplayValidationReason::IncompleteTargetIdentity:
    return "incomplete_target_identity";
  case ProcessReplayValidationReason::UnsupportedTargetFormat:
    return "unsupported_target_format";
  case ProcessReplayValidationReason::UnsupportedTargetArchitecture:
    return "unsupported_target_architecture";
  case ProcessReplayValidationReason::UnsupportedTargetBitness:
    return "unsupported_target_bitness";
  case ProcessReplayValidationReason::UnsupportedTargetMode:
    return "unsupported_target_mode";
  case ProcessReplayValidationReason::UnsupportedTargetEndianness:
    return "unsupported_target_endianness";
  case ProcessReplayValidationReason::RelocatableTarget:
    return "relocatable_target";
  case ProcessReplayValidationReason::IncompleteOccurrence:
    return "incomplete_occurrence";
  case ProcessReplayValidationReason::InvalidOccurrence:
    return "invalid_occurrence";
  case ProcessReplayValidationReason::InvalidLimits:
    return "invalid_limits";
  case ProcessReplayValidationReason::ArgumentLimitExceeded:
    return "argument_limit_exceeded";
  case ProcessReplayValidationReason::NonCanonicalArgumentOrder:
    return "noncanonical_argument_order";
  case ProcessReplayValidationReason::InvalidArgument:
    return "invalid_argument";
  case ProcessReplayValidationReason::LiteralByteBudgetExceeded:
    return "literal_byte_budget_exceeded";
  case ProcessReplayValidationReason::ArithmeticOverflow:
    return "arithmetic_overflow";
  case ProcessReplayValidationReason::InvalidTargetAddress:
    return "invalid_target_address";
  case ProcessReplayValidationReason::EnvironmentLimitExceeded:
    return "environment_limit_exceeded";
  case ProcessReplayValidationReason::NonCanonicalEnvironmentOrder:
    return "noncanonical_environment_order";
  case ProcessReplayValidationReason::InvalidEnvironmentName:
    return "invalid_environment_name";
  case ProcessReplayValidationReason::InvalidEnvironmentValue:
    return "invalid_environment_value";
  case ProcessReplayValidationReason::DuplicateEnvironmentName:
    return "duplicate_environment_name";
  case ProcessReplayValidationReason::ResourceLimitExceeded:
    return "resource_limit_exceeded";
  case ProcessReplayValidationReason::EventLimitExceeded:
    return "event_limit_exceeded";
  case ProcessReplayValidationReason::ReadRequestLimitExceeded:
    return "read_request_limit_exceeded";
  case ProcessReplayValidationReason::TotalReadRequestBudgetExceeded:
    return "total_read_request_budget_exceeded";
  case ProcessReplayValidationReason::NonCanonicalResourceOrder:
    return "noncanonical_resource_order";
  case ProcessReplayValidationReason::UnsupportedResourceKind:
    return "unsupported_resource_kind";
  case ProcessReplayValidationReason::DuplicateResource:
    return "duplicate_resource";
  case ProcessReplayValidationReason::UnreferencedResource:
    return "unreferenced_resource";
  case ProcessReplayValidationReason::NonCanonicalEventOrder:
    return "noncanonical_event_order";
  case ProcessReplayValidationReason::UnsupportedEventKind:
    return "unsupported_event_kind";
  case ProcessReplayValidationReason::DuplicateEventOccurrence:
    return "duplicate_event_occurrence";
  case ProcessReplayValidationReason::InvocationOutOfOrder:
    return "invocation_out_of_order";
  case ProcessReplayValidationReason::DanglingEnvironment:
    return "dangling_environment";
  case ProcessReplayValidationReason::DanglingResource:
    return "dangling_resource";
  case ProcessReplayValidationReason::InvalidReadOutcome:
    return "invalid_read_outcome";
  case ProcessReplayValidationReason::InvalidReadResult:
    return "invalid_read_result";
  case ProcessReplayValidationReason::StdinOffsetMismatch:
    return "stdin_offset_mismatch";
  case ProcessReplayValidationReason::InvalidEOFTransition:
    return "invalid_eof_transition";
  case ProcessReplayValidationReason::MissingArgumentVector:
    return "missing_argument_vector";
  case ProcessReplayValidationReason::UnsupportedRequiredCapability:
    return "unsupported_required_capability";
  case ProcessReplayValidationReason::NonCanonicalCapabilityOrder:
    return "noncanonical_capability_order";
  case ProcessReplayValidationReason::RequiredCapabilityMismatch:
    return "required_capability_mismatch";
  case ProcessReplayValidationReason::TargetOccurrenceCollision:
    return "target_occurrence_collision";
  }
  return "unknown";
}

ProcessReplayValidation validate(const ProcessReplayPlanCandidateV1 &Plan,
                                 const ProcessReplayValidationLimits &Limits) {
  if (Plan.Version != kProcessReplaySchemaVersion)
    return failure(ProcessReplayValidationReason::UnsupportedVersion,
                   "process-replay-v1 schema version is required");

  const ProcessReplayTargetIdentity &Target = Plan.Target;
  if (!Target.Format || !Target.Architecture || !Target.Bits || !Target.Mode ||
      !Target.Endianness || !Target.Relocatable || !Target.Base ||
      !Target.Entry || !Target.SHA256)
    return failure(ProcessReplayValidationReason::IncompleteTargetIdentity,
                   "target identity is incomplete");
  if (*Target.Format != BinaryFormat::ELF &&
      *Target.Format != BinaryFormat::MachO)
    return failure(
        ProcessReplayValidationReason::UnsupportedTargetFormat,
        "target format lacks process-replay-v1 POSIX input semantics");
  if (*Target.Architecture != Arch::X64 &&
      *Target.Architecture != Arch::AArch64)
    return failure(
        ProcessReplayValidationReason::UnsupportedTargetArchitecture,
        "target architecture is not a native 64-bit v1 architecture");
  if (*Target.Bits != Bitness::Bits64)
    return failure(ProcessReplayValidationReason::UnsupportedTargetBitness,
                   "target bitness is not 64");
  if (*Target.Mode != InstructionMode::Default)
    return failure(ProcessReplayValidationReason::UnsupportedTargetMode,
                   "target instruction mode is not the native default");
  if (*Target.Endianness != ProcessReplayEndianness::Little)
    return failure(ProcessReplayValidationReason::UnsupportedTargetEndianness,
                   "target endianness is not little-endian");
  if (*Target.Relocatable)
    return failure(ProcessReplayValidationReason::RelocatableTarget,
                   "a relocatable object is not a process target");
  if (*Target.Base == std::numeric_limits<uint64_t>::max() ||
      *Target.Entry == std::numeric_limits<uint64_t>::max())
    return failure(ProcessReplayValidationReason::InvalidTargetAddress,
                   "target base or entry uses the invalid address sentinel");

  const ProcessReplayValidationReason TargetOccurrenceReason =
      validateOccurrence(Plan.TargetOccurrence);
  if (TargetOccurrenceReason != ProcessReplayValidationReason::None)
    return failure(TargetOccurrenceReason,
                   "target call occurrence identity is invalid");

  if (Limits.MaxArguments > kProcessReplayMaxArguments ||
      Limits.MaxEnvironmentEntries > kProcessReplayMaxEnvironmentEntries ||
      Limits.MaxResources > kProcessReplayMaxResources ||
      Limits.MaxEvents > kProcessReplayMaxEvents ||
      Limits.MaxLiteralBytes > kProcessReplayMaxLiteralBytes ||
      Limits.MaxReadRequestBytes > kProcessReplayMaxReadRequestBytes ||
      Limits.MaxTotalReadRequestBytes > kProcessReplayMaxTotalReadRequestBytes)
    return failure(ProcessReplayValidationReason::InvalidLimits,
                   "validation limit exceeds its v1 hard ceiling");
  if (Plan.Arguments.empty())
    return failure(ProcessReplayValidationReason::MissingArgumentVector,
                   "process-replay-v1 requires argv including index zero");

  for (size_t Index = 0; Index < Plan.RequiredCapabilities.size(); ++Index) {
    const ProcessReplayRequiredCapability Capability =
        Plan.RequiredCapabilities[Index];
    switch (Capability) {
    case ProcessReplayRequiredCapability::ArgumentVectorInjection:
    case ProcessReplayRequiredCapability::EnvironmentIsolation:
    case ProcessReplayRequiredCapability::EnvironmentLookupInterposition:
    case ProcessReplayRequiredCapability::StandardInputInterposition:
    case ProcessReplayRequiredCapability::TargetIdentityAuthentication:
    case ProcessReplayRequiredCapability::TargetOccurrenceAttestation:
      break;
    default:
      return failure(
          ProcessReplayValidationReason::UnsupportedRequiredCapability,
          "required capability is unknown to process-replay-v1");
    }
    if (Index != 0 &&
        static_cast<uint8_t>(Plan.RequiredCapabilities[Index - 1]) >=
            static_cast<uint8_t>(Capability))
      return failure(
          ProcessReplayValidationReason::NonCanonicalCapabilityOrder,
          "required capabilities must be strictly increasing and unique");
  }
  const ProcessReplaySizePreflight SizePreflight = preflightSizes(Plan, Limits);
  if (!SizePreflight.ready())
    return failure(SizePreflight.Reason, SizePreflight.Detail,
                   SizePreflight.LiteralBytes, SizePreflight.EventCount);

  uint64_t LiteralBytes = 0;
  for (size_t Index = 0; Index < Plan.Arguments.size(); ++Index) {
    const ProcessReplayArgument &Argument = Plan.Arguments[Index];
    if (Argument.Index != Index)
      return failure(ProcessReplayValidationReason::NonCanonicalArgumentOrder,
                     "argument indexes must be dense and match manifest order",
                     LiteralBytes);
    if (containsNul(Argument.Bytes))
      return failure(ProcessReplayValidationReason::InvalidArgument,
                     "argument bytes contain an embedded NUL", LiteralBytes);

    const uint64_t ByteCount = static_cast<uint64_t>(Argument.Bytes.size());
    if (ByteCount == std::numeric_limits<uint64_t>::max())
      return failure(ProcessReplayValidationReason::ArithmeticOverflow,
                     "argument terminator length overflows", LiteralBytes);
    if (!checkedCharge(LiteralBytes, ByteCount + 1, Limits.MaxLiteralBytes))
      return failure(ProcessReplayValidationReason::LiteralByteBudgetExceeded,
                     "argument bytes exceed the validation limit",
                     LiteralBytes);
  }

  std::set<std::vector<uint8_t>> EnvironmentNames;
  for (size_t Index = 0; Index < Plan.Environment.size(); ++Index) {
    const ProcessReplayEnvironmentEntry &Entry = Plan.Environment[Index];
    if (Entry.Id != Index)
      return failure(
          ProcessReplayValidationReason::NonCanonicalEnvironmentOrder,
          "environment IDs must be dense and match manifest order",
          LiteralBytes);
    if (Entry.Name.empty() || containsNul(Entry.Name) ||
        std::find(Entry.Name.begin(), Entry.Name.end(), uint8_t{'='}) !=
            Entry.Name.end())
      return failure(ProcessReplayValidationReason::InvalidEnvironmentName,
                     "environment name is empty or contains NUL or '='",
                     LiteralBytes);
    if (!EnvironmentNames.insert(Entry.Name).second)
      return failure(ProcessReplayValidationReason::DuplicateEnvironmentName,
                     "environment name is declared more than once",
                     LiteralBytes);
    if ((!Entry.Present && !Entry.Value.empty()) || containsNul(Entry.Value))
      return failure(ProcessReplayValidationReason::InvalidEnvironmentValue,
                     "environment value contradicts its presence state",
                     LiteralBytes);

    if (!checkedCharge(LiteralBytes, static_cast<uint64_t>(Entry.Name.size()),
                       Limits.MaxLiteralBytes) ||
        !checkedCharge(LiteralBytes, 1, Limits.MaxLiteralBytes) ||
        (Entry.Present &&
         (!checkedCharge(LiteralBytes, 1, Limits.MaxLiteralBytes) ||
          !checkedCharge(LiteralBytes,
                         static_cast<uint64_t>(Entry.Value.size()),
                         Limits.MaxLiteralBytes))))
      return failure(ProcessReplayValidationReason::LiteralByteBudgetExceeded,
                     "environment bytes exceed the validation limit",
                     LiteralBytes);
  }

  std::set<ProcessReplayResourceKind> ResourceKinds;
  for (size_t Index = 0; Index < Plan.Resources.size(); ++Index) {
    const ProcessReplayResource &Resource = Plan.Resources[Index];
    if (Resource.Id != Index)
      return failure(ProcessReplayValidationReason::NonCanonicalResourceOrder,
                     "resource IDs must be dense and match manifest order",
                     LiteralBytes);
    if (Resource.Kind != ProcessReplayResourceKind::StandardInput)
      return failure(ProcessReplayValidationReason::UnsupportedResourceKind,
                     "resource kind is not supported by the plan-only slice",
                     LiteralBytes);
    if (!ResourceKinds.insert(Resource.Kind).second)
      return failure(ProcessReplayValidationReason::DuplicateResource,
                     "logical resource kind is declared more than once",
                     LiteralBytes);
  }

  std::set<uint32_t> ReferencedResources;
  std::set<DynamicOccurrenceKey> SeenOccurrences;
  std::map<StaticOccurrenceKey, uint64_t> NextInvocation;
  std::vector<uint64_t> ResourceOffsets(Plan.Resources.size(), 0);
  std::vector<bool> ResourceEOF(Plan.Resources.size(), false);
  uint64_t TotalRequestedBytes = 0;
  bool HasEnvironmentLookup = false;
  bool HasStdinRead = false;
  for (size_t Index = 0; Index < Plan.Events.size(); ++Index) {
    const ProcessReplayEvent &Event = Plan.Events[Index];
    const uint32_t EventCount = static_cast<uint32_t>(Index);
    if (Event.Id != Index)
      return failure(ProcessReplayValidationReason::NonCanonicalEventOrder,
                     "event IDs must be dense and match transcript order",
                     LiteralBytes, EventCount);
    if (Event.Payload.valueless_by_exception() ||
        std::holds_alternative<std::monostate>(Event.Payload))
      return failure(ProcessReplayValidationReason::UnsupportedEventKind,
                     "event has no supported typed payload", LiteralBytes,
                     EventCount);

    const ProcessReplayOccurrence *At = nullptr;
    if (const auto *Lookup =
            std::get_if<ProcessReplayEnvironmentLookupEvent>(&Event.Payload))
      At = &Lookup->At;
    else if (const auto *Read =
                 std::get_if<ProcessReplayStdinReadEvent>(&Event.Payload))
      At = &Read->At;
    if (!At)
      return failure(ProcessReplayValidationReason::UnsupportedEventKind,
                     "event has no supported typed payload", LiteralBytes,
                     EventCount);

    const ProcessReplayValidationReason OccurrenceReason =
        validateOccurrence(*At);
    if (OccurrenceReason != ProcessReplayValidationReason::None)
      return failure(OccurrenceReason, "event occurrence identity is invalid",
                     LiteralBytes, EventCount);
    if (dynamicKey(*At) == dynamicKey(Plan.TargetOccurrence))
      return failure(
          ProcessReplayValidationReason::TargetOccurrenceCollision,
          "an input event and the target use the same dynamic occurrence",
          LiteralBytes, EventCount);
    if (!SeenOccurrences.insert(dynamicKey(*At)).second)
      return failure(ProcessReplayValidationReason::DuplicateEventOccurrence,
                     "event occurrence is repeated", LiteralBytes, EventCount);
    uint64_t &ExpectedInvocation = NextInvocation[staticKey(*At)];
    if (*At->Invocation != ExpectedInvocation)
      return failure(ProcessReplayValidationReason::InvocationOutOfOrder,
                     "event invocation is not dense from zero", LiteralBytes,
                     EventCount);
    ++ExpectedInvocation;

    if (const auto *Lookup =
            std::get_if<ProcessReplayEnvironmentLookupEvent>(&Event.Payload)) {
      if (Lookup->EnvironmentId >= Plan.Environment.size())
        return failure(ProcessReplayValidationReason::DanglingEnvironment,
                       "environment event references no declared entry",
                       LiteralBytes, EventCount);
      HasEnvironmentLookup = true;
      continue;
    }

    const auto &Read = std::get<ProcessReplayStdinReadEvent>(Event.Payload);
    HasStdinRead = true;
    if (Read.ResourceId >= Plan.Resources.size())
      return failure(ProcessReplayValidationReason::DanglingResource,
                     "stdin event references no declared resource",
                     LiteralBytes, EventCount);
    ReferencedResources.insert(Read.ResourceId);
    if (Read.RequestedBytes > Limits.MaxReadRequestBytes)
      return failure(ProcessReplayValidationReason::ReadRequestLimitExceeded,
                     "stdin request exceeds the per-event limit", LiteralBytes,
                     EventCount);
    if (Read.RequestedBytes >
        std::numeric_limits<uint64_t>::max() - TotalRequestedBytes)
      return failure(ProcessReplayValidationReason::ArithmeticOverflow,
                     "aggregate stdin request bytes overflow", LiteralBytes,
                     EventCount);
    TotalRequestedBytes += Read.RequestedBytes;
    if (TotalRequestedBytes > Limits.MaxTotalReadRequestBytes)
      return failure(
          ProcessReplayValidationReason::TotalReadRequestBudgetExceeded,
          "aggregate stdin request bytes exceed the validation limit",
          LiteralBytes, EventCount);

    if (Read.ReturnedBytes != Read.Bytes.size() ||
        Read.ReturnedBytes > Read.RequestedBytes)
      return failure(ProcessReplayValidationReason::InvalidReadResult,
                     "stdin return count disagrees with request or bytes",
                     LiteralBytes, EventCount);
    if (Read.ReturnedBytes > std::numeric_limits<uint64_t>::max() - Read.Offset)
      return failure(ProcessReplayValidationReason::ArithmeticOverflow,
                     "stdin returned range overflows", LiteralBytes,
                     EventCount);
    if (Read.Offset != ResourceOffsets[Read.ResourceId])
      return failure(ProcessReplayValidationReason::StdinOffsetMismatch,
                     "stdin event offset does not match the logical cursor",
                     LiteralBytes, EventCount);

    const bool WasEOF = ResourceEOF[Read.ResourceId];
    switch (Read.Outcome) {
    case ProcessReplayReadOutcome::Data:
      if (Read.RequestedBytes == 0 || Read.ReturnedBytes == 0)
        return failure(ProcessReplayValidationReason::InvalidReadResult,
                       "data outcome has no returned data", LiteralBytes,
                       EventCount);
      if (WasEOF)
        return failure(ProcessReplayValidationReason::InvalidEOFTransition,
                       "stdin data appears after EOF", LiteralBytes,
                       EventCount);
      break;
    case ProcessReplayReadOutcome::EndOfFile:
      if (Read.RequestedBytes == 0 || Read.ReturnedBytes != 0 ||
          !Read.Bytes.empty() || !Read.EOFAfter)
        return failure(ProcessReplayValidationReason::InvalidReadResult,
                       "EOF outcome has inconsistent counts or state",
                       LiteralBytes, EventCount);
      break;
    case ProcessReplayReadOutcome::ZeroLength:
      if (Read.RequestedBytes != 0 || Read.ReturnedBytes != 0 ||
          !Read.Bytes.empty())
        return failure(ProcessReplayValidationReason::InvalidReadResult,
                       "zero-length outcome has nonzero counts or bytes",
                       LiteralBytes, EventCount);
      if (Read.EOFAfter != WasEOF)
        return failure(ProcessReplayValidationReason::InvalidEOFTransition,
                       "zero-length read changes EOF state", LiteralBytes,
                       EventCount);
      break;
    default:
      return failure(ProcessReplayValidationReason::InvalidReadOutcome,
                     "stdin event has an unknown outcome", LiteralBytes,
                     EventCount);
    }

    ResourceOffsets[Read.ResourceId] = Read.Offset + Read.ReturnedBytes;
    ResourceEOF[Read.ResourceId] = Read.EOFAfter;
    if (!checkedCharge(LiteralBytes, Read.ReturnedBytes,
                       Limits.MaxLiteralBytes))
      return failure(ProcessReplayValidationReason::LiteralByteBudgetExceeded,
                     "stdin bytes exceed the validation limit", LiteralBytes,
                     EventCount);
  }

  if (ReferencedResources.size() != Plan.Resources.size())
    return failure(ProcessReplayValidationReason::UnreferencedResource,
                   "declared resource has no transcript event", LiteralBytes,
                   static_cast<uint32_t>(Plan.Events.size()));

  std::vector<ProcessReplayRequiredCapability> ExpectedCapabilities = {
      ProcessReplayRequiredCapability::ArgumentVectorInjection,
      ProcessReplayRequiredCapability::EnvironmentIsolation};
  if (HasEnvironmentLookup)
    ExpectedCapabilities.push_back(
        ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
  if (HasStdinRead)
    ExpectedCapabilities.push_back(
        ProcessReplayRequiredCapability::StandardInputInterposition);
  ExpectedCapabilities.push_back(
      ProcessReplayRequiredCapability::TargetIdentityAuthentication);
  ExpectedCapabilities.push_back(
      ProcessReplayRequiredCapability::TargetOccurrenceAttestation);
  if (Plan.RequiredCapabilities != ExpectedCapabilities)
    return failure(ProcessReplayValidationReason::RequiredCapabilityMismatch,
                   "required capabilities do not match the typed plan",
                   LiteralBytes, static_cast<uint32_t>(Plan.Events.size()));

  return {ProcessReplayValidationReason::None,
          {},
          LiteralBytes,
          static_cast<uint32_t>(Plan.Events.size())};
}

} // namespace neverd::safety::process_replay
