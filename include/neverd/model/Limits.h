//===- Limits.h - Local model inspection budgets --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Local model inspection budgets.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_MODEL_LIMITS_H
#define NEVERD_MODEL_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::model::limits {
// Per request. Directory traversal, parser work and retained evidence are
// independently bounded even for many tiny files or nested metadata arrays.
inline constexpr uint64_t kHeaderBytes = 64ULL << 20;
inline constexpr uint64_t kJsonBytes = 16ULL << 20;
inline constexpr uint64_t kTemplateBytes = 1ULL << 20;
inline constexpr uint64_t kNameBytes = 4096;
inline constexpr uint64_t kWork = 4'000'000;
inline constexpr size_t kFiles = 1024;
inline constexpr size_t kEntries = 100'000;
inline constexpr size_t kFindings = 256;
/// Serialized evidence stops before untrusted labels can produce an oversized
/// report.
inline constexpr size_t kReportBytes = 4 << 20;
inline constexpr unsigned kDepth = 64;
inline constexpr size_t kChunk = 64 << 10;
inline constexpr uint64_t kGapBytes = 4ULL << 20;
/// Maximum size of either file in a verified GGUF reconstruction.
inline constexpr uint64_t kRebuildBytes = 64ULL << 30;
inline constexpr uint64_t kCompareBytes = 64ULL << 20;
inline constexpr size_t kPage = 32;
inline constexpr size_t kMaxPage = 512;
} // namespace neverd::model::limits
#endif // NEVERD_MODEL_LIMITS_H
