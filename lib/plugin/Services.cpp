//===- Services.cpp - Versioned owner-scoped service dispatch -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share service identity, context admission and invocation lifetime.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Services.h"

#include "neverd/plugin/Schema.h"

#include <map>
#include <utility>

namespace neverd::plugin {
Json serviceError(const Diagnostic &Error) {
  return {{"status", "error"},
          {"error", {{"code", Error.Code}, {"message", Error.Message}}}};
}
Json describeService(const ServiceDescription &S) {
  const auto &D = S.Definition;
  return {
      {"id", D.Id},
      {"version", D.Version},
      {"registration", std::to_string(S.Binding.id())},
      {"description", D.Description},
      {"consistency", D.Consistency == ServiceConsistency::None ? "none"
                      : D.Consistency == ServiceConsistency::Project
                          ? "project"
                          : "revision"},
      {"effect", D.Effect == ServiceEffect::Read ? "read" : "project_write"},
      {"input_schema", D.InputSchema},
      {"output_schema", D.OutputSchema},
      {"available", S.Unavailable.Code.empty()},
      {"unavailable",
       {{"code", S.Unavailable.Code}, {"message", S.Unavailable.Message}}}};
}
struct Services::Impl {
  struct Entry {
    Owner Provider, Domain;
    Registration Token;
    ServiceDefinition Definition;
    Handler Run;
  };
  struct Pending {
    PluginHost &Host;
    std::shared_ptr<Invocation> Lease;
    Registration Token;
    Reply Done;
    Json OutputSchema, Context;
    bool Completed = false;
    explicit Pending(PluginHost &Host) : Host(Host) {}
    ~Pending() { Host.unregister(Token); }
    void finish(Json Value) {
      // describe also enforces the control-sequence contract.
      const bool Active = Host.describe(Token).has_value();
      if (std::exchange(Completed, true))
        return;
      std::string Why;
      if (!Value.is_object() || !Value.contains("status") ||
          !Value["status"].is_string())
        Value = serviceError(
            {"invalid_service_reply", "Service reply requires status"});
      else if (Value["status"] == "ok" && !OutputSchema.is_null() &&
               !validateValue(OutputSchema, Value.value("payload", Json()),
                              Why))
        Value = serviceError({"invalid_service_output", Why});
      Value["context"] = Context;
      auto Notify = std::move(Done);
      Diagnostic Error;
      auto CallerLease = Active ? Host.invoke(Token, Error) : Invocation{};
      Host.unregister(Token);
      if (CallerLease && Notify)
        Notify(std::move(Value));
      Lease.reset();
    }
  };
  PluginHost &Host;
  Context Current;
  std::map<uint64_t, std::shared_ptr<Entry>> Entries;
  uint64_t NextCall = 0;
  Impl(PluginHost &Host, Context Current)
      : Host(Host), Current(std::move(Current)) {}
  std::shared_ptr<Entry> find(Registration Token) const {
    const auto I = Entries.find(Token.id());
    return I != Entries.end() && I->second->Token == Token &&
                   Host.describe(Token)
               ? I->second
               : nullptr;
  }
  Diagnostic admission(Owner Caller, const Entry &E,
                       const Json &Context) const {
    if (!Host.contains(E.Domain, Caller))
      return {"project_mismatch", "Service belongs to another project"};
    const auto Scope = Host.snapshot(Caller);
    if (!Scope || Scope->Phase != OwnerPhase::Active)
      return {"owner_unavailable", "Service caller is not active"};
    if (!Context.is_object())
      return {"invalid_context", "Service context must be an object"};
    if (E.Definition.Consistency == ServiceConsistency::None)
      return {};
    const auto Now = Current(E.Domain);
    if (!Now.is_object() || Now.value("loaded", Json()) != true ||
        Context.value("loaded", Json()) != true ||
        !Context.contains("project_id") || !Context["project_id"].is_string() ||
        Context["project_id"].empty() || !Context.contains("epoch") ||
        !Context["epoch"].is_string() || Context["epoch"].empty() ||
        Context["project_id"] != Now.value("project_id", Json()) ||
        Context["epoch"] != Now.value("epoch", Json()))
      return {"context_changed",
              "Service project context is no longer current"};
    if (E.Definition.Consistency == ServiceConsistency::Revision &&
        (!Context.contains("revision") ||
         Context["revision"] != Now.value("revision", Json())))
      return {"stale_revision",
              "Service analysis revision is no longer current"};
    return {};
  }
};
Services::Services(PluginHost &Host, Context Current)
    : State(std::make_shared<Impl>(Host, std::move(Current))) {}
Services::~Services() {
  const auto Entries = State->Entries;
  for (const auto &[Id, E] : Entries)
    State->Host.unregister(E->Token);
}
std::optional<Registration> Services::add(Owner Provider, Owner Domain,
                                          ServiceDefinition Definition,
                                          Handler Run, Diagnostic &Error) {
  std::string Why;
  const auto Scope = State->Host.snapshot(Domain);
  if (!Scope ||
      (Scope->Kind != OwnerKind::Host && Scope->Kind != OwnerKind::Project) ||
      !State->Host.contains(Domain, Provider) ||
      !validIdentifier(Definition.Id) || !Definition.Version || !Run ||
      (!Definition.InputSchema.is_null() &&
       !validateSchema(Definition.InputSchema, Why)) ||
      (!Definition.OutputSchema.is_null() &&
       !validateSchema(Definition.OutputSchema, Why))) {
    Error = {"invalid_service",
             Why.empty() ? "Invalid service declaration or domain" : Why};
    return std::nullopt;
  }
  for (const auto &[Id, E] : State->Entries)
    if (E->Domain == Domain && E->Definition.Id == Definition.Id &&
        E->Definition.Version == Definition.Version &&
        State->Host.snapshot(E->Provider) &&
        State->Host.snapshot(E->Provider)->Phase != OwnerPhase::Closed) {
      Error = {"duplicate_service",
               "Service version is already registered in this domain"};
      return std::nullopt;
    }
  auto E = std::make_shared<Impl::Entry>();
  E->Provider = Provider;
  E->Domain = Domain;
  E->Definition = std::move(Definition);
  E->Run = std::move(Run);
  const auto Token = State->Host.registerResource(
      Provider,
      {"service",
       E->Definition.Id + "@" + std::to_string(E->Definition.Version),
       "neverd.service.v1"},
      {},
      [E, Weak = std::weak_ptr<Impl>(State)](auto Done) {
        E->Run = {};
        if (auto S = Weak.lock())
          S->Entries.erase(E->Token.id());
        Done({});
      },
      Error);
  if (!Token)
    return std::nullopt;
  E->Token = *Token;
  State->Entries.emplace(Token->id(), std::move(E));
  return Token;
}
std::vector<ServiceDescription> Services::discover(Owner Caller,
                                                   const Json &Context) const {
  std::vector<ServiceDescription> Result;
  for (const auto &[Id, E] : State->Entries)
    if (State->Host.describe(E->Token) &&
        State->Host.contains(E->Domain, Caller))
      Result.push_back(
          {E->Definition, E->Token, State->admission(Caller, *E, Context)});
  return Result;
}
std::optional<Registration> Services::resolve(Owner Caller,
                                              const std::string &Id,
                                              uint32_t Version,
                                              Diagnostic &Error) const {
  std::optional<Registration> Result;
  for (const auto &[Key, E] : State->Entries)
    if (E->Definition.Id == Id && E->Definition.Version == Version &&
        State->Host.describe(E->Token) &&
        State->Host.contains(E->Domain, Caller)) {
      if (Result) {
        Error = {"ambiguous_service",
                 "More than one visible service has this version"};
        return std::nullopt;
      }
      Result = E->Token;
    }
  Error = Result ? Diagnostic{}
                 : Diagnostic{"service_unavailable",
                              "Requested service version is unavailable"};
  return Result;
}
std::optional<ServiceDefinition>
Services::definition(Registration Binding) const {
  const auto E = State->find(Binding);
  return E ? std::optional(E->Definition) : std::nullopt;
}
std::optional<Registration> Services::invoke(Owner Caller, Registration Binding,
                                             ServiceRequest Request, Reply Done,
                                             Diagnostic &Error) {
  const auto E = State->find(Binding);
  if (!E) {
    Error = {"service_unavailable",
             "Exact service registration is no longer active"};
    return std::nullopt;
  }
  Error = State->admission(Caller, *E, Request.Context);
  if (!Error.Code.empty())
    return std::nullopt;
  std::string Why;
  if (!E->Definition.InputSchema.is_null() &&
      !validateValue(E->Definition.InputSchema, Request.Payload, Why)) {
    Error = {"invalid_service_input", Why};
    return std::nullopt;
  }
  auto P = std::make_shared<Impl::Pending>(State->Host);
  P->Lease = std::make_shared<Invocation>(State->Host.invoke(Binding, Error));
  if (!*P->Lease)
    return std::nullopt;
  // A synchronous reply can close the provider and drain the control queue.
  // Keep its code leased until the handler itself has returned.
  const auto HandlerLease = P->Lease;
  P->Done = std::move(Done);
  P->OutputSchema = E->Definition.OutputSchema;
  P->Context = Request.Context;
  const auto Token = State->Host.registerResource(
      Caller,
      {"service_call", std::to_string(++State->NextCall),
       "neverd.service-call.v1"},
      [Weak = std::weak_ptr<Impl::Pending>(P)] {
        if (auto Call = Weak.lock())
          Call->Done = {};
      },
      {}, Error);
  if (!Token)
    return std::nullopt;
  P->Token = *Token;
  Request.Operation = E->Definition.Id;
  auto Run = E->Run;
  Run(Request, [P](Json Reply) { P->finish(std::move(Reply)); });
  return Token;
}
} // namespace neverd::plugin
