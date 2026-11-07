//===- Process.h - Native provider process transport -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pipe transport and process-tree lifetime; callbacks use the host executor.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_PROCESS_H
#define NEVERD_PLUGIN_PROCESS_H

#include "neverd/plugin/PluginHost.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neverd::plugin {
struct ProcessEvent {
  enum Kind { Output, Diagnostic, Error, Exit } Type;
  std::string Bytes;
  int ExitCode = 0;
};

/// Provider jobs bound all output over their lifetime. A persistent worker
/// instead bounds the bytes awaiting delivery, independently for each stream.
enum class ProcessOutputBudget { Lifetime, PendingDelivery };

/// One process per transport. I/O threads never call plugin or GUI code.
/// Exit is posted after the provider exits, its job (Windows) or process group
/// (POSIX) is terminated and both output streams drain. A POSIX descendant
/// that leaves the group is not terminated: if the pipes are still open
/// kShutdownWaitMs after the provider exits, they are abandoned and an Error
/// precedes Exit. The consumer must keep its completion queue alive through
/// join().
class Process {
  struct Impl;
  std::unique_ptr<Impl> State;

public:
  using Notify = std::function<void(ProcessEvent)>;
  Process(PluginHost::Post Post, Notify Changed,
          ProcessOutputBudget OutputBudget = ProcessOutputBudget::Lifetime);
  ~Process();
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;
  bool start(const std::filesystem::path &Program,
             const std::vector<std::string> &Arguments,
             const std::filesystem::path &Directory, std::string &Error);
  bool write(std::string Bytes);
  void stop();
  void join();
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_PROCESS_H

