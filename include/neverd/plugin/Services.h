//===- Services.h - Versioned owner-scoped services ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Discover exact service bindings and invoke them in a captured context.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_SERVICES_H
#define NEVERD_PLUGIN_SERVICES_H

#include "neverd/plugin/Manifest.h"
#include "neverd/plugin/PluginHost.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::plugin {
enum class ServiceConsistency { None, Project, Revision };
enum class ServiceEffect { Read, ProjectWrite };
struct ServiceDefinition {
  std::string Id, Description;
  uint32_t Version = 1;
  ServiceConsistency Consistency = ServiceConsistency::Revision;
  ServiceEffect Effect = ServiceEffect::Read;
  /// Null delegates payload validation to the service's semantic owner.
  Json InputSchema, OutputSchema;
};
struct ServiceRequest {
  std::string Task, Id, Operation;
  Json Context = Json::object(), Payload = Json::object();
};
struct ServiceDescription {
  ServiceDefinition Definition;
  Registration Binding;
  Diagnostic Unavailable;
};
Json serviceError(const Diagnostic &Error);
Json describeService(const ServiceDescription &Service);

/// All methods and reply callbacks run on the NPH control sequence. An async
/// handler retains its reply until completion, or destroys it when detached.
/// Its provider lease lasts through that completion/detachment. Retiring the
/// caller removes its callback; retiring the provider rejects new requests.
class Services {
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  using Reply = std::function<void(Json)>;
  using Handler = std::function<void(const ServiceRequest &, Reply)>;
  using Context = std::function<Json(Owner)>;
  Services(PluginHost &Host, Context Current);
  ~Services();
  Services(const Services &) = delete;
  Services &operator=(const Services &) = delete;
  std::optional<Registration> add(Owner Provider, Owner Domain,
                                  ServiceDefinition Definition, Handler Run,
                                  Diagnostic &Error);
  std::vector<ServiceDescription> discover(Owner Caller,
                                           const Json &Context) const;
  std::optional<Registration> resolve(Owner Caller, const std::string &Id,
                                      uint32_t Version,
                                      Diagnostic &Error) const;
  std::optional<ServiceDefinition> definition(Registration Binding) const;
  /// The returned call registration may be unregistered to detach a read. An
  /// accepted write still finishes in its adapter and retains its real receipt.
  std::optional<Registration> invoke(Owner Caller, Registration Binding,
                                     ServiceRequest Request, Reply Done,
                                     Diagnostic &Error);
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_SERVICES_H
