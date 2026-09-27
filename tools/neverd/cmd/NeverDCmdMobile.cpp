//===- NeverDCmdMobile.cpp - Native mobile application recovery -----------===//
#include "../NeverDCLI.h"
#include "../mobile/MobileAndroidQuery.h"
#include "../mobile/MobileCommon.h"
#include "../mobile/MobileIOSInternal.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
int runMobile(const char *Argv0) {
  try {
    const bool ReferenceMode = MobileFindRefs.getNumOccurrences();
    const bool ReferenceOptions = MobileReferenceQuery.getNumOccurrences() ||
                                  MobileReferenceExact.getNumOccurrences() ||
                                  MobileReferenceOwner.getNumOccurrences();
    if (MobileInternalIOSWorker) {
      if (!OutputFile.empty() || MobilePlatform != "auto" ||
          MobileArch != "auto" || MobileMetadataOnly || MobileListClasses ||
          ReferenceMode || ReferenceOptions ||
          MobileClassPrefix.getNumOccurrences() || !MobileBackend.empty() ||
          !MobileArtifact.empty() || MaxFunc || JsonOutput ||
          MobileTimeout.getNumOccurrences() ||
          MobileMaxFiles.getNumOccurrences() ||
          MobileMaxBytes.getNumOccurrences())
        throw mobile::Error("iOS worker accepts only its staging directory");
      mobile::ios::runIOSWorker(mobile::pathFromUTF8(InputFile.getValue()));
      return 0;
    }
    if (ReferenceMode) {
      if (MobileListClasses || MobileClassPrefix.getNumOccurrences() ||
          (MobilePlatform != "auto" && MobilePlatform != "android") ||
          MobileArch != "auto" || MobileMetadataOnly ||
          !MobileBackend.empty() || !MobileArtifact.empty() || MaxFunc)
        throw mobile::Error(
            "--find-refs accepts only Android reference query options");
      if (!MobileReferenceQuery.getNumOccurrences())
        throw mobile::Error("--find-refs requires --query");
      mobile::dalvik::DexReferenceQuery Query;
      Query.kind = mobile::androidReferenceKind(MobileFindRefs.getValue());
      Query.text = MobileReferenceQuery;
      Query.exact = MobileReferenceExact;
      if (MobileReferenceOwner.getNumOccurrences())
        Query.owner = MobileReferenceOwner;
      mobile::Budget Budget({MobileTimeout, MobileMaxFiles, MobileMaxBytes});
      auto Report = mobile::findAndroidReferences(
          mobile::pathFromUTF8(InputFile.getValue()), Query, Budget);
      std::string Payload;
      if (JsonOutput) {
        Payload = mobile::jsonText(std::move(Report));
      } else {
        llvm::raw_string_ostream Stream(Payload);
        for (const auto &Row : *Report.getArray("references"))
          Stream << Row << '\n';
      }
      if (Payload.size() > Budget.limits.max_bytes)
        throw mobile::Error("reference query output exceeds the byte limit");
      Budget.check();
      if (OutputFile.empty())
        llvm::outs() << Payload;
      else
        mobile::writeFile(
            mobile::fs::absolute(mobile::pathFromUTF8(OutputFile.getValue())),
            Payload);
      return 0;
    }
    if (ReferenceOptions)
      throw mobile::Error("--query, --exact, and --owner require --find-refs");
    if (MobileListClasses) {
      if ((MobilePlatform != "auto" && MobilePlatform != "android") ||
          MobileArch != "auto" || MobileMetadataOnly ||
          !MobileBackend.empty() || !MobileArtifact.empty() || MaxFunc)
        throw mobile::Error(
            "--list-classes accepts only Android inventory options");
      mobile::Budget Budget({MobileTimeout, MobileMaxFiles, MobileMaxBytes});
      auto Inventory =
          mobile::listAndroidClasses(mobile::pathFromUTF8(InputFile.getValue()),
                                     MobileClassPrefix, Budget);
      std::string Payload;
      if (JsonOutput) {
        llvm::json::Array Classes;
        for (auto &Name : Inventory.classes)
          Classes.push_back(std::move(Name));
        Payload = mobile::jsonText(llvm::json::Object{
            {"schema_version", 1},
            {"status", "success"},
            {"operation", "android-class-inventory"},
            {"validation_scope", "dex-envelope-and-class-identities"},
            {"unselected_zip_payloads_validated", false},
            {"method_bodies_validated", false},
            {"dex_count", Inventory.dex_count},
            {"total_class_count", Inventory.total_class_count},
            {"class_count", Classes.size()},
            {"classes", std::move(Classes)}});
      } else {
        for (const auto &Name : Inventory.classes) {
          Payload += Name;
          Payload += '\n';
        }
      }
      if (Payload.size() > Budget.limits.max_bytes)
        throw mobile::Error("class inventory output exceeds the byte limit");
      Budget.check();
      if (OutputFile.empty())
        llvm::outs() << Payload;
      else
        mobile::writeFile(
            mobile::fs::absolute(mobile::pathFromUTF8(OutputFile.getValue())),
            Payload);
      return 0;
    }
    if (MobileClassPrefix.getNumOccurrences())
      throw mobile::Error("--class-prefix requires --list-classes");
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
