#ifndef NEVERD_SDK_CAPI_SWIFTSOURCEPROPERTIES_H
#define NEVERD_SDK_CAPI_SWIFTSOURCEPROPERTIES_H

#include "SwiftSourcePropertyModel.h"

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd::sdk::swift_source {
using PropertyContext = std::tuple<std::string, std::string, std::string>;
using PropertyIdentity = std::tuple<PropertyContext, bool, std::string>;
inline PropertyContext propertyContext(const SwiftSourceSignature &S) {
  return {S.Module, S.ContextKind, S.ContextName};
}
inline PropertyIdentity propertyIdentity(const SwiftSourceSignature &S) {
  return {propertyContext(S), S.IsStatic, S.Name};
}
inline std::string storageName(const SwiftStorageField &Field) {
  return Field.BackingName.empty() ? Field.Name : Field.BackingName;
}

/// Freeze source storage names for a whole context before any body is emitted.
/// Failed accessors still participate, so another row's success never changes
/// the spelling an initializer or direct-fields self projection used.
inline void planPropertyStorage(
    std::vector<std::optional<SwiftSourceSignature>> &Signatures) {
  std::map<PropertyContext, std::vector<size_t>> Contexts;
  for (size_t I = 0; I < Signatures.size(); ++I)
    if (Signatures[I] && Signatures[I]->ContextKind != "global")
      Contexts[propertyContext(*Signatures[I])].push_back(I);
  for (const auto &[Context, Indices] : Contexts) {
    std::set<std::string> Taken, Properties;
    const auto &Fields = Signatures[Indices.front()]->ContextFields;
    std::string Canonical;
    auto Layout = [](const std::vector<SwiftStorageField> &Fields) {
      std::string Key;
      for (const auto &F : Fields)
        Key += std::to_string(F.Name.size()) + ":" + F.Name + ":" +
               typeSpelling(F.Type) + ":" + std::to_string(F.Offset) + ":" +
               std::to_string(F.IsMutable) + ";";
      return Key;
    };
    Canonical = Layout(Fields);
    bool Conflict = false;
    for (size_t I : Indices) {
      const auto &S = *Signatures[I];
      Conflict |= Layout(S.ContextFields) != Canonical;
      Taken.insert(S.Name);
      for (const auto &F : S.ContextFields)
        Taken.insert(F.Name);
      if (isAccessor(S) && !S.IsStatic)
        Properties.insert(S.Name);
    }
    if (Conflict) {
      for (size_t I : Indices)
        Signatures[I]->UnsupportedReason =
            "Swift context declarations disagree on native storage layout";
      continue;
    }
    std::map<std::string, std::string> Names;
    for (const auto &Field : Fields) {
      if (!Properties.count(Field.Name))
        continue;
      const auto Base = "neverd_storage_" + std::to_string(Field.Offset);
      auto Name = Base;
      for (unsigned Suffix = 1; Taken.count(Name); ++Suffix)
        Name = Base + "_" + std::to_string(Suffix);
      Taken.insert(Name);
      Names.emplace(Field.Name, std::move(Name));
    }
    for (size_t I : Indices)
      for (auto &Field : Signatures[I]->ContextFields)
        Field.BackingName =
            Names.count(Field.Name) ? Names.at(Field.Name) : std::string();
  }
}

inline std::map<size_t, std::string> incompleteProperties(
    const std::vector<std::optional<SwiftSourceSignature>> &Signatures,
    const std::set<size_t> &Recovered) {
  std::set<PropertyIdentity> Getters;
  for (size_t I : Recovered)
    if (Signatures[I] && Signatures[I]->DeclarationKind == "getter")
      Getters.insert(propertyIdentity(*Signatures[I]));
  std::map<size_t, std::string> Result;
  for (size_t I : Recovered)
    if (Signatures[I] && Signatures[I]->DeclarationKind == "setter" &&
        !Getters.count(propertyIdentity(*Signatures[I])))
      Result.emplace(
          I, "Swift setter has no recovered getter for its computed property");
  return Result;
}

struct PropertySourceMember {
  const SwiftSourceSignature *Signature;
  std::string MemberSource;
};

struct PropertyRuntimeSource {
  /// Only canonical, mutually proven modify/resume pairs may populate this.
  std::map<std::string, uint64_t> Modifiers;
  bool TrivialDestructor = false;
};

/// Assemble only real accessor bodies. A setter cannot manufacture a getter
/// from storage metadata. Identity and recovery counts remain per input row.
inline std::string
assemblePropertyContext(const SwiftSourceSignature &Context,
                        const std::vector<PropertySourceMember> &Members,
                        const PropertyRuntimeSource &Runtime = {}) {
  std::string Source =
      Context.ContextKind + " `" + Context.ContextName + "` {\n";
  for (const auto &Field : Context.ContextFields)
    Source += "    " +
              std::string(Field.BackingName.empty() ? "" : "private ") +
              (Field.IsMutable ? "var" : "let") + " `" + storageName(Field) +
              "`: " + typeSpelling(Field.Type) + "\n";
  struct Accessors {
    const SwiftSourceSignature *Signature = nullptr;
    std::string Getter, Setter;
  };
  std::map<std::pair<bool, std::string>, Accessors> Properties;
  for (const auto &Member : Members) {
    if (!Member.Signature ||
        propertyContext(*Member.Signature) != propertyContext(Context))
      throw std::invalid_argument(
          "Swift property source belongs to another context");
    const auto &S = *Member.Signature;
    if (!isAccessor(S)) {
      Source += Member.MemberSource + "\n";
      continue;
    }
    if (!propertyType(S))
      throw std::invalid_argument("Swift property has no scalar source type");
    auto &P = Properties[{S.IsStatic, S.Name}];
    if (P.Signature && typeSpelling(*propertyType(*P.Signature)) !=
                           typeSpelling(*propertyType(S)))
      throw std::invalid_argument("Swift getter and setter types disagree");
    P.Signature = &S;
    auto &Body = S.DeclarationKind == "getter" ? P.Getter : P.Setter;
    if (!Body.empty() || Member.MemberSource.empty())
      throw std::invalid_argument(
          "Swift property has an empty or duplicate accessor");
    Body = Member.MemberSource;
  }
  for (const auto &[Identity, P] : Properties) {
    if (P.Getter.empty())
      throw std::invalid_argument(
          "Swift property cannot publish only a setter");
    std::string Modify;
    if (auto It = Runtime.Modifiers.find(P.Signature->Name);
        It != Runtime.Modifiers.end()) {
      const SwiftStorageField *Storage = nullptr;
      for (const auto &Field : Context.ContextFields)
        if (Field.Name == P.Signature->Name && Field.Offset == It->second &&
            Field.IsMutable && !Field.BackingName.empty() &&
            typeSpelling(Field.Type) ==
                typeSpelling(*propertyType(*P.Signature)))
          Storage = &Field;
      if (Context.ContextKind != "struct" || P.Signature->IsStatic || !Storage)
        throw std::invalid_argument(
            "Swift modifier has no matching private mutable storage");
      Modify =
          "_modify {\n    yield &self.`" + storageName(*Storage) + "`\n}\n";
    }
    Source += "    public " +
              std::string(P.Signature->IsStatic ? "static " : "") + "var `" +
              P.Signature->Name +
              "`: " + typeSpelling(*propertyType(*P.Signature)) + " {\n" +
              P.Getter + P.Setter + Modify + "    }\n";
  }
  for (const auto &[Name, Offset] : Runtime.Modifiers)
    if (!Properties.count({false, Name}))
      throw std::invalid_argument(
          "Swift modifier has no recovered instance property");
  if (Runtime.TrivialDestructor) {
    if (Context.ContextKind != "class")
      throw std::invalid_argument(
          "Swift trivial destructor requires an emitted class");
    Source += "    deinit {}\n";
  }
  return Source + "}\n";
}
} // namespace neverd::sdk::swift_source

#endif
