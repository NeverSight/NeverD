//===- NeverDCmdWebInterfaces.cpp - Offline interface evidence CLI -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C API adapter with explicit hash-bound HAR redaction acceptance.
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
  auto R = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!R) {
    llvm::consumeError(R.takeError());
    llvm::errs() << "web: invalid_backend_response\n";
    return {};
  }
  if (!R->getAsObject() || R->getAsObject()->getString("status") != "ok") {
    llvm::outs() << *R << '\n';
    return {};
  }
  return std::move(*R);
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
bool index(size_t At, uint64_t &Value) {
  const llvm::StringRef Text = WebArguments[At];
  return !Text.empty() && !Text.getAsInteger(10, Value) &&
         std::to_string(Value) == Text;
}
} // namespace

int runWebInterfaces() {
  const auto &Command = WebArguments[0];
  const bool Preview = Command == "har-preview";
  const bool HAR = Command == "har-import";
  const bool Correlate = Command == "interface-correlate";
  const bool Source = Command == "interfaces" || Correlate;
  const auto Count = WebArguments.size();
  const bool ValidCount = Preview     ? Count == 2 || Count == 3
                          : HAR       ? Count == 5
                          : Correlate ? Count == 7
                                      : Count == 3 || Count == 4;
  if (!ValidCount ||
      (Source && WebArguments[2] != "module" && WebArguments[2] != "script" &&
       WebArguments[2] != "commonjs")) {
    llvm::errs()
        << "usage: neverd web har-preview <root> [artifact-index]\n"
           "       neverd web har-import <root> <artifact-index> "
           "<redaction-policy> <previewed-sha256>\n"
           "       neverd web interfaces <root> <source-type> [index]\n"
           "       neverd web interface-correlate <root> <source-type> "
           "<source-index> <har-index> <redaction-policy> "
           "<previewed-sha256>\n";
    return 2;
  }
  uint64_t SourceIndex = 0, HARIndex = 0;
  if ((Preview && Count == 3 && !index(2, HARIndex)) ||
      (HAR && !index(2, HARIndex)) ||
      (Source && Count >= 4 && !index(3, SourceIndex)) ||
      (Correlate && !index(4, HARIndex)))
    return 2;
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S) {
    reply(neverd_web_capabilities_json());
    return 1;
  }
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
  auto Artifact = [&](uint64_t I) {
    const auto Page = reply(
        neverd_web_artifacts_json(G.S, Revision.data(), Revision.size(), I, 1));
    const auto *Items = Page ? Page->getAsObject()->getArray("items") : nullptr;
    return Items && Items->size() == 1 ? field((*Items)[0], "artifact_id") : "";
  };
  std::string CaptureID, AnalysisID;
  if (Preview || HAR || Correlate) {
    const auto ID = Artifact(HARIndex);
    if (ID.empty())
      return 1;
    const auto HP = reply(neverd_web_har_preview_json(
        G.S, Revision.data(), Revision.size(), ID.data(), ID.size()));
    if (!HP)
      return 1;
    if (Preview) {
      llvm::outs() << *HP << '\n';
      return 0;
    }
    const size_t Receipt = HAR ? 3 : 5;
    if (WebArguments[Receipt] != field(*HP, "redaction_policy") ||
        WebArguments[Receipt + 1] != field(*HP, "blob_sha256")) {
      llvm::errs() << "web: har_preview_receipt_mismatch\n";
      return 1;
    }
    const auto HT = field(*HP, "preview_token");
    const auto Commit = reply(neverd_web_har_commit_json(
        G.S, Revision.data(), Revision.size(), HT.data(), HT.size()));
    if (!Commit)
      return 1;
    CaptureID = field(*Commit, "capture_id");
    llvm::outs() << *Commit << '\n';
  }
  if (Source) {
    const auto ID = Artifact(SourceIndex);
    const auto &Type = WebArguments[2];
    if (ID.empty())
      return 1;
    const auto S = reply(neverd_web_source_analyze_json(
        G.S, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Type.data(), Type.size()));
    if (!S)
      return 1;
    const auto SID = field(*S, "source_id");
    const auto A = reply(neverd_web_interfaces_analyze_json(
        G.S, Revision.data(), Revision.size(), SID.data(), SID.size()));
    if (!A)
      return 1;
    llvm::outs() << *A << '\n';
    if (field(*A, "analysis_status") != "partial")
      return 1;
    AnalysisID = field(*A, "interface_analysis_id");
  }
  auto Query =
      HAR ? neverd_web_har_records_json : neverd_web_interface_records_json;
  auto Selection = HAR ? CaptureID : AnalysisID;
  if (Correlate) {
    const auto C = reply(neverd_web_interfaces_compare_json(
        G.S, Revision.data(), Revision.size(), AnalysisID.data(),
        AnalysisID.size(), CaptureID.data(), CaptureID.size()));
    if (!C)
      return 1;
    llvm::outs() << *C << '\n';
    Selection = field(*C, "correlation_id");
    Query = neverd_web_interface_correlation_records_json;
  }
  for (uint64_t Offset = 0;;) {
    const auto Page =
        reply(Query(G.S, Revision.data(), Revision.size(), Selection.data(),
                    Selection.size(), Offset, 128));
    if (!Page)
      return 1;
    llvm::outs() << *Page << '\n';
    if (Page->getAsObject()->getBoolean("page_complete").value_or(false))
      return 0;
    const auto Next = Page->getAsObject()->getInteger("next_offset");
    if (!Next || *Next <= int64_t(Offset))
      return 1;
    Offset = *Next;
  }
}
} // namespace neverd::cli
