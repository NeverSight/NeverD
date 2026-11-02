//===- ExecutionLimits.h - Native transport policy -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_EXECUTIONLIMITS_H
#define NEVERD_EMULATION_CORE_EXECUTIONLIMITS_H
#include <cstdint>

namespace neverd::emulation::execution_limits {
#define NEVERD_EXECUTION_LIMIT(Name, Value)                                    \
  inline constexpr uint64_t Name = Value;
#include "ExecutionLimits.def"
#undef NEVERD_EXECUTION_LIMIT
} // namespace neverd::emulation::execution_limits
#endif
