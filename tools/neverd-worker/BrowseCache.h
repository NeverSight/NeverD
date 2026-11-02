//===- BrowseCache.h - Persistent browsing facts ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Disposable, input/configuration/build-bound browsing facts. No project
/// edits or executable engine state are restored from this cache.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_BROWSECACHE_H
#define NEVERD_TOOLS_NEVERD_WORKER_BROWSECACHE_H

#include "ProjectHistory.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace neverd::worker {

class BrowseCache {
public:
  using Producer = std::function<std::string()>;
  void configure(std::filesystem::path projectFolder,
                 std::function<Json()> identity, bool writable,
                 AtomicWriter writer);
  void invalidate();
  std::optional<std::string> read(const std::string &key);
  void defer(const std::string &key, Producer producer);
  void reject(std::string diagnostic);
  Json query(const std::string &key, std::function<Json()> producer,
             std::function<bool(const Json &)> valid);
  bool pending() const { return !pending_.empty(); }
  bool writable() const { return enabled_ && writable_ && writer_; }
  void flushOne() noexcept;
  void flush() noexcept;
  Json stats() const;
  static std::string digest(std::string_view bytes);

private:
  bool initialize();
  void prune();
  std::filesystem::path folder_;
  std::function<Json()> identity_;
  AtomicWriter writer_ = nullptr;
  bool writable_ = false, initialized_ = false, enabled_ = false;
  std::string namespace_, diagnostic_;
  std::map<std::string, Producer> pending_;
  std::size_t hits_ = 0, misses_ = 0, writes_ = 0, invalid_ = 0;
};

} // namespace neverd::worker

#endif // NEVERD_TOOLS_NEVERD_WORKER_BROWSECACHE_H
