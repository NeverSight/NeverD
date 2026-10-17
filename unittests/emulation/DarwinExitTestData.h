//===- DarwinExitTestData.h - Process exit controls ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Native-observed global, static and TLS callback ordering.
#ifndef NEVERD_UNITTESTS_EMULATION_DARWINEXITTESTDATA_H
#define NEVERD_UNITTESTS_EMULATION_DARWINEXITTESTDATA_H
#include "DarwinCallbackTestData.h"
namespace neverd::emulation::darwin_test {
constexpr CallbackCase GlobalExitCases[] = {
    {"return", "LPBTGMStvUYLns4Ng30p2l1", 37},
    {"exit", "LPBTGMStvUYLns4Ng30p2l1", 37},
    {"raw", "LPBTGMS", 38},
    {"global-raw", "LPBTGMStvUYR", 38},
    {"deep-exit", "LPBTGMStvUYKLns4Ng30p2l1", 37},
    {"ctor-exit", "LPBTtvUY0p2l1", 37},
    {"ctor-raw", "LPBT", 38},
    {"opaque", "LPBTGMStvUYnnZILns4Ng30p2l1", 37}};
inline void globalExitOptions(ProcessOptions &Options,
                              const std::filesystem::path &Directory,
                              const char *Mode) {
  Options.Arguments = {"different/argv-zero", Mode};
  Options.InstructionQuantum = 1024;
  Options.Darwin = DarwinProcessOptions{
      {{"@loader_path/libparent.dylib", Directory / "libparent.dylib"},
       {"@loader_path/libleaf.dylib", Directory / "libleaf.dylib"}}};
}
} // namespace neverd::emulation::darwin_test
#endif
