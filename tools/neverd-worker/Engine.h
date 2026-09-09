#pragma once
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPISession.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace neverd::worker {
class ProjectLock;
class ProjectHistory;
class Contributions;
class GraphSnapshot;
class Engine {
public:
  Engine();
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Json execute(const std::string &operation, const Json &payload);
  std::string revision() const { return std::to_string(revision_); }
  std::string projectId() const { return projectId_; }
  static std::string version();

private:
  neverd_session_t session_ = nullptr;
  std::unique_ptr<ProjectLock> lock_;
  std::unique_ptr<ProjectHistory> history_;
  std::unique_ptr<Contributions> contributions_;
  std::unique_ptr<GraphSnapshot> graph_;
  std::uint64_t revision_ = 0;
  std::string projectId_;
  bool analyzed_ = false;
  bool dirty_ = false;
  bool readOnly_ = false;
  std::uintmax_t loadedSize_ = 0;
  std::filesystem::file_time_type loadedTime_;
  Json stringsCache_;
  std::string textKey_, textCache_;
  std::vector<std::size_t> textLines_;
  std::string filterKey_;
  std::vector<int> filteredFunctions_;
  bool haveFilter_ = false;
  void invalidate();
  void requireLoaded() const;
  void requireWriter() const;
  void analyze();
  ProjectHistory &history();
  Json metadata() const;
  std::string error() const;
  Json backendJson(const char *owned, bool checkError = false) const;
};
} // namespace neverd::worker
