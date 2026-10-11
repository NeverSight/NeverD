//===- NeverDCmdWebSEA.cpp - SEA CLI adapters ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SEA CLI adapters.
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
int runWebSEA() {
  if (WebArguments.size() != 3 && WebArguments.size() != 4) {
    llvm::errs() << "usage: neverd web sea|sea-source <file-or-root> <profile> "
                    "[artifact-index]\n";
    return 2;
  }
  uint64_t Index = 0;
  if (WebArguments.size() == 4 &&
      (llvm::StringRef(WebArguments[3]).getAsInteger(10, Index) ||
       std::to_string(Index) != WebArguments[3]))
    return 2;
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S) {
    reply(neverd_web_capabilities_json(), true);
    llvm::errs() << "web: session_unavailable\n";
    return 1;
  }
  const auto &Path = WebArguments[1], &Profile = WebArguments[2];
  const auto Preview = reply(neverd_web_import_preview_json(
      G.S, Path.data(), Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  const auto Metadata = reply(
      neverd_web_import_commit_json(G.S, Token.data(), Token.size()), true);
  if (!Metadata)
    return 1;
  const auto Rev = field(*Metadata, "revision");
  const auto A =
      reply(neverd_web_artifacts_json(G.S, Rev.data(), Rev.size(), Index, 1));
  const auto *Items = A ? A->getAsObject()->getArray("items") : nullptr;
  if (!Items || Items->size() != 1) {
    llvm::errs() << "web: artifact_selection_unavailable\n";
    return 1;
  }
  const auto Artifact = field((*Items)[0], "artifact_id");
  const auto E = reply(neverd_web_sea_extract_json(
                           G.S, Rev.data(), Rev.size(), Artifact.data(),
                           Artifact.size(), Profile.data(), Profile.size()),
                       true);
  if (!E)
    return 1;
  const auto ID = field(*E, "extraction_id");
  if (WebArguments[0] == "sea-source") {
    const auto Source = field(*E, "source_artifact_id");
    if (Source.empty()) {
      llvm::errs() << "web: sea_source_not_stored\n";
      return 1;
    }
    const auto S = reply(neverd_web_source_analyze_json(
                             G.S, Rev.data(), Rev.size(), Source.data(),
                             Source.size(), "commonjs", 8),
                         true);
    return S && field(*S, "parse_status") == "parsed" ? 0 : 1;
  }
  for (uint64_t Offset = 0;;) {
    const auto Page =
        reply(neverd_web_sea_records_json(G.S, Rev.data(), Rev.size(),
                                          ID.data(), ID.size(), Offset, 128),
              true);
    if (!Page)
      return 1;
    const auto Next = Page->getAsObject()->getInteger("next_offset");
    if (!Next)
      return 0;
    if (*Next < 0 || uint64_t(*Next) <= Offset || *Next > 32768)
      return 1;
    Offset = *Next;
  }
}
} // namespace neverd::cli
