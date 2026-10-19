//===- DriverBugCheck.h - Guest crash request observation ----------------===//
// NeverD Decompiler
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest crash request observation.
///

#ifndef NEVERD_EMULATION_DRIVERBUGCHECK_H
#define NEVERD_EMULATION_DRIVERBUGCHECK_H
#include <array>
#include <cstdint>
#include <optional>
namespace neverd::emulation {
struct DriverBugCheck {
  uint32_t Code = 0;
  /// Only KeBugCheckEx supplies four public parameters. The legacy one-argument
  /// entry leaves other ABI slots unspecified; never invent zero arguments.
  std::optional<std::array<uint64_t, 4>> Parameters;
};
} // namespace neverd::emulation
#endif
