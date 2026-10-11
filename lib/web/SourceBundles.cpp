//===- SourceBundles.cpp - Qualified bundle source partitions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qualified bundle source partitions.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/SourceBundles.h"

#include "SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;

// The fixed runtime shape follows webpack's MIT-licensed renderRequire:
// 6c9f912af2dfbb3e0e1a2a3ecdc3881c96363432. Names are alpha-matched by lexical
// identity, never by regex. See LICENSES/webpack/LICENSE. This trusted profile
// text is parsed in C++, and neither it nor any target text is executed.
constexpr std::string_view RuntimeShape = R"JS(
var TABLE = [];
var CACHE = {};
function LOAD(ID) {
  var CACHED = CACHE[ID];
  if (CACHED !== undefined) { return CACHED.exports; }
  var MODULE = CACHE[ID] = { exports: {} };
  TABLE[ID](MODULE, MODULE.exports, LOAD);
  return MODULE.exports;
}
)JS";

uint32_t field(const SourceAnalysis &S, uint32_t I, std::string_view Name) {
  if (I == None)
    return None;
  for (const auto &C : S.Nodes[I].Children)
    if (C.Field == Name)
      return C.Index;
  return None;
}
struct BindingIndex {
  std::vector<uint32_t> Declarations, References;
  std::vector<bool> Written;
  explicit BindingIndex(const SourceAnalysis &S, const SourceBindingAnalysis &B)
      : Declarations(S.Nodes.size(), None), References(S.Nodes.size(), None),
        Written(B.Bindings.size(), false) {
    for (const auto &D : B.Declarations) {
      if (D.Binding >= B.Bindings.size() ||
          (D.Node != None && D.Node >= S.Nodes.size()))
        throw Error("invalid_source_binding_model");
      if (D.Node != None && Declarations[D.Node] == None)
        Declarations[D.Node] = D.Binding;
    }
    for (uint32_t I = 0; I < B.References.size(); ++I) {
      const auto &R = B.References[I];
      if (R.Node >= S.Nodes.size() || References[R.Node] != None ||
          (R.Binding != None && R.Binding >= B.Bindings.size()))
        throw Error("invalid_source_binding_model");
      References[R.Node] = I;
      if (R.Binding != None &&
          (R.Access == "write" || R.Access == "read_write"))
        Written[R.Binding] = true;
    }
  }
  uint32_t binding(uint32_t I, const SourceBindingAnalysis &B) const {
    if (I == None)
      return None;
    if (Declarations[I] != None)
      return Declarations[I];
    const auto Ref = References[I];
    return Ref == None || B.References[Ref].Resolution != "lexical_binding"
               ? None
               : B.References[Ref].Binding;
  }
};

struct Profile {
  SourceAnalysis Source =
      inspectJavaScript("webpack-runtime-profile", RuntimeShape, "script");
  SourceBindingAnalysis Bindings = analyzeSourceBindings(Source);
  BindingIndex Index{Source, Bindings};
  std::vector<uint32_t> Statements;
  uint32_t Wildcard = None;
  Profile() {
    if (Source.ParseStatus != "parsed" || Bindings.Status != "ok")
      throw Error("bundle_profile_unavailable");
    for (const auto &C : Source.Nodes[0].Children)
      Statements.push_back(C.Index);
    if (Statements.size() != 3)
      throw Error("bundle_profile_unavailable");
    Wildcard =
        field(Source, field(Source, Statements[0], "declarations"), "init");
  }
};
const Profile &profile() {
  static const Profile P;
  return P;
}

class Recovery {
  const SourceAnalysis &Source;
  const SourceBindingAnalysis &Bindings;
  std::string_view Bytes;
  SourceBundleAnalysis Result;
  BindingIndex Index;
  std::vector<uint32_t> Parent, NodeBundle, NodeModule;
  std::map<std::u16string, uint32_t> Keys;
  std::map<uint32_t, uint32_t> Symbols, ReverseSymbols;
  std::vector<std::map<uint32_t, uint32_t>> Members;
  uint64_t KeyUnits = 0;

  void step(uint64_t Count = 1) {
    if (Count > MaxJavaScriptBundleSteps - Result.Steps)
      throw Error("source_bundle_budget_exceeded");
    Result.Steps += Count;
  }
  const SyntaxNode &node(uint32_t I) const { return Source.Nodes[I]; }
  bool is(uint32_t I, std::string_view Kind) const {
    return I != None && node(I).Kind == Kind;
  }
  uint32_t child(uint32_t I, std::string_view Name) {
    if (I == None)
      return None;
    for (const auto &C : node(I).Children) {
      step();
      if (C.Field == Name)
        return C.Index;
    }
    return None;
  }
  std::vector<uint32_t> list(uint32_t I, std::string_view Name) {
    std::vector<uint32_t> R;
    for (const auto &C : node(I).Children) {
      step();
      if (C.Field == Name)
        R.push_back(C.Index);
    }
    return R;
  }
  uint32_t key(std::u16string_view Text) {
    step(Text.size() + 1);
    if (const auto It = Keys.find(std::u16string(Text)); It != Keys.end())
      return It->second;
    if (Text.size() > MaxJavaScriptBundleNameUnits - KeyUnits)
      throw Error("source_bundle_name_budget_exceeded");
    const auto I = uint32_t(Result.Keys.size());
    Keys.emplace(Text, I);
    Result.Keys.emplace_back(Text);
    KeyUnits += Text.size();
    return I;
  }
  uint32_t numericKey(uint32_t Value) {
    const auto Text = std::to_string(Value);
    return key(std::u16string(Text.begin(), Text.end()));
  }
  uint32_t literalKey(uint32_t I, bool AllowIdentifier = false) {
    if (is(I, "StringLiteral") || (AllowIdentifier && is(I, "Identifier"))) {
      const auto *Text = node(I).text(
          AllowIdentifier && is(I, "Identifier") ? "name" : "value");
      if (!Text)
        throw Error("invalid_source_model");
      return key(*Text);
    }
    if (is(I, "NumericLiteral")) {
      const auto *A = node(I).attribute("value");
      const auto *V = A ? std::get_if<double>(&A->Value) : nullptr;
      if (V && std::isfinite(*V) && *V >= 0 && *V <= UINT32_MAX &&
          std::floor(*V) == *V)
        return numericKey(uint32_t(*V));
    }
    return None;
  }
  void diagnostic(const char *Code, uint32_t I) {
    ++Result.DiagnosticCount;
    if (Result.Diagnostics.size() < 32)
      Result.Diagnostics.push_back(
          {Code, I == None ? -1 : int64_t(node(I).Start)});
  }
  std::string id(std::string_view Kind, uint32_t I, size_t Ordinal) const {
    return identity("source-bundle-record",
                    {Result.ID, Kind, node(I).ID, std::to_string(Ordinal)});
  }
  std::string digest(uint32_t I) {
    const auto &N = node(I);
    if (N.End > Bytes.size())
      throw Error("invalid_source_model");
    step(N.End - N.Start);
    return sha256(Bytes.substr(N.Start, N.End - N.Start));
  }
  bool function(uint32_t I) {
    return (is(I, "FunctionExpression") || is(I, "ArrowFunctionExpression")) &&
           !node(I).flag("async") && !node(I).flag("generator") &&
           child(I, "id") == None && is(child(I, "body"), "BlockStatement");
  }
  bool matchIdentifier(uint32_t P, uint32_t T) {
    const auto &Shape = profile();
    const auto PB = Shape.Index.binding(P, Shape.Bindings);
    const auto TB = Index.binding(T, Bindings);
    if (PB == None) {
      if (TB != None)
        return false;
      const auto PR = Shape.Index.References[P], TR = Index.References[T];
      if ((PR == None) != (TR == None) ||
          (TR != None && Bindings.References[TR].Resolution != "external"))
        return false;
      return Shape.Source.Nodes[P].text("name") && node(T).text("name") &&
             *Shape.Source.Nodes[P].text("name") == *node(T).text("name");
    }
    if (TB == None || Bindings.Bindings[TB].Conflicting ||
        Shape.Bindings.Bindings[PB].Kind != Bindings.Bindings[TB].Kind)
      return false;
    auto [It, Inserted] = Symbols.emplace(PB, TB);
    auto [Reverse, ReverseInserted] = ReverseSymbols.emplace(TB, PB);
    return It->second == TB && Reverse->second == PB;
  }
  bool match(uint32_t P, uint32_t T) {
    step();
    if (T == None)
      return false;
    const auto &Shape = profile();
    if (P == Shape.Wildcard)
      return is(T, "ArrayExpression") || is(T, "ObjectExpression");
    const auto &A = Shape.Source.Nodes[P], &B = node(T);
    if (A.Kind != B.Kind || A.Children.size() != B.Children.size() ||
        A.Attributes.size() != B.Attributes.size())
      return false;
    if (A.Kind == "Identifier" && !matchIdentifier(P, T))
      return false;
    for (size_t I = 0; I < A.Attributes.size(); ++I) {
      step();
      if (A.Attributes[I].Field != B.Attributes[I].Field ||
          !(A.Kind == "Identifier" && A.Attributes[I].Field == "name") &&
              A.Attributes[I].Value != B.Attributes[I].Value)
        return false;
    }
    for (size_t I = 0; I < A.Children.size(); ++I) {
      const auto &X = A.Children[I], &Y = B.Children[I];
      if (X.Field != Y.Field || X.Ordinal != Y.Ordinal ||
          !match(X.Index, Y.Index))
        return false;
    }
    return true;
  }
  uint32_t declaration(uint32_t Statement) {
    if (!is(Statement, "VariableDeclaration"))
      return None;
    const auto Decls = list(Statement, "declarations");
    return Decls.size() == 1 && is(Decls[0], "VariableDeclarator") ? Decls[0]
                                                                   : None;
  }
  bool strictDirective(uint32_t I) {
    const auto E = is(I, "ExpressionStatement") ? child(I, "expression") : None;
    return is(E, "StringLiteral") && node(E).text("value") &&
           *node(E).text("value") == u"use strict";
  }
  SourceBundleModule module(uint32_t Bundle, uint32_t Value, uint32_t Key) {
    if (Result.Modules.size() >= MaxJavaScriptBundleModules)
      throw Error("source_bundle_module_budget_exceeded");
    SourceBundleModule M;
    M.ID = id("module", Value, Result.Modules.size());
    M.Bundle = Bundle;
    M.Node = Value;
    M.Key = Key;
    if (is(Value, "BooleanLiteral") && !node(Value).flag("value")) {
      M.Kind = "removed_slot";
    } else {
      if (!function(Value))
        throw Error("unsupported_bundle_factory");
      const auto Params = list(Value, "params");
      if (Params.size() > 3)
        throw Error("unsupported_bundle_factory_parameters");
      std::set<uint32_t> Seen;
      for (const auto P : Params) {
        const auto B = Index.binding(P, Bindings);
        if (!is(P, "Identifier") || B == None ||
            Bindings.Bindings[B].Conflicting || !Seen.insert(B).second)
          throw Error("unsupported_bundle_factory_parameters");
      }
      M.Kind = "factory";
      M.Body = child(Value, "body");
      if (Params.size() == 3) {
        M.RequireBinding = Index.binding(Params[2], Bindings);
        M.RequireBindingWritten = Index.Written[M.RequireBinding];
      }
      M.BodyHash = digest(M.Body);
    }
    M.WrapperHash = digest(Value);
    return M;
  }
  bool recover(uint32_t Root) {
    const auto Body = child(Root, "body");
    auto Statements = list(Body, "body");
    if (!Statements.empty() && strictDirective(Statements[0]))
      Statements.erase(Statements.begin());
    if (Statements.size() < 3)
      return false;
    const auto TableDecl = declaration(Statements[0]);
    const auto Table = child(TableDecl, "init");
    if ((!is(Table, "ArrayExpression") && !is(Table, "ObjectExpression")) ||
        declaration(Statements[1]) == None ||
        !is(Statements[2], "FunctionDeclaration"))
      return false;
    Symbols.clear();
    ReverseSymbols.clear();
    for (size_t I = 0; I < 3; ++I)
      if (!match(profile().Statements[I], Statements[I]))
        throw Error("unsupported_bundle_loader_layout");
    if (Result.Bundles.size() >= MaxJavaScriptBundles)
      throw Error("source_bundle_count_budget_exceeded");
    const auto Bundle = uint32_t(Result.Bundles.size());
    SourceBundle B;
    B.ID = id("bundle", Root, Bundle);
    B.Node = Root;
    B.TableNode = Table;
    B.LoaderNode = Statements[2];
    B.TableBinding = Index.binding(child(TableDecl, "id"), Bindings);
    B.LoaderBinding = Index.binding(child(B.LoaderNode, "id"), Bindings);
    B.TableBindingWritten = Index.Written[B.TableBinding];
    B.LoaderBindingWritten = Index.Written[B.LoaderBinding];
    B.TableKind = is(Table, "ArrayExpression") ? "array" : "object";
    std::map<uint32_t, uint32_t> TableMembers;
    const auto Before = Result.Modules.size();
    try {
      for (const auto &C : node(Table).Children) {
        step();
        uint32_t Value, Key;
        if (B.TableKind == "array") {
          if (C.Field != "elements")
            throw Error("unsupported_bundle_table_field");
          if (is(C.Index, "Empty"))
            continue;
          Value = C.Index;
          Key = numericKey(C.Ordinal);
        } else {
          if (C.Field != "properties" || !is(C.Index, "Property") ||
              node(C.Index).flag("computed") || node(C.Index).flag("method") ||
              node(C.Index).flag("shorthand") || !node(C.Index).text("kind") ||
              *node(C.Index).text("kind") != u"init")
            throw Error("unsupported_bundle_table_property");
          Key = literalKey(child(C.Index, "key"), true);
          Value = child(C.Index, "value");
          if (Key == None || Result.Keys[Key] == u"__proto__")
            throw Error("unsupported_bundle_module_key");
        }
        if (!TableMembers.emplace(Key, Result.Modules.size()).second)
          throw Error("duplicate_bundle_module_id");
        Result.Modules.push_back(module(Bundle, Value, Key));
      }
    } catch (...) {
      Result.Modules.resize(Before);
      throw;
    }
    Result.Bundles.push_back(std::move(B));
    Members.push_back(std::move(TableMembers));
    NodeBundle[Root] = Bundle;
    for (size_t I = Before; I < Result.Modules.size(); ++I)
      NodeModule[Result.Modules[I].Node] = I;
    for (size_t I = 3; I < Statements.size(); ++I) {
      SourceBundleRegion R;
      R.ID = id("region", Statements[I], Result.Regions.size());
      R.Bundle = Bundle;
      R.Node = Statements[I];
      Result.Regions.push_back(std::move(R));
    }
    return true;
  }
  void dependencies() {
    for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
      step();
      if (Parent[I] != None) {
        if (NodeBundle[I] == None)
          NodeBundle[I] = NodeBundle[Parent[I]];
        if (NodeModule[I] == None)
          NodeModule[I] = NodeModule[Parent[I]];
      }
      const auto Bundle = NodeBundle[I], Caller = NodeModule[I];
      if (Bundle == None ||
          (!is(I, "CallExpression") && !is(I, "OptionalCallExpression")))
        continue;
      const auto Callee = child(I, "callee");
      if (!is(Callee, "Identifier"))
        continue;
      const auto B = Index.binding(Callee, Bindings);
      const auto &Layout = Result.Bundles[Bundle];
      const auto Param =
          Caller == None ? None : Result.Modules[Caller].RequireBinding;
      bool DynamicLookup = false;
      if (B == None && Index.References[Callee] != None) {
        const auto &Ref = Bindings.References[Index.References[Callee]];
        DynamicLookup =
            (Ref.Resolution == "dynamic_with" ||
             Ref.Resolution == "dynamic_eval") &&
            (Ref.Name == Bindings.Bindings[Layout.LoaderBinding].Name ||
             (Param != None && Ref.Name == Bindings.Bindings[Param].Name));
      }
      if (!DynamicLookup &&
          (B == None || (B != Layout.LoaderBinding && B != Param)))
        continue;
      if (Result.Dependencies.size() >= MaxJavaScriptBundleDependencies)
        throw Error("source_bundle_dependency_budget_exceeded");
      SourceBundleDependency D;
      D.ID = id("dependency", I, Result.Dependencies.size());
      D.Bundle = Bundle;
      D.Node = I;
      D.CallerModule = Caller;
      D.CalleeBinding = B;
      const auto Args = list(I, "arguments");
      const auto K = Args.size() == 1 ? literalKey(Args[0]) : None;
      if (DynamicLookup)
        D.Status = "dynamic_lookup";
      else if (Index.Written[B] || Layout.TableBindingWritten ||
               Layout.LoaderBindingWritten)
        D.Status = "binding_written";
      else if (is(I, "OptionalCallExpression"))
        D.Status = "optional_call_boundary";
      else if (K == None)
        D.Status = "dynamic_or_unsupported_argument";
      else if (const auto It = Members[Bundle].find(K);
               It == Members[Bundle].end())
        D.Status = "target_not_in_table";
      else if (Result.Modules[It->second].Kind != "factory")
        D.Status = "removed_target";
      else {
        D.Status = "table_member_candidate";
        D.TargetModule = It->second;
      }
      Result.Dependencies.push_back(std::move(D));
    }
  }

public:
  Recovery(const SourceAnalysis &S, const SourceBindingAnalysis &B,
           std::string_view Text)
      : Source(S), Bindings(B), Bytes(Text), Index(S, B),
        Parent(S.Nodes.size(), None), NodeBundle(S.Nodes.size(), None),
        NodeModule(S.Nodes.size(), None) {
    if (S.ID != B.SourceID || S.BlobHash != sha256(Text))
      throw Error("source_analysis_mismatch");
    Result.ID =
        identity("source-bundles", {JavaScriptBundleProfile, S.ID, B.ID});
    Result.SourceID = S.ID;
    Result.BindingID = B.ID;
    Result.BindingStatus = B.Status;
    Result.Status = "not_detected";
  }
  SourceBundleAnalysis run() {
    if (Source.ParseStatus != "parsed" ||
        (Bindings.Status != "ok" && Bindings.Status != "partial")) {
      Result.Status = "unavailable";
      diagnostic("source_or_bindings_unavailable", None);
      return std::move(Result);
    }
    try {
      step(validateSourceModel(Source));
      step(Bindings.Declarations.size() + Bindings.References.size());
      for (uint32_t I = 0; I < Source.Nodes.size(); ++I)
        for (const auto &C : node(I).Children) {
          step();
          Parent[C.Index] = I;
        }
      for (const auto &C : node(0).Children) {
        step();
        const auto Call = is(C.Index, "ExpressionStatement")
                              ? child(C.Index, "expression")
                              : None;
        if (!is(Call, "CallExpression") || !list(Call, "arguments").empty())
          continue;
        const auto Root = child(Call, "callee");
        if (!function(Root) || !list(Root, "params").empty())
          continue;
        try {
          recover(Root);
        } catch (const Error &E) {
          if (std::string_view(E.what()).find("budget_exceeded") !=
              std::string_view::npos)
            throw;
          diagnostic(E.what(), Root);
        }
      }
      dependencies();
      if (!Result.Bundles.empty() && Bindings.Status != "ok")
        diagnostic("partial_source_bindings", 0);
      Result.Status =
          Result.Bundles.empty()
              ? Result.DiagnosticCount ? "unsupported" : "not_detected"
          : Result.DiagnosticCount ? "partial"
                                   : "ok";
    } catch (const Error &E) {
      Result.Bundles.clear();
      Result.Modules.clear();
      Result.Dependencies.clear();
      Result.Regions.clear();
      Result.Keys.clear();
      Result.Diagnostics.clear();
      Result.DiagnosticCount = 0;
      diagnostic(E.what(), None);
      Result.Status = std::string_view(E.what()).find("budget_exceeded") !=
                              std::string_view::npos
                          ? "budget_exceeded"
                          : "unsupported";
    }
    return std::move(Result);
  }
};
} // namespace

SourceBundleAnalysis analyzeSourceBundles(const SourceAnalysis &Source,
                                          const SourceBindingAnalysis &Bindings,
                                          std::string_view SourceBytes) {
  return Recovery(Source, Bindings, SourceBytes).run();
}
} // namespace neverd::web
