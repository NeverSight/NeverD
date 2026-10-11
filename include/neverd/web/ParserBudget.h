//===- ParserBudget.h - JavaScript parser resource limits --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JavaScript parser resource limits.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace neverd::web {

/// Scope is thread-local because the embedded parser may be used by separate
/// SDK sessions concurrently. No global runtime or signal handler is installed.
class ParserBudget {
public:
  enum class Profile { Interactive, Recovery };
  explicit ParserBudget(Profile Selected = Profile::Interactive);
  ~ParserBudget();
  ParserBudget(const ParserBudget &) = delete;
  void charge(size_t Bytes);

private:
  ParserBudget *Previous;
  uint64_t Bytes = 0;
  uint64_t Work = 0;
  uint64_t MaxBytes = 32 * 1024 * 1024, MaxWork = 200000;
  std::chrono::steady_clock::time_point Deadline;
};

/// Called before AST/identifier allocation by the version-pinned overlay.
void chargeParserAllocation(size_t Bytes);

} // namespace neverd::web
