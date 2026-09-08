//===- NeverDCmdMobile.cpp - Mobile application source recovery ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace llvm;

namespace neverd::cli {

int runMobile(const char *Argv0) {
  if (OutputFile.empty()) {
    WithColor::error() << "mobile requires -o <new-output-directory>\n";
    return 1;
  }

  static int ExecutableAnchor;
  std::string Executable = sys::fs::getMainExecutable(Argv0, &ExecutableAnchor);
  std::filesystem::path Helper =
      std::filesystem::path(Executable).parent_path() / "mobile" /
      "__main__.py";
  std::error_code EC;
  if (!std::filesystem::is_regular_file(Helper, EC)) {
    WithColor::error() << "mobile helper is missing; distribute the mobile "
                          "directory alongside the neverd executable\n";
    return 1;
  }

  std::string PythonName = MobilePython;
  if (PythonName.empty()) {
    if (const char *Override = std::getenv("NEVERD_PYTHON"))
      PythonName = Override;
  }
  bool ExplicitPython = !PythonName.empty();
  auto Python = sys::findProgramByName(ExplicitPython ? PythonName : "python3");
  if (!Python && !ExplicitPython)
    Python = sys::findProgramByName("python");
  if (!Python) {
    WithColor::error() << "Python 3.10+ is required for mobile recovery; set "
                          "--python or NEVERD_PYTHON\n";
    return 1;
  }

  std::vector<std::string> Arguments = {*Python,
                                        "-I",
                                        Helper.string(),
                                        "--output",
                                        OutputFile,
                                        "--platform",
                                        MobilePlatform,
                                        "--neverd",
                                        Executable,
                                        "--arch",
                                        MobileArch,
                                        "--timeout",
                                        std::to_string(MobileTimeout),
                                        "--max-files",
                                        std::to_string(MobileMaxFiles),
                                        "--max-bytes",
                                        std::to_string(MobileMaxBytes),
                                        "--max-func",
                                        std::to_string(MaxFunc)};
  if (!MobileBackend.empty()) {
    Arguments.push_back("--jadx");
    Arguments.push_back(MobileBackend);
  }
  if (!MobileSwiftDemangle.empty()) {
    Arguments.push_back("--swift-demangle");
    Arguments.push_back(MobileSwiftDemangle);
  }
  if (!MobileArtifact.empty()) {
    Arguments.push_back("--artifact");
    Arguments.push_back(MobileArtifact);
  }
  if (MobileMetadataOnly)
    Arguments.push_back("--metadata-only");
  if (JsonOutput)
    Arguments.push_back("--json");
  Arguments.push_back("--");
  Arguments.push_back(InputFile);
  std::vector<StringRef> ArgRefs(Arguments.begin(), Arguments.end());
  std::string Error;
  int Result =
      sys::ExecuteAndWait(*Python, ArgRefs, std::nullopt, {}, 0, 0, &Error);
  if (Result < 0) {
    WithColor::error() << "mobile helper failed: " << Error << "\n";
    return 1;
  }
  return Result;
}

} // namespace neverd::cli
