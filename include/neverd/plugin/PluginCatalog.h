//===- PluginCatalog.h - Shared installations and activation ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Host-independent installation intent and owner-backed runtime snapshots.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_PLUGINCATALOG_H
#define NEVERD_PLUGIN_PLUGINCATALOG_H

#include "neverd/plugin/ProcessActions.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::plugin {
struct InstallationRecord {
  std::string Id;
  std::filesystem::path Path;
  bool Enabled = true;
};

struct InstallationSnapshot {
  InstallationRecord Record;
  std::optional<Manifest> Declaration;
  Diagnostic DiscoveryError, ActivationError;
  std::vector<Diagnostic> CleanupErrors;
  std::optional<OwnerSnapshot> Component;
  bool Installed = true;
  bool Bundled = false;
};

struct ProcessActionContribution {
  ActionDefinition Definition;
  ProtocolExtensions Extensions;
};

enum class CatalogChangeKind { Runtime, Record };
struct CatalogChange {
  CatalogChangeKind Kind = CatalogChangeKind::Runtime;
  std::string Plugin;
};

/// Consumers persist records and render snapshots; activation, retirement and
/// exact bindings have one owner here. GUI modules are activated by an adapter;
/// the catalog itself neither loads libraries nor installs package files.
class PluginCatalog {
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  using Changed = std::function<void(const CatalogChange &)>;
  using Bound = std::function<void(const std::string &)>;
  using ActivateGui =
      std::function<bool(Owner, const Manifest &, Diagnostic &)>;
  /// Additional projections register against the same staged action tokens.
  using Contributions = std::function<bool(
      Owner, const std::map<std::string, Registration> &, Diagnostic &)>;
  PluginCatalog(PluginHost &Host, ProcessActions &Processes, Owner Project,
                Changed Notify = {}, Bound Activated = {},
                ActivateGui Gui = {});
  ~PluginCatalog();
  PluginCatalog(const PluginCatalog &) = delete;
  PluginCatalog &operator=(const PluginCatalog &) = delete;
  /// Replaces an empty catalog from saved intent. Missing manifests remain
  /// visible with diagnostics. Invalid record sets change no registration.
  /// Bundled records come from the host distribution. They have unique IDs and
  /// do not consume slots reserved for user installations.
  bool restore(const std::vector<InstallationRecord> &Records,
               Diagnostic &Error,
               const std::vector<InstallationRecord> &Bundled = {});
  bool install(const std::filesystem::path &Path, Diagnostic &Error,
               std::vector<ProcessActionContribution> Actions = {},
               Contributions Register = {});
  /// Consume a declaration already normalized by loadManifest without
  /// rereading.
  bool install(const Manifest &Declaration, Diagnostic &Error,
               std::vector<ProcessActionContribution> Actions = {},
               Contributions Register = {});
  bool setEnabled(const std::string &Id, bool Enabled, Diagnostic &Error);
  void remove(const std::string &Id);
  /// Rebind enabled declarations to a new project owner without rereading
  /// files.
  void setProject(Owner Project);
  /// User installations that can be written to the persistent catalog.
  std::vector<InstallationRecord> records() const;
  std::vector<InstallationSnapshot> snapshots() const;
  std::optional<InstallationSnapshot> snapshot(const std::string &Id) const;
  std::optional<Owner> component(const std::string &Id) const;
  std::optional<Registration> action(const std::string &Id,
                                     const std::string &Action) const;
  bool retiring(const std::string &Id) const;
};

/// Wire/storage projections shared by CLI and GUI persistence adapters.
Json installationRecords(const std::vector<InstallationRecord> &Records);
std::optional<std::vector<InstallationRecord>>
readInstallationRecords(const Json &Document, Diagnostic &Error);
Json describeInstallation(const InstallationSnapshot &Snapshot);
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_PLUGINCATALOG_H
