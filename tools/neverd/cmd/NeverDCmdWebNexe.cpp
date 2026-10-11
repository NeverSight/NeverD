//===- NeverDCmdWebNexe.cpp - Offline nexe CLI selection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Thin C API selection of captured nexe containers and resource source text.
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
  const bool OK =
      V->getAsObject() && V->getAsObject()->getString("status") == "ok";
  if (Print || !OK)
    llvm::outs() << *V << '\n';
  if (!OK)
    return {};
  return std::move(*V);
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string selected(const char *Owned, const char *Key) {
  auto V = reply(Owned);
  const auto *Items = V ? V->getAsObject()->getArray("items") : nullptr;
  return Items && Items->size() == 1 ? field((*Items)[0], Key) : "";
}
bool decimal(llvm::StringRef S, uint64_t &N) {
  return !S.empty() && !S.getAsInteger(10, N) && std::to_string(N) == S;
}
} // namespace

int runWebNexe() {
  const auto &Command = WebArguments[0];
  const bool Archive = Command.starts_with("archive-");
  const bool Source = Command.ends_with("-source");
  auto Usage = [] {
    llvm::errs()
        << "usage: neverd web nexe <root> <profile> [artifact-index] | "
           "nexe-source <root> <profile> <artifact-index> <member-index> "
           "<script|module|commonjs> | archive-nexe <root> <profile> "
           "<tar|tgz|zip> <archive-index> <archive-member-index> | "
           "archive-nexe-source <root> <profile> <tar|tgz|zip> "
           "<archive-index> <archive-member-index> <member-index> "
           "<script|module|commonjs>\n";
    return 2;
  };
  const auto Count = WebArguments.size();
  if (Archive  ? Count != (Source ? 8U : 6U)
      : Source ? Count != 6U
               : Count != 3U && Count != 4U)
    return Usage();
  uint64_t ArtifactIndex = 0, ArchiveMember = 0, Member = 0;
  if ((Count > 3 && !decimal(WebArguments[Archive ? 4 : 3], ArtifactIndex)) ||
      (Archive && !decimal(WebArguments[5], ArchiveMember)) ||
      (Source && !decimal(WebArguments[Archive ? 6 : 4], Member)))
    return Usage();
  const auto Format = Archive ? WebArguments[3] : "";
  const auto Type = Source ? WebArguments[Count - 1] : "";
  if ((Archive && Format != "tar" && Format != "tgz" && Format != "zip") ||
      (Source && Type != "script" && Type != "module" && Type != "commonjs"))
    return Usage();
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S)
    return 1;
  const auto &Path = WebArguments[1], &Profile = WebArguments[2];
  const auto Preview = reply(neverd_web_import_preview_json(
      G.S, Path.data(), Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  const auto Capture =
      reply(neverd_web_import_commit_json(G.S, Token.data(), Token.size()));
  if (!Capture)
    return 1;
  const auto Rev = field(*Capture, "revision");
  auto Artifact = selected(
      neverd_web_artifacts_json(G.S, Rev.data(), Rev.size(), ArtifactIndex, 1),
      "artifact_id");
  if (Archive) {
    const auto A = reply(neverd_web_package_archive_extract_json(
        G.S, Rev.data(), Rev.size(), Artifact.data(), Artifact.size(),
        Format.data(), Format.size()));
    if (!A)
      return 1;
    const auto ID = field(*A, "archive_id");
    Artifact = selected(neverd_web_package_archive_records_json(
                            G.S, Rev.data(), Rev.size(), ID.data(), ID.size(),
                            ArchiveMember, 1),
                        "member_id");
  }
  if (Artifact.empty())
    return 1;
  const auto E = reply(neverd_web_nexe_extract_json(
                           G.S, Rev.data(), Rev.size(), Artifact.data(),
                           Artifact.size(), Profile.data(), Profile.size()),
                       true);
  if (!E)
    return 1;
  const auto ID = field(*E, "extraction_id");
  if (Source) {
    const auto A = selected(
        neverd_web_nexe_records_json(G.S, Rev.data(), Rev.size(), ID.data(),
                                     ID.size(), "members", 7, Member, 1),
        "selection_id");
    if (A.empty())
      return 1;
    const auto S = reply(
        neverd_web_source_analyze_json(G.S, Rev.data(), Rev.size(), A.data(),
                                       A.size(), Type.data(), Type.size()),
        true);
    return S && field(*S, "parse_status") == "parsed" ? 0 : 1;
  }
  for (const std::string Kind : {"regions", "members"}) {
    for (uint64_t Offset = 0;;) {
      const auto Page =
          reply(neverd_web_nexe_records_json(G.S, Rev.data(), Rev.size(),
                                             ID.data(), ID.size(), Kind.data(),
                                             Kind.size(), Offset, 128),
                true);
      if (!Page)
        return 1;
      const auto Next = Page->getAsObject()->getInteger("next_offset");
      if (!Next)
        break;
      if (*Next < 0 || uint64_t(*Next) <= Offset || *Next > 4096)
        return 1;
      Offset = *Next;
    }
  }
  return 0;
}
} // namespace neverd::cli
