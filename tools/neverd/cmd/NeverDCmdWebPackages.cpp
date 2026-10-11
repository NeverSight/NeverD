//===- NeverDCmdWebPackages.cpp - Offline package analysis commands ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline package analysis commands.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
std::optional<llvm::json::Value> reply(const char *Owned, bool Print = false) {
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
  const auto *O = V->getAsObject();
  if (!O || O->getString("status") != "ok") {
    llvm::outs() << *V << '\n';
    return {};
  }
  if (Print)
    llvm::outs() << *V << '\n';
  return std::move(*V);
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
} // namespace

int runWebPackages() {
  const bool Diff = WebArguments[0] == "package-diff";
  if ((Diff && WebArguments.size() != 5) ||
      (!Diff && WebArguments.size() != 3 && WebArguments.size() != 4) ||
      (WebArguments[2] != "npm-lock" && WebArguments[2] != "package-json")) {
    llvm::errs() << "usage: neverd web packages <file-or-root> "
                    "<npm-lock|package-json> [artifact-index] | "
                    "package-diff <root> <npm-lock|package-json> "
                    "<before-index> <after-index>\n";
    return 2;
  }
  uint64_t Before = 0, After = 0;
  auto Index = [&](size_t At, uint64_t &Out) {
    const llvm::StringRef Text = WebArguments[At];
    return !Text.empty() && !Text.getAsInteger(10, Out) &&
           std::to_string(Out) == Text;
  };
  if ((WebArguments.size() > 3 && !Index(3, Before)) ||
      (Diff && !Index(4, After))) {
    llvm::errs() << "web: invalid_package_selection\n";
    return 2;
  }
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S) {
    llvm::errs() << "web: capability_unavailable\n";
    return 1;
  }
  const auto &Path = WebArguments[1], &Kind = WebArguments[2];
  auto Preview = reply(neverd_web_import_preview_json(G.S, Path.data(),
                                                      Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  auto Metadata = reply(
      neverd_web_import_commit_json(G.S, Token.data(), Token.size()), true);
  if (!Metadata)
    return 1;
  const auto Revision = field(*Metadata, "revision");
  auto Analyze = [&](uint64_t Index) -> std::optional<llvm::json::Value> {
    auto Page = reply(neverd_web_artifacts_json(G.S, Revision.data(),
                                                Revision.size(), Index, 1));
    const auto *Items = Page ? Page->getAsObject()->getArray("items") : nullptr;
    if (!Items || Items->size() != 1) {
      llvm::errs() << "web: invalid_package_selection\n";
      return {};
    }
    const auto ID = field((*Items)[0], "artifact_id");
    return reply(neverd_web_packages_analyze_json(
                     G.S, Revision.data(), Revision.size(), ID.data(),
                     ID.size(), Kind.data(), Kind.size()),
                 true);
  };
  auto B = Analyze(Before);
  if (!B)
    return 1;
  const auto BID = field(*B, "package_analysis_id");
  auto Pages = [&](auto Query) {
    uint64_t Offset = 0;
    for (;;) {
      auto Page = reply(Query(Offset), true);
      if (!Page)
        return false;
      const auto Next = Page->getAsObject()->getInteger("next_offset");
      if (!Next)
        return true;
      if (*Next < 0 || uint64_t(*Next) <= Offset || *Next > 32768)
        return false;
      Offset = *Next;
    }
  };
  if (Diff) {
    const auto A = Analyze(After);
    if (!A)
      return 1;
    const auto AID = field(*A, "package_analysis_id");
    const auto D = reply(neverd_web_packages_compare_json(
                             G.S, Revision.data(), Revision.size(), BID.data(),
                             BID.size(), AID.data(), AID.size()),
                         true);
    if (!D)
      return 1;
    const auto ID = field(*D, "package_diff_id");
    return Pages([&](uint64_t Offset) {
      return neverd_web_package_diff_records_json(G.S, Revision.data(),
                                                  Revision.size(), ID.data(),
                                                  ID.size(), Offset, 128);
    })
               ? 0
               : 1;
  }
  for (const std::string K :
       {"packages", "dependencies", "scripts", "entries", "files"})
    if (!Pages([&](uint64_t Offset) {
          return neverd_web_package_records_json(
              G.S, Revision.data(), Revision.size(), BID.data(), BID.size(),
              K.data(), K.size(), Offset, 128);
        }))
      return 1;
  return 0;
}
} // namespace neverd::cli
