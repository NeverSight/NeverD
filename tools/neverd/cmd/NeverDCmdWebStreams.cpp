//===- NeverDCmdWebStreams.cpp - Passive transcript CLI
//--------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit profile and hash/profile/policy-bound redaction receipts.
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

int runWebStreams() {
  const bool Preview = WebArguments[0] == "stream-preview";
  if (Preview ? WebArguments.size() < 3 || WebArguments.size() > 4
              : WebArguments.size() != 6) {
    llvm::errs() << "usage: neverd web stream-preview <root> <profile> "
                    "[artifact-index]\n"
                    "       neverd web stream-import <root> <profile> "
                    "<artifact-index> <redaction-policy> <preview-receipt>\n";
    return 2;
  }
  uint64_t Index = 0;
  if (WebArguments.size() >= 4) {
    const llvm::StringRef Text = WebArguments[3];
    if (Text.empty() || Text.getAsInteger(10, Index) ||
        std::to_string(Index) != Text)
      return 2;
  }
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S) {
    reply(neverd_web_capabilities_json());
    return 1;
  }
  const auto &Path = WebArguments[1], &Profile = WebArguments[2];
  const auto P = reply(neverd_web_import_preview_json(G.S, Path.data(),
                                                      Path.size(), nullptr, 0));
  if (!P)
    return 1;
  const auto T = field(*P, "preview_token");
  const auto M = reply(neverd_web_import_commit_json(G.S, T.data(), T.size()));
  if (!M)
    return 1;
  const auto Revision = field(*M, "revision");
  const auto Page = reply(neverd_web_artifacts_json(G.S, Revision.data(),
                                                    Revision.size(), Index, 1));
  const auto *Items = Page ? Page->getAsObject()->getArray("items") : nullptr;
  if (!Items || Items->size() != 1)
    return 1;
  const auto Artifact = field((*Items)[0], "artifact_id");
  const auto SP = reply(neverd_web_stream_preview_json(
      G.S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      Profile.data(), Profile.size()));
  if (!SP)
    return 1;
  if (Preview) {
    llvm::outs() << *SP << '\n';
    return 0;
  }
  if (WebArguments[4] != field(*SP, "redaction_policy") ||
      WebArguments[5] != field(*SP, "redaction_receipt")) {
    llvm::errs() << "web: stream_preview_receipt_mismatch\n";
    return 1;
  }
  const auto ST = field(*SP, "preview_token");
  const auto C = reply(neverd_web_stream_commit_json(
      G.S, Revision.data(), Revision.size(), ST.data(), ST.size()));
  if (!C)
    return 1;
  llvm::outs() << *C << '\n';
  const auto ID = field(*C, "stream_capture_id");
  for (uint64_t Offset = 0;;) {
    const auto Records = reply(
        neverd_web_stream_records_json(G.S, Revision.data(), Revision.size(),
                                       ID.data(), ID.size(), Offset, 128));
    if (!Records)
      return 1;
    llvm::outs() << *Records << '\n';
    if (Records->getAsObject()->getBoolean("page_complete").value_or(false))
      return 0;
    const auto Next = Records->getAsObject()->getInteger("next_offset");
    if (!Next || *Next <= int64_t(Offset))
      return 1;
    Offset = *Next;
  }
}
} // namespace neverd::cli
