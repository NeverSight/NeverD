#ifndef NEVERD_TEST_FILTER_NAMES_FIXTURE_H
#define NEVERD_TEST_FILTER_NAMES_FIXTURE_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation::test {
inline DriverOptions filterNamesOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.FilterManagerFileNames = true;
  O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
      {"C",
       "\\Device\\HarddiskVolume1",
       {{"\\dir", true, {}}, {"\\dir\\file.txt", false, {1}}}}};
  O.Unload = true;
  return O;
}
inline llvm::json::Object filterNamesScenario() {
  return llvm::json::Object{
      {"windows_version",
       llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
      {"filter_manager_file_names", true},
      {"unload", true},
      {"finite_filesystem",
       llvm::json::Array{llvm::json::Object{
           {"drive", "C"},
           {"device_name", "\\Device\\HarddiskVolume1"},
           {"entries",
            llvm::json::Array{llvm::json::Object{{"path", "\\dir"},
                                                 {"directory", true},
                                                 {"data", ""}},
                              llvm::json::Object{{"path", "\\dir\\file.txt"},
                                                 {"directory", false},
                                                 {"data", "01"}}}}}}}};
}
} // namespace neverd::emulation::test
#endif
