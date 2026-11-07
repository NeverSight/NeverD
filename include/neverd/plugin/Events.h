//===- Events.h - Scoped event sources and subscriptions ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Immutable queued notifications with exact source and subscriber identity.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_EVENTS_H
#define NEVERD_PLUGIN_EVENTS_H

#include "neverd/plugin/Manifest.h"
#include "neverd/plugin/PluginHost.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::plugin {
struct EventDefinition {
  std::string Id, Description;
  uint32_t Version = 1;
  Json PayloadSchema;
};
struct Event {
  std::string Id;
  uint32_t Version = 1;
  uint64_t Sequence = 0;
  Registration Source;
  Json Context, Payload;
};
struct EventDescription {
  EventDefinition Definition;
  Registration Binding;
};
Json describeEvent(const Event &Value);
Json describeEventSource(const EventDescription &Value);

/// Publication queues immutable envelopes on the supplied control dispatcher.
/// Delivery rechecks both registrations and holds their invocation leases.
/// Queued notifications cannot revive an unloaded/replaced source or observer.
class Events {
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  using Handler = std::function<void(const Event &)>;
  Events(PluginHost &Host, PluginHost::Post Post);
  ~Events();
  Events(const Events &) = delete;
  Events &operator=(const Events &) = delete;
  std::optional<Registration> add(Owner Provider, Owner Domain,
                                  EventDefinition Definition,
                                  Diagnostic &Error);
  std::vector<EventDescription> discover(Owner Caller) const;
  std::optional<Registration> resolve(Owner Caller, const std::string &Id,
                                      uint32_t Version,
                                      Diagnostic &Error) const;
  std::optional<Registration> subscribe(Owner Subscriber, Registration Source,
                                        Handler Notify, Diagnostic &Error);
  bool publish(Registration Source, Json Context, Json Payload,
               Diagnostic &Error);
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_EVENTS_H
