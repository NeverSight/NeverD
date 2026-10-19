///
/// \file
/// NdisConfigurationTestFixture.
///

#ifndef NEVERD_TEST_NDIS_CONFIGURATION_FIXTURE_H
#define NEVERD_TEST_NDIS_CONFIGURATION_FIXTURE_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation::test {
inline constexpr uint64_t NdisBinding = 0xffffa00000001000ULL;
inline constexpr const char *NdisRegistry =
    "\\Registry\\Machine\\System\\ObservedNdis";
inline DriverOptions ndisConfigurationOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.Registry = std::vector<DriverRegistryKey>{
      {NdisRegistry,
       {{"Answer", 4, {42, 0, 0, 0}},
        {"Text", 1, {'4', 0, '2', 0, 0, 0}},
        {"Binary", 3, {1, 2, 3}},
        {"Multi", 7, {'a', 0, 0, 0, 'b', 0, 0, 0, 0, 0}}}}};
  O.NdisConfiguration =
      DriverNdisConfiguration{{{NdisBinding, NdisRegistry}}, 0x60054};
  O.Unload = true;
  return O;
}
inline llvm::json::Object ndisConfigurationScenario() {
  auto O = ndisConfigurationOptions();
  llvm::json::Array Values;
  for (const auto &V : O.Registry->at(0).Values)
    Values.push_back(llvm::json::Object{{"name", V.Name},
                                        {"type", V.Type},
                                        {"data", llvm::toHex(V.Data, true)}});
  return llvm::json::Object{
      {"windows_version",
       llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
      {"registry", llvm::json::Array{llvm::json::Object{
                       {"path", NdisRegistry}, {"values", std::move(Values)}}}},
      {"ndis_configuration",
       llvm::json::Object{{"version", 0x60054},
                          {"bindings", llvm::json::Array{llvm::json::Object{
                                           {"handle", NdisBinding},
                                           {"registry_path", NdisRegistry}}}}}},
      {"unload", true}};
}
} // namespace neverd::emulation::test
#endif
