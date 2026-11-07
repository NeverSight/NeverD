//===- PluginCatalog.cpp - Shared installations and activation ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Installation records survive unavailability; runtime state comes from NPH.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/PluginCatalog.h"

#include "neverd/plugin/Config.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace neverd::plugin {
namespace {
std::string utf8(const std::filesystem::path &Path) {
  const auto Text = Path.u8string();
  return {Text.begin(), Text.end()};
}
bool validRecord(const InstallationRecord &Record) {
  const auto Path = utf8(Record.Path);
  return validIdentifier(Record.Id) && Record.Path.is_absolute() &&
         !Path.empty() && Path.size() <= limits::kMaxPathCharacters &&
         Path.find('\0') == std::string::npos;
}
Json diagnostic(const Diagnostic &Error) {
  return {{"code", Error.Code},
          {"message", Error.Message},
          {"owner", Error.OwnerId},
          {"registration", Error.RegistrationId}};
}
} // namespace

struct PluginCatalog::Impl : std::enable_shared_from_this<Impl> {
  struct Entry {
    InstallationRecord Record;
    std::optional<Manifest> Declaration;
    Diagnostic DiscoveryError, ActivationError;
    std::vector<Diagnostic> CleanupErrors;
    std::optional<Owner> Component;
    std::map<std::string, Registration> Actions;
    std::vector<ProcessActionContribution> Overrides;
    Contributions Register;
    bool Installed = true;
  };
  PluginHost &Host;
  ProcessActions &Processes;
  Owner Project;
  Changed Notify;
  Bound Activated;
  std::map<std::string, Entry> Entries;

  Impl(PluginHost &Host, ProcessActions &Processes, Owner Project,
       Changed Notify, Bound Activated)
      : Host(Host), Processes(Processes), Project(Project),
        Notify(std::move(Notify)), Activated(std::move(Activated)) {}
  void changed(CatalogChange Change = {}) {
    const auto Callback = Notify;
    if (Callback)
      Callback(Change);
  }
  bool discover(Entry &E) {
    E.DiscoveryError = {};
    std::string Why;
    auto Declaration = loadManifest(E.Record.Path, Why);
    if (!Declaration || Declaration->Id != E.Record.Id) {
      E.DiscoveryError = {
          "manifest_unavailable",
          Declaration ? "The manifest now declares a different plugin ID"
                      : Why};
      return false;
    }
    E.Declaration = std::move(Declaration);
    return true;
  }
  bool activate(Entry &E) {
    E.ActivationError = {};
    if (!E.Record.Enabled || !E.Installed)
      return true;
    if (E.Component) {
      const auto Current = Host.snapshot(*E.Component);
      if (Current && Current->Phase == OwnerPhase::Active)
        return true;
      E.ActivationError = {"plugin_retiring",
                           "Previous plugin instance is still retiring"};
      return false;
    }
    if (!E.Declaration || !E.DiscoveryError.Code.empty())
      return false;
    std::set<std::string> Overrides;
    for (const auto &Item : E.Overrides) {
      const auto &Id = Item.Definition.Id;
      if (!Overrides.insert(Id).second ||
          std::none_of(
              E.Declaration->Actions.begin(), E.Declaration->Actions.end(),
              [&](const auto &Action) { return Action.at("id") == Id; })) {
        E.ActivationError = {
            "invalid_action",
            "Action contribution requires one existing manifest action"};
        return false;
      }
    }
    auto Scope = Host.createOwner(Project, OwnerKind::Component, E.Record.Id,
                                  E.ActivationError);
    if (!Scope)
      return false;
    E.Component = *Scope;
    E.CleanupErrors.clear();
    for (const auto &Item : E.Declaration->Actions) {
      ActionDefinition Definition;
      Definition.Id = Item.at("id");
      ProtocolExtensions Extensions;
      const auto Override = std::find_if(
          E.Overrides.begin(), E.Overrides.end(), [&](const auto &Value) {
            return Value.Definition.Id == Definition.Id;
          });
      if (Override != E.Overrides.end()) {
        Definition = Override->Definition;
        Extensions = Override->Extensions;
      }
      auto Token = Processes.add(*Scope, *E.Declaration, std::move(Definition),
                                 std::move(Extensions), E.ActivationError);
      if (!Token) {
        Host.failActivation(*Scope, E.ActivationError.Message);
        close(E);
        return false;
      }
      E.Actions.emplace(Item.at("id").get<std::string>(), *Token);
    }
    if (E.Register && !E.Register(*Scope, E.Actions, E.ActivationError)) {
      Host.failActivation(*Scope, E.ActivationError.Message);
      close(E);
      return false;
    }
    if (!Host.activate(*Scope, E.ActivationError)) {
      Host.failActivation(*Scope, E.ActivationError.Message);
      close(E);
      return false;
    }
    if (Activated)
      Activated(E.Record.Id);
    return true;
  }
  void close(Entry &E) {
    E.Actions.clear();
    if (!E.Component)
      return;
    const auto Scope = *E.Component;
    const auto Id = E.Record.Id;
    const auto Weak = weak_from_this();
    Host.close(Scope, [Weak, Scope, Id](const auto &Errors) {
      const auto S = Weak.lock();
      if (!S)
        return;
      S->Host.forget(Scope);
      const auto I = S->Entries.find(Id);
      if (I == S->Entries.end() || I->second.Component != Scope)
        return;
      I->second.Component.reset();
      I->second.CleanupErrors = Errors;
      if (!I->second.Installed)
        S->Entries.erase(I);
      S->changed({CatalogChangeKind::Runtime, Id});
    });
  }
  InstallationSnapshot snapshot(const Entry &E) const {
    return {E.Record,
            E.Declaration,
            E.DiscoveryError,
            E.ActivationError,
            E.CleanupErrors,
            E.Component ? Host.snapshot(*E.Component) : std::nullopt,
            E.Installed};
  }
};

PluginCatalog::PluginCatalog(PluginHost &Host, ProcessActions &Processes,
                             Owner Project, Changed Notify, Bound Activated)
    : State(std::make_shared<Impl>(Host, Processes, Project, std::move(Notify),
                                   std::move(Activated))) {}
PluginCatalog::~PluginCatalog() {
  State->Notify = {};
  State->Activated = {};
  for (auto &[Id, E] : State->Entries)
    State->close(E);
}
bool PluginCatalog::restore(const std::vector<InstallationRecord> &Records,
                            Diagnostic &Error) {
  if (!State->Entries.empty() || Records.size() > size_t(limits::kMaxPlugins)) {
    Error = {"invalid_catalog",
             "Restore requires an empty catalog within the installation limit"};
    return false;
  }
  std::set<std::string> Ids;
  for (const auto &Record : Records)
    if (!validRecord(Record) || !Ids.insert(Record.Id).second) {
      Error = {"invalid_catalog", "Invalid or repeated installation record"};
      return false;
    }
  for (const auto &Record : Records) {
    auto &E = State->Entries[Record.Id];
    E.Record = Record;
    if (State->discover(E))
      State->activate(E);
  }
  Error = {};
  State->changed();
  return true;
}
bool PluginCatalog::install(const std::filesystem::path &Path,
                            Diagnostic &Error,
                            std::vector<ProcessActionContribution> Actions,
                            Contributions Register) {
  std::string Why;
  auto Declaration = loadManifest(Path, Why);
  if (!Declaration) {
    Error = {"invalid_manifest", Why};
    return false;
  }
  return install(*Declaration, Error, std::move(Actions), std::move(Register));
}
bool PluginCatalog::install(const Manifest &Plugin, Diagnostic &Error,
                            std::vector<ProcessActionContribution> Actions,
                            Contributions Register) {
  const auto Id = Plugin.Id;
  const auto Found = State->Entries.find(Id);
  if (Found == State->Entries.end() &&
      records().size() >= size_t(limits::kMaxPlugins)) {
    Error = {"installation_limit", "Plugin installation limit reached"};
    return false;
  }
  if (Found != State->Entries.end() && Found->second.Component) {
    const auto Scope = State->Host.snapshot(*Found->second.Component);
    if (Scope && Scope->Phase != OwnerPhase::Active) {
      Error = {"plugin_retiring", "Previous plugin instance is still retiring"};
      return false;
    }
    std::error_code ErrorCode;
    if (!std::filesystem::equivalent(Found->second.Declaration->Path,
                                     Plugin.Path, ErrorCode)) {
      Error = {
          "plugin_already_active",
          "Uninstall the active plugin before importing a different manifest"};
      return false;
    }
    if (!Actions.empty() || Register) {
      Error = {"plugin_already_active", "Disable the active plugin before "
                                        "replacing its action contributions"};
      return false;
    }
    Error = {};
    return true;
  }
  auto &E = State->Entries[Id];
  E.Record = {Id, Plugin.Path, true};
  E.Declaration = Plugin;
  E.Overrides = std::move(Actions);
  E.Register = std::move(Register);
  E.Installed = true;
  E.DiscoveryError = {};
  const bool Active = State->activate(E);
  Error = E.ActivationError;
  State->changed({CatalogChangeKind::Record, Id});
  return Active;
}
bool PluginCatalog::setEnabled(const std::string &Id, bool Enabled,
                               Diagnostic &Error) {
  const auto I = State->Entries.find(Id);
  if (I == State->Entries.end() || !I->second.Installed) {
    Error = {"installation_missing", "Plugin installation no longer exists"};
    return false;
  }
  auto &E = I->second;
  E.Record.Enabled = Enabled;
  if (Enabled && component(Id)) {
    Error = {};
    State->changed({CatalogChangeKind::Record, Id});
    return true;
  }
  bool Active = true;
  if (Enabled) {
    Active = State->discover(E) && State->activate(E);
    Error =
        E.DiscoveryError.Code.empty() ? E.ActivationError : E.DiscoveryError;
  } else {
    E.ActivationError = {};
    State->close(E);
    Error = {};
  }
  State->changed({CatalogChangeKind::Record, Id});
  return Active;
}
void PluginCatalog::remove(const std::string &Id) {
  const auto I = State->Entries.find(Id);
  if (I == State->Entries.end())
    return;
  I->second.Installed = false;
  I->second.Record.Enabled = false;
  State->close(I->second);
  if (!I->second.Component)
    State->Entries.erase(I);
  State->changed({CatalogChangeKind::Record, Id});
}
void PluginCatalog::setProject(Owner Project) {
  if (State->Project == Project)
    return;
  for (auto &[Id, E] : State->Entries) {
    State->close(E);
    if (E.Installed)
      E.Component.reset();
  }
  State->Project = Project;
  for (auto &[Id, E] : State->Entries)
    State->activate(E);
  State->changed();
}
std::vector<InstallationRecord> PluginCatalog::records() const {
  std::vector<InstallationRecord> Result;
  for (const auto &[Id, E] : State->Entries)
    if (E.Installed)
      Result.push_back(E.Record);
  return Result;
}
std::vector<InstallationSnapshot> PluginCatalog::snapshots() const {
  std::vector<InstallationSnapshot> Result;
  for (const auto &[Id, E] : State->Entries)
    Result.push_back(State->snapshot(E));
  return Result;
}
std::optional<InstallationSnapshot>
PluginCatalog::snapshot(const std::string &Id) const {
  const auto I = State->Entries.find(Id);
  return I == State->Entries.end() ? std::nullopt
                                   : std::optional(State->snapshot(I->second));
}
std::optional<Owner> PluginCatalog::component(const std::string &Id) const {
  const auto I = State->Entries.find(Id);
  if (I == State->Entries.end() || !I->second.Component)
    return std::nullopt;
  const auto S = State->Host.snapshot(*I->second.Component);
  return S && S->Phase == OwnerPhase::Active ? I->second.Component
                                             : std::nullopt;
}
std::optional<Registration>
PluginCatalog::action(const std::string &Id, const std::string &Action) const {
  const auto I = State->Entries.find(Id);
  if (I == State->Entries.end())
    return std::nullopt;
  const auto A = I->second.Actions.find(Action);
  return A != I->second.Actions.end() && State->Host.describe(A->second)
             ? std::optional(A->second)
             : std::nullopt;
}
bool PluginCatalog::retiring(const std::string &Id) const {
  const auto S = snapshot(Id);
  return S && S->Component && S->Component->Phase == OwnerPhase::Retiring;
}
Json installationRecords(const std::vector<InstallationRecord> &Records) {
  Json Items = Json::array();
  for (const auto &Record : Records)
    Items.push_back({{"id", Record.Id},
                     {"path", utf8(Record.Path)},
                     {"enabled", Record.Enabled}});
  return {{"schema", "neverd.plugin-catalog.v1"},
          {"plugins", std::move(Items)}};
}
std::optional<std::vector<InstallationRecord>>
readInstallationRecords(const Json &Document, Diagnostic &Error) {
  std::vector<InstallationRecord> Result;
  std::set<std::string> Ids;
  try {
    if (!Document.is_object() || Document.size() != 2 ||
        Document.at("schema") != "neverd.plugin-catalog.v1" ||
        !Document.at("plugins").is_array() ||
        Document["plugins"].size() > size_t(limits::kMaxPlugins))
      throw std::invalid_argument("Invalid installation catalog");
    for (const auto &Item : Document["plugins"]) {
      if (!Item.is_object() || Item.size() != 3 || !Item.at("id").is_string() ||
          !Item.at("path").is_string() || !Item.at("enabled").is_boolean())
        throw std::invalid_argument("Invalid installation record");
      InstallationRecord Record{
          Item["id"], std::filesystem::u8path(Item["path"].get<std::string>()),
          Item["enabled"]};
      if (!validRecord(Record) || !Ids.insert(Record.Id).second)
        throw std::invalid_argument("Invalid or repeated installation record");
      Result.push_back(std::move(Record));
    }
  } catch (const std::exception &Exception) {
    Error = {"invalid_catalog", Exception.what()};
    return std::nullopt;
  }
  Error = {};
  return Result;
}
Json describeInstallation(const InstallationSnapshot &S) {
  Json Result{{"id", S.Record.Id},
              {"path", utf8(S.Record.Path)},
              {"enabled", S.Record.Enabled},
              {"installed", S.Installed},
              {"discovery_error", diagnostic(S.DiscoveryError)},
              {"activation_error", diagnostic(S.ActivationError)}};
  Result["cleanup_errors"] = Json::array();
  for (const auto &Error : S.CleanupErrors)
    Result["cleanup_errors"].push_back(diagnostic(Error));
  if (S.Declaration) {
    Result["name"] = S.Declaration->Name;
    Result["version"] = S.Declaration->Version;
    Result["actions"] = S.Declaration->Actions;
    Result["queries"] = S.Declaration->Queries;
  }
  const auto Phase = S.Component ? S.Component->Phase : OwnerPhase::Closed;
  Result["activation"] = Phase == OwnerPhase::Active       ? "active"
                         : Phase == OwnerPhase::Activating ? "activating"
                         : Phase == OwnerPhase::Retiring   ? "retiring"
                                                           : "inactive";
  if (S.Component) {
    Result["generation"] = S.Component->Id;
    Result["registrations"] = S.Component->Registrations;
    Result["invocations"] = S.Component->Invocations;
  }
  return Result;
}
} // namespace neverd::plugin
