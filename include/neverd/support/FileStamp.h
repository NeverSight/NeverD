//===- FileStamp.h - External file selection identity --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared size/time identity of an external file or directory selection.
/// This is a change detector, not a cryptographic content authentication.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_FILESTAMP_H
#define NEVERD_SUPPORT_FILESTAMP_H

#include "neverd/support/LoadProgress.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace neverd {
using FileStamp = std::map<std::string, std::pair<uintmax_t, int64_t>>;

inline std::optional<FileStamp> fileStamp(const std::filesystem::path &Path,
                                          const LoadProgress &Progress = {}) {
  Progress.check();
  if (!std::filesystem::exists(Path))
    return {};
  FileStamp Files;
  const auto Add = [&](const std::filesystem::path &File) {
    Progress.check();
    const auto Name = File.generic_u8string();
    Files[std::string(Name.begin(), Name.end())] = {
        std::filesystem::file_size(File),
        std::filesystem::last_write_time(File).time_since_epoch().count()};
  };
  if (std::filesystem::is_directory(Path)) {
    for (const auto &Entry :
         std::filesystem::recursive_directory_iterator(Path)) {
      Progress.check();
      if (Entry.is_regular_file())
        Add(Entry.path());
    }
  } else {
    Add(Path);
  }
  return Files;
}
} // namespace neverd

#endif // NEVERD_SUPPORT_FILESTAMP_H
