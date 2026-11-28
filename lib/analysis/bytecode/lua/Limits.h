//===- Limits.h - Lua artifact budgets -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One job shares these limits across its prototypes, instructions and exported
/// text.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_LUA_LIMITS_H
#define NEVERD_ANALYSIS_BYTECODE_LUA_LIMITS_H

#include <cstddef>

namespace neverd::lua::limits {
inline constexpr size_t kInputBytes = 512 * 1024;
inline constexpr size_t kItems = 32 * 1024;
inline constexpr size_t kInstructions = 8 * 1024;
inline constexpr size_t kTextBytes = 1024 * 1024;
// Bound the complete JSON document below the worker frame size, including its
// envelope.
inline constexpr size_t kReportBytes = 4 * 1024 * 1024;
inline constexpr unsigned kDepth = 64;
} // namespace neverd::lua::limits
#endif // NEVERD_ANALYSIS_BYTECODE_LUA_LIMITS_H
