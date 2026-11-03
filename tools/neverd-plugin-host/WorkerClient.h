//===- WorkerClient.h - Headless plugin project bridge ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Project queries are forwarded to the existing worker semantics owner.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_PLUGIN_WORKERCLIENT_H
#define NEVERD_TOOLS_PLUGIN_WORKERCLIENT_H

#include "neverd/plugin/Executor.h"
#include "neverd/plugin/Manifest.h"
#include "neverd/plugin/Process.h"

#include <map>

namespace neverd::plugin::cli {
class WorkerClient {
  Executor &Queue;
  Process Child;
  std::string Input, Error;
  bool Greeted = false;
  uint64_t NextId = 0;
  std::map<std::string, std::function<void(Json)>> Pending;
  void receive(ProcessEvent Event);

public:
  explicit WorkerClient(Executor &Queue);
  ~WorkerClient();
  bool open(const std::filesystem::path &Worker,
            const std::filesystem::path &File, Json &Context,
            std::function<bool()> Cancelled, std::string &Diagnostic,
            bool ReadOnly = true);
  void query(const std::string &Operation, const Json &Payload,
             const Json &Context, std::function<void(Json)> Done);
};
} // namespace neverd::plugin::cli
#endif // NEVERD_TOOLS_PLUGIN_WORKERCLIENT_H
