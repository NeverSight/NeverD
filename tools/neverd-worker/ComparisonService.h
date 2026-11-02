//===- ComparisonService.h - Independent binary comparison jobs -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qt-free adapter to the shared comparison C ABI. Comparison sessions never
/// replace the workbench's project and published snapshots survive a failed
/// run.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_WORKER_COMPARISONSERVICE_H
#define NEVERD_TOOLS_WORKER_COMPARISONSERVICE_H

#include "Protocol.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace neverd::worker {
class ComparisonService {
public:
  using Progress = std::function<void(const char *, std::uint64_t,
                                      std::uint64_t, const char *)>;
  using Cancellation = std::function<bool()>;
  using Writer =
      std::function<void(const std::filesystem::path &, const std::string &)>;
  explicit ComparisonService(Writer Write);
  ~ComparisonService();
  ComparisonService(const ComparisonService &) = delete;
  ComparisonService &operator=(const ComparisonService &) = delete;
  static bool supported();
  static bool sourceSupported();
  static bool sourceDifferenceSupported();
  /// Operations whose SDK calculation observes cooperative cancellation.
  /// Bounded transport/graph layout after calculation is not a hard deadline.
  static constexpr std::string_view CancellableOperations[] = {
      "comparison_dataflow",
      "comparison_start",
      "comparison_slice",
      "comparison_impact",
      "comparison_function_page",
      "comparison_function",
      "comparison_cfg_summary",
      "comparison_cfg_viewport",
      "comparison_cfg_focus",
      "comparison_observations",
      "comparison_scalar_validation",
      "comparison_source_page",
      "comparison_source_difference",
      "comparison_restore",
      "comparison_override",
      "comparison_undo",
      "comparison_redo"};
  static bool cancellable(std::string_view Operation);
  Json execute(const std::string &Operation, const Json &Payload,
               const Progress &Notify, const Cancellation &Cancelled);

private:
  struct Impl;
  std::unique_ptr<Impl> State;
};
} // namespace neverd::worker

#endif // NEVERD_TOOLS_WORKER_COMPARISONSERVICE_H
