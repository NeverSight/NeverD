#pragma once

#include <cstdint>

namespace neverd::web {

/// Admission limits are charged before allocation and shared by every member
/// in an input tree. Immutable originals use bounded private disk storage;
/// text/AST consumers have separate, smaller materialization budgets.
struct Limits {
  uint64_t MaxInputBytes = 512ULL * 1024 * 1024;
  uint64_t MaxMemberBytes = 256ULL * 1024 * 1024;
  uint64_t MaxEntries = 10000;
  uint64_t MaxDepth = 64;

  static constexpr uint64_t HardInputBytes = 512ULL * 1024 * 1024;
  static constexpr uint64_t HardMemberBytes = 256ULL * 1024 * 1024;
  static constexpr uint64_t HardEntries = 10000;
  static constexpr uint64_t HardDepth = 64;
};

} // namespace neverd::web
