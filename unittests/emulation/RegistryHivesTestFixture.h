///
/// \file
/// RegistryHivesTestFixture.
///

#ifndef NEVERD_TEST_REGISTRY_HIVES_FIXTURE_H
#define NEVERD_TEST_REGISTRY_HIVES_FIXTURE_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation::test {
inline DriverOptions registryHivesOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.Registry = std::vector<DriverRegistryKey>{
      {"\\Registry\\Machine\\Software\\A", {{"Mode", 4, {1, 0, 0, 0}}}},
      {"\\Registry\\Machine\\Software\\B", {{"Mode", 4, {2, 0, 0, 0}}}},
      {"\\Registry\\Machine\\System\\C", {{"Mode", 4, {3, 0, 0, 0}}}}};
  O.RegistryHives = std::vector<DriverRegistryHive>{
      {"\\Registry\\Machine\\Software"}, {"\\Registry\\Machine\\System"}};
  O.Unload = true;
  return O;
}
inline llvm::json::Object registryHivesScenario() {
  const auto O = registryHivesOptions();
  llvm::json::Array Keys, Hives;
  for (const auto &K : *O.Registry) {
    const char *Hex = K.Values[0].Data[0] == 1   ? "01000000"
                      : K.Values[0].Data[0] == 2 ? "02000000"
                                                 : "03000000";
    Keys.push_back(llvm::json::Object{
        {"path", K.Path},
        {"values", llvm::json::Array{llvm::json::Object{
                       {"name", "Mode"}, {"type", 4}, {"data", Hex}}}}});
  }
  for (const auto &H : *O.RegistryHives)
    Hives.push_back(
        llvm::json::Object{{"path", H.Path}, {"storage", "memory-backed"}});
  return llvm::json::Object{
      {"windows_version",
       llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
      {"registry", std::move(Keys)},
      {"registry_hives", std::move(Hives)},
      {"unload", true}};
}
} // namespace neverd::emulation::test
#endif
