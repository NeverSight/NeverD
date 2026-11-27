//===- Limits.h - JVM artifact resource limits ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounds for one immutable input and its ordered local dependency set.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_JVM_LIMITS_H
#define NEVERD_ANALYSIS_BYTECODE_JVM_LIMITS_H
#include "neverd/analysis/JVMVersions.h"

#include <cstddef>
namespace neverd::jvm::limits {
inline constexpr size_t InputBytes = 32 * 1024 * 1024;
inline constexpr size_t ExpandedBytes = 64 * 1024 * 1024;
inline constexpr size_t MemberBytes = 4 * 1024 * 1024;
inline constexpr size_t Items = 512 * 1024;
inline constexpr size_t MethodWork = 256 * 1024;
// Bound repeated references to the same UTF-8 pool entry and textual IR.
inline constexpr size_t MaterializedBytes = 32 * 1024 * 1024;
inline constexpr size_t CachedMemberBytes = 4 * 1024 * 1024;
inline constexpr size_t CachedItems = 64 * 1024;
inline constexpr size_t ReportBytes = 4 * 1024 * 1024;
inline constexpr size_t Members = 4096;
inline constexpr unsigned Dependencies = 32;
inline constexpr unsigned Nesting = 32;
inline constexpr unsigned Releases = LastRelease;
} // namespace neverd::jvm::limits
#endif
