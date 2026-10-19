///
/// \file
/// PciConfigurationTestFixture.
///

#ifndef NEVERD_TEST_PCICONFIGURATION_H
#define NEVERD_TEST_PCICONFIGURATION_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation {
inline DriverOptions pciConfigurationOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  DriverPciConfiguration P;
  P.MaximumBusNumber = 0x203;
  DriverPciFunction D;
  D.Segment = 2;
  D.Bus = 3;
  D.Device = 5;
  D.Configuration.resize(4096);
  for (size_t I = 0; I < D.Configuration.size(); ++I)
    D.Configuration[I] = uint8_t(I);
  D.Configuration[0] = 0x34;
  D.Configuration[1] = 0x12;
  D.Configuration[2] = 0x78;
  D.Configuration[3] = 0x56;
  D.Configuration[14] = 0;
  D.WriteMask = std::vector<uint8_t>(4096);
  D.WriteOneToClearMask = std::vector<uint8_t>(4096);
  (*D.WriteMask)[4] = (*D.WriteMask)[5] = 0xff;
  (*D.WriteOneToClearMask)[6] = 0xf0;
  (*D.WriteOneToClearMask)[7] = 0xff;
  for (size_t I = 0x100; I < 0x110; ++I)
    (*D.WriteMask)[I] = 0xff;
  P.Devices.push_back(std::move(D));
  O.PciConfiguration = std::move(P);
  return O;
}
inline llvm::json::Object pciConfigurationScenario() {
  const auto O = pciConfigurationOptions();
  const auto &D = O.PciConfiguration->Devices[0];
  llvm::json::Array Devices;
  Devices.push_back(llvm::json::Object{
      {"segment", D.Segment},
      {"bus", D.Bus},
      {"device", D.Device},
      {"function", D.Function},
      {"config_hex", llvm::toHex(D.Configuration, true)},
      {"write_mask_hex", llvm::toHex(*D.WriteMask, true)},
      {"write_one_to_clear_hex", llvm::toHex(*D.WriteOneToClearMask, true)}});
  return llvm::json::Object{
      {"unload", true},
      {"windows_version",
       llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
      {"pci_configuration",
       llvm::json::Object{{"minimum_bus_number", 0},
                          {"maximum_bus_number", 0x203},
                          {"devices", std::move(Devices)}}}};
}
} // namespace neverd::emulation
#endif
