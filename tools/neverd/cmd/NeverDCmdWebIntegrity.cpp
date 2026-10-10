//===- NeverDCmdWebIntegrity.cpp - Captured package verification --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C API adapter for explicit original and declaration selections.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
std::optional<llvm::json::Value> reply(const char *Owned) {
  if (!Owned) {
    llvm::errs() << "web: allocation_failed\n";
    return {};
  }
  auto V = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!V) {
    llvm::consumeError(V.takeError());
    llvm::errs() << "web: invalid_backend_response\n";
    return {};
  }
  if (!V->getAsObject() || V->getAsObject()->getString("status") != "ok") {
    llvm::outs() << *V << '\n';
    return {};
  }
  return std::move(*V);
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
} // namespace

int runWebIntegrity() {
  if ((WebArguments.size() != 5 && WebArguments.size() != 6) ||
      (WebArguments[3] != "registry-dist" && WebArguments[3] != "npm-lock") ||
      (WebArguments[3] == "npm-lock") != (WebArguments.size() == 6)) {
    llvm::errs()
        << "usage: neverd web integrity <root> <original-index> "
           "<registry-dist|npm-lock> <metadata-index> [package-index]\n";
    return 2;
  }
  const auto Index = [](size_t I, uint64_t &N) {
    const llvm::StringRef Text = WebArguments[I];
    return !Text.empty() && !Text.getAsInteger(10, N) &&
           std::to_string(N) == Text;
  };
  uint64_t OriginalIndex = 0, MetadataIndex = 0, PackageIndex = 0;
  const bool Lock = WebArguments[3] == "npm-lock";
  if (!Index(2, OriginalIndex) || !Index(4, MetadataIndex) ||
      (Lock && !Index(5, PackageIndex)))
    return 2;
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S)
    return 1;
  const auto &Path = WebArguments[1];
  const auto P = reply(neverd_web_import_preview_json(G.S, Path.data(),
                                                      Path.size(), nullptr, 0));
  if (!P)
    return 1;
  const auto Token = field(*P, "preview_token");
  const auto M =
      reply(neverd_web_import_commit_json(G.S, Token.data(), Token.size()));
  if (!M)
    return 1;
  const auto Revision = field(*M, "revision");
  auto Artifact = [&](uint64_t I) -> std::string {
    const auto Page = reply(
        neverd_web_artifacts_json(G.S, Revision.data(), Revision.size(), I, 1));
    const auto *Items = Page ? Page->getAsObject()->getArray("items") : nullptr;
    return Items && Items->size() == 1 ? field((*Items)[0], "artifact_id") : "";
  };
  const auto Original = Artifact(OriginalIndex);
  auto Declaration = Artifact(MetadataIndex);
  if (Original.empty() || Declaration.empty())
    return 1;
  std::string Package;
  if (Lock) {
    const auto Graph = reply(neverd_web_packages_analyze_json(
        G.S, Revision.data(), Revision.size(), Declaration.data(),
        Declaration.size(), "npm-lock", 8));
    if (!Graph)
      return 1;
    Declaration = field(*Graph, "package_analysis_id");
    const auto Page = reply(neverd_web_package_records_json(
        G.S, Revision.data(), Revision.size(), Declaration.data(),
        Declaration.size(), "packages", 8, PackageIndex, 1));
    const auto *Items = Page ? Page->getAsObject()->getArray("items") : nullptr;
    if (!Items || Items->size() != 1)
      return 1;
    Package = field((*Items)[0], "package_id");
  }
  const auto R = reply(neverd_web_package_integrity_verify_json(
      G.S, Revision.data(), Revision.size(), Original.data(), Original.size(),
      Declaration.data(), Declaration.size(), Package.data(), Package.size()));
  if (!R)
    return 1;
  llvm::outs() << *R << '\n';
  return field(*R, "integrity_status") == "match" ? 0 : 1;
}
} // namespace neverd::cli
