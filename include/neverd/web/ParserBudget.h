#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace neverd::web {

/// Scope is thread-local because the embedded parser may be used by separate
/// SDK sessions concurrently. No global runtime or signal handler is installed.
class ParserBudget {
public:
  ParserBudget();
  ~ParserBudget();
  ParserBudget(const ParserBudget &) = delete;
  void charge(size_t Bytes);

private:
  ParserBudget *Previous;
  uint64_t Bytes = 0;
  uint64_t Work = 0;
  std::chrono::steady_clock::time_point Deadline;
};

/// Called before AST/identifier allocation by the version-pinned overlay.
void chargeParserAllocation(size_t Bytes);

} // namespace neverd::web
