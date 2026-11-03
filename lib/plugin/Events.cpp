//===- Events.cpp - Scoped queued notifications --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Event schemas, project visibility and subscriptions share NPH ownership.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Events.h"

#include "neverd/plugin/Schema.h"

#include <map>
#include <utility>

namespace neverd::plugin {
Json describeEvent(const Event &E) {
  return {{"id", E.Id},
          {"version", E.Version},
          {"sequence", std::to_string(E.Sequence)},
          {"registration", std::to_string(E.Source.id())},
          {"context", E.Context},
          {"payload", E.Payload}};
}
Json describeEventSource(const EventDescription &E) {
  return {{"id", E.Definition.Id},
          {"version", E.Definition.Version},
          {"description", E.Definition.Description},
          {"payload_schema", E.Definition.PayloadSchema},
          {"registration", std::to_string(E.Binding.id())}};
}
struct Events::Impl {
  struct Source {
    Owner Provider, Domain;
    Registration Token;
    EventDefinition Definition;
    bool Retired = false;
  };
  struct Subscription {
    Registration Token, Source;
    Handler Notify;
  };
  PluginHost &Host;
  PluginHost::Post Post;
  uint64_t NextSubscription = 0, NextSequence = 0;
  std::map<uint64_t, std::shared_ptr<Source>> Sources;
  std::map<uint64_t, std::shared_ptr<Subscription>> Subscriptions;
  Impl(PluginHost &Host, PluginHost::Post Post)
      : Host(Host), Post(std::move(Post)) {}
  std::shared_ptr<Source> find(Registration Token, bool Active = true) const {
    const auto I = Sources.find(Token.id());
    return I != Sources.end() && I->second->Token == Token &&
                   !I->second->Retired && (!Active || Host.describe(Token))
               ? I->second
               : nullptr;
  }
};
Events::Events(PluginHost &Host, PluginHost::Post Post)
    : State(std::make_shared<Impl>(Host, std::move(Post))) {}
Events::~Events() {
  const auto Sources = State->Sources;
  const auto Subscriptions = State->Subscriptions;
  for (const auto &[Id, S] : Sources)
    State->Host.unregister(S->Token);
  for (const auto &[Id, S] : Subscriptions)
    State->Host.unregister(S->Token);
}
std::optional<Registration> Events::add(Owner Provider, Owner Domain,
                                        EventDefinition Definition,
                                        Diagnostic &Error) {
  std::string Why;
  const auto Scope = State->Host.snapshot(Domain);
  if (!Scope ||
      (Scope->Kind != OwnerKind::Host && Scope->Kind != OwnerKind::Project) ||
      !State->Host.contains(Domain, Provider) ||
      !validIdentifier(Definition.Id) || !Definition.Version ||
      (!Definition.PayloadSchema.is_null() &&
       !validateSchema(Definition.PayloadSchema, Why))) {
    Error = {"invalid_event",
             Why.empty() ? "Invalid event declaration or domain" : Why};
    return std::nullopt;
  }
  for (const auto &[Id, S] : State->Sources)
    if (S->Domain == Domain && S->Definition.Id == Definition.Id &&
        S->Definition.Version == Definition.Version) {
      Error = {"duplicate_event",
               "Event version is already registered in this domain"};
      return std::nullopt;
    }
  auto S = std::make_shared<Impl::Source>();
  S->Provider = Provider;
  S->Domain = Domain;
  S->Definition = std::move(Definition);
  auto Token = State->Host.registerResource(
      Provider,
      {"event", S->Definition.Id + "@" + std::to_string(S->Definition.Version),
       "neverd.event.v1"},
      [Weak = std::weak_ptr<Impl>(State), S] {
        S->Retired = true;
        if (auto State = Weak.lock()) {
          const auto Subscriptions = State->Subscriptions;
          for (const auto &[Id, Sub] : Subscriptions)
            if (Sub->Source == S->Token)
              State->Host.unregister(Sub->Token);
        }
      },
      [Weak = std::weak_ptr<Impl>(State), S](auto Done) {
        if (auto State = Weak.lock())
          State->Sources.erase(S->Token.id());
        Done({});
      },
      Error);
  if (!Token)
    return std::nullopt;
  S->Token = *Token;
  State->Sources.emplace(Token->id(), std::move(S));
  return Token;
}
std::vector<EventDescription> Events::discover(Owner Caller) const {
  std::vector<EventDescription> Result;
  for (const auto &[Id, S] : State->Sources)
    if (State->Host.describe(S->Token) &&
        State->Host.contains(S->Domain, Caller))
      Result.push_back({S->Definition, S->Token});
  return Result;
}
std::optional<Registration> Events::resolve(Owner Caller, const std::string &Id,
                                            uint32_t Version,
                                            Diagnostic &Error) const {
  std::optional<Registration> Result;
  for (const auto &D : discover(Caller))
    if (D.Definition.Id == Id && D.Definition.Version == Version) {
      if (Result) {
        Error = {"ambiguous_event",
                 "More than one visible event has this version"};
        return std::nullopt;
      }
      Result = D.Binding;
    }
  Error = Result ? Diagnostic{}
                 : Diagnostic{"event_unavailable",
                              "Requested event version is unavailable"};
  return Result;
}
std::optional<Registration> Events::subscribe(Owner Subscriber,
                                              Registration Source,
                                              Handler Notify,
                                              Diagnostic &Error) {
  const auto S = State->find(Source, false);
  if (!S || !Notify || !State->Host.contains(S->Domain, Subscriber)) {
    Error = {"event_unavailable",
             "Event registration is unavailable in this project"};
    return std::nullopt;
  }
  auto Sub = std::make_shared<Impl::Subscription>();
  Sub->Source = Source;
  Sub->Notify = std::move(Notify);
  auto Token = State->Host.registerResource(
      Subscriber,
      {"event_subscription", std::to_string(++State->NextSubscription),
       "neverd.subscription.v1"},
      {},
      [Weak = std::weak_ptr<Impl>(State), Sub](auto Done) {
        Sub->Notify = {};
        if (auto State = Weak.lock())
          State->Subscriptions.erase(Sub->Token.id());
        Done({});
      },
      Error);
  if (!Token)
    return std::nullopt;
  Sub->Token = *Token;
  State->Subscriptions.emplace(Token->id(), std::move(Sub));
  return Token;
}
bool Events::publish(Registration Source, Json Context, Json Payload,
                     Diagnostic &Error) {
  const auto S = State->find(Source);
  if (!S) {
    Error = {"event_unavailable", "Exact event source is no longer active"};
    return false;
  }
  std::string Why;
  if (!Context.is_object() ||
      (!S->Definition.PayloadSchema.is_null() &&
       !validateValue(S->Definition.PayloadSchema, Payload, Why))) {
    Error = {"invalid_event_payload",
             Why.empty() ? "Event context must be an object" : Why};
    return false;
  }
  const Event Value{S->Definition.Id,      S->Definition.Version,
                    ++State->NextSequence, Source,
                    std::move(Context),    std::move(Payload)};
  for (const auto &[Id, Sub] : State->Subscriptions)
    if (Sub->Source == Source && State->Host.describe(Sub->Token))
      State->Post(
          [Weak = std::weak_ptr<Impl>(State), Token = Sub->Token, Value] {
            const auto State = Weak.lock();
            if (!State)
              return;
            const auto I = State->Subscriptions.find(Token.id());
            if (I == State->Subscriptions.end() || I->second->Token != Token)
              return;
            Diagnostic Error;
            auto SourceLease = State->Host.invoke(Value.Source, Error);
            auto ObserverLease = State->Host.invoke(Token, Error);
            if (SourceLease && ObserverLease) {
              auto Notify = I->second->Notify;
              Notify(Value);
            }
          });
  Error = {};
  return true;
}
} // namespace neverd::plugin
