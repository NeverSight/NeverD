//===- DriverNetworkInterfaces.h - Explicit kernel NETIO row observations ===//
///
/// \file
/// Explicit kernel NETIO row observations.
///

#ifndef NEVERD_EMULATION_DRIVERNETWORKINTERFACES_H
#define NEVERD_EMULATION_DRIVERNETWORKINTERFACES_H
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
namespace neverd::emulation {
struct DriverOptions;
inline constexpr unsigned DriverNetworkRowSize = 0x548;
inline constexpr unsigned DriverNetworkMaxRows = 64;
/// Complete public x64 MIB_IF_ROW2 bytes, including explicitly observed zeros.
/// These are API-result observations, not NSI records or packet transports.
struct DriverNetworkInterfaces {
  using Rows = std::vector<std::vector<uint8_t>>;
  // Normal, raw, normal-without-statistics. Omission of a level is unknown.
  std::array<std::optional<Rows>, 3> Levels;
};
llvm::Error validateDriverNetworkInterfaces(const DriverOptions &);
} // namespace neverd::emulation
#endif
