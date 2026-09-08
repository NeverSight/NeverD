//===- SemanticFixtureToolProxy.cpp - Fixture tool boundary probe --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

// Test-only compiler/linker proxy. Executes the real tool and can substitute a
// malformed object only at the selected link stage.
#include "TestProcess.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int Argc, char **Argv) {
  const bool Compiler = std::filesystem::path(Argv[0]).stem() == "clang";
  const char *Real = std::getenv(Compiler ? "NEVERD_FIXTURE_REAL_CLANG"
                                          : "NEVERD_FIXTURE_REAL_LLD");
  if (!Real || !*Real) {
    std::cerr << "fixture proxy has no real tool\n";
    return 2;
  }
  const char *Failure = std::getenv("NEVERD_FIXTURE_LINK_FAILURE");
  const std::string Mode = Failure ? Failure : "none";
  std::string Command = neverd::test::shellQuote(Real);
  for (int I = 1; I < Argc; ++I) {
    std::string Arg = Argv[I];
    const bool Recompiled = Arg.ends_with("_recomp.o");
    const bool Replace = !Compiler && Arg.ends_with(".o") &&
                         ((Mode == "recompiled" && Recompiled) ||
                          (Mode == "original" && !Recompiled));
    if (Replace) {
      Arg += ".fixture-invalid-object";
      std::ofstream File(Arg, std::ios::binary);
      File << "THIS_IS_NOT_AN_OBJECT\n";
      File.close();
      if (!File) {
        std::cerr << "fixture proxy could not write malformed object\n";
        return 2;
      }
    }
    Command += " " + neverd::test::shellQuote(Arg);
  }
  const int Status =
      neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
  return Status < 0 ? 2 : Status;
}
