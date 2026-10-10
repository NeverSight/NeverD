#include "neverd/web/SourceOrigins.h"

#include "SourceModel.h"

#include "neverd/web/Session.h"

#include <optional>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;
using Origin = std::shared_ptr<const SourceOrigin>;
struct Resolver {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceModuleAnalysis &M;
  SourceOrigins A;
  struct Initializer {
    uint32_t Node = None;
    std::vector<std::u16string> Path;
    Origin Imported;
  };
  std::vector<Initializer> Initializers;
  std::vector<uint32_t> References, Declarations, Requests;
  std::vector<bool> Written, Ambiguous;
  std::vector<uint8_t> Visiting;
  bool UnboundRequireWrite = false;

  void step(uint64_t Count = 1) {
    if (Count > MaxSourceOriginSteps - A.Steps)
      throw Error("source_origin_budget_exceeded");
    A.Steps += Count;
  }
  void units(uint64_t Count) {
    if (Count > MaxSourceOriginUnits - A.AllocatedUnits)
      throw Error("source_origin_budget_exceeded");
    A.AllocatedUnits += Count;
  }
  uint32_t child(uint32_t I, std::string_view Field) {
    if (I == None)
      return None;
    for (const auto &C : S.Nodes.at(I).Children) {
      step();
      if (C.Field == Field)
        return C.Index;
    }
    return None;
  }
  bool is(uint32_t I, std::string_view Kind) const {
    return I != None && S.Nodes.at(I).Kind == Kind;
  }
  std::optional<std::u16string> property(uint32_t I, bool Computed) {
    if (I == None)
      return {};
    const auto &N = S.Nodes.at(I);
    const auto *Text =
        N.text(!Computed && N.Kind == "Identifier" ? "name" : "value");
    if (!Text || (Computed && N.Kind != "StringLiteral") ||
        (!Computed && N.Kind != "Identifier" && N.Kind != "StringLiteral"))
      return {};
    units(Text->size());
    return *Text;
  }
  Origin append(Origin Base, const std::u16string &Name) {
    if (!Base)
      return {};
    if (Base->Members.size() >= 32)
      throw Error("source_origin_budget_exceeded");
    uint64_t Count = Name.size();
    for (const auto &P : Base->Members) {
      step();
      Count += P.size();
    }
    units(Count);
    auto Result = std::make_shared<SourceOrigin>(*Base);
    Result->Members.push_back(Name);
    return Result;
  }
  Origin root(uint32_t Request, std::string Evidence) {
    if (Request >= M.Requests.size())
      throw Error("invalid_source_module_model");
    const auto &R = M.Requests[Request];
    if (R.SpecifierStatus != "literal")
      return {};
    if (R.Specifier >= M.Names.size())
      throw Error("invalid_source_module_model");
    auto O = std::make_shared<SourceOrigin>();
    O->Request = Request;
    O->Evidence = std::move(Evidence);
    return O;
  }
  void pattern(uint32_t I, uint32_t Value,
               const std::vector<std::u16string> &Path, unsigned Depth = 0) {
    step();
    if (Depth > 32)
      throw Error("source_origin_budget_exceeded");
    if (I == None)
      return;
    if (is(I, "Identifier")) {
      const auto Binding = Declarations.at(I);
      if (Binding == None || Ambiguous[Binding] || Written[Binding])
        return;
      auto &Init = Initializers[Binding];
      if (Init.Node != None || Init.Imported) {
        Ambiguous[Binding] = true;
        return;
      }
      Init.Node = Value;
      for (const auto &Name : Path)
        units(Name.size());
      Init.Path = Path;
    } else if (is(I, "ObjectPattern")) {
      for (const auto &C : S.Nodes[I].Children) {
        step();
        if (C.Field != "properties" || !is(C.Index, "Property"))
          continue;
        auto Name =
            property(child(C.Index, "key"), S.Nodes[C.Index].flag("computed"));
        if (!Name)
          continue;
        auto Next = Path;
        for (const auto &Part : Path)
          units(Part.size());
        Next.push_back(std::move(*Name));
        pattern(child(C.Index, "value"), Value, Next, Depth + 1);
      }
    }
    // Defaults, rest and positional destructuring need distinct semantics.
  }
  Origin node(uint32_t I, unsigned Depth = 0) {
    step();
    if (Depth > 64)
      throw Error("source_origin_budget_exceeded");
    if (I == None)
      return {};
    if (Visiting.at(I) == 2)
      return A.Nodes[I];
    if (Visiting[I] == 1)
      return {};
    Visiting[I] = 1;
    Origin O;
    const auto &N = S.Nodes[I];
    if (N.Kind == "Identifier" && References[I] != None) {
      const auto &R = B.References[References[I]];
      if (R.Resolution == "lexical_binding" && R.Binding != None &&
          !Written[R.Binding] && !Ambiguous[R.Binding] &&
          !B.Bindings[R.Binding].Conflicting) {
        const auto &Init = Initializers[R.Binding];
        O = Init.Imported ? Init.Imported : node(Init.Node, Depth + 1);
        for (const auto &Name : Init.Path)
          O = append(O, Name);
      }
    } else if (N.Kind == "MemberExpression" ||
               N.Kind == "OptionalMemberExpression") {
      auto Name = property(child(I, "property"), N.flag("computed"));
      if (Name)
        O = append(node(child(I, "object"), Depth + 1), *Name);
    } else if (N.Kind == "CallExpression" && Requests[I] != None) {
      const auto R = Requests[I];
      const auto &Request = M.Requests[R];
      if (Request.Kind == "require_call_candidate" && !Request.Optional &&
          !UnboundRequireWrite && Request.ArgumentCount == 1 &&
          (Request.CalleeEvidence == "external" ||
           Request.CalleeEvidence == "caller_selected_commonjs_parameter") &&
          (Request.CalleeBinding == None || !Written.at(Request.CalleeBinding)))
        O = root(R, Request.CalleeEvidence);
    } else if (N.Kind == "NewExpression") {
      if (auto C = node(child(I, "callee"), Depth + 1);
          C && C->Construction == None) {
        for (const auto &Name : C->Members)
          units(Name.size());
        auto Constructed = std::make_shared<SourceOrigin>(*C);
        Constructed->Construction = I;
        O = std::move(Constructed);
      }
    }
    Visiting[I] = 2;
    A.Nodes[I] = O;
    return O;
  }
  void run() {
    step(validateSourceModel(S));
    if (B.SourceID != S.ID || M.SourceID != S.ID || M.BindingID != B.ID ||
        B.References.size() > MaxJavaScriptNodes ||
        B.Declarations.size() > 2 * MaxJavaScriptNodes ||
        B.Bindings.size() > 2 * MaxJavaScriptNodes ||
        M.Requests.size() > MaxJavaScriptNodes ||
        M.Imports.size() > MaxJavaScriptNodes)
      throw Error("invalid_source_origin_inputs");
    if ((B.Status != "ok" && B.Status != "partial") ||
        (M.Status != "ok" && M.Status != "partial"))
      throw Error("source_origin_inputs_unavailable");
    for (const auto &Scope : B.Scopes) {
      step();
      if (Scope.PossibleDirectEval)
        throw Error("source_origin_dynamic_eval");
    }
    const auto Size = S.Nodes.size();
    References.assign(Size, None);
    Declarations.assign(Size, None);
    Requests.assign(Size, None);
    Visiting.resize(Size);
    A.Nodes.resize(Size);
    Initializers.resize(B.Bindings.size());
    Written.resize(B.Bindings.size());
    Ambiguous.resize(B.Bindings.size());
    for (uint32_t I = 0; I < B.References.size(); ++I) {
      step();
      const auto &R = B.References[I];
      if (R.Node >= Size ||
          (R.Binding != None && R.Binding >= B.Bindings.size()) ||
          References[R.Node] != None)
        throw Error("invalid_source_binding_model");
      References[R.Node] = I;
      const auto *Name = S.Nodes[R.Node].text("name");
      if (R.Binding == None && Name && *Name == u"require" &&
          (R.Access == "write" || R.Access == "read_write"))
        UnboundRequireWrite = true;
      if (R.Binding != None &&
          (R.Access == "write" || R.Access == "read_write"))
        Written[R.Binding] = true;
    }
    for (const auto &D : B.Declarations) {
      step();
      if (D.Binding >= B.Bindings.size())
        throw Error("invalid_source_binding_model");
      if (D.Node == None)
        continue;
      if (D.Node >= Size)
        throw Error("invalid_source_binding_model");
      if (Declarations[D.Node] != None && Declarations[D.Node] != D.Binding)
        throw Error("invalid_source_binding_model");
      Declarations[D.Node] = D.Binding;
    }
    for (uint32_t I = 0; I < M.Requests.size(); ++I) {
      step();
      const auto &R = M.Requests[I];
      if (R.Node >= Size ||
          (R.CalleeBinding != None && R.CalleeBinding >= B.Bindings.size()))
        throw Error("invalid_source_module_model");
      if (Requests[R.Node] != None)
        throw Error("invalid_source_module_model");
      Requests[R.Node] = I;
    }
    for (const auto &Import : M.Imports) {
      step();
      if (Import.Binding == None)
        continue;
      if (Import.Binding >= B.Bindings.size())
        throw Error("invalid_source_module_model");
      auto O = root(Import.Request, "esm_import_syntax");
      if (Import.Kind != "namespace") {
        if (Import.ImportedName >= M.Names.size())
          throw Error("invalid_source_module_model");
        O = append(O, M.Names[Import.ImportedName]);
      }
      Initializers[Import.Binding].Imported = O;
    }
    for (uint32_t I = 0; I < Size; ++I) {
      step();
      if (is(I, "VariableDeclarator"))
        pattern(child(I, "id"), child(I, "init"), {});
    }
    for (uint32_t I = 0; I < Size; ++I)
      node(I);
    A.Status = "partial";
    A.Reason = "syntactic_candidates_only";
  }
};
} // namespace

SourceOrigins analyzeSourceOrigins(const SourceAnalysis &S,
                                   const SourceBindingAnalysis &B,
                                   const SourceModuleAnalysis &M) {
  Resolver R{S, B, M};
  R.A.SourceID = S.ID;
  R.A.BindingID = B.ID;
  R.A.ModuleID = M.ID;
  R.A.ID =
      identity("source-origins", {S.ID, B.ID, M.ID, JavaScriptOriginProfile});
  try {
    R.run();
  } catch (const Error &E) {
    R.A.Nodes.clear();
    R.A.Reason = E.what();
    R.A.Status = R.A.Reason == "source_origin_budget_exceeded"
                     ? "budget_exceeded"
                     : "unavailable";
  }
  return std::move(R.A);
}
} // namespace neverd::web
