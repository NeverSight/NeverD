//===- Index.cpp - Declarative nexe resource index ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict first-shim JSON, private portable namespaces and exact range aliases.
///
//===----------------------------------------------------------------------===//

#include "../JsonReader.h"
#include "../archives/PathIndex.h"
#include "Reader.h"

#include "neverd/web/Error.h"

#include <algorithm>
#include <map>

namespace neverd::web::nexe_detail {
void step(NexeExtraction &E, uint64_t Count) {
  if (Count > MaxNexeSteps - E.Steps)
    throw Error("nexe_work_budget_exceeded");
  E.Steps += Count;
}

void readIndex(const Artifact &Input, std::string_view Bootstrap,
               NexeExtraction &E) {
  constexpr std::string_view Prefix = "!(function () {process.__nexe = ";
  constexpr std::string_view End = ";\n})();";
  if (!Bootstrap.starts_with(Prefix))
    throw Error("nexe_unsupported_bootstrap");
  const auto IndexAt = Prefix.size();
  const auto Search = Bootstrap.substr(
      IndexAt, std::min<uint64_t>(Bootstrap.size() - IndexAt,
                                  MaxNexeIndexBytes + End.size()));
  const auto EndAt = Search.find(End);
  if (EndAt == Search.npos) {
    if (Search.size() > MaxNexeIndexBytes)
      throw Error("nexe_index_budget_exceeded");
    throw Error("nexe_unsupported_bootstrap");
  }
  if (EndAt > MaxNexeIndexBytes)
    throw Error("nexe_index_budget_exceeded");
  const auto Text = Search.substr(0, EndAt);
  // Literal newlines cannot be embedded in JSON strings, so the fixed shim
  // terminator cannot be a valid string payload. Do not scan for later shims.
  uint64_t Nodes = 0;
  const auto JSON = parseBoundedJSON(
      Text,
      {MaxNexeIndexBytes, 4, 4 * (MaxNexeMembers + 1) + 3, MaxNexeIndexBytes},
      &Nodes);
  step(E, Text.size() + Nodes);
  const auto *Object = JSON.getAsObject();
  const auto *Resources = Object ? Object->getObject("resources") : nullptr;
  if (!Object || Object->size() != 1 || !Resources)
    throw Error("nexe_invalid_index");
  if (Resources->size() > MaxNexeMembers)
    throw Error("nexe_member_budget_exceeded");
  ArchivePathIndex Paths(E.PrivateBytes);
  for (const auto &[Key, Value] : *Resources) {
    const auto Name = Key.str();
    if (!Name.starts_with("./"))
      throw Error("nexe_unsupported_member_path");
    const auto *Pair = Value.getAsArray();
    if (!Pair || Pair->size() != 2)
      throw Error("nexe_invalid_member_range");
    const auto Offset = (*Pair)[0].getAsInteger();
    const auto Size = (*Pair)[1].getAsInteger();
    if (!Offset || !Size || *Offset < 0 || *Size < 0 ||
        uint64_t(*Offset) > E.ResourceSize ||
        uint64_t(*Size) > E.ResourceSize - uint64_t(*Offset))
      throw Error("nexe_invalid_member_range");
    NexeMember M;
    M.Path = Name.substr(2);
    step(E, M.Path.size() + 1);
    Paths.admit(M.Path, false);
    const auto Metadata = M.Path.size() + 3 * 64 + sizeof(NexeMember);
    if (E.PrivateBytes > MaxNexePrivateBytes ||
        Metadata > MaxNexePrivateBytes - E.PrivateBytes)
      throw Error("nexe_metadata_budget_exceeded");
    E.PrivateBytes += Metadata;
    M.Offset = E.ResourceOffset + uint64_t(*Offset);
    M.Content = Input.Content.slice(M.Offset, uint64_t(*Size));
    E.Members.push_back(std::move(M));
  }
  // JSON object iteration is unspecified. Stable byte order also makes record
  // pages deterministic across hosts and independent of producer key order.
  std::sort(E.Members.begin(), E.Members.end(),
            [&](const auto &A, const auto &B) {
              step(E, 1 + std::min(A.Path.size(), B.Path.size()));
              return A.Path < B.Path;
            });
  using Range = std::pair<uint64_t, uint64_t>;
  std::map<Range, std::string> Hashes;
  for (const auto &M : E.Members) {
    step(E);
    Hashes.try_emplace({M.Offset, M.Content.size()});
  }
  uint64_t CoveredEnd = E.ResourceOffset;
  for (const auto &[R, Hash] : Hashes) {
    step(E);
    // Empty entries do not cover bytes and may occur inside a nonempty range.
    if (!R.second)
      continue;
    if (R.first < CoveredEnd)
      throw Error("nexe_partial_member_overlap");
    CoveredEnd = R.first + R.second;
    E.ReferencedBytes += R.second;
  }
  E.UniqueMemberRanges = Hashes.size();
  for (auto &[R, Hash] : Hashes) {
    Hash = Input.Content.slice(R.first, R.second).digest();
    E.HashedBytes += R.second;
  }
  uint64_t Ordinal = 0;
  for (auto &M : E.Members) {
    M.BlobHash = Hashes.at({M.Offset, M.Content.size()});
    M.RangeID = identity("nexe-member-range",
                         {E.ID, std::to_string(M.Offset),
                          std::to_string(M.Content.size()), M.BlobHash});
    M.ID =
        identity("nexe-member", {E.ID, std::to_string(Ordinal++), M.RangeID});
  }
}
} // namespace neverd::web::nexe_detail
