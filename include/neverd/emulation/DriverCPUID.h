//===- DriverCPUID.h - Explicit guest CPUID resolution policies
//------------===//
///
/// \file
/// Explicit guest CPUID resolution policies.
///

#ifndef NEVERD_EMULATION_DRIVERCPUID_H
#define NEVERD_EMULATION_DRIVERCPUID_H
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace neverd::emulation {
struct DriverOptions;
using DriverCPUIDRegisters = std::array<uint32_t, 4>;
using DriverCPUIDTable =
    std::map<std::pair<uint32_t, uint32_t>, DriverCPUIDRegisters>;
enum class DriverCPUIDInvalidLeafPolicy { Strict, IntelMaxBasicV1 };
enum class DriverCPUIDHypervisorPolicy { Observed, NeverDV1 };
struct DriverCPUIDPolicy {
  DriverCPUIDInvalidLeafPolicy InvalidLeaf =
      DriverCPUIDInvalidLeafPolicy::Strict;
  DriverCPUIDHypervisorPolicy Hypervisor =
      DriverCPUIDHypervisorPolicy::Observed;
};
class DriverCPUIDModel {
public:
  static llvm::Expected<DriverCPUIDModel>
  fromDriverOptions(const DriverOptions &);
  static llvm::Expected<DriverCPUIDModel>
  create(const DriverCPUIDTable &Observations,
         const std::optional<DriverCPUIDPolicy> &Policy);
  std::optional<DriverCPUIDRegisters> query(uint32_t Leaf,
                                            uint32_t Subleaf) const;
  bool enabled() const { return !Observations.empty(); }
  static DriverCPUIDRegisters neverDV1(uint32_t Leaf);

private:
  DriverCPUIDTable Observations;
  std::map<uint32_t, DriverCPUIDRegisters> WildcardObservations;
  DriverCPUIDPolicy Policy;
  uint32_t MaximumBasic = 0, MaximumExtended = 0;
  bool invalidArchitecturalLeaf(uint32_t Leaf) const;
};
} // namespace neverd::emulation
#endif
