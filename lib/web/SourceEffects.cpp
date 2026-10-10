#include "neverd/web/SourceEffects.h"

#include "SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"

#include <optional>

namespace neverd::web {
namespace {
constexpr uint32_t None = NoSourceIndex;
constexpr uint32_t Invocation = Call | MayThrow | MayDiverge | UnknownEffect;

class Effects {
  const SourceAnalysis &Source;
  const SourceBindingAnalysis &Bindings;
  const SourceValueAnalysis &Values;
  SourceEffectAnalysis Result;
  std::vector<uint32_t> References;
  std::vector<uint32_t> DeclarationEffects;

  void step(uint64_t Count = 1) {
    if (Count > MaxJavaScriptEffectSteps - Result.Steps)
      throw Error("source_effect_budget_exceeded");
    Result.Steps += Count;
  }
  uint32_t child(uint32_t I, std::string_view Field) const {
    for (const auto &C : Source.Nodes[I].Children)
      if (C.Field == Field)
        return C.Index;
    return None;
  }
  std::u16string_view op(uint32_t I) const {
    const auto *Text = Source.Nodes[I].text("operator");
    return Text ? std::u16string_view(*Text) : std::u16string_view{};
  }
  const SourcePrimitive *primitive(uint32_t I) const {
    return I != None && Values.Nodes[I].Status == "constant"
               ? Values.Nodes[I].Value.get()
               : nullptr;
  }
  std::optional<bool> truth(uint32_t I) const {
    const auto *V = primitive(I);
    if (!V)
      return std::nullopt;
    return sourcePrimitiveTruthy(*V);
  }
  void merge(SourceNodeEffects &Out, uint32_t I, bool Deferred = false) {
    if (I == None)
      return;
    step();
    const auto &E = Result.Nodes[I];
    if (Deferred)
      Out.Deferred |= E.Immediate | E.Deferred;
    else {
      Out.Immediate |= E.Immediate;
      Out.Deferred |= E.Deferred;
    }
  }
  void mergeField(SourceNodeEffects &Out, uint32_t I, std::string_view Field,
                  bool Deferred = false) {
    for (const auto &C : Source.Nodes[I].Children)
      if (C.Field == Field)
        merge(Out, C.Index, Deferred);
  }
  void all(SourceNodeEffects &Out, uint32_t I) {
    for (const auto &C : Source.Nodes[I].Children)
      merge(Out, C.Index);
  }
  uint32_t reference(uint32_t I) const {
    if (References[I] == None)
      return 0;
    const auto &R = Bindings.References[References[I]];
    if (R.Access == "export")
      return 0; // module linkage, not GetValue
    uint32_t E = MayThrow;
    if (R.Access == "read" || R.Access == "read_write" || R.Access == "typeof")
      E |= ReadBinding;
    if (R.Access == "write" || R.Access == "read_write" || R.Access == "delete")
      E |= WriteBinding;
    bool ObjectEnvironment = R.Resolution != "lexical_binding";
    if (R.Binding != None) {
      const auto &B = Bindings.Bindings[R.Binding];
      ObjectEnvironment |= Bindings.Scopes[B.Scope].Kind == "script" &&
                           (B.Kind == "var" || B.Kind == "function_var");
    }
    if (ObjectEnvironment) {
      // Global object environments and with can call getters/proxy traps.
      E |= Invocation;
      if (E & ReadBinding)
        E |= ReadProperty;
      if (E & WriteBinding)
        E |= WriteProperty;
    }
    return E;
  }
  void coercion(SourceNodeEffects &Out, uint32_t I) {
    const auto &V = Values.Nodes[I];
    if (V.Status == "would_throw")
      Out.Immediate |= MayThrow;
    else if (V.Status != "constant")
      Out.Immediate |= Invocation;
  }
  SourceNodeEffects analyze(uint32_t I) {
    step();
    const auto &N = Source.Nodes[I];
    const auto &K = N.Kind;
    SourceNodeEffects Out;
    for (const auto &C : N.Children) {
      step();
      Out.ContainsDeclaration |= Result.Nodes[C.Index].ContainsDeclaration;
    }
    if (K == "Identifier") {
      Out.Immediate = reference(I) | DeclarationEffects[I];
      return Out;
    }
    if (K == "FunctionDeclaration" || K == "FunctionExpression" ||
        K == "ArrowFunctionExpression") {
      Out.Immediate = Allocate;
      Out.ContainsDeclaration |= K == "FunctionDeclaration";
      if (K == "FunctionDeclaration")
        Out.Immediate |= WriteBinding;
      mergeField(Out, I, "params", true);
      mergeField(Out, I, "body", true);
      return Out;
    }
    if (K == "ClassProperty" || K == "ClassPrivateProperty") {
      if (N.flag("computed")) {
        mergeField(Out, I, "key");
        if (!primitive(child(I, "key")))
          Out.Immediate |= Invocation;
      }
      // Defining own properties on the new instance avoids inherited setters,
      // but a derived constructor can return a Proxy or non-extensible object.
      const bool Deferred = !N.flag("static");
      mergeField(Out, I, "value", Deferred);
      if (Deferred)
        Out.Deferred |= WriteProperty | Invocation;
      else
        Out.Immediate |= WriteProperty | MayThrow;
      return Out;
    }
    if (K == "MethodDefinition" || K == "Property") {
      if (N.flag("computed")) {
        mergeField(Out, I, "key");
        if (!primitive(child(I, "key")))
          Out.Immediate |= Invocation;
      }
      mergeField(Out, I, "value");
      return Out;
    }
    if (K == "ClassDeclaration" || K == "ClassExpression") {
      Out.ContainsDeclaration |= K == "ClassDeclaration";
      Out.Immediate = Allocate;
      if (K == "ClassDeclaration")
        Out.Immediate |= WriteBinding;
      mergeField(Out, I, "superClass");
      if (child(I, "superClass") != None)
        Out.Immediate |= ReadProperty | Invocation;
      mergeField(Out, I, "body");
      return Out;
    }
    if (K == "LogicalExpression") {
      const auto Left = child(I, "left");
      merge(Out, Left);
      const auto *V = primitive(Left);
      bool EvaluateRight = true;
      if (V) {
        if (op(I) == u"&&")
          EvaluateRight = sourcePrimitiveTruthy(*V);
        else if (op(I) == u"||")
          EvaluateRight = !sourcePrimitiveTruthy(*V);
        else if (op(I) == u"??")
          EvaluateRight = V->Kind == PrimitiveKind::Null ||
                          V->Kind == PrimitiveKind::Undefined;
        else
          Out.Immediate |= UnknownEffect;
      }
      if (EvaluateRight)
        mergeField(Out, I, "right");
      return Out;
    }
    if (K == "IfStatement" || K == "ConditionalExpression") {
      mergeField(Out, I, "test");
      const auto Test = truth(child(I, "test"));
      if (!Test || *Test)
        mergeField(Out, I, "consequent");
      if (!Test || !*Test)
        mergeField(Out, I, "alternate");
      return Out;
    }
    if (K == "WhileStatement" || K == "ForStatement") {
      mergeField(Out, I, "init");
      mergeField(Out, I, "test");
      const auto Test = truth(child(I, "test"));
      if (!Test || *Test) {
        mergeField(Out, I, "body");
        mergeField(Out, I, "update");
        Out.Immediate |= Control | MayDiverge;
      }
      return Out;
    }
    if (K == "ImportDeclaration" || K == "ExportAllDeclaration") {
      Out.ContainsDeclaration = true;
      Out.Immediate = ModuleLink | Invocation;
      return Out;
    }
    if (K == "ExportNamedDeclaration" || K == "ExportDefaultDeclaration") {
      mergeField(Out, I, "declaration");
      Out.Immediate |= ModuleLink;
      if (child(I, "source") != None)
        Out.Immediate |= Invocation;
      return Out;
    }
    if (K == "MetaProperty") {
      const auto Meta = child(I, "meta");
      const auto *Name =
          Meta == None ? nullptr : Source.Nodes[Meta].text("name");
      Out.Immediate = Name && *Name == u"import"
                          ? ModuleLink | Allocate | UnknownEffect
                          : ReadBinding;
      return Out;
    }
    if (K == "PrivateName" || K == "ImportSpecifier" ||
        K == "ImportDefaultSpecifier" || K == "ImportNamespaceSpecifier" ||
        K == "ImportAttribute" || K == "ExportSpecifier" ||
        K == "ExportNamespaceSpecifier")
      return Out;
    all(Out, I);
    // Global declaration instantiation checks the host global object before
    // statement evaluation, even for a var inside an unselected branch.
    if (K == "Program" && Source.SourceType == "script" &&
        Out.ContainsDeclaration)
      Out.Immediate |= UnknownEffect | MayThrow;
    if (K == "MemberExpression" || K == "OptionalMemberExpression") {
      // Conservatively retain effects even when an optional chain might skip.
      Out.Immediate |= ReadProperty | Invocation;
    } else if (K == "CallExpression" || K == "OptionalCallExpression" ||
               K == "NewExpression" || K == "TaggedTemplateExpression") {
      Out.Immediate |= Invocation;
      if (K == "NewExpression")
        Out.Immediate |= Allocate;
    } else if (K == "AssignmentExpression" || K == "UpdateExpression") {
      Out.Immediate |= MayThrow;
      const auto Left =
          child(I, K == "AssignmentExpression" ? "left" : "argument");
      if (Left == None || Source.Nodes[Left].Kind != "Identifier")
        Out.Immediate |= WriteProperty | Invocation;
      if (K == "UpdateExpression" || op(I) != u"=")
        Out.Immediate |= Invocation;
    } else if (K == "UnaryExpression") {
      if (op(I) == u"delete") {
        if (!primitive(child(I, "argument")))
          Out.Immediate |= WriteProperty | Invocation;
      } else if (op(I) == u"+" || op(I) == u"-" || op(I) == u"~")
        coercion(Out, I);
      // !, void and typeof do not coerce objects or call methods themselves.
    } else if (K == "BinaryExpression" || K == "TemplateLiteral") {
      const bool StrictEquality =
          K == "BinaryExpression" && (op(I) == u"===" || op(I) == u"!==");
      if (!StrictEquality)
        coercion(Out, I);
    } else if (K == "VariableDeclaration") {
      Out.ContainsDeclaration = true;
      const auto *Kind = N.text("kind");
      if (Kind && (*Kind == u"using" || *Kind == u"await using")) {
        // Registration reads dispose methods (including getters); leaving the
        // owning scope can invoke arbitrary code even through return/throw.
        // Immediate includes the scope-exit obligation, not its exact timing.
        // Deferred is reserved for function/instance bodies in this model.
        Out.Immediate |= ReadProperty | Invocation | Control;
        if (*Kind == u"await using")
          Out.Immediate |= Suspend;
      }
    } else if (K == "VariableDeclarator") {
      Out.Immediate |= WriteBinding;
    } else if (K == "ArrayPattern" || K == "ObjectPattern" ||
               K == "RestElement" || K == "SpreadElement") {
      Out.Immediate |= ReadProperty | Invocation;
      if (K == "RestElement" || K == "SpreadElement")
        Out.Immediate |= Allocate;
    } else if (K == "ArrayExpression" || K == "ObjectExpression" ||
               K == "RegExpLiteral") {
      Out.Immediate |= Allocate;
    } else if (K == "AwaitExpression" || K == "YieldExpression") {
      Out.Immediate |= Suspend | Control | Invocation;
    } else if (K == "ImportExpression") {
      Out.Immediate |= ModuleLink | Invocation;
    } else if (K == "ThrowStatement") {
      Out.Immediate |= MayThrow | Control;
    } else if (K == "ReturnStatement" || K == "BreakStatement" ||
               K == "ContinueStatement") {
      Out.Immediate |= Control;
    } else if (K == "ForInStatement" || K == "ForOfStatement") {
      Out.Immediate |=
          ReadProperty | WriteBinding | WriteProperty | Invocation | Control;
      if (N.flag("await"))
        Out.Immediate |= Suspend;
    } else if (K == "DoWhileStatement") {
      Out.Immediate |= Control | MayDiverge;
    } else if (K == "WithStatement") {
      Out.Immediate |= ReadProperty | Invocation;
    } else if (K == "DebuggerStatement") {
      Out.Immediate |= Debugger | UnknownEffect;
    } else if (K == "ThisExpression" || K == "Super") {
      Out.Immediate |= ReadBinding | MayThrow;
    } else if (K != "Program" && K != "BlockStatement" && K != "StaticBlock" &&
               K != "ClassBody" && K != "ExpressionStatement" &&
               K != "EmptyStatement" && K != "Empty" && K != "NullLiteral" &&
               K != "BooleanLiteral" && K != "NumericLiteral" &&
               K != "StringLiteral" && K != "BigIntLiteral" &&
               K != "DirectiveLiteral" && K != "Directive" &&
               K != "TemplateElement" && K != "SequenceExpression" &&
               K != "AssignmentPattern" && K != "LabeledStatement" &&
               K != "TryStatement" && K != "CatchClause" &&
               K != "SwitchStatement" && K != "SwitchCase") {
      Out.Immediate |= UnknownEffect;
      Result.Status = "partial";
    }
    return Out;
  }

public:
  Effects(const SourceAnalysis &S, const SourceBindingAnalysis &B,
          const SourceValueAnalysis &V)
      : Source(S), Bindings(B), Values(V) {
    Result.SourceID = S.ID;
    Result.ID =
        identity("source-effects", {S.ID, B.ID, V.ID, JavaScriptEffectProfile});
    Result.Status = B.Status == "partial" ? "partial" : "ok";
  }
  SourceEffectAnalysis run() {
    if (Bindings.SourceID != Source.ID || Values.SourceID != Source.ID)
      throw Error("source_analysis_mismatch");
    if (Source.ParseStatus != "parsed" ||
        (Bindings.Status != "ok" && Bindings.Status != "partial") ||
        Values.Nodes.size() != Source.Nodes.size()) {
      Result.Status = "unavailable";
      const auto Code = Source.ParseStatus != "parsed" ? "source_not_parsed"
                        : Bindings.Status == "budget_exceeded"
                            ? "source_bindings_budget_exceeded"
                        : Bindings.Status == "unsupported"
                            ? "source_bindings_unsupported"
                        : Values.Status == "budget_exceeded"
                            ? "source_values_budget_exceeded"
                            : "source_semantic_prerequisite_unavailable";
      Result.Diagnostics.push_back({Code, -1});
      return std::move(Result);
    }
    step(validateSourceModel(Source));
    DeclarationEffects.assign(Source.Nodes.size(), 0);
    for (const auto &D : Bindings.Declarations) {
      step();
      if (D.Node == None)
        continue;
      if (D.Node >= Source.Nodes.size() ||
          D.Binding >= Bindings.Bindings.size() ||
          Bindings.Bindings[D.Binding].Scope >= Bindings.Scopes.size())
        throw Error("invalid_source_binding_model");
      DeclarationEffects[D.Node] |= WriteBinding;
      const auto &B = Bindings.Bindings[D.Binding];
      if (D.Kind == "var" && Bindings.Scopes[B.Scope].Kind == "script")
        DeclarationEffects[D.Node] |= WriteProperty | Invocation;
    }
    References.assign(Source.Nodes.size(), None);
    for (uint32_t I = 0; I < Bindings.References.size(); ++I) {
      step();
      const auto &R = Bindings.References[I];
      if (R.Node >= References.size() || References[R.Node] != None ||
          (R.Binding != None &&
           (R.Binding >= Bindings.Bindings.size() ||
            Bindings.Bindings[R.Binding].Scope >= Bindings.Scopes.size())))
        throw Error("invalid_source_binding_model");
      References[R.Node] = I;
    }
    Result.Nodes.resize(Source.Nodes.size());
    try {
      for (size_t I = Source.Nodes.size(); I-- != 0;)
        Result.Nodes[I] = analyze(uint32_t(I));
    } catch (const Error &E) {
      Result.Nodes.clear();
      Result.Status =
          std::string_view(E.what()) == "source_effect_budget_exceeded"
              ? "budget_exceeded"
              : "unsupported";
      Result.Diagnostics.push_back({E.what(), -1});
    }
    return std::move(Result);
  }
};
} // namespace

SourceEffectAnalysis analyzeSourceEffects(const SourceAnalysis &Source,
                                          const SourceBindingAnalysis &Bindings,
                                          const SourceValueAnalysis &Values) {
  return Effects(Source, Bindings, Values).run();
}

std::vector<std::string> sourceEffectNames(uint32_t Effects) {
  std::vector<std::string> Names;
  for (const auto &[Flag, Name] :
       {std::pair<uint32_t, const char *>{ReadBinding, "read_binding"},
        {WriteBinding, "write_binding"},
        {ReadProperty, "read_property"},
        {WriteProperty, "write_property"},
        {Call, "call"},
        {MayThrow, "may_throw"},
        {Allocate, "allocate"},
        {Suspend, "suspend"},
        {Control, "control"},
        {UnknownEffect, "unknown"},
        {Debugger, "debugger"},
        {ModuleLink, "module_link"},
        {MayDiverge, "may_diverge"}})
    if (Effects & Flag)
      Names.emplace_back(Name);
  return Names;
}
} // namespace neverd::web
