///
/// \file
/// ServiceImageTestFixture.
///

#ifndef NEVERD_TEST_SERVICEIMAGETESTFIXTURE_H
#define NEVERD_TEST_SERVICEIMAGETESTFIXTURE_H
#include "neverd/emulation/DriverSession.h"
namespace neverd::emulation {
inline std::filesystem::path serviceFixture(llvm::StringRef Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) / (Name.str() + ".sys");
}
inline DriverOptions serviceOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.Unload = true;
  std::vector<DriverRegistryKey> Keys;
  std::vector<DriverServiceImage> Images;
  for (const auto &[Name, File, Base] :
       {std::tuple{"ServiceChild", "driver_service_child", 0x1a0000000ULL},
        std::tuple{"ServiceFailure", "driver_service_fail", 0x1b0000000ULL}}) {
    const auto Path =
        "\\SystemRoot\\System32\\drivers\\" + std::string(Name) + ".sys";
    DriverRegistryValue V{"ImagePath", 1, {}};
    for (auto C : Path) {
      V.Data.push_back(uint8_t(C));
      V.Data.push_back(0);
    }
    V.Data.push_back(0);
    V.Data.push_back(0);
    Keys.push_back(
        {"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\" +
             std::string(Name),
         {{"Type", 4, {1, 0, 0, 0}}, V}});
    Images.push_back({Name, Path, serviceFixture(File), Base});
  }
  O.Registry = std::move(Keys);
  O.ServiceImages = std::move(Images);
  return O;
}
} // namespace neverd::emulation
#endif
