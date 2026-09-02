//===- RuntimeSanitizer.cpp - Strict counted-write guard plans -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/RuntimeSanitizer.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::safety;
using namespace neverd::safety_callsite_md;

namespace {

constexpr uint32_t MaxSignedIndex =
    static_cast<uint32_t>(std::numeric_limits<int32_t>::max());

bool isNativePublicationFormat(BinaryFormat Format) {
  return Format == BinaryFormat::ELF || Format == BinaryFormat::COFF ||
         Format == BinaryFormat::MachO;
}

bool validOccurrence(const SafetyCallsiteOccurrence &Occurrence) {
  return Occurrence.FuncEntry != InvalidVA && Occurrence.CallVA != InvalidVA &&
         Occurrence.BlockId <= MaxSignedIndex &&
         Occurrence.OpIdx <= MaxSignedIndex &&
         Occurrence.OriginSeq <= MaxSignedIndex && Occurrence.CallSiteId != 0;
}

std::optional<SafetyCallsiteOccurrence> occurrence(const Finding &F) {
  if (F.FuncEntry == InvalidVA || F.CallVA == InvalidVA || F.BlockId < 0 ||
      F.OpIdx < 0 || F.OriginSeq < 0 || F.CallSiteId == 0)
    return std::nullopt;
  return SafetyCallsiteOccurrence{F.FuncEntry,
                                  F.CallVA,
                                  static_cast<uint32_t>(F.BlockId),
                                  static_cast<uint32_t>(F.OpIdx),
                                  static_cast<uint32_t>(F.OriginSeq),
                                  F.CallSiteId};
}

void unsupported(RuntimeSanitizerPlan &Plan,
                 RuntimeSanitizerUnsupportedReason Reason,
                 std::optional<SafetyCallsiteOccurrence> Occurrence,
                 std::string Detail) {
  Plan.Unsupported.push_back(
      {Reason, std::move(Occurrence), std::move(Detail)});
}

RuntimeSanitizerUnsupportedReason unsupportedFindingReason(const Finding &F) {
  if (F.Class == VulnClass::UseAfterFree)
    return RuntimeSanitizerUnsupportedReason::UnsupportedUseAfterFree;
  if (F.Class == VulnClass::FormatString || F.Kind == SinkKind::Format)
    return RuntimeSanitizerUnsupportedReason::UnsupportedFormatWrite;
  if (F.Class == VulnClass::BufferOverflow && F.Kind == SinkKind::Copy)
    return RuntimeSanitizerUnsupportedReason::UnsupportedStringWrite;
  return RuntimeSanitizerUnsupportedReason::UnsupportedFinding;
}

llvm::StringRef semanticName(const Finding &F) {
  return F.Sink.empty() ? llvm::StringRef(F.Name) : llvm::StringRef(F.Sink);
}

} // namespace

const char *neverd::safety::toString(RuntimeSanitizerUnsupportedReason Reason) {
  switch (Reason) {
  case RuntimeSanitizerUnsupportedReason::InvalidIdentity:
    return "invalid_identity";
  case RuntimeSanitizerUnsupportedReason::DuplicateAnalyzedSite:
    return "duplicate_analyzed_site";
  case RuntimeSanitizerUnsupportedReason::DuplicateMetadata:
    return "duplicate_metadata";
  case RuntimeSanitizerUnsupportedReason::MissingMetadata:
    return "missing_metadata";
  case RuntimeSanitizerUnsupportedReason::StaleMetadata:
    return "stale_metadata";
  case RuntimeSanitizerUnsupportedReason::UnknownCapacity:
    return "unknown_capacity";
  case RuntimeSanitizerUnsupportedReason::InexactCapacity:
    return "inexact_capacity";
  case RuntimeSanitizerUnsupportedReason::UnsupportedBinaryFormat:
    return "unsupported_binary_format";
  case RuntimeSanitizerUnsupportedReason::UnsupportedStringWrite:
    return "unsupported_string_write";
  case RuntimeSanitizerUnsupportedReason::UnsupportedFormatWrite:
    return "unsupported_format_write";
  case RuntimeSanitizerUnsupportedReason::UnsupportedUseAfterFree:
    return "unsupported_use_after_free";
  case RuntimeSanitizerUnsupportedReason::UnsupportedFinding:
    return "unsupported_finding";
  case RuntimeSanitizerUnsupportedReason::SemanticKindMismatch:
    return "semantic_kind_mismatch";
  case RuntimeSanitizerUnsupportedReason::OperandMismatch:
    return "operand_mismatch";
  case RuntimeSanitizerUnsupportedReason::ElementWidthMismatch:
    return "element_width_mismatch";
  }
  return "unknown";
}

RuntimeSanitizerPlan neverd::safety::planRuntimeSanitizer(
    BinaryFormat Format, llvm::ArrayRef<Finding> Findings,
    llvm::ArrayRef<SafetyCallsiteRecord> Records) {
  RuntimeSanitizerPlan Plan;
  if (!isNativePublicationFormat(Format)) {
    unsupported(
        Plan, RuntimeSanitizerUnsupportedReason::UnsupportedBinaryFormat,
        std::nullopt, "strict counted-write guards require PE, ELF, or Mach-O");
    return Plan;
  }

  std::vector<std::optional<SafetyCallsiteOccurrence>> FindingOccurrences;
  FindingOccurrences.reserve(Findings.size());
  std::vector<bool> DuplicateFindings(Findings.size(), false);
  for (size_t Index = 0; Index < Findings.size(); ++Index) {
    FindingOccurrences.push_back(occurrence(Findings[Index]));
    if (!FindingOccurrences.back()) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::InvalidIdentity,
                  std::nullopt,
                  "finding has an incomplete instrumentation occurrence");
      continue;
    }
    for (size_t Earlier = 0; Earlier < Index; ++Earlier) {
      if (!FindingOccurrences[Earlier] ||
          *FindingOccurrences[Earlier] != *FindingOccurrences[Index])
        continue;
      if (!DuplicateFindings[Earlier])
        unsupported(Plan,
                    RuntimeSanitizerUnsupportedReason::DuplicateAnalyzedSite,
                    FindingOccurrences[Index],
                    "multiple findings claim the same call occurrence");
      DuplicateFindings[Earlier] = true;
      DuplicateFindings[Index] = true;
      break;
    }
  }

  std::vector<bool> ValidRecords(Records.size(), false);
  std::vector<bool> DuplicateRecords(Records.size(), false);
  std::vector<bool> ConsumedRecords(Records.size(), false);
  for (size_t Index = 0; Index < Records.size(); ++Index) {
    if (!validOccurrence(Records[Index].Occurrence)) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::InvalidIdentity,
                  Records[Index].Occurrence,
                  "metadata has an incomplete instrumentation occurrence");
      ConsumedRecords[Index] = true;
      continue;
    }
    ValidRecords[Index] = true;
    for (size_t Earlier = 0; Earlier < Index; ++Earlier) {
      if (!ValidRecords[Earlier] ||
          Records[Earlier].Occurrence != Records[Index].Occurrence)
        continue;
      if (!DuplicateRecords[Earlier])
        unsupported(Plan, RuntimeSanitizerUnsupportedReason::DuplicateMetadata,
                    Records[Index].Occurrence,
                    "multiple metadata records claim the same occurrence");
      DuplicateRecords[Earlier] = true;
      DuplicateRecords[Index] = true;
      break;
    }
  }

  for (size_t Index = 0; Index < Findings.size(); ++Index) {
    if (!FindingOccurrences[Index])
      continue;
    const SafetyCallsiteOccurrence &Site = *FindingOccurrences[Index];
    if (DuplicateFindings[Index]) {
      for (size_t RecordIndex = 0; RecordIndex < Records.size(); ++RecordIndex)
        if (ValidRecords[RecordIndex] &&
            Records[RecordIndex].Occurrence == Site)
          ConsumedRecords[RecordIndex] = true;
      continue;
    }

    const Finding &F = Findings[Index];
    const std::optional<counted_write::Semantics> Expected =
        counted_write::classify(semanticName(F), Format);
    if (!Expected) {
      unsupported(Plan, unsupportedFindingReason(F), Site,
                  "finding is not an exact counted-write semantic");
      continue;
    }

    size_t Match = Records.size();
    unsigned MatchCount = 0;
    for (size_t RecordIndex = 0; RecordIndex < Records.size(); ++RecordIndex) {
      if (!ValidRecords[RecordIndex] || Records[RecordIndex].Occurrence != Site)
        continue;
      ConsumedRecords[RecordIndex] = true;
      Match = RecordIndex;
      ++MatchCount;
    }
    if (MatchCount == 0) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::MissingMetadata,
                  Site, "counted-write finding has no emitted metadata");
      continue;
    }
    if (MatchCount != 1)
      continue; // DuplicateMetadata was emitted during record validation.

    const SafetyCallsiteRecord &Record = Records[Match];
    if (!F.Capacity) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::UnknownCapacity,
                  Site, "remaining destination capacity is unknown");
      continue;
    }
    if (F.CapacityKind != CapacityPrecision::TypedBufferExact ||
        !F.CapacityExact) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::InexactCapacity,
                  Site,
                  "remaining destination capacity is only an upper bound");
      continue;
    }
    if (Record.Kind != Expected->Kind) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::SemanticKindMismatch,
                  Site, "finding semantic kind differs from emitted metadata");
      continue;
    }
    if (F.ArgIndex != static_cast<int>(Expected->LengthOperandIndex) ||
        Record.DestinationOperandIndex != Expected->DestinationOperandIndex ||
        Record.LengthOperandIndex != Expected->LengthOperandIndex) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::OperandMismatch,
                  Site, "finding or metadata has a different operand layout");
      continue;
    }
    if (Record.ElementBytes != Expected->ElementBytes) {
      unsupported(Plan, RuntimeSanitizerUnsupportedReason::ElementWidthMismatch,
                  Site,
                  "metadata element width differs from binary ABI policy");
      continue;
    }

    Plan.Guards.push_back({kRuntimeSanitizerPlanVersion, Site, Record.Kind,
                           *F.Capacity, Record.DestinationOperandIndex,
                           Record.LengthOperandIndex, Record.ElementBytes});
  }

  for (size_t Index = 0; Index < Records.size(); ++Index) {
    if (!ValidRecords[Index] || DuplicateRecords[Index] ||
        ConsumedRecords[Index])
      continue;
    unsupported(Plan, RuntimeSanitizerUnsupportedReason::StaleMetadata,
                Records[Index].Occurrence,
                "emitted metadata has no exact analyzed finding");
  }

  Plan.Complete = Plan.Unsupported.empty();
  return Plan;
}
