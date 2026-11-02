//===- NeverDCmdAnalysisSections.cpp - Analysis image sections -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Analysis image section commands.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/Limits.h"
#include "neverd/support/AtomicOutput.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectFiles.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>
#include <memory>

using namespace llvm;

namespace neverd::cli {
namespace {
constexpr size_t MaxStateBytes = neverd::sdk::kMaxAnalysisSectionStateBytes;
using OwnedText = std::unique_ptr<const char, decltype(&neverd_free_string)>;

Expected<std::filesystem::path> statePath(neverd_session_t Sess,
                                          bool ForWrite = false) {
  OwnedText Input(neverd_session_file_path(Sess), neverd_free_string);
  if (!Input)
    return createStringError(inconvertibleErrorCode(), "no input path");
  std::error_code EC;
  auto Path =
      std::filesystem::canonical(std::filesystem::u8path(Input.get()), EC);
  if (EC)
    return createStringError(EC, "cannot locate analysis section source");
  auto State = neverd::project_files::path(
      Path, neverd::project_files::File::AnalysisSections);
  if (ForWrite) {
    if (auto Error = neverd::project_files::prepare(Path))
      return createStringError(Error, "cannot prepare project folder");
  } else if (!std::filesystem::exists(State)) {
    auto Legacy = Path;
    Legacy += neverd::project_files::describe(
                  neverd::project_files::File::AnalysisSections)
                  .Legacy;
    if (std::filesystem::exists(Legacy))
      return Legacy;
  }
  return State;
}

Expected<std::string> readBounded(const std::filesystem::path &Path,
                                  size_t Limit) {
  std::error_code EC;
  const auto Size = std::filesystem::file_size(Path, EC);
  if (EC)
    return createStringError(EC, "cannot read %s", pathToUTF8(Path).c_str());
  if (Size > Limit)
    return createStringError(inconvertibleErrorCode(),
                             "file exceeds byte limit: %s",
                             pathToUTF8(Path).c_str());
  std::ifstream Input(Path, std::ios::binary);
  std::string Bytes(size_t(Size), '\0');
  if (!Input.read(Bytes.data(), std::streamsize(Size)) ||
      Input.peek() != std::char_traits<char>::eof())
    return createStringError(inconvertibleErrorCode(),
                             "cannot read complete file: %s",
                             pathToUTF8(Path).c_str());
  return Bytes;
}

Error saveState(neverd_session_t Sess) {
  auto Path = statePath(Sess, true);
  if (!Path)
    return Path.takeError();
  OwnedText State(neverd_analysis_sections_json(Sess, 1), neverd_free_string);
  if (!State)
    return createStringError(inconvertibleErrorCode(), "%s",
                             takeLastError(Sess).c_str());
  StringRef Text(State.get());
  if (Text.size() > MaxStateBytes)
    return createStringError(inconvertibleErrorCode(),
                             "analysis section state exceeds byte limit");
  const auto UTF8 = pathToUTF8(*Path);
  auto Temporary = sys::fs::TempFile::create(
      UTF8 + ".tmp-%%%%%%", sys::fs::owner_read | sys::fs::owner_write);
  if (!Temporary)
    return Temporary.takeError();
  std::error_code EC;
  {
    raw_fd_ostream OS(Temporary->FD, false);
    OS << Text << '\n';
    OS.flush();
    EC = OS.error();
    OS.clear_error();
  }
  if (EC) {
    consumeError(support::atomic_output::discardTemporaryOutput(*Temporary));
    return createStringError(EC, "cannot write analysis section state");
  }
  return support::atomic_output::closeAndCommitTemporaryOutput(*Temporary,
                                                               UTF8);
}

int failed(Error Error) {
  WithColor::error() << toString(std::move(Error)) << '\n';
  return 1;
}
} // namespace

bool loadAnalysisSections(neverd_session_t Sess) {
  auto Path = statePath(Sess);
  if (!Path) {
    failed(Path.takeError());
    return false;
  }
  std::error_code EC;
  const bool Exists = std::filesystem::exists(*Path, EC);
  if (EC) {
    failed(createStringError(EC, "cannot inspect analysis section state"));
    return false;
  }
  if (!Exists)
    return true;
  auto Text = readBounded(*Path, MaxStateBytes);
  if (!Text) {
    failed(Text.takeError());
    return false;
  }
  // A C string must not silently truncate a malformed file at an embedded NUL.
  if (Text->find('\0') != std::string::npos ||
      !neverd_analysis_sections_import_json(Sess, Text->c_str())) {
    WithColor::error() << "cannot load analysis sections: "
                       << (Text->find('\0') != std::string::npos
                               ? "embedded NUL in state file"
                               : takeLastError(Sess))
                       << '\n';
    return false;
  }
  return true;
}

int runAnalysisSections(neverd_session_t Sess) {
  const bool Add = AnalysisSectionAdd.getNumOccurrences() != 0;
  const bool Remove = AnalysisSectionRemove.getNumOccurrences() != 0;
  const bool Data = AnalysisSectionData.getNumOccurrences() != 0;
  if ((Add && Remove) || Data != Add ||
      (Add && (AnalysisSectionAdd.empty() || AnalysisSectionData.empty())) ||
      (Remove && AnalysisSectionRemove.empty())) {
    WithColor::error() << "choose --add description.json --data bytes.bin, "
                          "--remove name, or no edit to list sections\n";
    return 1;
  }
  if (Add) {
    auto Description =
        readBounded(std::filesystem::u8path(AnalysisSectionAdd.getValue()),
                    sdk::kMaxAnalysisSectionDescriptionBytes);
    if (!Description)
      return failed(Description.takeError());
    auto Bytes =
        readBounded(std::filesystem::u8path(AnalysisSectionData.getValue()),
                    sdk::kMaxAnalysisSectionBytes);
    if (!Bytes)
      return failed(Bytes.takeError());
    if (Description->find('\0') != std::string::npos) {
      WithColor::error() << "embedded NUL in section description\n";
      return 1;
    }
    if (!neverd_analysis_section_add(
            Sess, Description->c_str(),
            reinterpret_cast<const unsigned char *>(Bytes->data()),
            Bytes->size())) {
      WithColor::error() << takeLastError(Sess) << '\n';
      return 1;
    }
  } else if (Remove && !neverd_analysis_section_remove(
                           Sess, AnalysisSectionRemove.c_str())) {
    WithColor::error() << takeLastError(Sess) << '\n';
    return 1;
  }
  if (Add || Remove)
    if (auto Error = saveState(Sess))
      return failed(std::move(Error));
  OwnedText Summary(neverd_analysis_sections_json(Sess, 0), neverd_free_string);
  if (!Summary) {
    WithColor::error() << takeLastError(Sess) << '\n';
    return 1;
  }
  if (JsonOutput) {
    outs() << Summary.get() << '\n';
    return 0;
  }
  auto Parsed = json::parse(Summary.get());
  if (!Parsed)
    return failed(Parsed.takeError());
  const auto *Sections = Parsed->getAsObject()->getArray("sections");
  if (Add || Remove)
    outs() << "Saved analysis sections beside the binary.\n";
  for (const auto &Value : *Sections) {
    const auto &Section = *Value.getAsObject();
    outs() << *Section.getString("address") << "  "
           << *Section.getString("name") << "  "
           << *Section.getString("permissions") << "  "
           << *Section.getInteger("size") << " bytes, "
           << Section.getArray("functions")->size() << " functions\n";
  }
  outs() << Sections->size() << " analysis sections.\n";
  return 0;
}
} // namespace neverd::cli
