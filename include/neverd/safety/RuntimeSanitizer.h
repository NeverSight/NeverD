//===- RuntimeSanitizer.h - Strict counted-write guard plans ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Builds a fail-closed, versioned plan for guarding exact-capacity native
/// writes.  This layer neither mutates LLVM nor publishes a binary: it binds
/// safety findings to persistent emitter records and describes the guards a
/// later last-mutation pass must install.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_RUNTIMESANITIZER_H
#define NEVERD_SAFETY_RUNTIMESANITIZER_H

#include "neverd/backend/llvm/SafetyCallsiteMetadata.h"
#include "neverd/safety/CountedWriteSemantics.h"
#include "neverd/safety/SafetyTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::safety {

inline constexpr uint32_t kRuntimeSanitizerPlanVersion = 1;

enum class RuntimeSanitizerUnsupportedReason : uint8_t {
  InvalidIdentity,
  DuplicateAnalyzedSite,
  DuplicateMetadata,
  MissingMetadata,
  StaleMetadata,
  UnknownCapacity,
  InexactCapacity,
  UnsupportedBinaryFormat,
  UnsupportedStringWrite,
  UnsupportedFormatWrite,
  UnsupportedUseAfterFree,
  UnsupportedFinding,
  SemanticKindMismatch,
  OperandMismatch,
  ElementWidthMismatch,
};

const char *toString(RuntimeSanitizerUnsupportedReason Reason);

struct RuntimeSanitizerGuard {
  uint32_t Version = kRuntimeSanitizerPlanVersion;
  safety_callsite_md::SafetyCallsiteOccurrence Occurrence;
  safety_callsite_md::SemanticKind Kind =
      safety_callsite_md::SemanticKind::Memcpy;
  /// Finding::Capacity is already relative to an interior destination.
  uint64_t RemainingCapacity = 0;
  uint32_t DestinationOperandIndex = 0;
  uint32_t LengthOperandIndex = 0;
  /// Runtime byte count is checked as length * ElementBytes with overflow.
  uint32_t ElementBytes = 1;
};

struct RuntimeSanitizerUnsupported {
  RuntimeSanitizerUnsupportedReason Reason =
      RuntimeSanitizerUnsupportedReason::UnsupportedFinding;
  std::optional<safety_callsite_md::SafetyCallsiteOccurrence> Occurrence;
  std::string Detail;
};

struct RuntimeSanitizerPlan {
  uint32_t Version = kRuntimeSanitizerPlanVersion;
  /// True only when every finding and every emitted record mapped exactly once.
  bool Complete = false;
  std::vector<RuntimeSanitizerGuard> Guards;
  std::vector<RuntimeSanitizerUnsupported> Unsupported;
};

RuntimeSanitizerPlan planRuntimeSanitizer(
    BinaryFormat Format, llvm::ArrayRef<Finding> Findings,
    llvm::ArrayRef<safety_callsite_md::SafetyCallsiteRecord> Records);

} // namespace neverd::safety

#endif // NEVERD_SAFETY_RUNTIMESANITIZER_H
