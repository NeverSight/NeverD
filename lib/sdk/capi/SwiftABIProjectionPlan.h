#ifndef NEVERD_SDK_CAPI_SWIFTABIPROJECTIONPLAN_H
#define NEVERD_SDK_CAPI_SWIFTABIPROJECTIONPLAN_H

#include "SwiftSourceIdentity.h"

#include "neverd/ir/SourceABI.h"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sdk::swift_source {

struct ABIProjectionRow {
  size_t Index;
  va_t Entry;
  std::string MangledSymbol;
  SourceFunctionTypeHint Hint;
};

struct ABIProjectionBatch {
  /// Each entry occurs once. These describe bodies being projected, not
  /// additional declarations that callers are permitted to select by address.
  std::map<va_t, SourceFunctionTypeHint> BodyHints;
  std::vector<size_t> Rows;
};

struct ABIProjectionPlan {
  std::vector<size_t> BaseRows;
  std::vector<ABIProjectionBatch> Batches;
  /// A frozen copy of the independently unambiguous base declarations. Never
  /// add per-batch body overrides to the direct-call identity evidence.
  std::map<va_t, SourceFunctionTypeHint> BaseCalleeHints;
};

namespace projection_plan_detail {
inline void number(std::string &Key, uint64_t Value) {
  Key += std::to_string(Value);
  Key += ':';
}
inline void type(std::string &Key, const TypeRef &Type, unsigned Depth = 0) {
  if (!Type || Depth > 16)
    throw std::invalid_argument("Swift ABI projection has an invalid type");
  if ((Type->Kind == NdTypeKind::Void && Type->Size != 0) ||
      (Type->Kind == NdTypeKind::Int && Type->Size != 1 && Type->Size != 2 &&
       Type->Size != 4 && Type->Size != 8) ||
      (Type->Kind == NdTypeKind::Float && Type->Size != 4 && Type->Size != 8) ||
      (Type->Kind == NdTypeKind::Ptr && Type->Size != 8) ||
      (Type->Kind != NdTypeKind::Void && Type->Kind != NdTypeKind::Int &&
       Type->Kind != NdTypeKind::Float && Type->Kind != NdTypeKind::Ptr))
    throw std::invalid_argument(
        "Swift ABI projection has an unsupported pointee type");
  number(Key, static_cast<unsigned>(Type->Kind));
  number(Key, Type->Size);
  number(Key, Type->IsSigned);
  if (Type->Kind == NdTypeKind::Ptr)
    type(Key, Type->Pointee, Depth + 1);
}
inline void location(std::string &Key, const SourceABIValueLocation &Location) {
  number(Key, static_cast<unsigned>(Location.Kind));
  number(Key, Location.RegisterOffset);
  number(Key, static_cast<uint64_t>(Location.EntryStackOffset));
  number(Key, Location.ValueBytes);
}
inline std::string key(const SourceFunctionTypeHint &Hint) {
  std::string Diagnostic;
  if (!validateSourceABI(Hint, Diagnostic))
    throw std::invalid_argument("invalid Swift ABI projection: " + Diagnostic);
  std::string Key;
  number(Key, static_cast<unsigned>(Hint.Origin));
  number(Key, static_cast<unsigned>(Hint.Architecture));
  number(Key, Hint.HasExplicitABI);
  type(Key, Hint.ReturnType);
  location(Key, Hint.ReturnLocation);
  number(Key, Hint.Parameters.size());
  for (const auto &Parameter : Hint.Parameters) {
    number(Key, Parameter.Name.size());
    Key += Parameter.Name;
    type(Key, Parameter.Type);
    location(Key, Parameter.Location);
  }
  return Key;
}
} // namespace projection_plan_detail

/// Share one pipeline body across equal full ABI declarations at an entry.
/// The nth distinct ABI at each entry can run in the same batch, so the number
/// of extra runs is the maximum per-entry variant count, not the alias count.
inline ABIProjectionPlan planABIProjections(
    const std::vector<ABIProjectionRow> &Rows,
    const std::map<va_t, SourceFunctionTypeHint> &BaseCalleeHints) {
  ABIProjectionPlan Plan;
  Plan.BaseCalleeHints = BaseCalleeHints;
  std::map<va_t, std::string> BaseKeys;
  for (const auto &[Entry, Hint] : BaseCalleeHints)
    BaseKeys.emplace(Entry, projection_plan_detail::key(Hint));
  struct Group {
    SourceFunctionTypeHint Hint;
    std::vector<size_t> Rows;
  };
  std::map<va_t, std::map<std::string, Group>> Groups;
  std::set<size_t> RowIndices;
  std::set<std::pair<va_t, std::string>> Identities;
  for (const auto &Row : Rows) {
    if (!Row.Entry || Row.MangledSymbol.empty() ||
        !RowIndices.insert(Row.Index).second ||
        !Identities.insert(sourceIdentity(Row.Entry, Row.MangledSymbol)).second)
      throw std::invalid_argument(
          "duplicate or invalid Swift projection identity");
    auto Key = projection_plan_detail::key(Row.Hint);
    if (auto Base = BaseKeys.find(Row.Entry); Base != BaseKeys.end()) {
      if (Base->second != Key)
        throw std::invalid_argument(
            "Swift ABI projection conflicts with an unambiguous base callee");
      Plan.BaseRows.push_back(Row.Index);
      continue;
    }
    auto [It, Added] = Groups[Row.Entry].try_emplace(std::move(Key));
    if (Added)
      It->second.Hint = Row.Hint;
    It->second.Rows.push_back(Row.Index);
  }
  for (auto &[Entry, Variants] : Groups) {
    if (Plan.Batches.size() < Variants.size())
      Plan.Batches.resize(Variants.size());
    size_t Index = 0;
    for (auto &[Key, Group] : Variants) {
      auto &Batch = Plan.Batches[Index++];
      Batch.BodyHints.emplace(Entry, std::move(Group.Hint));
      Batch.Rows.insert(Batch.Rows.end(), Group.Rows.begin(), Group.Rows.end());
    }
  }
  return Plan;
}

} // namespace neverd::sdk::swift_source
#endif
