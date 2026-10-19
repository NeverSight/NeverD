///
/// \file
/// CPUIDPolicyTestFixture.
///

#ifndef NEVERD_TESTS_CPUIDPOLICYFIXTURE_H
#define NEVERD_TESTS_CPUIDPOLICYFIXTURE_H
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
namespace neverd::emulation::cpuidpolicyfixture {
inline DriverCPUIDRegisters &entry(DriverOptions &O, uint32_t Leaf,
                                   uint32_t Subleaf) {
  for (auto &E : O.CPUID)
    if (E.Leaf == Leaf && E.Subleaf == Subleaf)
      return E.Registers;
  O.CPUID.push_back({Leaf, Subleaf, {}});
  return O.CPUID.back().Registers;
}
inline void erase(DriverOptions &O, uint32_t Leaf, uint32_t Subleaf) {
  std::erase_if(O.CPUID, [&](const auto &E) {
    return E.Leaf == Leaf && E.Subleaf == Subleaf;
  });
}
inline DriverOptions options() {
  DriverOptions O;
  O.CPUID = {{0, 0, {7, 0x756e6547, 0x6c65746e, 0x49656e69}},
             {1, 0, {0, 0, 0x80000000, 0}},
             {0x80000000, 0, {0x80000008, 0, 0, 0}},
             {7, 0, {2, 0x76543210, 0x89abcdef, 0xf00d0001}},
             {7, 2, {0xa1b2c3d4, 0x11223344, 0x55667788, 0x99aabbcc}},
             {0x40000000, 0, {0x40000001, 0x6576654e, 0x45204472, 0x2020756d}}};
  O.CPUIDPolicy =
      DriverCPUIDPolicy{DriverCPUIDInvalidLeafPolicy::IntelMaxBasicV1,
                        DriverCPUIDHypervisorPolicy::NeverDV1};
  return O;
}
} // namespace neverd::emulation::cpuidpolicyfixture
#endif
