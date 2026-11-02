//===- ProjectInput.cpp - Session-owned saved project input ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Materializes authenticated NDDB bytes and known project files for the SDK.
//===----------------------------------------------------------------------===//

#include "ProjectInput.h"

#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectDatabase.h"
#include "neverd/support/ProjectFiles.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

#include <fstream>
#include <system_error>

namespace neverd::sdk {
namespace {
class FileSink final : public project_database::InputSink {
public:
  explicit FileSink(const std::filesystem::path &Path)
      : Out(Path, std::ios::binary | std::ios::trunc) {
    if (!Out)
      throw project_database::Error("NDDB: cannot create temporary input");
  }
  void write(std::span<const uint8_t> Bytes) override {
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    if (!Out)
      throw project_database::Error("NDDB: cannot write temporary input");
    Hash.update(llvm::ArrayRef(Bytes.data(), Bytes.size()));
  }
  std::string sha256() override {
    Out.close();
    if (!Out)
      throw project_database::Error("NDDB: cannot finish temporary input");
    return llvm::toHex(Hash.final(), true);
  }

private:
  std::ofstream Out;
  llvm::SHA256 Hash;
};
} // namespace

ProjectInput::~ProjectInput() {
  if (!Directory.empty()) {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
  }
}

std::shared_ptr<ProjectInput>
prepareProjectInput(const std::filesystem::path &Source,
                    const LoadProgress &Progress) {
  if (!project_database::isDatabase(Source))
    return {};
  project_database::Reader Reader(Source, Progress);
  Reader.readProject(false);
  const auto &Contents = Reader.contents();
  auto Project = std::make_shared<ProjectInput>();
  Project->Source = Source;
  Project->InputName = Contents.InputName;
  llvm::SmallString<128> Directory;
  if (const auto EC =
          llvm::sys::fs::createUniqueDirectory("neverd-project", Directory))
    throw project_database::Error("NDDB: cannot create temporary directory: " +
                                  EC.message());
  Project->Directory = std::filesystem::u8path(Directory.str().str());
  Project->Input =
      Project->Directory / std::filesystem::u8path(Contents.InputName);
  FileSink Sink(Project->Input);
  Reader.unpackInput(Sink);
  if (const auto EC = project_files::prepare(Project->Input))
    throw project_database::Error("NDDB: cannot create temporary project: " +
                                  EC.message());
  llvm::SHA256 State;
  const auto Bind = [&](llvm::StringRef Value) {
    // Length prefixes make names and bytes unambiguous, independent of SQLite
    // row ordering, compression and local temporary directory names.
    State.update(std::to_string(Value.size()));
    State.update(":");
    State.update(Value);
  };
  Bind("neverd-nddb-project-v1");
  Bind(Contents.InputName);
  for (const auto &[Key, Bytes] : Contents.Sidecars) {
    Progress.check();
    auto Parsed = llvm::json::parse(Bytes);
    if (!Parsed)
      throw project_database::Error("NDDB: invalid project JSON in " + Key +
                                    ": " + llvm::toString(Parsed.takeError()));
    if (!Parsed->getAsObject() && !Parsed->getAsArray())
      throw project_database::Error(
          "NDDB: project file must be an object or array: " + Key);
    if (Key == project_database::fileKey(
                   project_files::describe(project_files::File::Manifest))) {
      const auto *Root = Parsed->getAsObject();
      const auto *Input = Root ? Root->getObject("input") : nullptr;
      if (!Root || Root->getInteger("neverd_project") != 1 || !Input ||
          Input->getString("sha256") != Contents.InputSHA256 ||
          Input->getInteger("size") != Contents.InputSize)
        throw project_database::Error(
            "NDDB: project manifest does not describe the stored input");
    }
    for (const auto &File : project_files::Files) {
      if (!project_database::isPackedFile(File) ||
          project_database::fileKey(File) != Key)
        continue;
      if (File.Id == project_files::File::DebugFiles)
        Project->DebugChoices = Bytes;
      const auto Path = project_files::path(Project->Input, File.Id);
      std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
      Out.write(Bytes.data(), Bytes.size());
      Out.close();
      if (!Out)
        throw project_database::Error(
            "NDDB: cannot write temporary project file: " + Key);
      // UI bookmarks and undo history do not alter the native analysis. Other
      // saved assertions remain bound even when a current profile does not
      // consume them, so validation cannot silently carry evidence across
      // edits.
      if (File.Id != project_files::File::Bookmarks &&
          File.Id != project_files::File::History) {
        Bind(Key);
        Bind(Bytes);
      }
    }
  }
  Project->StateSHA256 = llvm::toHex(State.final(), true);
  Progress.check();
  return Project;
}

ProjectLoadSettings projectLoadSettings(const ProjectInput &Project,
                                        const DebugInfoRequest &Requested,
                                        bool ReadStaticSymbols,
                                        const LoadProgress &Progress) {
  ProjectLoadSettings Result{Requested, ReadStaticSymbols, {}};
  if (Project.DebugChoices.empty())
    return Result;
  auto Value = llvm::json::parse(Project.DebugChoices);
  if (!Value)
    throw project_database::Error(llvm::toString(Value.takeError()));
  const auto *Root = Value->getAsObject();
  if (!Root)
    throw project_database::Error("NDDB: debug choices must be an object");
  for (const auto &[Key, Item] : *Root) {
    Progress.check();
    const auto *Choice = Item.getAsObject();
    if (!Choice)
      throw project_database::Error("NDDB: invalid debug choice: " + Key.str());
    if (Choice->get("mode") && !Choice->getString("mode"))
      throw project_database::Error("NDDB: debug mode must be a string");
    const auto Mode = Choice->getString("mode").value_or(
        Key == "pdb" ? "verified" : "checked");
    if (Key == "symbols") {
      if (Mode != "none")
        throw project_database::Error("NDDB: unsupported symbol table choice");
      Result.ReadStaticSymbols = false;
      continue;
    }
    auto &Debug = Result.Debug;
    bool *Search = nullptr;
    std::filesystem::path *Selected = nullptr;
    if (Key == "pdb") {
      Search = &Debug.SearchPDB;
      Selected = &Debug.PDBPath;
    } else if (Key == "dwarf") {
      Search = &Debug.SearchDWARF;
      Selected = &Debug.DWARFPath;
    } else if (Key == "map") {
      Search = &Debug.SearchMap;
      Selected = &Debug.MapPath;
    } else {
      throw project_database::Error("NDDB: unsupported debug choice: " +
                                    Key.str());
    }
    // The SDK caller's explicit path or disabled search remains authoritative.
    if (!Selected->empty() || !*Search || !Debug.Enabled)
      continue;
    *Search = false;
    if (Mode == "none" || (Key == "pdb" && Mode == "display"))
      continue;
    const bool Unchecked = (Key == "pdb" && Mode == "analysis") ||
                           (Key == "map" && Mode == "unchecked");
    if (!Unchecked && Mode != (Key == "pdb" ? "verified" : "checked"))
      throw project_database::Error("NDDB: unsupported debug mode: " +
                                    Mode.str());
    const auto Path = Choice->getString("path");
    if (!Path || Path->empty() || Path->contains('\0'))
      throw project_database::Error(
          "NDDB: selected debug file has no valid path");
    *Selected = std::filesystem::u8path(Path->str());
    if (Selected->is_relative())
      *Selected =
          (std::filesystem::absolute(Project.Source).parent_path() / *Selected)
              .lexically_normal();
    const auto Stamp = fileStamp(*Selected, Progress);
    if (!Stamp)
      throw project_database::Error(
          "NDDB: external debug file is unavailable: " + pathToUTF8(*Selected));
    if (Unchecked) {
      llvm::json::Object Current;
      for (const auto &[Name, Identity] : *Stamp)
        Current[Name] =
            llvm::json::Array{int64_t(Identity.first), Identity.second};
      const auto *Saved = Choice->get("stamp");
      if (!Saved || *Saved != llvm::json::Value(std::move(Current)))
        throw project_database::Error(
            "NDDB: unchecked external debug selection changed; select the file "
            "again in the project");
      if (Key == "pdb")
        Debug.AllowMissingPDBIdentity = true;
      else
        Debug.UncheckedMap = true;
    }
    Result.ExternalFiles.emplace_back(*Selected, *Stamp);
  }
  return Result;
}

void checkProjectDebugFiles(const ProjectLoadSettings &Settings,
                            const LoadProgress &Progress) {
  for (const auto &[Path, Stamp] : Settings.ExternalFiles)
    if (fileStamp(Path, Progress) != Stamp)
      throw project_database::Error(
          "NDDB: external debug file changed while loading: " +
          pathToUTF8(Path));
}

} // namespace neverd::sdk
