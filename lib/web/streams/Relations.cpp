//===- Relations.cpp - Recorded JSON-RPC occurrence relationships ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Conservative typed-ID joins using explicit session, direction and order.
///
//===----------------------------------------------------------------------===//

#include "StreamInternal.h"

#include <map>
#include <tuple>

namespace neverd::web::stream {
void Reader::relate() {
  if (Capture.Profile != "recorded-jsonrpc-2.0-jsonl-v1") {
    if (Capture.Profile == "jsonrpc-2.0-jsonl-v1" ||
        Capture.Profile == "mcp-2025-06-18-stdio-jsonl-v1")
      Capture.RelationStatus = "missing_recorded_context";
    return;
  }
  if (CoverageGap) {
    Capture.RelationStatus = "coverage_gap";
    for (auto &R : Capture.Records)
      if (R.Relation == "pending")
        R.Relation = "coverage_gap";
    return;
  }
  // Identity storage is complete and immutable during this join. Borrow it
  // instead of retaining a second copy of private session/ID strings.
  using Key = std::tuple<std::string_view, std::string_view, bool>;
  struct Group {
    std::vector<uint32_t> Requests, Responses;
  };
  std::map<Key, Group> Groups;
  for (uint32_t Index = 0; Index < Capture.Records.size(); ++Index) {
    auto &R = Capture.Records[Index];
    const auto &I = Identities[Index];
    if (!I.Request && !I.Response)
      continue;
    if (I.ID.empty()) {
      R.Relation = R.IDKind == "null" ? "null_id" : "unsupported_numeric_id";
      continue;
    }
    bool ClientRequest = R.Direction == "client_to_server";
    if (I.Response)
      ClientRequest = !ClientRequest;
    auto &G = Groups[{I.Session, I.ID, ClientRequest}];
    (I.Request ? G.Requests : G.Responses).push_back(Index);
  }
  Capture.RelationStatus = "recorded_candidate_pairs";
  for (const auto &[K, G] : Groups) {
    const bool Duplicate = G.Requests.size() > 1 || G.Responses.size() > 1;
    for (auto I : G.Requests)
      Capture.Records[I].Relation =
          Duplicate ? "duplicate_id" : "unmatched_request";
    for (auto I : G.Responses)
      Capture.Records[I].Relation =
          Duplicate ? "duplicate_id" : "unmatched_response";
    if (Duplicate || G.Requests.empty() || G.Responses.empty())
      continue;
    const auto Request = G.Requests[0], Response = G.Responses[0];
    auto &A = Capture.Records[Request];
    auto &B = Capture.Records[Response];
    if (Request >= Response) {
      A.Relation = B.Relation = "response_before_request";
      continue;
    }
    A.Peer = Response;
    B.Peer = Request;
    A.Relation = B.Relation = "recorded_id_candidate";
    ++Capture.PairCount;
  }
}
} // namespace neverd::web::stream
