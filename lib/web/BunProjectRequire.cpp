//===- BunProjectRequire.cpp - Bound Bun require alias candidates ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Narrow cross-module aliases built from the shared lexical/module owners.
///
//===----------------------------------------------------------------------===//

#include "BunProjectRequire.h"

#include "SourceModel.h"

#include "neverd/web/Error.h"
#include "neverd/web/SourceModules.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;
constexpr uint64_t MaxSteps = 32ULL * 1024 * 1024;
constexpr uint64_t MaxNameUnits = 1024 * 1024;

bool virtualName(std::u16string_view Text) {
  return (Text.starts_with(u"/$bunfs/") || Text.starts_with(u"B:/~BUN/")) &&
         Text.find(u'\0') == Text.npos;
}

std::u16string utf16(std::string_view Text) {
  llvm::SmallVector<llvm::UTF16, 64> Units;
  if (!llvm::convertUTF8ToUTF16String(llvm::StringRef(Text), Units))
    throw Error("bun_require_invalid_virtual_name");
  return {Units.begin(), Units.end()};
}

std::string unitBytes(std::u16string_view Text) {
  std::string Result;
  Result.reserve(Text.size() * 2);
  for (auto Unit : Text) {
    Result.push_back(char(Unit));
    Result.push_back(char(Unit >> 8));
  }
  return Result;
}

bool hash(std::string_view Text) {
  return Text.size() == 64 && std::all_of(Text.begin(), Text.end(), [](char C) {
           return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
         });
}

std::string providerID(const BunProjectRequireProvider::Data &P) {
  const auto Virtual = unitBytes(P.VirtualName),
             Export = unitBytes(P.ExportedName);
  const auto Start = std::to_string(P.InitializerStart);
  const auto End = std::to_string(P.InitializerEnd);
  return identity("bun-require-provider",
                  {BunProjectRequireProfile, P.SourceArtifactID, P.SourceID,
                   P.SourceHash, P.BindingID, P.ExportID, Virtual, Export,
                   Start, End});
}

bool validProvider(const BunProjectRequireProvider &Provider) {
  const auto &P = Provider.data();
  return !P.SourceArtifactID.empty() && !P.SourceID.empty() &&
         hash(P.SourceHash) && !P.BindingID.empty() && !P.ExportID.empty() &&
         virtualName(P.VirtualName) && !P.ExportedName.empty() &&
         P.ExportedName != u"default" &&
         P.InitializerStart < P.InitializerEnd && !P.ID.empty();
}

std::string callID(const BunProjectRequireCall::Data &C) {
  const auto Target = unitBytes(C.Target);
  const auto Start = std::to_string(C.CallStart),
             End = std::to_string(C.CallEnd);
  const auto CalleeStart = std::to_string(C.CalleeStart);
  const auto CalleeEnd = std::to_string(C.CalleeEnd);
  const auto LiteralStart = std::to_string(C.OriginalStart);
  const auto LiteralEnd = std::to_string(C.OriginalEnd);
  return identity("bun-require-call",
                  {BunProjectRequireProfile, C.SourceArtifactID, C.SourceHash,
                   C.ProviderID, C.ProviderSourceHash, C.ProviderBindingID,
                   C.ProviderExportID, C.ConsumerBindingID, C.ConsumerImportID,
                   Start, End, CalleeStart, CalleeEnd, LiteralStart, LiteralEnd,
                   Target});
}

struct Model {
  SourceAnalysis S;
  SourceBindingAnalysis B;
  SourceModuleAnalysis M;
  std::vector<uint32_t> Parent, Declarations, DeclarationCounts, References;
  std::vector<bool> Written;
  uint64_t Steps = 0;

  void step(uint64_t Count = 1) {
    if (Count > MaxSteps - Steps)
      throw Error("bun_require_work_budget_exceeded");
    Steps += Count;
  }

  Model(std::string_view ArtifactID, std::string_view Text,
        std::string_view SourceType) {
    if (SourceType != "module" || ArtifactID.empty())
      throw Error("bun_require_module_source_required");
    S = inspectLargeJavaScript(ArtifactID, Text, SourceType);
    if (S.ParseStatus != "parsed" || S.LexemeStatus != "ok")
      throw Error("bun_require_source_unavailable");
    step(validateSourceModel(S));
    B = analyzeSourceBindings(S);
    if (B.Status != "ok")
      throw Error("bun_require_bindings_unavailable");
    M = analyzeSourceModules(S, B);
    if (M.Status != "ok")
      throw Error("bun_require_modules_unavailable");
    for (const auto &Scope : B.Scopes) {
      step();
      if (Scope.PossibleDirectEval || Scope.ObjectEnvironment)
        throw Error("bun_require_dynamic_scope");
    }
    auto Index = indexSourceDeclarations(S, B);
    step(Index.Steps);
    Declarations = std::move(Index.UniqueBindings);
    DeclarationCounts.resize(B.Bindings.size());
    Written.resize(B.Bindings.size());
    Parent.assign(S.Nodes.size(), None);
    References.assign(S.Nodes.size(), None);
    for (uint32_t I = 0; I < S.Nodes.size(); ++I)
      for (const auto &C : S.Nodes[I].Children) {
        step();
        Parent[C.Index] = I;
      }
    for (const auto &D : B.Declarations) {
      step();
      ++DeclarationCounts.at(D.Binding);
    }
    for (uint32_t I = 0; I < B.References.size(); ++I) {
      step();
      const auto &R = B.References[I];
      References.at(R.Node) = I;
      if (R.Binding != None &&
          (R.Access == "write" || R.Access == "read_write"))
        Written.at(R.Binding) = true;
    }
  }

  bool is(uint32_t I, std::string_view Kind) const {
    return I != None && S.Nodes.at(I).Kind == Kind;
  }
  uint32_t child(uint32_t I, std::string_view Field) {
    if (I == None)
      return None;
    for (const auto &C : S.Nodes.at(I).Children) {
      step();
      if (C.Field == Field && C.Ordinal == 0)
        return C.Index;
    }
    return None;
  }
  bool identifier(uint32_t I, std::u16string_view Name) const {
    const auto *Text = is(I, "Identifier") ? S.Nodes[I].text("name") : nullptr;
    return Text && *Text == Name;
  }
  bool importMeta(uint32_t I) {
    return is(I, "MetaProperty") && identifier(child(I, "meta"), u"import") &&
           identifier(child(I, "property"), u"meta");
  }
  bool metaRequire(uint32_t I) {
    return is(I, "MemberExpression") && !S.Nodes[I].flag("computed") &&
           identifier(child(I, "property"), u"require") &&
           importMeta(child(I, "object"));
  }
  bool stable(uint32_t Binding) const {
    return Binding < B.Bindings.size() && !B.Bindings[Binding].Conflicting &&
           !Written[Binding] && DeclarationCounts[Binding] == 1;
  }
  bool topLevelDeclarator(uint32_t I) const {
    if (!is(I, "VariableDeclarator"))
      return false;
    const auto Declaration = Parent[I];
    if (!is(Declaration, "VariableDeclaration"))
      return false;
    const auto Above = Parent[Declaration];
    return Above == 0 ||
           (is(Above, "ExportNamedDeclaration") && Parent[Above] == 0);
  }
};

void diagnostic(std::vector<SourceDiagnostic> &Output, std::string_view Code,
                uint64_t Offset) {
  if (Output.size() >= MaxBunProjectRequireRecords)
    throw Error("bun_require_record_budget_exceeded");
  Output.push_back({std::string(Code), int64_t(Offset)});
}
} // namespace

bool validBunProjectRequireCall(const BunProjectRequireCall &Call) {
  const auto &C = Call.data();
  return !C.SourceArtifactID.empty() && hash(C.SourceHash) &&
         !C.ProviderID.empty() && hash(C.ProviderSourceHash) &&
         !C.ProviderBindingID.empty() && !C.ProviderExportID.empty() &&
         !C.ConsumerBindingID.empty() && !C.ConsumerImportID.empty() &&
         C.CallStart <= C.CalleeStart && C.CalleeStart < C.CalleeEnd &&
         C.CalleeEnd <= C.OriginalStart && C.OriginalStart < C.OriginalEnd &&
         C.OriginalEnd < C.CallEnd && C.Target.size() <= MaxNameUnits &&
         virtualName(C.Target) && !C.ID.empty();
}

BunProjectRequireProviders collectBunProjectRequireProviders(
    std::string_view ArtifactID, std::string_view Text,
    std::string_view SourceType, std::string_view VirtualName) {
  BunProjectRequireProviders Result;
  try {
    if (VirtualName.size() > MaxNameUnits)
      throw Error("bun_require_name_budget_exceeded");
    const auto Name = utf16(VirtualName);
    if (!virtualName(Name))
      throw Error("bun_require_invalid_virtual_name");
    Model Model(ArtifactID, Text, SourceType);
    // This first profile deliberately has no provider dependency chain or
    // re-export graph. In particular it cannot admit cycles by name guessing.
    if (!Model.M.Requests.empty())
      throw Error("bun_require_provider_dependencies_unsupported");
    std::map<uint32_t, uint32_t> Seeds;
    for (uint32_t I = 0; I < Model.S.Nodes.size(); ++I) {
      Model.step();
      if (!Model.importMeta(I))
        continue;
      const auto Member = Model.Parent[I];
      const auto Init = Member == None ? None : Model.Parent[Member];
      if (!Model.metaRequire(Member) || !Model.topLevelDeclarator(Init) ||
          Model.child(Init, "init") != Member)
        throw Error("bun_require_import_meta_escape_or_write");
      const auto Decl = Model.child(Init, "id");
      if (!Model.is(Decl, "Identifier"))
        throw Error("bun_require_initializer_pattern_unsupported");
      const auto Binding = Model.Declarations.at(Decl);
      if (!Model.stable(Binding)) {
        diagnostic(Result.Diagnostics,
                   "bun_require_provider_binding_written_or_ambiguous",
                   Model.S.Nodes[Decl].Start);
        continue;
      }
      if (Model.B.Bindings[Binding].Scope != 0)
        throw Error("bun_require_provider_binding_not_module_local");
      Seeds.emplace(Binding, Member);
    }
    uint64_t ExportUnits = 0;
    for (const auto &Export : Model.M.Exports) {
      Model.step();
      const auto Found = Seeds.find(Export.Binding);
      if (Found == Seeds.end() || Export.Conflicting ||
          (Export.Kind != "local" && Export.Kind != "declaration") ||
          Export.LinkStatus != "lexical_binding" || Export.Request != None ||
          Export.ExportedName == None)
        continue;
      const auto &Exported = Model.M.Names.at(Export.ExportedName);
      if (Exported.empty() || Exported == u"default")
        continue;
      const auto Units = Name.size() + Exported.size();
      if (Units > MaxNameUnits - ExportUnits)
        throw Error("bun_require_name_budget_exceeded");
      ExportUnits += Units;
      if (Result.Providers.size() >= MaxBunProjectRequireRecords)
        throw Error("bun_require_record_budget_exceeded");
      const auto &Node = Model.S.Nodes[Found->second];
      BunProjectRequireProvider::Data P;
      P.SourceArtifactID = ArtifactID;
      P.SourceID = Model.S.ID;
      P.SourceHash = Model.S.BlobHash;
      P.BindingID = Model.B.Bindings[Export.Binding].ID;
      P.ExportID = Export.ID;
      P.VirtualName = Name;
      P.ExportedName = Exported;
      P.InitializerStart = Node.Start;
      P.InitializerEnd = Node.End;
      P.ID = providerID(P);
      Result.Providers.push_back(BunProjectRequireProvider(std::move(P)));
    }
    Result.Status = "ok";
  } catch (const Error &E) {
    Result.Status = E.what();
    Result.Providers.clear();
    Result.Diagnostics.push_back({Result.Status, -1});
  }
  return Result;
}

BunProjectRequireCalls proveBunProjectRequireCalls(
    std::string_view ArtifactID, std::string_view Text,
    std::string_view SourceType,
    std::span<const BunProjectRequireProvider> Providers,
    std::span<const BunProjectSourceTarget> Targets) {
  BunProjectRequireCalls Result;
  try {
    if (Providers.size() > MaxBunProjectRequireRecords ||
        Targets.size() > MaxBunProjectSourceTargets)
      throw Error("bun_require_record_budget_exceeded");
    using Key = std::pair<std::u16string, std::u16string>;
    std::map<Key, const BunProjectRequireProvider *> Exports;
    std::map<std::u16string, std::string> Sources;
    std::set<std::u16string> TargetNames;
    uint64_t Units = 0;
    auto Charge = [&](uint64_t Count) {
      if (Count > MaxNameUnits - Units)
        throw Error("bun_require_name_budget_exceeded");
      Units += Count;
    };
    for (const auto &Provider : Providers) {
      const auto &P = Provider.data();
      Charge(P.VirtualName.size());
      Charge(P.ExportedName.size());
      if (!validProvider(Provider))
        throw Error("bun_require_invalid_provider_evidence");
      if (!Exports.emplace(Key{P.VirtualName, P.ExportedName}, &Provider)
               .second)
        throw Error("bun_require_ambiguous_provider");
      const auto [It, Added] = Sources.emplace(P.VirtualName, P.SourceHash);
      if (!Added && It->second != P.SourceHash)
        throw Error("bun_require_ambiguous_provider");
    }
    for (const auto &T : Targets) {
      Charge(T.VirtualName.size());
      const auto Name = utf16(T.VirtualName);
      if (!virtualName(Name) || !TargetNames.insert(Name).second)
        throw Error("bun_require_invalid_target_mapping");
    }
    Model Model(ArtifactID, Text, SourceType);
    std::set<uint32_t> AttributedRequests;
    for (const auto &Attribute : Model.M.Attributes) {
      Model.step();
      AttributedRequests.insert(Attribute.Request);
    }
    std::map<uint32_t,
             std::pair<const BunProjectRequireProvider *, std::string>>
        Imported;
    for (const auto &Import : Model.M.Imports) {
      Model.step();
      if (Import.Kind != "named" || Import.ImportedName == None ||
          !Model.stable(Import.Binding) ||
          Model.B.Bindings[Import.Binding].Kind != "import")
        continue;
      const auto &Request = Model.M.Requests.at(Import.Request);
      if (Request.Kind != "esm_import" || Request.SpecifierStatus != "literal")
        continue;
      const auto &Specifier = Model.M.Names.at(Request.Specifier);
      const auto Found =
          Exports.find({Specifier, Model.M.Names.at(Import.ImportedName)});
      if (Found == Exports.end())
        continue;
      const bool Attributes = AttributedRequests.count(Import.Request);
      if (Attributes || !TargetNames.count(Specifier)) {
        diagnostic(Result.Diagnostics, "bun_require_provider_import_unverified",
                   Model.S.Nodes[Import.Node].Start);
        continue;
      }
      Imported.emplace(Import.Binding,
                       std::make_pair(Found->second, Import.ID));
    }
    for (uint32_t I = 0; I < Model.S.Nodes.size(); ++I) {
      Model.step();
      if (!Model.is(I, "CallExpression"))
        continue;
      const auto Callee = Model.child(I, "callee");
      if (!Model.is(Callee, "Identifier") || Model.References[Callee] == None)
        continue;
      const auto &Ref = Model.B.References[Model.References[Callee]];
      if (Ref.Resolution != "lexical_binding" || Ref.Access != "read")
        continue;
      const auto Import = Imported.find(Ref.Binding);
      if (Import == Imported.end())
        continue;
      uint32_t Argument = None;
      uint64_t Arguments = 0;
      for (const auto &C : Model.S.Nodes[I].Children) {
        Model.step();
        if (C.Field == "arguments") {
          Argument = C.Index;
          ++Arguments;
        }
      }
      if (Arguments != 1 || !Model.is(Argument, "StringLiteral")) {
        diagnostic(Result.Diagnostics, "bun_require_nonliteral_or_arity",
                   Model.S.Nodes[I].Start);
        continue;
      }
      const auto *Target = Model.S.Nodes[Argument].text("value");
      if (!Target)
        throw Error("bun_require_invalid_literal_model");
      Charge(Target->size());
      if (!virtualName(*Target)) {
        if (Result.Calls.size() + Result.ExternalReferences.size() >=
            MaxBunProjectRequireRecords)
          throw Error("bun_require_record_budget_exceeded");
        const auto &Literal = Model.S.Nodes[Argument];
        Result.ExternalReferences.push_back(
            {Literal.Start, Literal.End, *Target});
        continue;
      }
      if (!TargetNames.count(*Target)) {
        diagnostic(Result.Diagnostics, "bun_require_unknown_virtual_target",
                   Model.S.Nodes[Argument].Start);
        continue;
      }
      if (Result.Calls.size() + Result.ExternalReferences.size() >=
          MaxBunProjectRequireRecords)
        throw Error("bun_require_record_budget_exceeded");
      const auto &[P, ImportID] = Import->second;
      BunProjectRequireCall::Data C;
      C.SourceArtifactID = ArtifactID;
      C.SourceHash = Model.S.BlobHash;
      C.ProviderID = P->data().ID;
      C.ProviderSourceHash = P->data().SourceHash;
      C.ProviderBindingID = P->data().BindingID;
      C.ProviderExportID = P->data().ExportID;
      C.ConsumerBindingID = Model.B.Bindings[Ref.Binding].ID;
      C.ConsumerImportID = ImportID;
      C.CallStart = Model.S.Nodes[I].Start;
      C.CallEnd = Model.S.Nodes[I].End;
      C.CalleeStart = Model.S.Nodes[Callee].Start;
      C.CalleeEnd = Model.S.Nodes[Callee].End;
      C.OriginalStart = Model.S.Nodes[Argument].Start;
      C.OriginalEnd = Model.S.Nodes[Argument].End;
      C.Target = *Target;
      C.ID = callID(C);
      auto Proof = BunProjectRequireCall(std::move(C));
      if (!validBunProjectRequireCall(Proof))
        throw Error("bun_require_invalid_call_evidence");
      Result.Calls.push_back(std::move(Proof));
    }
    Result.Status = "ok";
  } catch (const Error &E) {
    Result.Status = E.what();
    Result.Calls.clear();
    Result.ExternalReferences.clear();
    Result.Diagnostics.push_back({Result.Status, -1});
  }
  return Result;
}
} // namespace neverd::web
