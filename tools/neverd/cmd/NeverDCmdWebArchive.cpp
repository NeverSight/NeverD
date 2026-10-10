//===- NeverDCmdWebArchive.cpp - Offline archive commands --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C API adapters for archive inventories and explicitly selected consumers.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
std::optional<llvm::json::Value> reply(const char *Owned, bool Print = false) {
  if (!Owned)
    return {};
  auto V = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!V) {
    llvm::consumeError(V.takeError());
    return {};
  }
  if (Print || !V->getAsObject() ||
      V->getAsObject()->getString("status") != "ok")
    llvm::outs() << *V << '\n';
  if (!V->getAsObject() || V->getAsObject()->getString("status") != "ok")
    return {};
  return std::move(*V);
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
} // namespace

int runWebArchive() {
  const bool Packages = WebArguments[0] == "archive-packages";
  const bool Bun = WebArguments[0] == "archive-bun";
  const bool Inventory = WebArguments[0] == "archive";
  if ((!Packages && !Bun && !Inventory) ||
      (Inventory && WebArguments.size() != 3 && WebArguments.size() != 4) ||
      (Bun && WebArguments.size() != 5) ||
      (Packages && WebArguments.size() != 6) ||
      (WebArguments[2] != "tar" && WebArguments[2] != "tgz")) {
    llvm::errs() << "usage: neverd web archive <file-or-root> <tar|tgz> "
                    "[artifact-index] | archive-packages <file-or-root> "
                    "<tar|tgz> <artifact-index> <member-index> "
                    "<npm-lock|package-json> | archive-bun <file-or-root> "
                    "<tar|tgz> <artifact-index> <member-index>\n";
    return 2;
  }
  auto Index = [](size_t At, uint64_t &Out) {
    const llvm::StringRef Text = WebArguments[At];
    return !Text.empty() && !Text.getAsInteger(10, Out) &&
           std::to_string(Out) == Text;
  };
  uint64_t ArtifactIndex = 0, MemberIndex = 0;
  if ((WebArguments.size() > 3 && !Index(3, ArtifactIndex)) ||
      (!Inventory && !Index(4, MemberIndex)))
    return 2;
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S)
    return 1;
  const auto &Path = WebArguments[1], &Format = WebArguments[2];
  const auto P = reply(neverd_web_import_preview_json(G.S, Path.data(),
                                                      Path.size(), nullptr, 0));
  if (!P)
    return 1;
  const auto Token = field(*P, "preview_token");
  const auto Metadata = reply(
      neverd_web_import_commit_json(G.S, Token.data(), Token.size()), true);
  if (!Metadata)
    return 1;
  const auto Revision = field(*Metadata, "revision");
  auto Selected = reply(neverd_web_artifacts_json(
      G.S, Revision.data(), Revision.size(), ArtifactIndex, 1));
  const auto *Items =
      Selected ? Selected->getAsObject()->getArray("items") : nullptr;
  if (!Items || Items->size() != 1)
    return 1;
  const auto Artifact = field((*Items)[0], "artifact_id");
  const auto A =
      reply(neverd_web_package_archive_extract_json(
                G.S, Revision.data(), Revision.size(), Artifact.data(),
                Artifact.size(), Format.data(), Format.size()),
            true);
  if (!A)
    return 1;
  const auto ID = field(*A, "archive_id");
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
  if (Inventory)
    return Pages([&](uint64_t Offset) {
      return neverd_web_package_archive_records_json(G.S, Revision.data(),
                                                     Revision.size(), ID.data(),
                                                     ID.size(), Offset, 128);
    })
               ? 0
               : 1;
  auto Member = reply(neverd_web_package_archive_records_json(
      G.S, Revision.data(), Revision.size(), ID.data(), ID.size(), MemberIndex,
      1));
  Items = Member ? Member->getAsObject()->getArray("items") : nullptr;
  if (!Items || Items->size() != 1)
    return 1;
  const auto MID = field((*Items)[0], "member_id");
  if (Bun)
    return reply(neverd_web_bun_extract_json(G.S, Revision.data(),
                                             Revision.size(), MID.data(),
                                             MID.size()),
                 true)
               ? 0
               : 1;
  const auto &Kind = WebArguments[5];
  const auto Graph =
      reply(neverd_web_packages_analyze_json(
                G.S, Revision.data(), Revision.size(), MID.data(), MID.size(),
                Kind.data(), Kind.size()),
            true);
  if (!Graph)
    return 1;
  const auto GID = field(*Graph, "package_analysis_id");
  for (const std::string K :
       {"packages", "dependencies", "scripts", "entries", "files"})
    if (!Pages([&](uint64_t Offset) {
          return neverd_web_package_records_json(
              G.S, Revision.data(), Revision.size(), GID.data(), GID.size(),
              K.data(), K.size(), Offset, 128);
        }))
      return 1;
  return 0;
}
} // namespace neverd::cli
