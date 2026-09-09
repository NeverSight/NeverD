#pragma once
#include "Protocol.h"

#include <filesystem>

namespace neverd::worker {
using AtomicWriter = void (*)(const std::filesystem::path &, const Json &);

// Durable user-edit history above the public C ABI. No analysis objects or
// pointers enter the store; recovery only replaces known user sidecar files.
class ProjectHistory {
public:
  ProjectHistory(std::filesystem::path binary, std::string sourceHash,
                 std::string engineVersion, bool readOnly, AtomicWriter writer);
  static bool recoveryPending(const std::filesystem::path &binary);
  bool recovered() const { return recovered_; }
  const Json &committedState() const { return committedState_; }
  Json listing(std::size_t offset, std::size_t limit) const;
  void stage(Json command);
  Json next(bool redo) const;
  void advance(bool redo);
  void persist(const Json &state);
  void reset();
  std::string blockedReason() const { return blocked_; }
  void verifyLoadedState(const Json &state);
  static Json normalizedState(Json state);

private:
  std::filesystem::path binary_, historyPath_, journalPath_;
  std::string hash_, engineVersion_, blocked_;
  bool readOnly_, recovered_ = false;
  std::uintmax_t fileSize_ = 0;
  std::filesystem::file_time_type fileTime_;
  AtomicWriter writer_;
  Json commands_ = Json::array(), committedState_, previousDocument_;
  std::size_t cursor_ = 0;
  Json diskState() const;
  Json document(const Json &state) const;
  void recover();
  void requireUsable() const;
};
} // namespace neverd::worker
