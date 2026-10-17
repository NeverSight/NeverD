//===- DarwinGuardTestData.h - Original default static guards -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_DARWINGUARDTESTDATA_H
#define NEVERD_UNITTESTS_EMULATION_DARWINGUARDTESTDATA_H
#include "DarwinCallbackTestData.h"
namespace neverd::emulation::darwin_test {
constexpr CallbackCase GuardCases[] = {
    {"return", "LAPBCSMTHRtUYuXVvhspbla", 37},
    {"exit", "LAPBCSMTHRtUYuXVvhspbla", 37},
    {"raw", "LAPBCSMTHR", 38},
    {"global-raw", "LAPBCSMTHRtUYuQ", 38},
    {"ctor-exit", "LAPBCSspbla", 37},
    {"ctor-raw", "LAPBCS", 38}};
inline void guardOptions(ProcessOptions &Options,
                         const std::filesystem::path &Directory,
                         const char *Mode) {
  Options.Arguments = {"different/argv-zero", Mode};
  Options.InstructionQuantum = 1024;
  Options.DarwinSystem.emplace().ThreadSelfPort = 0xf1234567;
  Options.Darwin = DarwinProcessOptions{
      {{"@loader_path/libparent.dylib", Directory / "libparent.dylib"},
       {"@loader_path/libleaf.dylib", Directory / "libleaf.dylib"}}};
}
} // namespace neverd::emulation::darwin_test
#endif
