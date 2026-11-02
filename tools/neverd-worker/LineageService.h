//===- LineageService.h - Offline multi-version evidence jobs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// File-oriented, Qt-free adapter. Whole source documents remain in the worker;
/// only bounded public summaries/pages cross the IPC transport.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_WORKER_LINEAGESERVICE_H
#define NEVERD_TOOLS_WORKER_LINEAGESERVICE_H

#include "Protocol.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace neverd::worker {
class LineageService {
public:
  using Progress = std::function<void(const char *, std::uint64_t,
                                      std::uint64_t, const char *)>;
  using Cancellation = std::function<bool()>;
  using Writer =
      std::function<void(const std::filesystem::path &, const std::string &,
                         const std::function<void()> &)>;
  explicit LineageService(Writer Write);
  ~LineageService();
  static bool supported();
  static bool releaseEditingSupported();
  static constexpr std::string_view CancellableOperations[] = {
      "lineage_load", "lineage_query", "lineage_save", "lineage_save_report",
      "lineage_edit_releases"};
  static bool cancellable(std::string_view Operation);
  Json execute(const std::string &Operation, const Json &Payload,
               const Progress &Notify, const Cancellation &Cancelled);

private:
  struct Impl;
  std::unique_ptr<Impl> State;
};
} // namespace neverd::worker

#endif // NEVERD_TOOLS_WORKER_LINEAGESERVICE_H
