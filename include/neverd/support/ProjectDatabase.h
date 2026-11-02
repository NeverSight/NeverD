//===- ProjectDatabase.h - Shared NDDB reader -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Transaction-consistent, read-only NDDB input and project extraction.
/// Independent of Qt, LLVM, sessions and the caller's working directory.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_PROJECTDATABASE_H
#define NEVERD_SUPPORT_PROJECTDATABASE_H

#include "neverd/support/Limits.h"
#include "neverd/support/LoadProgress.h"
#include "neverd/support/ProjectFiles.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::project_database {

inline constexpr char Extension[] = ".nddb";
inline constexpr int ApplicationId = 0x4E444442;
inline constexpr int FormatVersion = 1;
inline constexpr char InputBlob[] = "input";
inline constexpr char SqliteMagic[] = "SQLite format 3";
inline constexpr size_t ApplicationIdOffset = 68;
inline constexpr size_t HeaderPrefixBytes =
    ApplicationIdOffset + sizeof(uint32_t);
inline constexpr const char *Schema[] = {
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT "
    "NULL)",
    "CREATE TABLE IF NOT EXISTS blobs(name TEXT NOT NULL, chunk INTEGER NOT "
    "NULL, "
    "size INTEGER NOT NULL, data BLOB NOT NULL, PRIMARY KEY(name, chunk))",
    "CREATE TABLE IF NOT EXISTS sidecars(suffix TEXT PRIMARY KEY, data BLOB "
    "NOT NULL)",
    "CREATE TABLE IF NOT EXISTS state(key TEXT PRIMARY KEY, value BLOB NOT "
    "NULL)",
};

std::string_view fileKey(const project_files::Description &File);
bool isPackedFile(const project_files::Description &File);
std::vector<std::string> legacyFileKeys();
bool validInputName(std::string_view Name);
/// Recognizes the suffix or stamped header. Legacy unstamped files need the
/// suffix; all candidates still undergo full validation by Reader.
bool isDatabase(const std::filesystem::path &Path);

class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ReadLimits {
  uint64_t MetadataBytes = limits::support::kProjectMetadataBytes;
  uint64_t SidecarBytes = limits::support::kProjectSidecarBytes;
  uint64_t StateBytes = limits::support::kProjectStateBytes;
  uint64_t Rows = limits::support::kProjectMetadataRows;
};

struct Contents {
  std::string InputName, InputSHA256, Generator, SavedAt;
  uint64_t InputSize = 0;
  std::vector<std::string> PackedFiles;
  std::map<std::string, std::string> Sidecars, State;
};

/// An unpublished sink. write() consumes bytes and updates its SHA-256;
/// sha256() finishes that hash as lowercase hex. Errors throw Error. Reader
/// verifies size and digest before the caller may publish the result.
class InputSink {
public:
  virtual ~InputSink() = default;
  virtual void write(std::span<const uint8_t> Bytes) = 0;
  virtual std::string sha256() = 0;
};

class Reader final {
public:
  /// Opens a read-only transaction and validates identity metadata. Progress
  /// callbacks are borrowed for this Reader's lifetime. Failure throws Error
  /// or LoadCancelled; no SQLite callback throws through C frames.
  explicit Reader(const std::filesystem::path &Path,
                  const LoadProgress &Progress = {}, ReadLimits Limits = {});
  ~Reader();
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  const Contents &contents() const;
  /// Reads the recognized packed project files and, optionally, UI state from
  /// the same transaction. Unknown sidecars are refused, never materialized.
  void readProject(bool IncludeState = true);
  /// Streams independently compressed chunks with bounded buffers, checks
  /// their sequence, lengths, exact zlib framing, total length and SHA-256.
  /// No policy ceiling on total input bytes; allocation is independent of it.
  void unpackInput(InputSink &Sink);

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace neverd::project_database

#endif // NEVERD_SUPPORT_PROJECTDATABASE_H
