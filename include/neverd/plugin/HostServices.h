//===- HostServices.h - Shared process-service adapter --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project query compatibility and process capability sessions for all hosts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_HOSTSERVICES_H
#define NEVERD_PLUGIN_HOSTSERVICES_H

#include "neverd/plugin/Events.h"
#include "neverd/plugin/Services.h"

namespace neverd::plugin {
class HostServiceSession;
class HostServices {
  friend class HostServiceSession;
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  HostServices(PluginHost &Host, PluginHost::Post Post,
               Services::Handler Query);
  ~HostServices();
  HostServices(const HostServices &) = delete;
  HostServices &operator=(const HostServices &) = delete;
  /// Builtins stage together under one component. Analysis services are only
  /// offered when a real query adapter exists for this host.
  bool bind(Owner Project, Json Context, bool AnalysisAvailable,
            Diagnostic &Error);
  void setContext(Owner Project, Json Context);
  void publish(Owner Project, const std::string &Id, Json Payload);
  Services &services();
  Events &events();
  std::shared_ptr<HostServiceSession>
  attach(Owner Caller, const Manifest &Plugin, Json Context,
         std::function<void(Json)> Notification);
};

/// Exact visible bindings are captured once for this task. Discovery never
/// refreshes a vanished binding into a same-name replacement.
class HostServiceSession
    : public std::enable_shared_from_this<HostServiceSession> {
  friend class HostServices;
  struct Impl;
  std::shared_ptr<Impl> State;
  explicit HostServiceSession(std::shared_ptr<Impl> State);

public:
  void request(const std::string &Id, const std::string &Operation,
               const Json &Payload, Services::Reply Done);
  void query(const ServiceRequest &Request, Services::Reply Done);
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_HOSTSERVICES_H
