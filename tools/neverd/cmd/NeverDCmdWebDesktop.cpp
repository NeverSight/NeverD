//===- NeverDCmdWebDesktop.cpp - Desktop manifest CLI ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit captured-artifact selection through the shared web C API.
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
  return OK ? std::optional<llvm::json::Value>(std::move(*V)) : std::nullopt;
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
} // namespace

int runWebDesktop() {
  if ((WebArguments.size() != 3 && WebArguments.size() != 4) ||
      (WebArguments[2] != "nwjs" && WebArguments[2] != "vsix")) {
    llvm::errs() << "usage: neverd web desktop-manifest <file-or-root> "
                    "<nwjs|vsix> [artifact-index]\n";
    return 2;
  }
  uint64_t Index = 0;
  if (WebArguments.size() == 4) {
    const llvm::StringRef Text = WebArguments[3];
    if (Text.empty() || Text.getAsInteger(10, Index) ||
        Text != std::to_string(Index))
      return 2;
  }
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S)
    return 1;
  const auto &Path = WebArguments[1], &Kind = WebArguments[2];
  const auto Preview = reply(neverd_web_import_preview_json(
      G.S, Path.data(), Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  const auto Metadata = reply(
      neverd_web_import_commit_json(G.S, Token.data(), Token.size()), true);
  if (!Metadata)
    return 1;
  const auto Revision = field(*Metadata, "revision");
  const auto Page = reply(neverd_web_artifacts_json(G.S, Revision.data(),
                                                    Revision.size(), Index, 1));
  const auto *Items = Page ? Page->getAsObject()->getArray("items") : nullptr;
  if (!Items || Items->size() != 1)
    return 1;
  const auto ID = field(Items->front(), "artifact_id");
  return reply(neverd_web_desktop_manifest_analyze_json(
                   G.S, Revision.data(), Revision.size(), ID.data(), ID.size(),
                   Kind.data(), Kind.size()),
               true)
             ? 0
             : 1;
}
} // namespace neverd::cli
