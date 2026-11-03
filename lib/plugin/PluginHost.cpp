//===- PluginHost.cpp - Plugin ownership and registration -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic component activation and asynchronous, owner-scoped retirement.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/PluginHost.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

namespace neverd::plugin {
namespace detail {

struct Scope {
  OwnerSnapshot Info;
  std::vector<uint64_t> Children;
  std::vector<uint64_t> Resources;
  std::vector<PluginHost::CloseDone> Waiters;
};

struct Resource {
  uint64_t Id = 0;
  uint64_t OwnerId = 0;
  RegistrationInfo Info;
  PluginHost::Work Retire;
  PluginHost::Cleanup Dispose;
  size_t Invocations = 0;
  bool Registered = true;
  bool RetirementStarted = false;
  bool Retired = false;
  bool Disposing = false;
  bool Disposed = false;
};

struct HostState : std::enable_shared_from_this<HostState> {
  PluginHost::Post Post;
  std::thread::id Thread = std::this_thread::get_id();
  uint64_t NextOwner = 1;
  uint64_t NextRegistration = 1;
  std::map<uint64_t, Scope> Scopes;
  std::map<uint64_t, Resource> Resources;

  explicit HostState(PluginHost::Post Dispatch) : Post(std::move(Dispatch)) {
    if (!Post)
      throw std::invalid_argument("PluginHost requires a control dispatcher");
    Scope Root;
    Root.Info.Id = NextOwner++;
    Root.Info.Kind = OwnerKind::Host;
    Root.Info.Phase = OwnerPhase::Active;
    Root.Info.Name = "host";
    Scopes.emplace(Root.Info.Id, std::move(Root));
  }

  void checkThread() const {
    if (Thread != std::this_thread::get_id())
      throw std::logic_error(
          "PluginHost accessed outside its control sequence");
  }

  bool owns(const Owner &Token) const {
    return Token.Host.lock().get() == this && Scopes.contains(Token.Id);
  }

  bool owns(const Registration &Token) const {
    return Token.Host.lock().get() == this && Resources.contains(Token.Id);
  }

  bool live(uint64_t Id, bool RequireActive) const {
    while (Id) {
      const auto &S = Scopes.at(Id).Info;
      if (S.Phase == OwnerPhase::Retiring || S.Phase == OwnerPhase::Closed ||
          (RequireActive && S.Phase != OwnerPhase::Active))
        return false;
      Id = S.ParentId;
    }
    return true;
  }

  void recordError(uint64_t OwnerId, uint64_t RegistrationId,
                   std::string Message) {
    Scopes.at(OwnerId).Info.CleanupErrors.push_back(
        {"cleanup_failed", std::move(Message), OwnerId, RegistrationId});
  }

  void finishScope(uint64_t Id) {
    auto &S = Scopes.at(Id);
    if (S.Info.Phase != OwnerPhase::Retiring)
      return;
    for (auto Child : S.Children)
      if (Scopes.at(Child).Info.Phase != OwnerPhase::Closed)
        return;
    for (auto Id : S.Resources) {
      const auto &R = Resources.at(Id);
      if (!R.Retired || R.Invocations)
        return;
    }
    // Teardown is a scope barrier, not just a per-registration barrier. An
    // instance registered first may be used by every action and child task.
    // Dispose in reverse registration order, awaiting each asynchronous step.
    for (auto I = S.Resources.rbegin(); I != S.Resources.rend(); ++I) {
      dispose(*I);
      if (!Resources.at(*I).Disposed)
        return;
    }
    S.Info.Phase = OwnerPhase::Closed;
    for (auto Child : S.Children) {
      const auto &Errors = Scopes.at(Child).Info.CleanupErrors;
      S.Info.CleanupErrors.insert(S.Info.CleanupErrors.end(), Errors.begin(),
                                  Errors.end());
    }
    auto Waiters = std::move(S.Waiters);
    auto Errors = S.Info.CleanupErrors;
    for (auto &Done : Waiters)
      Post([Done = std::move(Done), Errors] { Done(Errors); });
    if (S.Info.ParentId)
      finishScope(S.Info.ParentId);
  }

  void dispose(uint64_t Id) {
    auto &R = Resources.at(Id);
    if (!R.Retired || R.Invocations || R.Disposing || R.Disposed)
      return;
    R.Disposing = true;
    if (!R.Dispose) {
      R.Disposed = true;
      return;
    }
    auto Self = shared_from_this();
    auto Done = [Self, Id](std::string Error) {
      Self->Post([Self, Id, Error = std::move(Error)] {
        Self->checkThread();
        auto It = Self->Resources.find(Id);
        if (It == Self->Resources.end() || It->second.Disposed)
          return;
        auto &Item = It->second;
        if (!Error.empty())
          Self->recordError(Item.OwnerId, Id, Error);
        Item.Disposed = true;
        Item.Dispose = {};
        Self->finishScope(Item.OwnerId);
      });
    };
    // Retain the registered cleanup (and any module capture) until Done.
    auto Cleanup = R.Dispose;
    try {
      Cleanup(Done);
    } catch (const std::exception &E) {
      Done(E.what());
    } catch (...) {
      Done("Plugin cleanup threw an unknown exception");
    }
  }

  void retire(uint64_t Id) {
    auto &R = Resources.at(Id);
    if (R.RetirementStarted)
      return;
    R.Registered = false;
    R.RetirementStarted = true;
    auto Stop = std::move(R.Retire);
    if (Stop) {
      try {
        Stop();
      } catch (const std::exception &E) {
        recordError(R.OwnerId, Id, E.what());
      } catch (...) {
        recordError(R.OwnerId, Id,
                    "Plugin retirement threw an unknown exception");
      }
    }
    R.Retired = true;
    if (Scopes.at(R.OwnerId).Info.Phase == OwnerPhase::Retiring)
      finishScope(R.OwnerId);
    else
      dispose(Id);
  }

  void close(uint64_t Id, PluginHost::CloseDone Done) {
    checkThread();
    auto &S = Scopes.at(Id);
    if (S.Info.Phase == OwnerPhase::Closed) {
      if (Done)
        Post([Done = std::move(Done), Errors = S.Info.CleanupErrors] {
          Done(Errors);
        });
      return;
    }
    if (Done)
      S.Waiters.push_back(std::move(Done));
    if (S.Info.Phase == OwnerPhase::Retiring)
      return;
    std::vector<uint64_t> Pending{Id};
    for (size_t I = 0; I < Pending.size(); ++I) {
      auto &Current = Scopes.at(Pending[I]);
      if (Current.Info.Phase == OwnerPhase::Closed)
        continue;
      Current.Info.Phase = OwnerPhase::Retiring;
      Pending.insert(Pending.end(), Current.Children.begin(),
                     Current.Children.end());
    }
    // Revoke the entire subtree before allowing callbacks to reenter the host.
    for (auto ScopeId : Pending)
      for (auto R : Scopes.at(ScopeId).Resources)
        Resources.at(R).Registered = false;
    for (auto I = Pending.rbegin(); I != Pending.rend(); ++I) {
      const auto Registrations = Scopes.at(*I).Resources;
      for (auto R : Registrations)
        retire(R);
      finishScope(*I);
    }
  }

  void release(uint64_t Id) {
    checkThread();
    auto &R = Resources.at(Id);
    --R.Invocations;
    if (Scopes.at(R.OwnerId).Info.Phase == OwnerPhase::Retiring)
      finishScope(R.OwnerId);
    else
      dispose(Id);
  }

  OwnerSnapshot snapshot(uint64_t Id) const {
    auto Result = Scopes.at(Id).Info;
    for (auto R : Scopes.at(Id).Resources) {
      Result.Registrations += Resources.at(R).Registered;
      Result.Invocations += Resources.at(R).Invocations;
    }
    return Result;
  }
};
} // namespace detail

Owner::Owner(std::weak_ptr<detail::HostState> Host, uint64_t Id)
    : Host(std::move(Host)), Id(Id) {}
Registration::Registration(std::weak_ptr<detail::HostState> Host, uint64_t Id)
    : Host(std::move(Host)), Id(Id) {}

Invocation::Invocation(std::shared_ptr<detail::HostState> Host, uint64_t Id)
    : Host(std::move(Host)), RegistrationId(Id) {}
Invocation::Invocation(Invocation &&Other) noexcept
    : Host(std::move(Other.Host)), RegistrationId(Other.RegistrationId) {}
Invocation &Invocation::operator=(Invocation &&Other) noexcept {
  if (this != &Other) {
    reset();
    Host = std::move(Other.Host);
    RegistrationId = Other.RegistrationId;
  }
  return *this;
}
Invocation::~Invocation() { reset(); }
void Invocation::reset() {
  if (auto State = std::move(Host))
    State->Post([State, Id = RegistrationId] { State->release(Id); });
}

PluginHost::PluginHost(Post Dispatch)
    : State(std::make_shared<detail::HostState>(std::move(Dispatch))) {}
PluginHost::~PluginHost() { State->close(1, {}); }
Owner PluginHost::root() const { return Owner(State, 1); }

std::optional<Owner> PluginHost::createOwner(Owner Parent, OwnerKind Kind,
                                             std::string Name,
                                             Diagnostic &Error) {
  State->checkThread();
  if (!State->owns(Parent) || !State->live(Parent.Id, false)) {
    Error = {"owner_unavailable", "Parent owner is invalid or retiring",
             Parent.Id};
    return std::nullopt;
  }
  detail::Scope S;
  S.Info.Id = State->NextOwner++;
  S.Info.ParentId = Parent.Id;
  S.Info.Kind = Kind;
  S.Info.Name = std::move(Name);
  auto Id = S.Info.Id;
  State->Scopes.emplace(Id, std::move(S));
  State->Scopes.at(Parent.Id).Children.push_back(Id);
  Error = {};
  return Owner(State, Id);
}

bool PluginHost::activate(Owner Scope, Diagnostic &Error) {
  State->checkThread();
  if (!State->owns(Scope) ||
      State->Scopes.at(Scope.Id).Info.Phase != OwnerPhase::Activating ||
      !State->live(State->Scopes.at(Scope.Id).Info.ParentId, true)) {
    Error = {"activation_failed", "Owner or its parent is not activatable",
             Scope.Id};
    return false;
  }
  State->Scopes.at(Scope.Id).Info.Phase = OwnerPhase::Active;
  Error = {};
  return true;
}

void PluginHost::failActivation(Owner Scope, std::string Message) {
  State->checkThread();
  if (!State->owns(Scope))
    return;
  auto &Info = State->Scopes.at(Scope.Id).Info;
  if (Info.Phase != OwnerPhase::Activating)
    return;
  Info.ActivationFailure = std::move(Message);
  State->close(Scope.Id, {});
}

std::optional<Registration>
PluginHost::registerResource(Owner Scope, RegistrationInfo Info, Work Retire,
                             Cleanup Dispose, Diagnostic &Error) {
  State->checkThread();
  if (!State->owns(Scope) || !State->live(Scope.Id, false)) {
    Error = {"owner_unavailable", "Registration owner is invalid or retiring",
             Scope.Id};
    return std::nullopt;
  }
  if (Info.Kind.empty() || Info.Name.empty()) {
    Error = {"invalid_registration", "Registration kind and name are required",
             Scope.Id};
    return std::nullopt;
  }
  auto &S = State->Scopes.at(Scope.Id);
  for (auto Id : S.Resources) {
    const auto &R = State->Resources.at(Id);
    if (R.Registered && R.Info.Kind == Info.Kind && R.Info.Name == Info.Name) {
      Error = {"duplicate_registration", "Registration already exists",
               Scope.Id, Id};
      return std::nullopt;
    }
  }
  detail::Resource R;
  R.Id = State->NextRegistration++;
  R.OwnerId = Scope.Id;
  R.Info = std::move(Info);
  R.Retire = std::move(Retire);
  R.Dispose = std::move(Dispose);
  const auto Id = R.Id;
  S.Resources.reserve(S.Resources.size() + 1);
  State->Resources.emplace(Id, std::move(R));
  S.Resources.push_back(Id);
  Error = {};
  return Registration(State, Id);
}

bool PluginHost::unregister(Registration Token) {
  State->checkThread();
  if (!State->owns(Token))
    return false;
  State->retire(Token.Id);
  return true;
}

std::optional<Registration> PluginHost::find(Owner Scope,
                                             const std::string &Kind,
                                             const std::string &Name) const {
  State->checkThread();
  if (!State->owns(Scope) || !State->live(Scope.Id, true))
    return std::nullopt;
  for (auto Id : State->Scopes.at(Scope.Id).Resources) {
    const auto &R = State->Resources.at(Id);
    if (R.Registered && R.Info.Kind == Kind && R.Info.Name == Name)
      return Registration(State, Id);
  }
  return std::nullopt;
}

std::optional<RegistrationInfo> PluginHost::describe(Registration Token) const {
  State->checkThread();
  if (!State->owns(Token))
    return std::nullopt;
  const auto &R = State->Resources.at(Token.Id);
  if (!R.Registered || !State->live(R.OwnerId, true))
    return std::nullopt;
  return R.Info;
}

Invocation PluginHost::invoke(Registration Token, Diagnostic &Error) {
  State->checkThread();
  if (!State->owns(Token)) {
    Error = {"invalid_registration", "Registration belongs to another host"};
    return {};
  }
  auto &R = State->Resources.at(Token.Id);
  if (!R.Registered || !State->live(R.OwnerId, true)) {
    Error = {"registration_retired", "Registration is not active", R.OwnerId,
             R.Id};
    return {};
  }
  ++R.Invocations;
  Error = {};
  return Invocation(State, R.Id);
}

std::optional<Owner> PluginHost::ownerOf(Registration Token) const {
  if (!describe(Token))
    return std::nullopt;
  return Owner(State, State->Resources.at(Token.Id).OwnerId);
}

void PluginHost::close(Owner Scope, CloseDone Done) {
  State->checkThread();
  if (!State->owns(Scope)) {
    if (Done)
      State->Post([Done = std::move(Done)] {
        Done({{"invalid_owner", "Owner belongs to another host"}});
      });
    return;
  }
  State->close(Scope.Id, std::move(Done));
}

std::optional<OwnerSnapshot> PluginHost::snapshot(Owner Scope) const {
  State->checkThread();
  if (!State->owns(Scope))
    return std::nullopt;
  return State->snapshot(Scope.Id);
}

bool PluginHost::contains(Owner Ancestor, Owner Scope) const {
  State->checkThread();
  if (!State->owns(Ancestor) || !State->owns(Scope))
    return false;
  for (auto Id = Scope.Id; Id; Id = State->Scopes.at(Id).Info.ParentId)
    if (Id == Ancestor.Id)
      return true;
  return false;
}

std::vector<OwnerSnapshot> PluginHost::owners() const {
  State->checkThread();
  std::vector<OwnerSnapshot> Result;
  for (const auto &[Id, S] : State->Scopes)
    Result.push_back(State->snapshot(Id));
  return Result;
}

bool PluginHost::forget(Owner Scope) {
  State->checkThread();
  if (!State->owns(Scope) || Scope.Id == 1 ||
      State->Scopes.at(Scope.Id).Info.Phase != OwnerPhase::Closed)
    return false;
  const auto &S = State->Scopes.at(Scope.Id);
  auto &Parent = State->Scopes.at(S.Info.ParentId);
  if (Parent.Info.Phase != OwnerPhase::Closed)
    Parent.Info.CleanupErrors.insert(Parent.Info.CleanupErrors.end(),
                                     S.Info.CleanupErrors.begin(),
                                     S.Info.CleanupErrors.end());
  std::erase(Parent.Children, Scope.Id);
  std::vector<uint64_t> Pending{Scope.Id};
  for (size_t I = 0; I < Pending.size(); ++I) {
    const auto &Current = State->Scopes.at(Pending[I]);
    Pending.insert(Pending.end(), Current.Children.begin(),
                   Current.Children.end());
    for (auto Id : Current.Resources)
      State->Resources.erase(Id);
  }
  for (auto Id : Pending)
    State->Scopes.erase(Id);
  return true;
}

} // namespace neverd::plugin
