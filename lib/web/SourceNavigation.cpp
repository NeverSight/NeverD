#include "neverd/web/SourceNavigation.h"

#include "SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"

namespace neverd::web {
namespace {
bool function(const SyntaxNode &N) {
  return N.Kind == "FunctionDeclaration" || N.Kind == "FunctionExpression" ||
         N.Kind == "ArrowFunctionExpression";
}
uint32_t child(const SyntaxNode &N, std::string_view Field) {
  for (const auto &C : N.Children)
    if (C.Field == Field)
      return C.Index;
  return NoSourceIndex;
}
} // namespace

SourceNavigation analyzeSourceNavigation(const SourceAnalysis &S,
                                         const SourceBindingAnalysis &B) {
  SourceNavigation A;
  A.SourceID = S.ID;
  A.BindingID = B.ID;
  A.BindingStatus = B.Status;
  A.ID = identity("source-navigation",
                  {S.ID, JavaScriptNavigationProfile, B.ID, B.Status});
  try {
    auto Charge = [&](uint64_t Steps = 1) {
      if (Steps > MaxJavaScriptNavigationSteps - A.Steps)
        throw Error("source_navigation_budget_exceeded");
      A.Steps += Steps;
    };
    Charge(validateSourceModel(S));
    if (B.SourceID != S.ID)
      throw Error("source_navigation_binding_mismatch");
    const bool Available = B.Status == "ok" || B.Status == "partial";
    const auto Size = S.Nodes.size();
    std::vector<uint32_t> Parents(Size, NoSourceIndex),
        Enclosing(Size, NoSourceIndex), Functions(Size, NoSourceIndex),
        DeclBindings(Size, NoSourceIndex), Refs(Size, NoSourceIndex);
    std::vector<bool> Ambiguous(Size);
    if (Available) {
      if (B.Declarations.size() > 2 * MaxJavaScriptNodes ||
          B.References.size() > MaxJavaScriptNodes)
        throw Error("source_navigation_budget_exceeded");
      for (const auto &D : B.Declarations) {
        Charge();
        if (D.Binding >= B.Bindings.size())
          throw Error("invalid_source_binding_model");
        if (D.Node == NoSourceIndex)
          continue;
        if (D.Node >= Size)
          throw Error("invalid_source_binding_model");
        if (B.Bindings[D.Binding].Conflicting)
          continue;
        if (DeclBindings[D.Node] != NoSourceIndex &&
            DeclBindings[D.Node] != D.Binding)
          Ambiguous[D.Node] = true;
        DeclBindings[D.Node] = D.Binding;
      }
      for (uint32_t I = 0; I < B.References.size(); ++I) {
        Charge();
        const auto &R = B.References[I];
        if (R.Node >= Size ||
            (R.Binding != NoSourceIndex && R.Binding >= B.Bindings.size()) ||
            Refs[R.Node] != NoSourceIndex)
          throw Error("invalid_source_binding_model");
        Refs[R.Node] = I;
      }
    }
    auto Binding = [&](uint32_t Node) {
      return Node == NoSourceIndex || Ambiguous[Node] ? NoSourceIndex
                                                      : DeclBindings[Node];
    };
    for (uint32_t I = 0; I < Size; ++I) {
      const auto &N = S.Nodes[I];
      Charge(1 + 4 * N.Children.size());
      auto Owner = Enclosing[I];
      if (function(N)) {
        SourceFunction F;
        F.ID = identity("source-function", {A.ID, N.ID});
        F.Node = I;
        F.ParentFunction = Owner;
        F.Name = child(N, "id");
        F.Body = child(N, "body");
        if (F.Body == NoSourceIndex)
          throw Error("invalid_source_function_model");
        F.NameBinding = Binding(F.Name);
        for (const auto &C : N.Children)
          F.Parameters += C.Field == "params";
        if (Parents[I] != NoSourceIndex) {
          const auto &P = S.Nodes[Parents[I]];
          if (P.Kind == "VariableDeclarator" && child(P, "init") == I) {
            const auto Name = child(P, "id");
            if (Name != NoSourceIndex && S.Nodes[Name].Kind == "Identifier")
              F.InitializerBinding = Binding(Name);
          }
        }
        Owner = Functions[I] = A.Functions.size();
        A.Functions.push_back(std::move(F));
      }
      for (const auto &C : N.Children) {
        Parents[C.Index] = I;
        Enclosing[C.Index] = Owner;
      }
      std::string Kind;
      if (N.Kind == "CallExpression")
        Kind = "call";
      else if (N.Kind == "OptionalCallExpression")
        Kind = "optional_call";
      else if (N.Kind == "NewExpression")
        Kind = "construct";
      else if (N.Kind == "TaggedTemplateExpression")
        Kind = "tagged_template";
      else if (N.Kind == "ImportExpression")
        Kind = "dynamic_import";
      if (!Kind.empty()) {
        SourceCall C;
        C.ID = identity("source-call", {A.ID, N.ID});
        C.Node = I;
        C.EnclosingFunction = Enclosing[I];
        C.Kind = Kind;
        if (Kind == "dynamic_import") {
          C.Argument = child(N, "source");
          if (C.Argument == NoSourceIndex)
            throw Error("invalid_source_call_model");
        } else {
          C.Callee = child(N, Kind == "tagged_template" ? "tag" : "callee");
          if (C.Callee == NoSourceIndex)
            throw Error("invalid_source_call_model");
          C.Reference = Refs[C.Callee];
        }
        A.Calls.push_back(std::move(C));
      }
    }
    for (auto &C : A.Calls) {
      Charge();
      if (C.Callee != NoSourceIndex)
        C.SyntacticFunction = Functions[C.Callee];
    }
    if (Available)
      for (uint32_t I = 0; I < B.References.size(); ++I) {
        Charge();
        A.References.push_back({I, Enclosing[B.References[I].Node]});
      }
    A.Status = B.Status == "ok" ? "ok" : "partial";
  } catch (const Error &E) {
    A.Functions.clear();
    A.Calls.clear();
    A.References.clear();
    A.Status = std::string_view(E.what()) == "source_navigation_budget_exceeded"
                   ? "budget_exceeded"
                   : "unavailable";
    A.Diagnostics.push_back({E.what(), -1});
  }
  return A;
}
} // namespace neverd::web
