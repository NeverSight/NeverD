///
/// \file
/// HalDispatchTestFixture.
///

#ifndef NEVERD_TEST_HAL_DISPATCH_FIXTURE_H
#define NEVERD_TEST_HAL_DISPATCH_FIXTURE_H
#include "AcpiTestFixture.h"
#include "McfgTestFixture.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation::test {
inline DriverOptions halDispatchOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.AcpiTables = acpifixture::inventory();
  // Distinct OEM identities make ordered duplicate selection observable.
  auto &Second = O.AcpiTables->Tables[2];
  std::copy_n("OTHER ", 6, Second.begin() + 10);
  std::copy_n("ALT     ", 8, Second.begin() + 16);
  acpifixture::checksum(Second);
  O.AcpiTables->Tables.push_back(mcgffixture::table().Table);
  O.Unload = true;
  return O;
}
inline llvm::json::Object halDispatchScenario() {
  auto O = halDispatchOptions();
  llvm::json::Array Tables;
  for (const auto &T : O.AcpiTables->Tables)
    Tables.push_back(llvm::toHex(T, true));
  return llvm::json::Object{
      {"windows_version",
       llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
      {"acpi_tables", llvm::json::Object{{"tables_hex", std::move(Tables)}}},
      {"unload", true}};
}
} // namespace neverd::emulation::test
#endif
