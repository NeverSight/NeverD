//===- PluginHost.h - Plugin ownership and registration --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qt-independent plugin scopes, atomic activation and asynchronous retirement.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_PLUGINHOST_H
#define NEVERD_PLUGIN_PLUGINHOST_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::plugin {

namespace detail {
struct HostState;
}

enum class OwnerKind { Host, Project, Component, Task, View };
enum class OwnerPhase { Activating, Active, Retiring, Closed };

/// Handles belong to one host and never identify a replacement registration.
class Owner {
  friend class PluginHost;
  friend struct detail::HostState;
  std::weak_ptr<detail::HostState> Host;
  uint64_t Id = 0;
  Owner(std::weak_ptr<detail::HostState> Host, uint64_t Id);

public:
  Owner() = default;
  uint64_t id() const { return Id; }
  bool operator==(const Owner &Other) const {
    return Id == Other.Id && !Host.owner_before(Other.Host) &&
           !Other.Host.owner_before(Host);
  }
};

class Registration {
  friend class PluginHost;
  friend struct detail::HostState;
  std::weak_ptr<detail::HostState> Host;
  uint64_t Id = 0;
  Registration(std::weak_ptr<detail::HostState> Host, uint64_t Id);

public:
  Registration() = default;
  uint64_t id() const { return Id; }
  bool operator==(const Registration &Other) const {
    return Id == Other.Id && !Host.owner_before(Other.Host) &&
           !Other.Host.owner_before(Host);
  }
};

struct Diagnostic {
  std::string Code;
  std::string Message;
  uint64_t OwnerId = 0;
  uint64_t RegistrationId = 0;
};

struct RegistrationInfo {
  std::string Kind;
  std::string Name;
  std::string Contract;
};

struct OwnerSnapshot {
  uint64_t Id = 0;
  uint64_t ParentId = 0;
  OwnerKind Kind = OwnerKind::Host;
  OwnerPhase Phase = OwnerPhase::Activating;
  std::string Name;
  std::string ActivationFailure;
  std::vector<Diagnostic> CleanupErrors;
  size_t Registrations = 0;
  size_t Invocations = 0;
};

/// Move-only execution lease. It can be released on another thread; the host's
/// injected dispatcher returns completion to its control sequence.
class Invocation {
  friend class PluginHost;
  std::shared_ptr<detail::HostState> Host;
  uint64_t RegistrationId = 0;
  Invocation(std::shared_ptr<detail::HostState> Host, uint64_t RegistrationId);

public:
  Invocation() = default;
  Invocation(Invocation &&Other) noexcept;
  Invocation &operator=(Invocation &&Other) noexcept;
  Invocation(const Invocation &) = delete;
  Invocation &operator=(const Invocation &) = delete;
  ~Invocation();
  explicit operator bool() const { return bool(Host); }
  void reset();
};

class PluginHost {
public:
  using Work = std::function<void()>;
  /// Post must enqueue rather than execute inline, including on the control
  /// thread, and must remain available until close completes.
  using Post = std::function<void(Work)>;
  using CleanupDone = std::function<void(std::string)>;
  using Cleanup = std::function<void(CleanupDone)>;
  using CloseDone = std::function<void(const std::vector<Diagnostic> &)>;

  /// Public state operations run on the constructing thread. Plugin callbacks
  /// run without a registry lock. The dispatcher may be a Qt queue or a
  /// headless event loop; neither representation enters this interface.
  explicit PluginHost(Post Dispatch);
  ~PluginHost();
  PluginHost(const PluginHost &) = delete;
  PluginHost &operator=(const PluginHost &) = delete;

  Owner root() const;
  std::optional<Owner> createOwner(Owner Parent, OwnerKind Kind,
                                   std::string Name, Diagnostic &Error);
  bool activate(Owner Scope, Diagnostic &Error);
  void failActivation(Owner Scope, std::string Message);

  /// Registration and cleanup ownership are committed together. Activating
  /// scopes stage definitions; active scopes publish new definitions directly.
  /// Retire stops production/cancels work. Closing a scope waits for children
  /// and all its invocations before disposing resources in reverse registration
  /// order, awaiting each completion. Register instance cleanup before its
  /// users. An individual unregister waits only for that registration's
  /// invocations.
  std::optional<Registration> registerResource(Owner Scope,
                                               RegistrationInfo Info,
                                               Work Retire, Cleanup Dispose,
                                               Diagnostic &Error);
  bool unregister(Registration Token);
  std::optional<Registration> find(Owner Scope, const std::string &Kind,
                                   const std::string &Name) const;
  std::optional<RegistrationInfo> describe(Registration Token) const;
  std::optional<Owner> ownerOf(Registration Token) const;
  Invocation invoke(Registration Token, Diagnostic &Error);

  /// Idempotent. New calls and descendant registrations are rejected before
  /// any retirement callback is delivered. Done runs after children, leases
  /// and asynchronous cleanup finish; an error never skips other cleanup.
  void close(Owner Scope, CloseDone Done = {});
  std::optional<OwnerSnapshot> snapshot(Owner Scope) const;
  /// True only for this host's scopes, including the ancestor itself.
  bool contains(Owner Ancestor, Owner Scope) const;
  std::vector<OwnerSnapshot> owners() const;
  /// Drop completed history. IDs are never reused, so old tokens stay invalid.
  bool forget(Owner Scope);

private:
  std::shared_ptr<detail::HostState> State;
};

} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_PLUGINHOST_H
