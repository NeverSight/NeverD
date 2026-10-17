//===- ProjectFiles.h - The files of an input's project ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Where an input's project keeps its files: one folder beside the input,
/// `<input>.neverd/`, which a repository holds as it holds source.  Every
/// writer and reader (the engine, the worker, the workbench, the command
/// line) takes its paths from here and from ProjectFiles.def.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace neverd::project_files {

enum class File {
#define NEVERD_PROJECT_FILE(Name, Path, Legacy, Shared) Name,
#include "neverd/support/ProjectFiles.def"
};

struct Description {
  File Id;
  /// The file's path in the project folder, '/'-separated.
  std::string_view Path;
  /// The suffix an older NeverD gave the file beside the input, or empty.
  std::string_view Legacy;
  /// Shared under version control, or each user's own.
  bool Shared;
};

inline constexpr Description Files[] = {
#define NEVERD_PROJECT_FILE(Name, Path, Legacy, Shared)                        \
  {File::Name, Path, Legacy, Shared},
#include "neverd/support/ProjectFiles.def"
};

inline const Description &describe(File Id) {
  return Files[static_cast<std::size_t>(Id)];
}

/// The project folder of \p Input: its path with ".neverd" appended.
inline std::filesystem::path folder(std::filesystem::path Input) {
  Input += ".neverd";
  return Input;
}

inline std::filesystem::path path(const std::filesystem::path &Input, File Id) {
  return (folder(Input) / std::string(describe(Id).Path)).make_preferred();
}

/// What the folder's .gitignore and .gitattributes say: each user's own
/// files stay out of version control, and git merges the project's files
/// row by row once a clone names the driver (`neverd setup-git`).
inline constexpr std::string_view GitIgnore =
    "# Each user's own files: undo history, desktop, writer lock\n/local/\n";
inline constexpr std::string_view GitAttributes =
    "# Merged row by row by neverd merge-project (neverd setup-git)\n"
    "*.json merge=neverd text eol=lf\n";

/// Creates the project folder of \p Input, and its folder for each user's
/// own files, with the folder's .gitignore and .gitattributes where they
/// are missing.
inline std::error_code prepare(const std::filesystem::path &Input) {
  std::error_code Error;
  const std::filesystem::path Folder = folder(Input);
  std::filesystem::create_directories(Folder / "local", Error);
  if (Error)
    return Error;
  const std::pair<const char *, std::string_view> Setup[] = {
      {".gitignore", GitIgnore}, {".gitattributes", GitAttributes}};
  for (const auto &[Name, Text] : Setup) {
    const std::filesystem::path File = Folder / Name;
    if (std::filesystem::exists(File, Error) || Error)
      continue;
    std::ofstream Out(File, std::ios::binary);
    Out.write(Text.data(), static_cast<std::streamsize>(Text.size()));
  }
  return {};
}

/// Moves the files an older NeverD kept beside \p Input, such as
/// `<input>.neverd-annotations.json`, into its project folder.  A file the
/// folder already has stays where it is, as does one that cannot move.
inline void migrate(const std::filesystem::path &Input) {
  std::error_code Error;
  bool Prepared = false;
  for (const Description &File : Files) {
    if (File.Legacy.empty())
      continue;
    std::filesystem::path Old = Input;
    Old += std::string(File.Legacy);
    if (!std::filesystem::is_regular_file(Old, Error))
      continue;
    const std::filesystem::path New = path(Input, File.Id);
    if (std::filesystem::exists(New, Error) || Error)
      continue;
    if (!Prepared) {
      if (prepare(Input))
        return;
      Prepared = true;
    }
    std::filesystem::rename(Old, New, Error);
  }
}

} // namespace neverd::project_files
