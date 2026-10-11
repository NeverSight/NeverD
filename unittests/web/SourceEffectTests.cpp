//===- SourceEffectTests.cpp - Source Effect tests ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source Effect tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/SourceEffects.h"

namespace {
using namespace neverd::web;

struct Effects {
  SourceAnalysis Source;
  SourceBindingAnalysis Bindings;
  SourceValueAnalysis Values;
  SourceEffectAnalysis Analysis;
  explicit Effects(std::string_view Text, std::string_view Type = "script") {
    Source = inspectJavaScript(identity("effects-test", {Text}), Text, Type);
    EXPECT_EQ(Source.ParseStatus, "parsed");
    Bindings = analyzeSourceBindings(Source);
    Values = analyzeSourceValues(Source);
    Analysis = analyzeSourceEffects(Source, Bindings, Values);
    EXPECT_EQ(Analysis.Nodes.size(), Source.Nodes.size()) << Analysis.Status;
  }
  SourceNodeEffects node(std::string_view Kind, unsigned Occurrence = 0) const {
    for (size_t I = 0; I < Source.Nodes.size(); ++I)
      if (Source.Nodes[I].Kind == Kind && Occurrence-- == 0) {
        if (I < Analysis.Nodes.size())
          return Analysis.Nodes[I];
        break;
      }
    ADD_FAILURE() << "Missing effect node: " << Kind;
    return {UnknownEffect, UnknownEffect, false};
  }
};

TEST(WebSourceEffects, FunctionBodiesAndDefaultsRemainDeferred) {
  const Effects E("function f(x = defaultCall()) { return bodyCall(x); } "
                  "const a = (x = arrowDefault()) => arrowBody(x);");
  for (const auto Kind : {"FunctionDeclaration", "ArrowFunctionExpression"}) {
    const auto F = E.node(Kind);
    EXPECT_FALSE(F.Immediate & Call);
    EXPECT_TRUE(F.Immediate & Allocate);
    EXPECT_TRUE(F.Deferred & Call);
    EXPECT_TRUE(F.Deferred & MayThrow);
  }
  EXPECT_FALSE(E.node("Program").Immediate & Call);
  EXPECT_TRUE(E.node("Program").ContainsDeclaration);
}

TEST(WebSourceEffects, ResourceAcquisitionAndScopeExitNeverLookPure) {
  const Effects E("using x=null; async function f(){await using y=null;}",
                  "module");
  const auto Sync = E.node("VariableDeclaration");
  EXPECT_TRUE(Sync.Immediate & ReadProperty);
  EXPECT_TRUE(Sync.Immediate & Call);
  EXPECT_TRUE(Sync.Immediate & MayThrow);
  EXPECT_TRUE(Sync.Immediate & Control);
  EXPECT_TRUE(Sync.Immediate & UnknownEffect);
  EXPECT_FALSE(Sync.Immediate & Suspend);
  const auto Async = E.node("VariableDeclaration", 1);
  EXPECT_TRUE(Async.Immediate & Suspend);
  EXPECT_TRUE(Async.Immediate & Call);
  const auto Function = E.node("FunctionDeclaration");
  EXPECT_FALSE(Function.Immediate & Suspend);
  EXPECT_FALSE(Function.Immediate & Call);
  EXPECT_TRUE(Function.Deferred & Suspend);
  EXPECT_TRUE(Function.Deferred & Call);
  const Effects Loop("for(using x=null;false;){}", "module");
  EXPECT_TRUE(Loop.node("ForStatement").Immediate & Call);
  const Effects Unchosen("if(false){using x=null;}", "module");
  EXPECT_FALSE(Unchosen.node("IfStatement").Immediate & Call);
}

TEST(WebSourceEffects, ClassInstanceInitializersAndMethodsAreDeferred) {
  const Effects E("class A { field = init(); #p = privateInit(); "
                  "method() { return methodBody(); } }");
  const auto C = E.node("ClassDeclaration");
  EXPECT_TRUE(C.Immediate & WriteBinding);
  EXPECT_FALSE(C.Immediate & Call);
  EXPECT_TRUE(C.Deferred & Call);
  EXPECT_TRUE(C.Deferred & WriteProperty);
  const auto F = E.node("ClassProperty");
  EXPECT_FALSE(F.Immediate & Call);
  EXPECT_TRUE(F.Deferred & Call);
}

TEST(WebSourceEffects, ClassKeysHeritageStaticsAndStaticBlocksExecuteNow) {
  const Effects E("class A extends base() { [key()] = later(); "
                  "static x = staticInit(); static { block(); } "
                  "[methodKey()]() { methodBody(); } }");
  EXPECT_TRUE(E.node("ClassDeclaration").Immediate & Call);
  EXPECT_TRUE(E.node("ClassProperty").Immediate & Call);
  EXPECT_TRUE(E.node("ClassProperty").Deferred & Call);
  const auto Static = E.node("ClassProperty", 1);
  EXPECT_TRUE(Static.Immediate & Call);
  EXPECT_FALSE(Static.Deferred & Call);
  EXPECT_TRUE(E.node("StaticBlock").Immediate & Call);
  EXPECT_TRUE(E.node("MethodDefinition").Immediate & Call);
  EXPECT_TRUE(E.node("MethodDefinition").Deferred & Call);
}

TEST(WebSourceEffects, CreatingGettersDoesNotInvokeTheirBodies) {
  const Effects E("const obj = { get field() { return getter(); }, "
                  "set field(x) { setter(x); }, method() { body(); } };");
  EXPECT_FALSE(E.node("ObjectExpression").Immediate & Call);
  EXPECT_TRUE(E.node("ObjectExpression").Deferred & Call);
  EXPECT_FALSE(E.node("Program").Immediate & Call);
}

TEST(WebSourceEffects, ShortCircuitDropsUnchosenEffectsButKeepsDeclarations) {
  const Effects E("if(false) {var hoisted = skipped();} "
                  "false && other(); true ? 1 : (1n/0n); "
                  "while(false) {var another = never();}");
  for (const auto Kind : {"IfStatement", "LogicalExpression",
                          "ConditionalExpression", "WhileStatement"}) {
    const auto N = E.node(Kind);
    EXPECT_FALSE(N.Immediate & Call) << Kind;
    EXPECT_FALSE(N.Immediate & MayThrow) << Kind;
  }
  EXPECT_TRUE(E.node("IfStatement").ContainsDeclaration);
  EXPECT_TRUE(E.node("WhileStatement").ContainsDeclaration);
  EXPECT_TRUE(E.node("Program").ContainsDeclaration);
}

TEST(WebSourceEffects, UnknownValuesDoNotLoseCallsCoercionOrGetterEffects) {
  const Effects E("function f(x) { x + 1; x === 1; x.p; x.p = 2; x++; }");
  EXPECT_TRUE(E.node("BinaryExpression", 0).Immediate & Call);
  EXPECT_FALSE(E.node("BinaryExpression", 1).Immediate & Call);
  EXPECT_TRUE(E.node("BinaryExpression", 1).Immediate & ReadBinding);
  EXPECT_TRUE(E.node("MemberExpression").Immediate & ReadProperty);
  EXPECT_TRUE(E.node("MemberExpression").Immediate & Call);
  EXPECT_TRUE(E.node("AssignmentExpression").Immediate & WriteProperty);
  EXPECT_TRUE(E.node("UpdateExpression").Immediate & WriteBinding);
  EXPECT_TRUE(E.node("UpdateExpression").Immediate & Call);
}

TEST(WebSourceEffects, NameRolesAndDynamicObjectEnvironmentsStayDistinct) {
  const Effects Global("var existing = 1;");
  EXPECT_TRUE(Global.node("VariableDeclarator").Immediate & WriteProperty);
  EXPECT_TRUE(Global.node("VariableDeclarator").Immediate & Call);
  EXPECT_TRUE(Global.node("Program").Immediate & UnknownEffect);
  const Effects E(
      "function f(local) { const x = {key: 1}; label: {break label;} "
      "local; external; } var globalVar; globalVar;");
  EXPECT_FALSE(E.node("ObjectExpression").Immediate & ReadBinding);
  EXPECT_FALSE(E.node("LabeledStatement").Immediate & ReadBinding);
  EXPECT_TRUE(E.node("Program").Immediate & Call); // global var can be accessor
  EXPECT_TRUE(E.node("FunctionDeclaration").Deferred & Call); // external getter
  const Effects W("with(obj) { name; }");
  EXPECT_EQ(W.Analysis.Status, "partial");
  EXPECT_TRUE(W.node("WithStatement").Immediate & UnknownEffect);
  EXPECT_TRUE(W.node("WithStatement").Immediate & Call);
}

TEST(WebSourceEffects, DestructuringSpreadAndIterationAreNeverAssumedInert) {
  const Effects E("function f(input) { const [a, ...rest] = input; "
                  "const {[key()]: b = fallback()} = input; "
                  "const copy = {...input}; for (obj.x of input) {} }");
  EXPECT_TRUE(E.node("ArrayPattern").Immediate & Call);
  EXPECT_TRUE(E.node("ObjectPattern").Immediate & Call);
  EXPECT_TRUE(E.node("SpreadElement").Immediate & ReadProperty);
  EXPECT_TRUE(E.node("ForOfStatement").Immediate & WriteProperty);
  EXPECT_TRUE(E.node("ForOfStatement").Immediate & MayDiverge);
}

TEST(WebSourceEffects, ModuleAndAsyncBoundariesAreExplicit) {
  const Effects E("import {x} from 'not-fetched'; export {x}; "
                  "async function f() { await x; } import.meta;",
                  "module");
  EXPECT_TRUE(E.node("ImportDeclaration").Immediate & ModuleLink);
  EXPECT_TRUE(E.node("ImportDeclaration").Immediate & Call);
  EXPECT_FALSE(E.node("ExportNamedDeclaration").Immediate & ReadBinding);
  EXPECT_FALSE(E.node("FunctionDeclaration").Immediate & Suspend);
  EXPECT_TRUE(E.node("FunctionDeclaration").Deferred & Suspend);
  EXPECT_TRUE(E.node("MetaProperty").Immediate & UnknownEffect);
}

TEST(WebSourceEffects, UnknownModelsAndMismatchedAnalysesFailClosed) {
  Effects E("1 + 2;");
  const auto Again = analyzeSourceEffects(E.Source, E.Bindings, E.Values);
  EXPECT_EQ(Again.ID, E.Analysis.ID);
  EXPECT_EQ(E.node("BinaryExpression").Immediate, 0);
  EXPECT_EQ(E.node("BinaryExpression").Deferred, 0);
  E.Values.SourceID = "other-source";
  EXPECT_THROW(analyzeSourceEffects(E.Source, E.Bindings, E.Values), Error);
  E.Source.Nodes.back().Kind = "FutureExpression";
  E.Values = analyzeSourceValues(E.Source);
  EXPECT_EQ(E.Values.Nodes.back().Status, "unsupported");
  E.Bindings = analyzeSourceBindings(E.Source);
  EXPECT_EQ(E.Bindings.Status, "unsupported");
  const auto Unsupported = analyzeSourceEffects(E.Source, E.Bindings, E.Values);
  EXPECT_EQ(Unsupported.Status, "unavailable");
  EXPECT_TRUE(Unsupported.Nodes.empty());
}
} // namespace
