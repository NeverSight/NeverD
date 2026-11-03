//===- PluginCatalogTests.cpp - Installation and generation behavior ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise saved intent, independent activation, cleanup and exact bindings.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/PluginCatalog.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace neverd::plugin;
namespace {
void check(bool Condition, const std::string &Why) {
  if (!Condition)
    throw std::runtime_error(Why);
}
Owner project(PluginHost &Host, const std::string &Name) {
  Diagnostic Error;
  const auto Owner =
      Host.createOwner(Host.root(), OwnerKind::Project, Name, Error);
  check(Owner && Host.activate(*Owner, Error), Error.Message);
  return *Owner;
}
struct Fixture {
  std::filesystem::path Root;
  Executor Queue;
  PluginHost Host;
  Actions ActionService;
  ProcessActions Processes;
  Owner Project;
  PluginCatalog Catalog;
  std::filesystem::path Program;
  explicit Fixture(const char *Executable)
      : Root(std::filesystem::temp_directory_path() /
             ("neverd-catalog-" + std::to_string(std::random_device{}()))),
        Host([this](auto Work) { Queue.post(std::move(Work)); }),
        ActionService(Host), Processes(Host, ActionService, Queue, Root, {}),
        Project(project(Host, "first")), Catalog(Host, Processes, Project),
        Program(
            std::filesystem::absolute(std::filesystem::u8path(Executable))) {
    check(std::filesystem::create_directory(Root), "create fixture directory");
  }
  ~Fixture() {
    Host.close(Host.root());
    Queue.drain();
    std::error_code Error;
    std::filesystem::remove(Root / "plugin.json", Error);
    std::filesystem::remove(Root / "missing.json", Error);
    std::filesystem::remove(Root, Error);
  }
  std::filesystem::path manifest(const char *File = "plugin.json",
                                 const char *Id = "sample") {
    const auto Path = Root / File;
    const auto ProgramUtf8 = Program.u8string();
    Json M{{"schema", "neverd.gui-plugin.v1"},
           {"id", Id},
           {"name", "Sample"},
           {"version", "1"},
           {"program", std::string(ProgramUtf8.begin(), ProgramUtf8.end())},
           {"arguments", {"--unexpected-provider-run"}},
           {"actions", Json::array({{{"id", "inspect"},
                                     {"title", "Inspect"},
                                     {"context", "none"}}})}};
    std::ofstream Output(Path, std::ios::binary);
    Output << M.dump();
    check(bool(Output), "write fixture manifest");
    return Path;
  }
};
void intent(Fixture &F) {
  Diagnostic Error;
  const auto Path = F.manifest();
  check(F.Catalog.restore({{"sample", Path, false}}, Error), Error.Message);
  auto S = *F.Catalog.snapshot("sample");
  check(!S.Record.Enabled && S.Declaration && !S.Component,
        "disabled installation activated or lost metadata");
  check(F.Catalog.setEnabled("sample", true, Error), Error.Message);
  const auto Old = *F.Catalog.action("sample", "inspect");
  auto Lease = F.Host.invoke(Old, Error);
  check(bool(Lease), "acquire original invocation");
  check(F.Catalog.setEnabled("sample", false, Error), Error.Message);
  check(F.Catalog.retiring("sample") && !F.Host.describe(Old),
        "disable admitted a new call or skipped retirement");
  check(!F.Catalog.setEnabled("sample", true, Error) &&
            Error.Code == "plugin_retiring",
        "enabled replacement while the previous component was leased");
  check(F.Catalog.snapshot("sample")->Record.Enabled,
        "activation failure changed saved enablement");
  Lease.reset();
  F.Queue.drain();
  check(F.Catalog.setEnabled("sample", true, Error), Error.Message);
  const auto New = *F.Catalog.action("sample", "inspect");
  check(New != Old, "replacement reused action identity");
  check(!F.ActionService.invoke(
            Old, {"stale", Json::object(), Json::object(), {}}, {}, Error),
        "old action reached a replacement");
  F.Catalog.remove("sample");
  check(F.Catalog.records().empty(), "uninstall retained saved intent");
  F.Queue.drain();
  check(!F.Catalog.snapshot("sample") && std::filesystem::exists(Path),
        "uninstall retained a closed row or removed plugin files");
}
void repair(Fixture &F) {
  Diagnostic Error;
  const auto Good = F.manifest();
  const auto Missing = F.Root / "missing.json";
  check(F.Catalog.restore({{"sample", Good, true}, {"repair", Missing, true}},
                          Error),
        Error.Message);
  auto Broken = *F.Catalog.snapshot("repair");
  check(Broken.Record.Enabled && !Broken.DiscoveryError.Code.empty() &&
            F.Catalog.component("sample"),
        "discovery failure hid intent or disabled an independent component");
  F.manifest("missing.json", "wrong");
  check(!F.Catalog.setEnabled("repair", true, Error) &&
            !F.Catalog.component("wrong"),
        "changed manifest ID was adopted");
  F.manifest("missing.json", "repair");
  check(F.Catalog.setEnabled("repair", true, Error), Error.Message);
  check(F.Catalog.component("repair") &&
            F.Catalog.snapshot("repair")->DiscoveryError.Code.empty(),
        "repaired installation did not activate");
}
void generation(Fixture &F) {
  Diagnostic Error;
  check(F.Catalog.install(F.manifest(), Error), Error.Message);
  const auto Old = *F.Catalog.action("sample", "inspect");
  auto Lease = F.Host.invoke(Old, Error);
  const auto Next = project(F.Host, "next");
  F.Catalog.setProject(Next);
  const auto New = *F.Catalog.action("sample", "inspect");
  check(New != Old && !F.Host.describe(Old),
        "project rebind retained old actions");
  Lease.reset();
  F.Queue.drain();
  check(F.Catalog.action("sample", "inspect") == New,
        "old component cleanup erased the new generation");
  PluginCatalog Other(F.Host, F.Processes, F.Project);
  check(Other.install(F.Root / "plugin.json", Error), Error.Message);
  check(F.Catalog.setEnabled("sample", false, Error), Error.Message);
  F.Queue.drain();
  check(Other.action("sample", "inspect").has_value(),
        "disable crossed project catalogs");
}
void records(Fixture &F) {
  Diagnostic Error;
  const std::vector<InstallationRecord> Saved{{"sample", F.manifest(), false}};
  auto Document = installationRecords(Saved);
  const auto Read = readInstallationRecords(Document, Error);
  check(Read && !Read->at(0).Enabled && Read->at(0).Path == Saved[0].Path,
        "installation projection lost path or enablement");
  Document["plugins"].push_back(Document["plugins"][0]);
  check(!readInstallationRecords(Document, Error),
        "duplicate saved ID accepted");
  Document = installationRecords(Saved);
  Document["plugins"][0]["enabled"] = "false";
  check(!readInstallationRecords(Document, Error),
        "non-boolean intent accepted");
  check(!F.Catalog.restore({Saved[0], Saved[0]}, Error) &&
            F.Catalog.records().empty(),
        "invalid record set partially registered components");
}
void cleanup(Fixture &F) {
  Diagnostic Error;
  check(F.Catalog.install(F.manifest(), Error), Error.Message);
  const auto Scope = *F.Catalog.component("sample");
  check(F.Host
            .registerResource(
                Scope, {"fixture", "cleanup", "1"}, {},
                [](auto Done) { Done("fixture cleanup failed"); }, Error)
            .has_value(),
        Error.Message);
  check(F.Catalog.setEnabled("sample", false, Error), Error.Message);
  F.Queue.drain();
  const auto State = *F.Catalog.snapshot("sample");
  check(!State.Component && !State.CleanupErrors.empty(),
        "retirement discarded cleanup diagnostics");
}
void queue(Fixture &F) {
  Diagnostic Error;
  check(F.Catalog.install(F.manifest(), Error), Error.Message);
  Tasks Tasks(F.Host, [](const auto &) {}, {});
  const auto A = *F.Catalog.action("sample", "inspect");
  check(Tasks.enqueue("z-first", A, Error) &&
            Tasks.enqueue("a-second", A, Error),
        Error.Message);
  check(!Tasks.begin("a-second") && Tasks.begin("z-first"),
        "serial admission sorted operation IDs instead of preserving enqueue "
        "order");
  Tasks.stop("z-first", TaskPhase::Cancelled, "done");
  Tasks.finish("z-first", false);
  check(Tasks.begin("a-second"), "next queued task did not become admissible");
  Tasks.stop("a-second", TaskPhase::Cancelled, "done");
  Tasks.finish("a-second", false);
}
} // namespace
int main(int Argc, char **Argv) {
  try {
    check(Argc == 2, "one catalog case required");
    Fixture F(Argv[0]);
    const std::string Mode = Argv[1];
    if (Mode == "intent")
      intent(F);
    else if (Mode == "repair")
      repair(F);
    else if (Mode == "generation")
      generation(F);
    else if (Mode == "records")
      records(F);
    else if (Mode == "cleanup")
      cleanup(F);
    else if (Mode == "queue")
      queue(F);
    else
      throw std::runtime_error("Unknown catalog case");
    return 0;
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    return 1;
  }
}
