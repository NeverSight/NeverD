//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_LIMITS_H
#define NEVERD_SUPPORT_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd {

// include/neverd/support/CaseList.h
/// The shortest consecutive run written as a range.
inline constexpr std::size_t CaseRangeMinimum = 3;

} // namespace neverd

namespace neverd::strings {

// include/neverd/support/StringScan.h
/// The largest minimum length a scan accepts.
inline constexpr unsigned MaxMinLength = 1024;

} // namespace neverd::strings

namespace neverd::limits::support {

// Hashing and copying loaded bytes. This is a checkpoint granularity, not a
// maximum input size or a wall-clock deadline. Keep it a SHA-256 block
// multiple.
inline constexpr size_t kLoadByteCheckpointBytes = 1024 * 1024;

// NDDB storage and streaming. Metadata limits are aggregate admission budgets,
// adjustable through project_database::ReadLimits; input bytes stream without
// a total-size policy ceiling. Exhaustion is an explicit load error.
inline constexpr size_t kProjectDatabaseChunkBytes = 8 * 1024 * 1024;
inline constexpr int kProjectDatabaseCompressionLevel = 6;
inline constexpr size_t kProjectStreamBytes = 64 * 1024;
inline constexpr uint64_t kProjectMetadataBytes = 1024 * 1024;
inline constexpr uint64_t kProjectSidecarBytes = 256 * 1024 * 1024;
inline constexpr uint64_t kProjectStateBytes = 32 * 1024 * 1024;
inline constexpr uint64_t kProjectMetadataRows = 65536;
inline constexpr int kProjectSqlCheckpointOps = 1000;

// lib/support/TextEvidence.cpp
inline constexpr size_t kTextEvidenceMinTableAscentIdeographs = 3;
inline constexpr unsigned kTextEvidenceMinWordLetters = 3;
inline constexpr unsigned kTextEvidenceMinLettersBeyond = 2;
inline constexpr unsigned kTextEvidenceMinDistinctASCIILetters = 3;

// lib/support/TextEvidence.h
/// Text mostly beyond ASCII needs this many characters: two random code
/// points read as text too often.
inline constexpr size_t kTextEvidenceMinTextBeyondASCII = 3;

// lib/support/ByteScan.cpp
inline constexpr int kByteScanLongTextASCIIPrefixSizeLimit = 32;

// lib/support/SortedSearch.cpp
// Keep short searches inline; compare a bounded 128-byte tail in-place.
#if defined(__clang__)
inline constexpr size_t kSortedSearchMinimumSIMDEntries = 64;
#else
// Keep medium ordered inputs on predictable scalar bisection.
inline constexpr size_t kSortedSearchMinimumSIMDEntries = 4096;
#endif
inline constexpr size_t kSortedSearchSIMDTailEntries = 16;
#if defined(__GNUC__) && !defined(__clang__)
// Direct membership changes GCC's small scalar caller loops unfavorably.
// Retain rank-based membership until caller measurements justify switching.
inline constexpr bool kSortedSearchUseDirectMembership = false;
#else
inline constexpr bool kSortedSearchUseDirectMembership = true;
#endif
// Narrow x86 targets lose to scalar binary search on medium/ordered inputs.
inline constexpr size_t kSortedSearchMinimumX86SIMDLanes = 4;

// lib/support/TextEncoding.cpp
inline constexpr int kTextEncodingEscapeInvalidUTF8TextSizeLimit = 64;

// Default display-column length for binary string discovery, shared by every
// surface.
inline constexpr unsigned kDefaultStringMinLength = 4;
} // namespace neverd::limits::support

#endif // NEVERD_SUPPORT_LIMITS_H
