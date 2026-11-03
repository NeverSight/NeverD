//===- CatalogCommand.cpp - Headless installation management --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// CLI management consumes the same catalog and lifecycle as the workbench.
//===----------------------------------------------------------------------===//

#include "CatalogCommand.h"

#include "neverd/plugin/Artifacts.h"
#include "neverd/plugin/Config.h"
#include "neverd/plugin/PluginCatalog.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace neverd::plugin;

int runPluginCatalog(int Argc, char **Argv) {
  if (Argc < 4)
    throw std::runtime_error(
        "plugins requires <catalog.json> "
        "<list|explain|import|enable|disable|remove> [target]");
  const auto Path = std::filesystem::absolute(std::filesystem::u8path(Argv[2]));
  const std::string Command = Argv[3];
  if ((Command == "list" && Argc != 4) || (Command != "list" && Argc != 5))
    throw std::runtime_error("Invalid catalog command arguments");
  if (Command != "list" && Command != "explain" && Command != "import" &&
      Command != "enable" && Command != "disable" && Command != "remove")
    throw std::runtime_error("Unknown catalog command: " + Command);
  Executor Queue;
  PluginHost Host([&](auto Work) { Queue.post(std::move(Work)); });
  Actions ActionService(Host);
  ProcessActions Processes(Host, ActionService, Queue, Path.parent_path(), {});
  Diagnostic Error;
  const auto Project =
      Host.createOwner(Host.root(), OwnerKind::Project, "catalog", Error);
  if (!Project || !Host.activate(*Project, Error))
    throw std::runtime_error(Error.Message);
  PluginCatalog Catalog(Host, Processes, *Project);
  std::vector<InstallationRecord> Records;
  if (std::filesystem::exists(Path)) {
    if (std::filesystem::file_size(Path) > limits::kMaxManifestBytes)
      throw std::runtime_error("Installation catalog exceeds 64 KiB");
    std::ifstream Input(Path, std::ios::binary);
    const auto Document = Json::parse(Input, nullptr, false);
    auto Parsed = readInstallationRecords(Document, Error);
    if (!Parsed)
      throw std::runtime_error(Error.Message);
    Records = std::move(*Parsed);
  }
  if (!Catalog.restore(Records, Error))
    throw std::runtime_error(Error.Message);
  bool Accepted = true;
  const std::string Target = Argc == 5 ? Argv[4] : "";
  if (Command == "import")
    Accepted = Catalog.install(std::filesystem::u8path(Target), Error);
  else if (Command == "enable" || Command == "disable")
    Accepted = Catalog.setEnabled(Target, Command == "enable", Error);
  else if (Command == "remove") {
    if (!Catalog.snapshot(Target)) {
      Error = {"installation_missing", "Plugin installation no longer exists"};
      Accepted = false;
    } else
      Catalog.remove(Target);
  }
  Queue.drain();
  if (Command != "list" && Command != "explain") {
    std::filesystem::create_directories(Path.parent_path());
    std::string Why;
    const auto Document = installationRecords(Catalog.records());
    if (Document.dump(2).size() > size_t(limits::kMaxManifestBytes) ||
        !writeJsonFile(Path, Document, Why))
      throw std::runtime_error(
          Why.empty() ? "Installation catalog exceeds 64 KiB" : Why);
  }
  Json Result{{"status", Accepted ? "ok" : "error"},
              {"plugins", Json::array()}};
  if (Command == "explain") {
    const auto Snapshot = Catalog.snapshot(Target);
    if (!Snapshot)
      throw std::runtime_error("Unknown plugin installation: " + Target);
    Result["plugins"].push_back(describeInstallation(*Snapshot));
  } else
    for (const auto &Snapshot : Catalog.snapshots())
      Result["plugins"].push_back(describeInstallation(Snapshot));
  if (!Accepted)
    Result["error"] = {{"code", Error.Code}, {"message", Error.Message}};
  std::cout << Result.dump(2, ' ', false, Json::error_handler_t::replace)
            << '\n';
  Host.close(Host.root());
  Queue.drain();
  return Accepted ? 0 : 1;
}
