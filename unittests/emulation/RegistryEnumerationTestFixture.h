//===- RegistryEnumerationTestFixture.h - Explicit ordered key observations
//===//
///
/// \file
/// Explicit ordered key observations.
///

#ifndef NEVERD_TEST_REGISTRY_ENUMERATION_FIXTURE_H
#define NEVERD_TEST_REGISTRY_ENUMERATION_FIXTURE_H
#include "neverd/emulation/DriverSession.h"
namespace neverd::emulation::test {
inline DriverOptions registryEnumerationOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  const std::string Root = "\\Registry\\Machine\\Software\\NeverDEnum";
  DriverRegistryKey Parent{Root, {}};
  Parent.SubkeyOrder = std::vector<std::string>{"Zulu", "Alpha"};
  DriverRegistryKey A{Root + "\\Alpha",
                      {{"Mode", 4, {1, 2, 3, 4}}, {"Blob", 3, {5, 6, 7}}}};
  A.Information = DriverRegistryKeyInformation{0x1122334455667788, ""};
  A.SubkeyOrder = std::vector<std::string>{};
  DriverRegistryKey Z{Root + "\\Zulu", {}};
  Z.Information = DriverRegistryKeyInformation{0x8000000000000001, ""};
  Z.SubkeyOrder = std::vector<std::string>{};
  O.Registry = std::vector<DriverRegistryKey>{std::move(Parent), std::move(A),
                                              std::move(Z)};
  return O;
}
} // namespace neverd::emulation::test
#endif
