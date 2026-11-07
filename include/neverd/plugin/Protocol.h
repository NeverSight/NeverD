//===- Protocol.h - Shared provider wire protocol --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Framing, request identity, declared queries and result-before-exit rules.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_PROTOCOL_H
#define NEVERD_PLUGIN_PROTOCOL_H

#include "neverd/plugin/Manifest.h"
#include "neverd/plugin/Tasks.h"

#include <functional>
#include <set>
#include <string_view>

namespace neverd::plugin {
struct ProtocolCallbacks {
  std::function<bool(std::string)> Send;
  std::function<void()> Hello;
  std::function<void(const Json &)> Progress;
  std::function<void(const std::string &)> Log;
  std::function<void(const std::string &, const std::string &, const Json &)>
      Query;
  std::function<void(const std::string &)> Failed;
  std::function<void(const std::string &, const std::string &, const Json &)>
      Command;
  std::function<void(const std::string &, const std::string &, const Json &)>
      Host;
};

struct ProtocolExtensions {
  bool StructuredData = false;
  std::vector<std::string> Commands;
  bool HostCapabilities = false;
};

class Protocol {
  Tasks &Controller;
  std::string Task;
  Json Start, Result;
  std::vector<std::string> Queries;
  ProtocolCallbacks Callbacks;
  ProtocolExtensions Extensions;
  std::string Input, Failure;
  std::set<std::string> Pending;
  std::set<std::string> CommandIds;
  std::set<std::string> HostIds;
  size_t ReadBytes = 0, LogBytes = 0, Messages = 0;
  bool Greeted = false, Completed = false;
  void message(const Json &Value);
  void fail(std::string Error);
  void send(Json Value);

public:
  Protocol(Tasks &Controller, Json Start, std::vector<std::string> Queries,
           ProtocolCallbacks Callbacks, ProtocolExtensions Extensions = {});
  void receive(std::string_view Bytes);
  bool diagnostic(std::string_view Bytes);
  void answer(const std::string &Id, const Json &Reply);
  void event(const Json &Notification);
  /// Validates the final wire state; process status and artifact bytes are
  /// checked by the runtime before calling Tasks::finish.
  std::string exitError() const;
  const Json &result() const { return Result; }
  static std::string frame(Json Value);
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_PROTOCOL_H
