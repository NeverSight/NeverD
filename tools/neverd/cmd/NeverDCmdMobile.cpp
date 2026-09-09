//===- NeverDCmdMobile.cpp - Native mobile application recovery -----------===//
#include "../NeverDCLI.h"
#include "../mobile/MobileCommon.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
int runMobile(const char *Argv0) {
  try {
    mobile::Options Options;
    Options.input = mobile::pathFromUTF8(InputFile.getValue());
    Options.output = mobile::pathFromUTF8(OutputFile.getValue());
    Options.platform = MobilePlatform;
    Options.architecture = MobileArch;
    Options.metadata_only = MobileMetadataOnly;
    Options.max_functions = MaxFunc;
    Options.limits = {MobileTimeout, MobileMaxFiles, MobileMaxBytes};
    static int ExecutableAnchor;
    Options.executable =
        llvm::sys::fs::getMainExecutable(Argv0, &ExecutableAnchor);
    if (!MobileBackend.empty())
      Options.jadx = MobileBackend;
    if (!MobileArtifact.empty())
      Options.artifact = MobileArtifact;
    auto Report = mobile::recover(Options);
    if (JsonOutput)
      llvm::outs() << mobile::jsonText(std::move(Report));
    else
      llvm::outs() << "Mobile recovery written to " << OutputFile
                   << "\nReport: "
                   << mobile::pathText(Options.output / "report.json") << "\n";
    return 0;
  } catch (const std::exception &Error) {
    // Filesystem errors may use a native encoding; JSON remains valid UTF-8.
    std::string Message = llvm::json::fixUTF8(Error.what());
    if (JsonOutput)
      llvm::outs() << mobile::jsonText(llvm::json::Object{
          {"schema_version", 1}, {"status", "error"}, {"error", Message}});
    else
      llvm::WithColor::error() << Message << "\n";
    return 1;
  }
}
} // namespace neverd::cli
