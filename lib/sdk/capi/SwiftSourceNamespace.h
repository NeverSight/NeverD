#ifndef NEVERD_SDK_CAPI_SWIFTSOURCENAMESPACE_H
#define NEVERD_SDK_CAPI_SWIFTSOURCENAMESPACE_H

#include "SwiftSourcePropertyModel.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::sdk::swift_source {

struct NominalSourceContext {
  std::string Module;
  std::string Kind;
  std::string Name;
};

struct SourceNamespaceConflicts {
  std::set<size_t> Methods;
  std::set<size_t> Nominals;
};

/// A single emitted source file has one top-level namespace. Preserve method
/// identities separately, but never publish colliding source declarations.
inline SourceNamespaceConflicts namespaceConflicts(
    const std::vector<std::optional<SwiftSourceSignature>> &Signatures,
    const std::vector<NominalSourceContext> &Nominals) {
  using Owner = std::tuple<std::string, std::string>;
  using Reference = std::pair<bool, size_t>;
  std::map<std::string, std::map<Owner, std::vector<Reference>>> Names;
  using Declaration = std::tuple<std::string, std::string, std::string,
                                 std::string, bool, std::string>;
  std::map<Declaration, std::vector<size_t>> Declarations;
  using Property =
      std::tuple<std::string, std::string, std::string, std::string, bool>;
  std::map<Property, std::vector<size_t>> Properties, OtherMembers;
  SourceNamespaceConflicts Result;
  auto &Conflicts = Result.Methods;
  for (size_t Index = 0; Index < Signatures.size(); ++Index) {
    if (!Signatures[Index])
      continue;
    const auto &S = *Signatures[Index];
    const auto Name = S.ContextKind == "global" ? S.Name : S.ContextName;
    Names[Name][{S.Module, S.ContextKind}].push_back({false, Index});
    const Property PropertyKey{S.Module, S.ContextKind, S.ContextName, S.Name,
                               S.IsStatic};
    if (isAccessor(S)) {
      Properties[PropertyKey].push_back(Index);
      if (!propertyType(S))
        Conflicts.insert(Index);
      continue;
    }
    OtherMembers[PropertyKey].push_back(Index);
    std::string Parameters;
    for (size_t I = 0; I < S.Parameters.size(); ++I)
      Parameters +=
          S.Labels[I] + ":" + typeSpelling(S.Parameters[I].Type) + ";";
    Parameters += "->" + typeSpelling(S.ReturnType);
    Declarations[{S.Module, S.ContextKind, S.ContextName, S.Name, S.IsStatic,
                  Parameters}]
        .push_back(Index);
  }
  for (size_t Index = 0; Index < Nominals.size(); ++Index) {
    const auto &N = Nominals[Index];
    Names[N.Name][{N.Module, N.Kind}].push_back({true, Index});
  }
  for (const auto &[Name, Owners] : Names)
    if (Name == "Swift" || Owners.size() > 1)
      for (const auto &[Owner, Indices] : Owners)
        for (const auto &[Nominal, Index] : Indices)
          (Nominal ? Result.Nominals : Conflicts).insert(Index);
  for (const auto &[Declaration, Indices] : Declarations)
    if (Indices.size() > 1)
      Conflicts.insert(Indices.begin(), Indices.end());
  for (const auto &[Property, Indices] : Properties) {
    std::set<std::string> Kinds, Types;
    bool Invalid = false;
    for (size_t Index : Indices) {
      const auto &S = *Signatures[Index];
      Invalid |= !Kinds.insert(S.DeclarationKind).second;
      if (const auto *Type = propertyType(S))
        Types.insert(typeSpelling(*Type));
      for (const auto &Field : S.ContextFields)
        if (!S.IsStatic && Field.Name == S.Name)
          Invalid |= Field.BackingName.empty() || !propertyType(S) ||
                     typeSpelling(Field.Type) != typeSpelling(*propertyType(S));
    }
    Invalid |= Types.size() != 1;
    if (auto Other = OtherMembers.find(Property); Other != OtherMembers.end()) {
      Invalid = true;
      Conflicts.insert(Other->second.begin(), Other->second.end());
    }
    if (Invalid)
      Conflicts.insert(Indices.begin(), Indices.end());
  }
  return Result;
}

inline std::set<size_t> namespaceConflicts(
    const std::vector<std::optional<SwiftSourceSignature>> &Signatures) {
  return namespaceConflicts(Signatures, {}).Methods;
}

} // namespace neverd::sdk::swift_source
#endif
