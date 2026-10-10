#include "neverd/web/SourceModules.h"

#include "SourceModel.h"

#include "neverd/web/Session.h"

#include <algorithm>
#include <map>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;

class Inventory {
  const SourceAnalysis &Source;
  const SourceBindingAnalysis &Bindings;
  SourceModuleAnalysis Result;
  std::map<std::u16string, uint32_t> Names;
  std::map<uint32_t, uint32_t> Exported;
  std::vector<uint32_t> DeclarationAt, ReferenceAt, Parent;
  uint64_t NameUnits = 0;

  void step(uint64_t Count = 1) {
    if (Count > MaxJavaScriptModuleSteps - Result.Steps)
      throw Error("source_module_budget_exceeded");
    Result.Steps += Count;
  }
  const SyntaxNode &node(uint32_t I) const { return Source.Nodes.at(I); }
  uint32_t child(uint32_t I, std::string_view Field) {
    for (const auto &C : node(I).Children) {
      step();
      if (C.Field == Field)
        return C.Index;
    }
    return None;
  }
  bool is(uint32_t I, std::string_view Kind) const {
    return I != None && node(I).Kind == Kind;
  }
  bool textIs(uint32_t I, std::string_view Field,
              std::u16string_view Value) const {
    const auto *Text = node(I).text(Field);
    return Text && *Text == Value;
  }
  uint32_t name(std::u16string_view Text) {
    step(Text.size() + 1);
    if (auto It = Names.find(std::u16string(Text)); It != Names.end())
      return It->second;
    if (Text.size() > MaxJavaScriptModuleNameUnits - NameUnits)
      throw Error("source_module_name_budget_exceeded");
    const auto Index = uint32_t(Result.Names.size());
    Names.emplace(Text, Index);
    Result.Names.emplace_back(Text);
    NameUnits += Text.size();
    return Index;
  }
  uint32_t sourceName(uint32_t I) {
    if (I == None)
      throw Error("invalid_source_model");
    const auto *Text = node(I).text(is(I, "Identifier") ? "name" : "value");
    if ((!is(I, "Identifier") && !is(I, "StringLiteral")) || !Text)
      throw Error("invalid_source_model");
    return name(*Text);
  }
  void diagnostic(const char *Code, uint32_t I) {
    Result.Status = "partial";
    ++Result.DiagnosticCount;
    if (Result.Diagnostics.size() < 32)
      Result.Diagnostics.push_back(
          {Code, I == None ? -1 : int64_t(node(I).Start)});
  }
  std::string id(std::string_view Kind, uint32_t I, size_t Ordinal) const {
    return identity("source-module-record",
                    {Result.ID, Kind, node(I).ID, std::to_string(Ordinal)});
  }
  uint32_t bindingAt(uint32_t I, bool Declaration) const {
    if (I == None)
      return None;
    const auto Index = Declaration ? DeclarationAt[I] : ReferenceAt[I];
    return Index == None ? None
           : Declaration ? Bindings.Declarations[Index].Binding
                         : Bindings.References[Index].Binding;
  }
  uint32_t request(uint32_t I, std::string_view Kind, uint32_t Specifier) {
    step();
    SourceModuleRequest R;
    R.ID = id(Kind, I, Result.Requests.size());
    R.Kind = Kind;
    R.Node = I;
    R.SpecifierNode = Specifier;
    if (is(Specifier, "StringLiteral")) {
      R.Specifier = sourceName(Specifier);
      R.SpecifierStatus = "literal";
    } else {
      R.SpecifierStatus = Specifier == None ? "missing" : "dynamic";
    }
    const auto Index = uint32_t(Result.Requests.size());
    Result.Requests.push_back(std::move(R));
    return Index;
  }
  void exportEntry(uint32_t I, std::string_view Kind, uint32_t ExportedName,
                   uint32_t LocalName, uint32_t Binding, uint32_t Request) {
    step();
    SourceModuleExport E;
    E.ID = id(Kind, I, Result.Exports.size());
    E.Kind = Kind;
    E.Node = I;
    E.ExportedName = ExportedName;
    E.LocalName = LocalName;
    E.Binding = Binding;
    E.Request = Request;
    E.LinkStatus = Request != None   ? "foreign_module_unlinked"
                   : Binding == None ? "expression_or_unbound"
                   : Bindings.Bindings[Binding].Conflicting
                       ? "conflicting_binding"
                       : "lexical_binding";
    if (ExportedName != None) {
      auto [It, Inserted] =
          Exported.emplace(ExportedName, Result.Exports.size());
      if (!Inserted) {
        E.Conflicting = true;
        Result.Exports[It->second].Conflicting = true;
        diagnostic("duplicate_export_name", I);
      }
    }
    Result.Exports.push_back(std::move(E));
  }
  void requireModuleTopLevel(uint32_t I) const {
    if (Source.SourceType != "module" || Parent[I] != 0)
      throw Error("unsupported_module_placement");
  }
  void importDeclaration(uint32_t I) {
    requireModuleTopLevel(I);
    const auto R = request(I, "esm_import", child(I, "source"));
    if (Result.Requests[R].SpecifierStatus != "literal")
      throw Error("invalid_source_model");
    std::map<uint32_t, uint32_t> Keys;
    for (const auto &C : node(I).Children) {
      step();
      if (C.Field == "assertions") {
        if (!is(C.Index, "ImportAttribute"))
          throw Error("unsupported_import_attribute");
        SourceModuleAttribute A;
        A.ID = id("import_assertion", C.Index, Result.Attributes.size());
        A.Node = C.Index;
        A.Request = R;
        A.Key = sourceName(child(C.Index, "key"));
        const auto Value = child(C.Index, "value");
        if (!is(Value, "StringLiteral"))
          throw Error("invalid_source_model");
        A.Value = sourceName(Value);
        auto [It, Inserted] = Keys.emplace(A.Key, Result.Attributes.size());
        if (!Inserted) {
          A.Conflicting = true;
          Result.Attributes[It->second].Conflicting = true;
          diagnostic("duplicate_import_assertion", C.Index);
        }
        Result.Attributes.push_back(std::move(A));
      } else if (C.Field == "specifiers") {
        SourceModuleImport Import;
        Import.ID = id("import", C.Index, Result.Imports.size());
        Import.Node = C.Index;
        Import.Request = R;
        const auto Local = child(C.Index, "local");
        Import.Binding = bindingAt(Local, true);
        if (!is(Local, "Identifier") || Import.Binding == None)
          throw Error("invalid_source_binding_model");
        if (is(C.Index, "ImportDefaultSpecifier")) {
          Import.Kind = "default";
          Import.ImportedName = name(u"default");
        } else if (is(C.Index, "ImportNamespaceSpecifier")) {
          Import.Kind = "namespace";
        } else if (is(C.Index, "ImportSpecifier")) {
          Import.Kind = "named";
          Import.ImportedName = sourceName(child(C.Index, "imported"));
        } else {
          throw Error("unsupported_import_specifier");
        }
        Result.Imports.push_back(std::move(Import));
      }
    }
  }
  void declaredExports(uint32_t I) {
    // Walk only binding positions. Scanning a declaration's whole subtree
    // would accidentally export function parameters or initializer locals.
    step();
    if (is(I, "Identifier")) {
      const auto B = bindingAt(I, true);
      if (B == None || Bindings.Bindings[B].Scope != 0)
        throw Error("invalid_source_binding_model");
      const auto Name = sourceName(I);
      exportEntry(I, "declaration", Name, Name, B, None);
    } else if (is(I, "FunctionDeclaration") || is(I, "ClassDeclaration")) {
      declaredExports(child(I, "id"));
    } else if (is(I, "VariableDeclaration")) {
      for (const auto &C : node(I).Children) {
        step();
        if (C.Field != "declarations" || !is(C.Index, "VariableDeclarator"))
          throw Error("invalid_source_model");
        declaredExports(child(C.Index, "id"));
      }
    } else if (is(I, "ArrayPattern") || is(I, "ObjectPattern")) {
      for (const auto &C : node(I).Children) {
        step();
        if ((is(I, "ArrayPattern") && C.Field == "elements") ||
            (is(I, "ObjectPattern") && C.Field == "properties"))
          declaredExports(C.Index);
      }
    } else if (is(I, "Property")) {
      declaredExports(child(I, "value"));
    } else if (is(I, "AssignmentPattern")) {
      declaredExports(child(I, "left"));
    } else if (is(I, "RestElement")) {
      declaredExports(child(I, "argument"));
    } else if (!is(I, "Empty")) {
      throw Error("unsupported_export_declaration");
    }
  }
  void exportDeclaration(uint32_t I) {
    requireModuleTopLevel(I);
    const auto SourceNode = child(I, "source");
    const auto R =
        SourceNode == None ? None : request(I, "esm_reexport", SourceNode);
    if (R != None && Result.Requests[R].SpecifierStatus != "literal")
      throw Error("invalid_source_model");
    if (is(I, "ExportAllDeclaration")) {
      if (R == None)
        throw Error("invalid_source_model");
      exportEntry(I, "star_reexport", None, None, None, R);
    } else if (is(I, "ExportDefaultDeclaration")) {
      const auto D = child(I, "declaration");
      if (D == None)
        throw Error("invalid_source_model");
      const auto Named =
          is(D, "FunctionDeclaration") || is(D, "ClassDeclaration")
              ? child(D, "id")
              : None;
      exportEntry(I,
                  Named == None ? "default_expression" : "default_declaration",
                  name(u"default"), Named == None ? None : sourceName(Named),
                  bindingAt(Named, true), None);
    } else {
      if (const auto D = child(I, "declaration"); D != None)
        declaredExports(D);
      for (const auto &C : node(I).Children) {
        step();
        if (C.Field != "specifiers")
          continue;
        const auto ExportedName = sourceName(child(C.Index, "exported"));
        if (is(C.Index, "ExportNamespaceSpecifier") && R != None) {
          exportEntry(C.Index, "namespace_reexport", ExportedName, None, None,
                      R);
        } else if (is(C.Index, "ExportSpecifier")) {
          const auto Local = child(C.Index, "local");
          const auto B = R == None ? bindingAt(Local, false) : None;
          exportEntry(C.Index, R == None ? "local" : "named_reexport",
                      ExportedName, sourceName(Local), B, R);
          if (R == None && B == None)
            diagnostic("unbound_export_name", C.Index);
        } else {
          throw Error("unsupported_export_specifier");
        }
      }
    }
  }
  void call(uint32_t I) {
    auto Callee = child(I, "callee");
    std::string_view Kind = "require_call_candidate";
    const bool MemberOptional = is(Callee, "OptionalMemberExpression");
    if (is(Callee, "MemberExpression") || MemberOptional) {
      const auto Property = child(Callee, "property");
      if (Property == None || !(node(Callee).flag("computed")
                                    ? is(Property, "StringLiteral") &&
                                          textIs(Property, "value", u"resolve")
                                    : is(Property, "Identifier") &&
                                          textIs(Property, "name", u"resolve")))
        return;
      Callee = child(Callee, "object");
      Kind = "require_resolve_candidate";
    }
    if (!is(Callee, "Identifier") || !textIs(Callee, "name", u"require"))
      return;
    uint32_t Argument = None, Count = 0;
    for (const auto &C : node(I).Children) {
      step();
      if (C.Field == "arguments") {
        if (C.Ordinal == 0)
          Argument = C.Index;
        ++Count;
      }
    }
    const auto R = request(I, Kind, Argument);
    auto &Entry = Result.Requests[R];
    Entry.CalleeNode = Callee;
    Entry.CalleeBinding = bindingAt(Callee, false);
    Entry.ArgumentCount = Count;
    Entry.Optional = is(I, "OptionalCallExpression") || MemberOptional;
    if (ReferenceAt[Callee] == None)
      throw Error("invalid_source_binding_model");
    const auto &Ref = Bindings.References[ReferenceAt[Callee]];
    if (Ref.Resolution != "lexical_binding")
      Entry.CalleeEvidence = Ref.Resolution;
    else if (Entry.CalleeBinding != None &&
             Bindings.Bindings[Entry.CalleeBinding].Kind ==
                 "commonjs_parameter")
      Entry.CalleeEvidence = "caller_selected_commonjs_parameter";
    else
      Entry.CalleeEvidence = "other_lexical_binding";
  }
  void admitBindings() {
    DeclarationAt.assign(Source.Nodes.size(), None);
    ReferenceAt.assign(Source.Nodes.size(), None);
    Parent.assign(Source.Nodes.size(), None);
    for (uint32_t I = 0; I < Source.Nodes.size(); ++I)
      for (const auto &C : node(I).Children) {
        step();
        Parent[C.Index] = I;
      }
    for (uint32_t I = 0; I < Bindings.Declarations.size(); ++I) {
      step();
      const auto &D = Bindings.Declarations[I];
      if (D.Node == None && D.Binding < Bindings.Bindings.size() &&
          Bindings.Bindings[D.Binding].Implicit)
        continue;
      if (D.Node >= DeclarationAt.size() ||
          D.Binding >= Bindings.Bindings.size())
        throw Error("invalid_source_binding_model");
      // A class declaration's one identifier introduces both an outer binding
      // and an inner class-name binding. Export linkage uses the outer one.
      if (DeclarationAt[D.Node] != None &&
          bindingAt(D.Node, true) != D.Binding) {
        const auto &Old = Bindings.Declarations[DeclarationAt[D.Node]];
        if (Old.Kind == "class" && D.Kind == "class_name")
          continue;
        if (Old.Kind != "class_name" || D.Kind != "class")
          throw Error("invalid_source_binding_model");
      }
      DeclarationAt[D.Node] = I;
    }
    for (uint32_t I = 0; I < Bindings.References.size(); ++I) {
      step();
      const auto &R = Bindings.References[I];
      if (R.Node >= ReferenceAt.size() || ReferenceAt[R.Node] != None ||
          (R.Binding != None && R.Binding >= Bindings.Bindings.size()))
        throw Error("invalid_source_binding_model");
      ReferenceAt[R.Node] = I;
    }
  }

public:
  Inventory(const SourceAnalysis &S, const SourceBindingAnalysis &B)
      : Source(S), Bindings(B) {
    if (B.SourceID != S.ID)
      throw Error("source_analysis_mismatch");
    Result.SourceID = S.ID;
    Result.BindingID = B.ID;
    Result.ID =
        identity("source-modules", {JavaScriptModuleProfile, S.ID, B.ID});
    Result.Status = "ok";
  }
  SourceModuleAnalysis run() {
    if (Source.ParseStatus != "parsed" ||
        (Bindings.Status != "ok" && Bindings.Status != "partial")) {
      Result.Status = "unavailable";
      Result.DiagnosticCount = 1;
      Result.Diagnostics.push_back({Source.ParseStatus == "parsed"
                                        ? "source_bindings_unavailable"
                                        : "source_not_parsed",
                                    -1});
      return std::move(Result);
    }
    try {
      step(validateSourceModel(Source));
      admitBindings();
      for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
        step();
        const auto &Kind = node(I).Kind;
        if (Kind == "ImportDeclaration")
          importDeclaration(I);
        else if (Kind == "ExportNamedDeclaration" ||
                 Kind == "ExportAllDeclaration" ||
                 Kind == "ExportDefaultDeclaration")
          exportDeclaration(I);
        else if (Kind == "ImportExpression") {
          const auto R = request(I, "dynamic_import", child(I, "source"));
          Result.Requests[R].AttributesNode = child(I, "attributes");
        } else if (Kind == "CallExpression" || Kind == "OptionalCallExpression")
          call(I);
      }
    } catch (const Error &E) {
      Result.Requests.clear();
      Result.Imports.clear();
      Result.Exports.clear();
      Result.Attributes.clear();
      Result.Names.clear();
      Result.Diagnostics = {{E.what(), -1}};
      Result.DiagnosticCount = 1;
      Result.Status = std::string_view(E.what()).find("budget_exceeded") !=
                              std::string_view::npos
                          ? "budget_exceeded"
                          : "unsupported";
    }
    return std::move(Result);
  }
};

// Only the unambiguous literal subset of relative URL paths is compared.
// Reject encoding/query/fragment/authority syntax rather than approximating it.
bool relativePath(std::u16string_view Text, std::string &UTF8) {
  if (!(Text.substr(0, 2) == u"./" || Text.substr(0, 3) == u"../") ||
      Text.size() > MaxJavaScriptModulePathUnits)
    return false;
  for (size_t I = 0; I < Text.size(); ++I) {
    uint32_t C = Text[I];
    if (C <= 0x20 || C == 0x7f || C == '\\' || C == '%' || C == '?' ||
        C == '#' || C == ':')
      return false;
    if (C >= 0xd800 && C <= 0xdbff) {
      if (++I == Text.size() || Text[I] < 0xdc00 || Text[I] > 0xdfff)
        return false;
      C = 0x10000 + ((C - 0xd800) << 10) + Text[I] - 0xdc00;
    } else if (C >= 0xdc00 && C <= 0xdfff) {
      return false;
    }
    if (C < 0x80) {
      UTF8 += char(C);
    } else if (C < 0x800) {
      UTF8 += char(0xc0 | (C >> 6));
      UTF8 += char(0x80 | (C & 63));
    } else if (C < 0x10000) {
      UTF8 += char(0xe0 | (C >> 12));
      UTF8 += char(0x80 | ((C >> 6) & 63));
      UTF8 += char(0x80 | (C & 63));
    } else {
      UTF8 += char(0xf0 | (C >> 18));
      UTF8 += char(0x80 | ((C >> 12) & 63));
      UTF8 += char(0x80 | ((C >> 6) & 63));
      UTF8 += char(0x80 | (C & 63));
    }
  }
  return true;
}

bool normalize(std::string_view Path, std::string &Result) {
  std::vector<std::string_view> Parts;
  while (true) {
    const auto Slash = Path.find('/');
    const auto Part = Path.substr(0, Slash);
    if (Part.empty())
      return false; // No URL authority, double slash or trailing directory
                    // slash.
    if (Part == "..") {
      if (Parts.empty())
        return false;
      Parts.pop_back();
    } else if (Part != ".") {
      Parts.push_back(Part);
    }
    if (Slash == std::string_view::npos)
      break;
    Path.remove_prefix(Slash + 1);
  }
  for (const auto Part : Parts) {
    if (!Result.empty())
      Result += '/';
    Result += Part;
  }
  return !Result.empty();
}
} // namespace

SourceModuleAnalysis
analyzeSourceModules(const SourceAnalysis &Source,
                     const SourceBindingAnalysis &Bindings) {
  return Inventory(Source, Bindings).run();
}

SourceModuleLinks linkSourceModules(const SourceAnalysis &Source,
                                    const SourceModuleAnalysis &Modules,
                                    const Snapshot &Input) {
  if (Source.ID != Modules.SourceID)
    throw Error("source_analysis_mismatch");
  SourceModuleLinks Result;
  Result.ID = identity("source-module-links",
                       {JavaScriptModuleLinkProfile, Modules.ID, Input.ID});
  Result.Status = "ok";
  if (Modules.Status != "ok" && Modules.Status != "partial") {
    Result.Status = "unavailable";
    return Result;
  }
  auto Step = [&](uint64_t Count = 1) {
    if (Count > MaxJavaScriptModuleSteps - Result.Steps)
      throw Error("source_module_link_budget_exceeded");
    Result.Steps += Count;
  };
  try {
    const Artifact *Origin = nullptr;
    bool DirectoryRoot = false;
    std::map<std::string_view, const Artifact *> Members;
    for (const auto &A : Input.Artifacts) {
      Step(A.MemberPath.size() + 1);
      if (A.MemberPath.empty())
        DirectoryRoot = A.Directory;
      if (A.ID == Source.ArtifactID)
        Origin = &A;
      if (!Members.emplace(A.MemberPath, &A).second)
        throw Error("invalid_artifact_snapshot");
    }
    for (const auto &R : Modules.Requests) {
      Step();
      SourceModuleLink L;
      if (R.Kind == "dynamic_import")
        L.Status = "dynamic_import_boundary";
      else if (R.Kind != "esm_import" && R.Kind != "esm_reexport")
        L.Status = "unverified_callee";
      else if (R.Specifier == None)
        L.Status = "nonliteral_specifier";
      else if (!Origin || !DirectoryRoot || Origin->Directory ||
               Origin->MemberPath.empty())
        L.Status = "directory_origin_unavailable";
      else {
        if (R.Specifier >= Modules.Names.size())
          throw Error("invalid_source_module_model");
        const auto &Name = Modules.Names[R.Specifier];
        Step(Name.size() + Origin->MemberPath.size() + 1);
        std::string Relative, Path;
        if (!relativePath(Name, Relative)) {
          L.Status = "unsupported_specifier";
        } else {
          const auto Slash = Origin->MemberPath.rfind('/');
          const auto Base = Slash == std::string::npos
                                ? std::string()
                                : Origin->MemberPath.substr(0, Slash + 1);
          if (!normalize(Base + Relative, Path)) {
            L.Status = "outside_or_unsupported_path";
          } else if (const auto It = Members.find(Path); It == Members.end()) {
            L.Status = "not_in_snapshot";
          } else if (It->second->Directory) {
            L.Status = "directory_target";
          } else {
            L.Status = "exact_admitted_file_candidate";
            L.ArtifactID = It->second->ID;
          }
        }
      }
      Result.Requests.push_back(std::move(L));
    }
  } catch (const Error &E) {
    if (std::string_view(E.what()) != "source_module_link_budget_exceeded")
      throw;
    Result.Status = "budget_exceeded";
    Result.Requests.clear();
  }
  return Result;
}
} // namespace neverd::web
