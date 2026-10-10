#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/SourceBindings.h"

#include <algorithm>
#include <stdexcept>

namespace {
using namespace neverd::web;
constexpr auto None = NoSourceIndex;

struct Analysis {
  SourceAnalysis Syntax;
  SourceBindingAnalysis Bindings;
  explicit Analysis(std::string_view Text, std::string_view Type = "script")
      : Syntax(inspectJavaScript(identity("test-source", {Text}), Text, Type)),
        Bindings(analyzeSourceBindings(Syntax)) {}
  std::vector<const SourceReference *>
  references(std::u16string_view Name) const {
    std::vector<const SourceReference *> Result;
    for (const auto &R : Bindings.References)
      if (Bindings.Names[R.Name] == Name)
        Result.push_back(&R);
    return Result;
  }
  const SourceBinding &binding(const SourceReference *R) const {
    return Bindings.Bindings.at(R->Binding);
  }
  std::vector<const SourceBinding *> named(std::u16string_view Name) const {
    std::vector<const SourceBinding *> Result;
    for (const auto &B : Bindings.Bindings)
      if (Bindings.Names[B.Name] == Name)
        Result.push_back(&B);
    return Result;
  }
  bool diagnostic(std::string_view Code) const {
    return std::any_of(Bindings.Diagnostics.begin(), Bindings.Diagnostics.end(),
                       [&](const auto &D) { return D.Code == Code; });
  }
};

TEST(WebBindings, ShadowingHoistingAndReferencesHaveStableIdentities) {
  const auto Text = R"JS(
    x; var x; function f(a) { y; var y; { x; let x = a; } return x + y; }
    x;
  )JS";
  const Analysis A(Text), Again(Text);
  ASSERT_EQ(A.Bindings.Status, "ok");
  const auto X = A.references(u"x"), Y = A.references(u"y");
  ASSERT_EQ(X.size(), 4);
  ASSERT_EQ(Y.size(), 2);
  EXPECT_EQ(X[0]->Binding, X[2]->Binding);
  EXPECT_EQ(X[0]->Binding, X[3]->Binding);
  EXPECT_NE(X[0]->Binding, X[1]->Binding);
  EXPECT_EQ(A.binding(X[1]).Kind, "let");
  EXPECT_TRUE(A.binding(X[1]).HasTemporalDeadZone);
  EXPECT_EQ(Y[0]->Binding, Y[1]->Binding);
  EXPECT_EQ(A.binding(Y[0]).Kind, "var");
  ASSERT_EQ(A.Bindings.References.size(), Again.Bindings.References.size());
  for (size_t I = 0; I < A.Bindings.References.size(); ++I)
    EXPECT_EQ(A.Bindings.References[I].ID, Again.Bindings.References[I].ID);
}

TEST(WebBindings, PropertyNamesLabelsAndPrivateNamesAreNotVariableReads) {
  const Analysis A(R"JS(
    const value = 1, key = 'x';
    const object = { value, [key]: value, get getter() { return value; } };
    object.value; object[key];
    outer: for (;;) { break outer; }
    class C { #private; method() { return this.#private; } }
  )JS");
  ASSERT_EQ(A.Bindings.Status, "ok");
  EXPECT_EQ(A.references(u"value").size(), 3);
  EXPECT_EQ(A.references(u"key").size(), 2);
  EXPECT_EQ(A.references(u"object").size(), 2);
  for (const auto Name : {u"outer", u"getter", u"method", u"private"})
    EXPECT_TRUE(A.references(Name).empty());
}

TEST(WebBindings, ParametersWithExpressionsCannotSeeBodyVariables) {
  const Analysis A(R"JS(
    let x = 1;
    function f(a = x, b = () => x) { var x = 2; return x; }
    function g(x) { var x; return x; }
    function h({[x]: a}) { var x; return x; }
  )JS");
  ASSERT_EQ(A.Bindings.Status, "ok");
  const auto X = A.references(u"x");
  ASSERT_EQ(X.size(), 6);
  EXPECT_EQ(X[0]->Binding, X[1]->Binding);
  EXPECT_NE(X[0]->Binding, X[2]->Binding);
  EXPECT_EQ(A.binding(X[0]).Kind, "let");
  EXPECT_EQ(A.binding(X[2]).Kind, "var");
  EXPECT_EQ(A.binding(X[3]).Kind, "parameter");
  EXPECT_EQ(X[0]->Binding, X[4]->Binding);
  EXPECT_NE(X[4]->Binding, X[5]->Binding);
  EXPECT_EQ(A.Bindings.Scopes[A.binding(X[2]).Scope].Kind,
            "function_variables");
}

TEST(WebBindings, DestructuringDeclarationsAndAssignmentsKeepEvaluationScope) {
  const Analysis A(R"JS(
    let x;
    const { [key]: local = fallback, ...rest } = source;
    [x, ...object.items] = input;
    ({fixed: x = local} = input);
    x += local; ++x; typeof missing; delete absent;
  )JS");
  ASSERT_EQ(A.Bindings.Status, "ok");
  EXPECT_TRUE(A.references(u"fixed").empty());
  EXPECT_TRUE(A.references(u"items").empty());
  EXPECT_EQ(A.references(u"key").size(), 1);
  EXPECT_EQ(A.references(u"object").size(), 1);
  const auto X = A.references(u"x");
  ASSERT_EQ(X.size(), 4);
  EXPECT_EQ(X[0]->Access, "write");
  EXPECT_EQ(X[1]->Access, "write");
  EXPECT_EQ(X[2]->Access, "read_write");
  EXPECT_EQ(X[3]->Access, "read_write");
  for (const auto *R : X)
    EXPECT_EQ(R->Binding, X[0]->Binding);
  ASSERT_EQ(A.references(u"missing").size(), 1);
  EXPECT_EQ(A.references(u"missing")[0]->Access, "typeof");
  EXPECT_EQ(A.references(u"missing")[0]->Resolution, "external");
  EXPECT_EQ(A.references(u"absent")[0]->Access, "delete");
}

TEST(WebBindings, LoopsSwitchAndCatchCreateTheRightLexicalFamilies) {
  const Analysis A(R"JS(
    let item;
    for (let item of item) { (() => item); }
    for (const fixed = 1; test;) { fixed; }
    switch (item) { case 1: let item; break; default: item; }
    try {} catch ({message}) { message; { let message; message; } }
    message; item;
  )JS");
  ASSERT_EQ(A.Bindings.Status, "ok");
  const auto Items = A.references(u"item");
  ASSERT_EQ(Items.size(), 5);
  EXPECT_EQ(Items[0]->Binding, Items[1]->Binding);
  EXPECT_NE(Items[0]->Binding, Items[2]->Binding);
  EXPECT_EQ(Items[2]->Binding, Items[4]->Binding);
  EXPECT_NE(Items[2]->Binding, Items[3]->Binding);
  EXPECT_TRUE(A.Bindings.Scopes[A.binding(Items[0]).Scope].PerIteration);
  EXPECT_FALSE(A.Bindings.Scopes[A.named(u"fixed")[0]->Scope].PerIteration);
  const auto Messages = A.references(u"message");
  ASSERT_EQ(Messages.size(), 3);
  EXPECT_EQ(A.binding(Messages[0]).Kind, "catch_parameter");
  EXPECT_NE(Messages[0]->Binding, Messages[1]->Binding);
  EXPECT_EQ(Messages[2]->Resolution, "external");
}

TEST(WebBindings, NamedFunctionsClassesAndStaticBlocksKeepPrivateEnvironments) {
  const Analysis A(R"JS(
    const value = function self(arg = self) { return self; };
    self;
    class C extends C { [C]() { return C; } field = C; static { var local; local; } }
    C; local;
    const D = class Hidden { method() { return Hidden; } };
    Hidden;
  )JS");
  ASSERT_EQ(A.Bindings.Status, "ok");
  const auto Self = A.references(u"self");
  ASSERT_EQ(Self.size(), 3);
  EXPECT_EQ(Self[0]->Binding, Self[1]->Binding);
  EXPECT_EQ(A.binding(Self[0]).Kind, "function_name");
  EXPECT_EQ(Self[2]->Resolution, "external");
  const auto C = A.references(u"C");
  ASSERT_EQ(C.size(), 5);
  for (size_t I = 0; I < 4; ++I) {
    EXPECT_EQ(C[I]->Binding, C[0]->Binding);
    EXPECT_EQ(A.binding(C[I]).Kind, "class_name");
    EXPECT_TRUE(A.binding(C[I]).Immutable);
  }
  EXPECT_NE(C[0]->Binding, C[4]->Binding);
  EXPECT_EQ(A.binding(C[4]).Kind, "class");
  const auto Local = A.references(u"local");
  ASSERT_EQ(Local.size(), 2);
  EXPECT_EQ(A.Bindings.Scopes[A.binding(Local[0]).Scope].Kind, "class_static");
  EXPECT_EQ(Local[1]->Resolution, "external");
  const auto Hidden = A.references(u"Hidden");
  ASSERT_EQ(Hidden.size(), 2);
  EXPECT_EQ(A.binding(Hidden[0]).Kind, "class_name");
  EXPECT_EQ(Hidden[1]->Resolution, "external");
}

TEST(WebBindings, ImportsAndExportsDoNotResolveRemoteSpecifierNames) {
  const Analysis A(R"JS(
    import primary, {remote as local} from 'not-read';
    import * as space from 'also-not-read';
    export {local as publicName};
    export {remote as forwarded} from 'not-read';
    export * from 'not-read';
    export default function main() { return primary + local + space; }
  )JS",
                   "module");
  ASSERT_EQ(A.Bindings.Status, "ok");
  EXPECT_TRUE(A.references(u"remote").empty());
  EXPECT_TRUE(A.references(u"publicName").empty());
  EXPECT_TRUE(A.references(u"forwarded").empty());
  ASSERT_EQ(A.references(u"local").size(), 2);
  EXPECT_EQ(A.references(u"local")[0]->Access, "export");
  for (const auto Name : {u"local", u"primary", u"space"}) {
    ASSERT_EQ(A.named(Name).size(), 1);
    EXPECT_EQ(A.named(Name)[0]->Kind, "import");
    EXPECT_TRUE(A.named(Name)[0]->Immutable);
  }
}

TEST(WebBindings, CommonJsWrapperAndArgumentsAreExplicitAndShadowable) {
  const Analysis A(R"JS(
    require('not-loaded'); exports.x = module;
    function f() { return () => arguments; }
    function g(arguments) { return arguments; }
    function h() { function arguments() {} return arguments; }
    (() => arguments);
  )JS",
                   "commonjs");
  ASSERT_EQ(A.Bindings.Status, "ok");
  const auto Require = A.references(u"require");
  ASSERT_EQ(Require.size(), 1);
  EXPECT_EQ(A.binding(Require[0]).Kind, "commonjs_parameter");
  EXPECT_TRUE(A.binding(Require[0]).Implicit);
  const auto Arguments = A.references(u"arguments");
  ASSERT_EQ(Arguments.size(), 4);
  EXPECT_EQ(A.binding(Arguments[0]).Kind, "arguments");
  EXPECT_EQ(A.binding(Arguments[1]).Kind, "parameter");
  EXPECT_EQ(A.binding(Arguments[2]).Kind, "function_var");
  EXPECT_EQ(A.binding(Arguments[3]).Kind, "arguments");
  EXPECT_NE(Arguments[0]->Binding, Arguments[3]->Binding);
  const Analysis Script("require('x'); (() => arguments);", "script");
  for (const auto &R : Script.Bindings.References)
    EXPECT_EQ(R.Resolution, "external");
}

TEST(WebBindings, WithAndPotentialEvalNeverBecomeConfidentGlobalApiClaims) {
  const Analysis A(R"JS(
    let x;
    with (object) { x; external; { let x; x; } }
    function f(eval) { eval(code); external; var known; known; }
    function strictFn() { 'use strict'; eval(code); external; }
    function indirect() { (0, eval)(code); eval?.(code); external; }
  )JS");
  ASSERT_EQ(A.Bindings.Status, "partial");
  EXPECT_TRUE(A.diagnostic("dynamic_with_environment"));
  EXPECT_TRUE(A.diagnostic("possible_direct_eval"));
  const auto X = A.references(u"x");
  ASSERT_EQ(X.size(), 2);
  EXPECT_EQ(X[0]->Resolution, "dynamic_with");
  EXPECT_EQ(X[1]->Resolution, "lexical_binding");
  const auto External = A.references(u"external");
  ASSERT_EQ(External.size(), 4);
  EXPECT_EQ(External[0]->Resolution, "dynamic_with");
  EXPECT_EQ(External[1]->Resolution, "dynamic_eval");
  EXPECT_EQ(External[2]->Resolution, "external");
  EXPECT_EQ(External[3]->Resolution, "external");
  ASSERT_EQ(A.references(u"known").size(), 1);
  EXPECT_EQ(A.references(u"known")[0]->Resolution, "lexical_binding");
}

TEST(WebBindings, AnnexBAndDeclarationConflictsRemainExplicit) {
  const Analysis Legacy("{ function block() {} block; } block;");
  ASSERT_EQ(Legacy.Bindings.Status, "partial");
  EXPECT_TRUE(Legacy.diagnostic("annex_b_block_function"));
  const auto Block = Legacy.references(u"block");
  ASSERT_EQ(Block.size(), 2);
  EXPECT_EQ(Block[0]->Resolution, "lexical_binding");
  EXPECT_EQ(Block[1]->Resolution, "annex_b_uncertain");
  const Analysis Strict("'use strict'; { function block() {} block; } block;");
  ASSERT_EQ(Strict.Bindings.Status, "ok");
  EXPECT_EQ(Strict.references(u"block")[1]->Resolution, "external");
  for (const auto Text :
       {"let duplicate; let duplicate; duplicate;",
        "{ let duplicate; {var duplicate;} duplicate; }",
        "function f(duplicate) {let duplicate; duplicate;}"}) {
    const Analysis Conflict(Text);
    EXPECT_NE(Conflict.Bindings.Status, "ok") << Text;
    for (const auto *R : Conflict.references(u"duplicate"))
      EXPECT_EQ(R->Resolution, "conflicting_declaration") << Text;
  }
}

TEST(WebBindings,
     EscapedIdentifiersResolveByCodeUnitsAndUnknownNodesFailClosed) {
  const Analysis A(R"JS(const 𝒜 = 1; \u{1D49C};)JS");
  ASSERT_EQ(A.Bindings.Status, "ok");
  ASSERT_EQ(A.references(u"𝒜").size(), 1);
  EXPECT_EQ(A.references(u"𝒜")[0]->Resolution, "lexical_binding");
  auto Invalid = A.Syntax;
  Invalid.Nodes.back().Kind = "FutureUnsupportedScopeNode";
  const auto Unsupported = analyzeSourceBindings(Invalid);
  EXPECT_EQ(Unsupported.Status, "unsupported");
  EXPECT_TRUE(Unsupported.Bindings.empty());
  EXPECT_TRUE(Unsupported.References.empty());
  Invalid = A.Syntax;
  Invalid.Nodes[0].Children[0].Index = 0;
  EXPECT_THROW(analyzeSourceBindings(Invalid), Error);
}
TEST(WebBindings,
     BudgetsDiscardIncompleteGraphsAndDiagnosticsReportTheirCeiling) {
  std::string Text(40, '{');
  for (unsigned I = 0; I != 40000; ++I)
    Text += "absent;";
  Text += std::string(40, '}');
  const Analysis Budgeted(Text);
  ASSERT_EQ(Budgeted.Syntax.ParseStatus, "parsed");
  EXPECT_EQ(Budgeted.Bindings.Status, "budget_exceeded");
  EXPECT_TRUE(Budgeted.Bindings.Bindings.empty());
  EXPECT_TRUE(Budgeted.Bindings.Scopes.empty());
  EXPECT_TRUE(Budgeted.Bindings.References.empty());
  EXPECT_TRUE(Budgeted.diagnostic("source_binding_budget_exceeded"));
  EXPECT_LE(Budgeted.Bindings.Steps, MaxJavaScriptBindingSteps);
  Text.clear();
  for (unsigned I = 0; I != 40; ++I)
    Text += "eval(input);";
  const Analysis Diagnostics(Text);
  EXPECT_EQ(Diagnostics.Bindings.Status, "partial");
  EXPECT_EQ(Diagnostics.Bindings.DiagnosticCount, 40);
  EXPECT_EQ(Diagnostics.Bindings.Diagnostics.size(), 32);
}

TEST(WebBindings, WrapperAndParameterConflictsDoNotAuthorizeBindings) {
  const Analysis Wrapper("let require; require;", "commonjs");
  ASSERT_EQ(Wrapper.Syntax.ParseStatus, "parsed");
  EXPECT_EQ(Wrapper.Bindings.Status, "partial");
  EXPECT_TRUE(Wrapper.diagnostic("wrapper_parameter_lexical_conflict"));
  ASSERT_EQ(Wrapper.references(u"require").size(), 1);
  EXPECT_EQ(Wrapper.references(u"require")[0]->Resolution,
            "conflicting_declaration");
  const Analysis Duplicate("function f(a, a) { return a; }");
  ASSERT_EQ(Duplicate.Bindings.Status, "ok");
  EXPECT_EQ(Duplicate.named(u"a").size(), 1);
  const Analysis Strict("function f() { 'use strict'; return arguments; }");
  ASSERT_EQ(Strict.Bindings.Status, "ok");
  ASSERT_EQ(Strict.references(u"arguments").size(), 1);
  EXPECT_TRUE(Strict.binding(Strict.references(u"arguments")[0]).Immutable);
}
} // namespace
