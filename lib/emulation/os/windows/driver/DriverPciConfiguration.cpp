///
/// \file
/// DriverPciConfiguration.
///

#include "neverd/emulation/DriverPciConfiguration.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <set>
#include <tuple>
namespace neverd::emulation {
llvm::Error validateDriverPciConfiguration(const DriverOptions &O) {
  if (!O.PciConfiguration)
    return llvm::Error::success();
  auto Invalid = [](const llvm::Twine &Text) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pci_configuration: " + Text);
  };
  if (!O.Version || O.Version->Major != 10 || O.Version->Minor != 0 ||
      O.Version->Build != 19041)
    return Invalid("requires windows_version 10.0.19041");
  const auto &P = *O.PciConfiguration;
  if (P.MinimumBusNumber > P.MaximumBusNumber ||
      P.MaximumBusNumber > 0xffffff || P.Devices.size() > MaxDriverPciFunctions)
    return Invalid(
        "requires bounded encoded bus numbers and at most 32 functions");
  std::set<std::tuple<uint16_t, uint8_t, uint8_t, uint8_t>> Seen;
  for (const auto &D : P.Devices) {
    const uint32_t Bus = (uint32_t(D.Segment) << 8) | D.Bus;
    if (D.Device > 31 || D.Function > 7 || Bus < P.MinimumBusNumber ||
        Bus > P.MaximumBusNumber ||
        !Seen.emplace(D.Segment, D.Bus, D.Device, D.Function).second)
      return Invalid(
          "requires distinct valid BDF identities inside the bus window");
    if (D.Configuration.size() != DriverPciConfigurationBytes ||
        (D.Configuration[0] == 255 && D.Configuration[1] == 255) ||
        (D.Configuration[14] & 0x7f) > 1)
      return Invalid(
          "requires a complete 4096-byte present type-0/type-1 function");
    if (D.WriteMask.has_value() != D.WriteOneToClearMask.has_value())
      return Invalid("both write masks must be supplied together");
    if (D.WriteMask) {
      if (D.WriteMask->size() != DriverPciConfigurationBytes ||
          D.WriteOneToClearMask->size() != DriverPciConfigurationBytes)
        return Invalid("write masks require exactly 4096 bytes each");
      for (size_t I = 0; I < DriverPciConfigurationBytes; ++I) {
        const auto R = (*D.WriteMask)[I], C = (*D.WriteOneToClearMask)[I];
        if (R & C)
          return Invalid("ordinary and write-one-to-clear bits cannot overlap");
        if (I < 64 && (I < 4 || I >= 8) && (R | C))
          return Invalid("header writes outside command/status need additional "
                         "device semantics");
      }
    }
  }
  for (const auto &D : P.Devices)
    if (D.Function) {
      const auto Base =
          std::find_if(P.Devices.begin(), P.Devices.end(), [&](const auto &B) {
            return B.Segment == D.Segment && B.Bus == D.Bus &&
                   B.Device == D.Device && !B.Function;
          });
      if (Base == P.Devices.end() || !(Base->Configuration[14] & 0x80))
        return Invalid("additional functions require a declared multifunction "
                       "function zero");
    }
  return llvm::Error::success();
}
} // namespace neverd::emulation
