//===- SourceBindings.cpp - JavaScript lexical binding analysis --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JavaScript lexical binding analysis.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/SourceBindings.h"

#include "SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
namespace {
constexpr uint32_t None = NoSourceIndex;

class Resolver {
  const SourceAnalysis &Source;
  SourceBindingAnalysis Result;
  std::map<std::u16string, uint32_t> Names;
  struct ScopeState {
    std::map<uint32_t, uint32_t> Symbols;
    std::set<uint32_t> AnnexBNames;
    bool DuplicateParameters = false;
    bool SimpleCatch = false;
  };
  std::vector<ScopeState> State;

  void step(uint64_t Count = 1) {
    if (Count > MaxJavaScriptBindingSteps - Result.Steps)
      throw Error("source_binding_budget_exceeded");
    Result.Steps += Count;
  }
  const SyntaxNode &node(uint32_t I) const { return Source.Nodes[I]; }
  uint32_t child(uint32_t I, std::string_view Field) const {
    for (const auto &C : node(I).Children)
      if (C.Field == Field)
        return C.Index;
    return None;
  }
  bool is(uint32_t I, std::string_view Kind) const {
    return I != None && node(I).Kind == Kind;
  }
  bool textIs(uint32_t I, std::string_view Field,
              std::u16string_view Text) const {
    const auto *Value = node(I).text(Field);
    return Value && *Value == Text;
  }
  void diagnostic(std::string_view Code, uint32_t I) {
    Result.Status = "partial";
    ++Result.DiagnosticCount;
    if (Result.Diagnostics.size() < 32)
      Result.Diagnostics.push_back(
          {std::string(Code), I == None ? -1 : int64_t(node(I).Start)});
  }
  uint32_t name(std::u16string_view Value) {
    step(Value.size() + 1);
    auto [It, Inserted] = Names.emplace(Value, uint32_t(Result.Names.size()));
    if (Inserted)
      Result.Names.emplace_back(Value);
    return It->second;
  }
  uint32_t identifierName(uint32_t I) {
    const auto *Value = node(I).text("name");
    if (node(I).Kind != "Identifier" || !Value)
      throw Error("invalid_source_model");
    return name(*Value);
  }
  uint32_t scope(std::string_view Kind, uint32_t Parent, uint32_t I,
                 bool Strict, bool Variable = false) {
    step();
    if (I >= Source.Nodes.size() ||
        (Parent != None && Parent >= Result.Scopes.size()))
      throw Error("invalid_source_model");
    SourceScope S;
    S.Kind = Kind;
    S.Parent = Parent;
    S.Node = I;
    S.Strict = Strict;
    const auto Index = uint32_t(Result.Scopes.size());
    S.VariableScope = Variable || Parent == None
                          ? Index
                          : Result.Scopes[Parent].VariableScope;
    const auto Ordinal = std::to_string(Index);
    S.ID = identity("source-scope", {Result.ID, Kind, node(I).ID, Ordinal});
    Result.Scopes.push_back(std::move(S));
    State.emplace_back();
    return Index;
  }
  void mark(uint32_t I, uint32_t S) {
    if (I == None)
      return;
    step();
    if (Result.NodeScopes[I] != None)
      throw Error("invalid_source_model");
    Result.NodeScopes[I] = S;
  }
  void ignore(uint32_t I, uint32_t S) {
    if (I == None)
      return;
    mark(I, S);
    for (const auto &C : node(I).Children)
      ignore(C.Index, S);
  }
  static bool variableKind(std::string_view Kind) {
    return Kind == "var" || Kind == "function_var" || Kind == "parameter" ||
           Kind == "commonjs_parameter" || Kind == "arguments";
  }
  uint32_t declareName(uint32_t Name, uint32_t I, uint32_t S,
                       uint32_t Occurrence, std::string_view Kind) {
    step();
    const auto Existing = State[S].Symbols.find(Name);
    uint32_t B;
    if (Existing != State[S].Symbols.end()) {
      B = Existing->second;
      auto &Old = Result.Bindings[B];
      const bool BothParameters = Kind == "parameter" && Old.Kind == Kind;
      if (!variableKind(Kind) || !variableKind(Old.Kind) ||
          (BothParameters && !State[S].DuplicateParameters)) {
        Old.Conflicting = true;
        diagnostic("conflicting_declaration", I);
      }
    } else {
      B = uint32_t(Result.Bindings.size());
      SourceBinding New;
      New.Scope = S;
      New.Name = Name;
      New.Kind = Kind;
      New.Implicit = I == None;
      New.Immutable = Kind == "const" || Kind == "using" ||
                      Kind == "await using" || Kind == "import" ||
                      Kind == "function_name" || Kind == "class_name" ||
                      (Kind == "arguments" && Result.Scopes[S].Strict);
      New.HasTemporalDeadZone =
          Kind == "let" || Kind == "const" || Kind == "using" ||
          Kind == "await using" || Kind == "class" || Kind == "class_name" ||
          Kind == "parameter" || Kind == "catch_parameter" || Kind == "import";
      const auto Ordinal = std::to_string(B);
      New.ID = identity("source-binding",
                        {Result.ID, Result.Scopes[S].ID, Kind, Ordinal});
      Result.Bindings.push_back(std::move(New));
      State[S].Symbols.emplace(Name, B);
    }
    Result.Declarations.push_back({I, B, Occurrence, std::string(Kind)});
    return B;
  }
  void reference(uint32_t I, uint32_t S, std::string_view Access) {
    SourceReference R;
    R.Node = I;
    R.Scope = S;
    R.Name = identifierName(I);
    R.Access = Access;
    R.ID = identity("source-reference", {Result.ID, node(I).ID, Access});
    Result.References.push_back(std::move(R));
  }

  // Pattern destinations and evaluation environments are deliberately
  // separate: var names hoist, but computed keys/defaults evaluate in place.
  void pattern(uint32_t I, uint32_t Current, uint32_t Destination,
               std::string_view Kind, std::string_view Access = "write") {
    if (I == None)
      return;
    mark(I, Current);
    const auto &N = node(I);
    if (N.Kind == "Identifier") {
      if (Destination == None)
        reference(I, Current, Access);
      else
        declareName(identifierName(I), I, Destination, Current, Kind);
    } else if (N.Kind == "ArrayPattern" || N.Kind == "ObjectPattern") {
      for (const auto &C : N.Children)
        pattern(C.Index, Current, Destination, Kind, Access);
    } else if (N.Kind == "Property") {
      if (N.flag("computed"))
        walk(child(I, "key"), Current);
      else
        ignore(child(I, "key"), Current);
      pattern(child(I, "value"), Current, Destination, Kind, Access);
    } else if (N.Kind == "AssignmentPattern") {
      pattern(child(I, "left"), Current, Destination, Kind, Access);
      walk(child(I, "right"), Current);
    } else if (N.Kind == "RestElement") {
      pattern(child(I, "argument"), Current, Destination, Kind, Access);
    } else if (N.Kind == "MemberExpression" && Destination == None) {
      walk(child(I, "object"), Current);
      if (N.flag("computed"))
        walk(child(I, "property"), Current);
      else
        ignore(child(I, "property"), Current);
    } else if (N.Kind != "Empty") {
      throw Error("unsupported_binding_pattern");
    }
  }
  bool parameterExpression(uint32_t I) {
    step();
    const auto &N = node(I);
    if (N.Kind == "AssignmentPattern" ||
        (N.Kind == "Property" && N.flag("computed")))
      return true;
    for (const auto &C : N.Children)
      if (parameterExpression(C.Index))
        return true;
    return false;
  }
  bool patternName(uint32_t I, std::u16string_view Name) {
    if (I == None)
      return false;
    step();
    const auto &N = node(I);
    if (N.Kind == "Identifier")
      return textIs(I, "name", Name);
    if (N.Kind == "Property")
      return patternName(child(I, "value"), Name);
    if (N.Kind == "AssignmentPattern")
      return patternName(child(I, "left"), Name);
    if (N.Kind == "RestElement")
      return patternName(child(I, "argument"), Name);
    if (N.Kind == "ObjectPattern" || N.Kind == "ArrayPattern")
      for (const auto &C : N.Children)
        if (patternName(C.Index, Name))
          return true;
    return false;
  }
  bool bodySuppressesArguments(uint32_t Body) {
    if (!is(Body, "BlockStatement") && !is(Body, "Program"))
      return false;
    for (const auto &C : node(Body).Children) {
      step();
      const auto &N = node(C.Index);
      if ((N.Kind == "FunctionDeclaration" || N.Kind == "ClassDeclaration") &&
          patternName(child(C.Index, "id"), u"arguments"))
        return true;
      if (N.Kind == "VariableDeclaration" && !textIs(C.Index, "kind", u"var"))
        for (const auto &D : N.Children)
          if (patternName(child(D.Index, "id"), u"arguments"))
            return true;
    }
    return false;
  }
  void statements(uint32_t I, uint32_t S, bool FunctionBody = false) {
    for (const auto &C : node(I).Children)
      walk(C.Index, S, FunctionBody);
  }
  void function(uint32_t I, uint32_t Outer, bool DirectBody, bool Method) {
    const auto &N = node(I);
    const auto Id = child(I, "id");
    uint32_t Parent = Outer;
    if (N.Kind == "FunctionDeclaration" && Id != None) {
      mark(Id, Outer);
      const bool Var = DirectBody && Result.Scopes[Outer].Kind != "module";
      const auto Destination = Var ? Result.Scopes[Outer].VariableScope : Outer;
      declareName(identifierName(Id), Id, Destination, Outer,
                  Var ? "function_var" : "function_lexical");
      if (!DirectBody && !Result.Scopes[Outer].Strict && !N.flag("generator") &&
          !N.flag("async")) {
        State[Result.Scopes[Outer].VariableScope].AnnexBNames.insert(
            identifierName(Id));
        diagnostic("annex_b_block_function", I);
      }
    } else if (Id != None) {
      Parent = scope("function_name", Outer, I, N.Strict);
      mark(Id, Parent);
      declareName(identifierName(Id), Id, Parent, Parent, "function_name");
    }
    bool Expressions = false, Simple = true, HasArguments = false;
    for (const auto &C : N.Children)
      if (C.Field == "params") {
        Expressions |= parameterExpression(C.Index);
        Simple &= is(C.Index, "Identifier");
        HasArguments |= patternName(C.Index, u"arguments");
      }
    const auto Parameters =
        scope("function_parameters", Parent, I, N.Strict, true);
    State[Parameters].DuplicateParameters =
        Simple && !N.Strict && !Method && N.Kind != "ArrowFunctionExpression" &&
        !N.flag("async") && !N.flag("generator");
    const auto Body = child(I, "body");
    if (N.Kind != "ArrowFunctionExpression" && !HasArguments &&
        (Expressions || !bodySuppressesArguments(Body)))
      declareName(name(u"arguments"), None, Parameters, Parameters,
                  "arguments");
    for (const auto &C : N.Children)
      if (C.Field == "params")
        pattern(C.Index, Parameters, Parameters, "parameter");
    const auto Variables =
        Expressions ? scope("function_variables", Parameters, I, N.Strict, true)
                    : Parameters;
    const auto BodyScope = scope("function_body", Variables, Body, N.Strict);
    if (is(Body, "BlockStatement")) {
      mark(Body, BodyScope);
      statements(Body, BodyScope, true);
    } else {
      walk(Body, BodyScope);
    }
    // Body lexical names cannot redeclare parameters, even when defaults
    // require a separate variable environment.
    for (const auto &[Name, B] : State[BodyScope].Symbols) {
      step();
      if (State[Parameters].Symbols.count(Name) &&
          Result.Bindings[State[Parameters].Symbols.at(Name)].Kind ==
              "parameter") {
        Result.Bindings[B].Conflicting = true;
        diagnostic("parameter_lexical_conflict", Body);
      }
    }
  }

  void walk(uint32_t I, uint32_t S, bool DirectBody = false,
            bool Method = false) {
    if (I == None)
      return;
    mark(I, S);
    const auto &N = node(I);
    const auto &K = N.Kind;
    if (K == "Identifier") {
      reference(I, S, "read");
    } else if (K == "Program") {
      statements(I, S, true);
    } else if (K == "FunctionDeclaration" || K == "FunctionExpression" ||
               K == "ArrowFunctionExpression") {
      function(I, S, DirectBody, Method);
    } else if (K == "BlockStatement" || K == "StaticBlock") {
      const bool Static = K == "StaticBlock";
      const auto Inner = scope(Static ? "class_static" : "block", S, I,
                               Static || Result.Scopes[S].Strict, Static);
      statements(I, Inner);
    } else if (K == "VariableDeclaration") {
      const auto *Kind = N.text("kind");
      if (!Kind || (*Kind != u"var" && *Kind != u"let" && *Kind != u"const" &&
                    *Kind != u"using" && *Kind != u"await using"))
        throw Error("unsupported_declaration_kind");
      const std::string KindText(Kind->begin(), Kind->end());
      const auto Destination =
          *Kind == u"var" ? Result.Scopes[S].VariableScope : S;
      for (const auto &C : N.Children) {
        if (!is(C.Index, "VariableDeclarator"))
          throw Error("invalid_source_model");
        mark(C.Index, S);
        pattern(child(C.Index, "id"), S, Destination, KindText);
        walk(child(C.Index, "init"), S);
      }
    } else if (K == "AssignmentExpression") {
      pattern(child(I, "left"), S, None, {},
              textIs(I, "operator", u"=") ? "write" : "read_write");
      walk(child(I, "right"), S);
    } else if (K == "UpdateExpression") {
      pattern(child(I, "argument"), S, None, {}, "read_write");
    } else if (K == "UnaryExpression" &&
               (textIs(I, "operator", u"typeof") ||
                textIs(I, "operator", u"delete")) &&
               is(child(I, "argument"), "Identifier")) {
      const auto Argument = child(I, "argument");
      mark(Argument, S);
      reference(Argument, S,
                textIs(I, "operator", u"typeof") ? "typeof" : "delete");
    } else if (K == "MemberExpression" || K == "OptionalMemberExpression") {
      walk(child(I, "object"), S);
      if (N.flag("computed"))
        walk(child(I, "property"), S);
      else
        ignore(child(I, "property"), S);
    } else if (K == "Property" || K == "MethodDefinition") {
      if (N.flag("computed"))
        walk(child(I, "key"), S);
      else
        ignore(child(I, "key"), S);
      walk(child(I, "value"), S, false,
           K == "MethodDefinition" || N.flag("method") ||
               textIs(I, "kind", u"get") || textIs(I, "kind", u"set"));
    } else if (K == "ClassDeclaration" || K == "ClassExpression") {
      const auto Id = child(I, "id");
      if (Id != None && K == "ClassDeclaration")
        declareName(identifierName(Id), Id, S, S, "class");
      const auto Inner = scope("class", S, I, true);
      if (Id != None) {
        mark(Id, Inner);
        declareName(identifierName(Id), Id, Inner, Inner, "class_name");
      }
      walk(child(I, "superClass"), Inner);
      walk(child(I, "body"), Inner);
    } else if (K == "ClassProperty" || K == "ClassPrivateProperty") {
      if (N.flag("computed"))
        walk(child(I, "key"), S);
      else
        ignore(child(I, "key"), S);
      const auto Value = child(I, "value");
      if (Value != None)
        walk(Value, scope("class_initializer", S, I, true, true));
    } else if (K == "CatchClause") {
      const auto Inner = scope("catch", S, I, Result.Scopes[S].Strict);
      const auto Param = child(I, "param");
      State[Inner].SimpleCatch = is(Param, "Identifier");
      pattern(Param, Inner, Inner, "catch_parameter");
      walk(child(I, "body"), Inner);
    } else if (K == "ForStatement" || K == "ForInStatement" ||
               K == "ForOfStatement") {
      const auto Left = child(I, K == "ForStatement" ? "init" : "left");
      auto Inner = S;
      if (is(Left, "VariableDeclaration") && !textIs(Left, "kind", u"var")) {
        Inner = scope("loop", S, I, Result.Scopes[S].Strict);
        Result.Scopes[Inner].PerIteration =
            K != "ForStatement" || textIs(Left, "kind", u"let");
      }
      if (K != "ForStatement" && !is(Left, "VariableDeclaration"))
        pattern(Left, Inner, None, {});
      else
        walk(Left, Inner);
      for (const auto &C : N.Children)
        if (C.Index != Left)
          walk(C.Index, Inner);
    } else if (K == "SwitchStatement") {
      walk(child(I, "discriminant"), S);
      const auto Inner = scope("switch", S, I, Result.Scopes[S].Strict);
      for (const auto &C : N.Children)
        if (C.Field == "cases")
          walk(C.Index, Inner);
    } else if (K == "WithStatement") {
      walk(child(I, "object"), S);
      const auto Inner = scope("with", S, I, Result.Scopes[S].Strict);
      Result.Scopes[Inner].ObjectEnvironment = true;
      diagnostic("dynamic_with_environment", I);
      walk(child(I, "body"), Inner);
    } else if (K == "CallExpression" || K == "OptionalCallExpression") {
      const auto Callee = child(I, "callee");
      if (K == "CallExpression" && is(Callee, "Identifier") &&
          textIs(Callee, "name", u"eval")) {
        // Even a shadowing local named eval can hold the intrinsic %eval%.
        // No runtime value proof exists at this stage.
        Result.Scopes[S].PossibleDirectEval = true;
        if (!Result.Scopes[S].Strict)
          Result.Scopes[Result.Scopes[S].VariableScope]
              .PossibleEvalDeclarations = true;
        diagnostic("possible_direct_eval", I);
      }
      statements(I, S);
    } else if (K == "ImportDeclaration") {
      for (const auto &C : N.Children) {
        if (C.Field != "specifiers") {
          ignore(C.Index, S);
          continue;
        }
        mark(C.Index, S);
        for (const auto &Part : node(C.Index).Children)
          if (Part.Field == "local")
            pattern(Part.Index, S, S, "import");
          else
            ignore(Part.Index, S);
      }
    } else if (K == "ExportNamedDeclaration") {
      walk(child(I, "declaration"), S, DirectBody);
      const bool Reexport = child(I, "source") != None;
      ignore(child(I, "source"), S);
      for (const auto &C : N.Children)
        if (C.Field == "specifiers") {
          mark(C.Index, S);
          for (const auto &Part : node(C.Index).Children) {
            if (!Reexport && Part.Field == "local" &&
                is(Part.Index, "Identifier")) {
              mark(Part.Index, S);
              reference(Part.Index, S, "export");
            } else {
              ignore(Part.Index, S);
            }
          }
        }
    } else if (K == "ExportDefaultDeclaration") {
      walk(child(I, "declaration"), S, DirectBody);
    } else if (K == "ExportAllDeclaration" || K == "MetaProperty" ||
               K == "PrivateName" || K == "BreakStatement" ||
               K == "ContinueStatement") {
      for (const auto &C : N.Children)
        ignore(C.Index, S);
    } else if (K == "LabeledStatement") {
      if (is(child(I, "body"), "FunctionDeclaration"))
        throw Error("unsupported_labelled_function");
      ignore(child(I, "label"), S);
      walk(child(I, "body"), S);
    } else if (K == "IfStatement") {
      walk(child(I, "test"), S);
      for (const auto &C : N.Children)
        if (C.Field != "test") {
          if (is(C.Index, "FunctionDeclaration")) {
            const auto Inner =
                scope("annex_b_if", S, I, Result.Scopes[S].Strict);
            walk(C.Index, Inner);
          } else {
            walk(C.Index, S);
          }
        }
    } else if (K == "Empty" || K == "NullLiteral" || K == "BooleanLiteral" ||
               K == "StringLiteral" || K == "NumericLiteral" ||
               K == "RegExpLiteral" || K == "BigIntLiteral" ||
               K == "TemplateElement" || K == "DirectiveLiteral" ||
               K == "ThisExpression" || K == "Super" ||
               K == "DebuggerStatement" || K == "EmptyStatement") {
      if (!N.Children.empty())
        throw Error("unsupported_binding_node");
    } else if (K == "WhileStatement" || K == "DoWhileStatement" ||
               K == "ThrowStatement" || K == "ReturnStatement" ||
               K == "ExpressionStatement" || K == "TryStatement" ||
               K == "SequenceExpression" || K == "ObjectExpression" ||
               K == "ArrayExpression" || K == "SpreadElement" ||
               K == "NewExpression" || K == "YieldExpression" ||
               K == "AwaitExpression" || K == "ImportExpression" ||
               K == "UnaryExpression" || K == "LogicalExpression" ||
               K == "ConditionalExpression" || K == "BinaryExpression" ||
               K == "Directive" || K == "SwitchCase" ||
               K == "TemplateLiteral" || K == "TaggedTemplateExpression" ||
               K == "ClassBody") {
      statements(I, S);
    } else {
      throw Error("unsupported_binding_node");
    }
  }

  void conflicts() {
    for (const auto &D : Result.Declarations) {
      step();
      if (D.Node == None)
        continue;
      auto &B = Result.Bindings[D.Binding];
      if (D.Kind == "var" || D.Kind == "function_var") {
        for (auto S = D.OccurrenceScope; S != B.Scope;
             S = Result.Scopes[S].Parent) {
          step();
          if (S == None)
            throw Error("invalid_source_model");
          const auto Existing = State[S].Symbols.find(B.Name);
          if (Existing == State[S].Symbols.end())
            continue;
          if (Result.Scopes[S].Kind == "catch" && State[S].SimpleCatch) {
            diagnostic("annex_b_catch_var", D.Node);
            continue;
          }
          B.Conflicting = true;
          Result.Bindings[Existing->second].Conflicting = true;
          diagnostic("lexical_var_conflict", D.Node);
        }
      }
      if (Result.Scopes[B.Scope].Kind == "block") {
        const auto Parent = Result.Scopes[B.Scope].Parent;
        if (Parent != None && Result.Scopes[Parent].Kind == "catch" &&
            State[Parent].Symbols.count(B.Name)) {
          B.Conflicting = true;
          diagnostic("catch_lexical_conflict", D.Node);
        }
      }
    }
  }
  void resolve() {
    for (auto &R : Result.References) {
      R.Resolution = "external";
      for (auto S = R.Scope; S != None; S = Result.Scopes[S].Parent) {
        step();
        const auto &Scope = Result.Scopes[S];
        if (Scope.ObjectEnvironment) {
          R.Resolution = "dynamic_with";
          break;
        }
        const auto Found = State[S].Symbols.find(R.Name);
        if (Found != State[S].Symbols.end()) {
          if (Result.Bindings[Found->second].Conflicting)
            R.Resolution = "conflicting_declaration";
          else {
            R.Resolution = "lexical_binding";
            R.Binding = Found->second;
          }
          break;
        }
        if (State[S].AnnexBNames.count(R.Name)) {
          R.Resolution = "annex_b_uncertain";
          break;
        }
        if (Scope.PossibleEvalDeclarations) {
          R.Resolution = "dynamic_eval";
          break;
        }
      }
      if (R.Access == "export" && R.Resolution != "lexical_binding")
        diagnostic("unresolved_local_export", R.Node);
    }
  }

public:
  explicit Resolver(const SourceAnalysis &Source) : Source(Source) {
    Result.SourceID = Source.ID;
    Result.ID =
        identity("source-bindings", {Source.ID, JavaScriptBindingProfile});
    Result.Status = "ok";
  }
  SourceBindingAnalysis run() {
    if (Source.ParseStatus != "parsed") {
      Result.Status = "unavailable";
      Result.Diagnostics.push_back({"source_not_parsed", -1});
      Result.DiagnosticCount = 1;
      return std::move(Result);
    }
    step(validateSourceModel(Source));
    Result.NodeScopes.assign(Source.Nodes.size(), None);
    const auto Root = scope(Source.SourceType, None, 0, node(0).Strict, true);
    auto Body = Root;
    if (Source.SourceType == "commonjs") {
      for (const auto Name :
           {u"exports", u"require", u"module", u"__filename", u"__dirname"})
        declareName(name(Name), None, Root, Root, "commonjs_parameter");
      if (!bodySuppressesArguments(0))
        declareName(name(u"arguments"), None, Root, Root, "arguments");
      Body = scope("function_body", Root, 0, node(0).Strict);
    }
    try {
      walk(0, Body);
      if (std::find(Result.NodeScopes.begin(), Result.NodeScopes.end(), None) !=
          Result.NodeScopes.end())
        throw Error("unsupported_binding_node");
      if (Source.SourceType == "commonjs")
        for (const auto &[Name, B] : State[Body].Symbols)
          if (State[Root].Symbols.count(Name) &&
              Result.Bindings[State[Root].Symbols.at(Name)].Kind ==
                  "commonjs_parameter") {
            Result.Bindings[B].Conflicting = true;
            diagnostic("wrapper_parameter_lexical_conflict", 0);
          }
      conflicts();
      resolve();
    } catch (const Error &E) {
      const std::string Code = E.what();
      const auto Steps = Result.Steps;
      Result = {};
      Result.Steps = Steps;
      Result.SourceID = Source.ID;
      Result.ID =
          identity("source-bindings", {Source.ID, JavaScriptBindingProfile});
      Result.Status = Code == "source_binding_budget_exceeded"
                          ? "budget_exceeded"
                          : "unsupported";
      Result.Diagnostics.push_back({Code, -1});
      Result.DiagnosticCount = 1;
    }
    return std::move(Result);
  }
};
} // namespace

SourceBindingAnalysis analyzeSourceBindings(const SourceAnalysis &Source) {
  return Resolver(Source).run();
}
} // namespace neverd::web
