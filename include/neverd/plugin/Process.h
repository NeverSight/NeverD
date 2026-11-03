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

/// One process per transport. I/O threads never call plugin or GUI code.
/// Exit is posted after both output streams drain and descendants terminate.
/// The consumer must keep its completion queue alive through join().
class Process {
  struct Impl;
  std::unique_ptr<Impl> State;

public:
  using Notify = std::function<void(ProcessEvent)>;
  Process(PluginHost::Post Post, Notify Changed);
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
