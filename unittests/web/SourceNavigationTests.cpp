#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/SourceNavigation.h"

namespace {
using namespace neverd::web;
struct Inspection {
  std::string Text;
  SourceAnalysis Source;
  SourceBindingAnalysis Bindings;
  SourceNavigation Navigation;
  explicit Inspection(std::string Text, std::string_view Type = "script")
      : Text(std::move(Text)) {
    Source = inspectJavaScript(identity("navigation-test", {this->Text}),
                               this->Text, Type);
    EXPECT_EQ(Source.ParseStatus, "parsed");
    Bindings = analyzeSourceBindings(Source);
    Navigation = analyzeSourceNavigation(Source, Bindings);
    EXPECT_TRUE(Navigation.Status == "ok" || Navigation.Status == "partial");
  }
  std::string_view text(uint32_t Node) const {
    const auto &N = Source.Nodes.at(Node);
    return std::string_view(Text).substr(N.Start, N.End - N.Start);
  }
};

TEST(WebSourceNavigation, FunctionsAndReferencesHaveLexicalSourceContainment) {
  Inspection I(R"JS(function outer(param) {
    function inner() { return param; }
    const closure = function self(value) { return self(value) || inner(); };
    const arrow = (value) => closure(value);
    return arrow(param);
  }
  outer(1);)JS");
  const auto &A = I.Navigation;
  ASSERT_EQ(A.Functions.size(), 4);
  EXPECT_EQ(A.Functions[0].ParentFunction, NoSourceIndex);
  for (uint32_t F = 1; F < 4; ++F)
    EXPECT_EQ(A.Functions[F].ParentFunction, 0);
  EXPECT_EQ(A.Functions[0].Parameters, 1);
  EXPECT_EQ(A.Functions[1].Parameters, 0);
  EXPECT_NE(A.Functions[0].NameBinding, NoSourceIndex);
  EXPECT_NE(A.Functions[2].NameBinding, NoSourceIndex);
  EXPECT_NE(A.Functions[2].InitializerBinding, NoSourceIndex);
  EXPECT_NE(A.Functions[2].NameBinding, A.Functions[2].InitializerBinding);
  EXPECT_EQ(A.Functions[3].NameBinding, NoSourceIndex);
  EXPECT_NE(A.Functions[3].InitializerBinding, NoSourceIndex);
  ASSERT_EQ(A.Calls.size(), 5);
  const std::vector<uint32_t> Expected{2, 2, 3, 0, NoSourceIndex};
  for (size_t C = 0; C < A.Calls.size(); ++C) {
    EXPECT_EQ(A.Calls[C].EnclosingFunction, Expected[C]);
    EXPECT_NE(A.Calls[C].Reference, NoSourceIndex);
    EXPECT_EQ(A.Calls[C].SyntacticFunction, NoSourceIndex);
  }
  EXPECT_EQ(A.References.size(), I.Bindings.References.size());
  for (const auto &R : A.References) {
    const auto &Ref = I.Bindings.References[R.Reference];
    if (R.EnclosingFunction != NoSourceIndex) {
      const auto &F = I.Source.Nodes[A.Functions[R.EnclosingFunction].Node];
      EXPECT_GE(I.Source.Nodes[Ref.Node].Start, F.Start);
      EXPECT_LE(I.Source.Nodes[Ref.Node].End, F.End);
    }
  }
  const auto Again = analyzeSourceNavigation(I.Source, I.Bindings);
  EXPECT_EQ(Again.ID, A.ID);
  EXPECT_EQ(Again.Functions[2].ID, A.Functions[2].ID);
}

TEST(WebSourceNavigation, DynamicAndReassignedBindingsNeverBecomeCallTargets) {
  Inspection I(R"JS(let f = () => 1; f = other; f();
    { let f = function () {}; f(); }
    with (holder) { f(); }
    obj.method(); unknown(); (function direct() {})();
  )JS");
  const auto &A = I.Navigation;
  ASSERT_EQ(A.Functions.size(), 3);
  ASSERT_EQ(A.Calls.size(), 6);
  const auto &First = I.Bindings.References[A.Calls[0].Reference];
  EXPECT_EQ(First.Binding, A.Functions[0].InitializerBinding);
  EXPECT_EQ(First.Resolution, "lexical_binding");
  // An initializer relationship survives as source evidence even after a write.
  EXPECT_EQ(A.Calls[0].SyntacticFunction, NoSourceIndex);
  const auto &Shadow = I.Bindings.References[A.Calls[1].Reference];
  EXPECT_EQ(Shadow.Binding, A.Functions[1].InitializerBinding);
  EXPECT_NE(Shadow.Binding, First.Binding);
  EXPECT_EQ(I.Bindings.References[A.Calls[2].Reference].Resolution,
            "dynamic_with");
  EXPECT_EQ(A.Calls[3].Reference, NoSourceIndex);
  EXPECT_EQ(I.Bindings.References[A.Calls[4].Reference].Resolution, "external");
  EXPECT_EQ(A.Calls[5].SyntacticFunction, 2);
  EXPECT_EQ(A.Calls[5].EnclosingFunction, NoSourceIndex);
}

TEST(WebSourceNavigation,
     SyntaxKindsCoverMethodsConstructorsTagsOptionalCallsAndImports) {
  Inspection I(R"JS(export default function () {}
    class C { method(x = init()) { return x; } static #p = setup(); }
    async function* g() { yield await work?.(); }
    new C(); tag`private`; import('private');
  )JS",
               "module");
  EXPECT_EQ(I.Navigation.Functions.size(), 3);
  bool Optional = false, Construct = false, Tagged = false, Import = false;
  for (const auto &C : I.Navigation.Calls) {
    Optional |= C.Kind == "optional_call";
    Construct |= C.Kind == "construct";
    Tagged |= C.Kind == "tagged_template";
    Import |= C.Kind == "dynamic_import";
    if (C.Kind == "dynamic_import") {
      EXPECT_EQ(C.Callee, NoSourceIndex);
      EXPECT_EQ(C.Reference, NoSourceIndex);
      EXPECT_EQ(C.SyntacticFunction, NoSourceIndex);
      EXPECT_NE(C.Argument, NoSourceIndex);
    }
    if (I.text(C.Node) == "init()")
      EXPECT_EQ(C.EnclosingFunction, 1);
    if (I.text(C.Node) == "setup()")
      EXPECT_EQ(C.EnclosingFunction, NoSourceIndex);
  }
  EXPECT_TRUE(Optional && Construct && Tagged && Import);
  const auto &G = I.Source.Nodes[I.Navigation.Functions.back().Node];
  EXPECT_TRUE(G.flag("async"));
  EXPECT_TRUE(G.flag("generator"));
}

TEST(WebSourceNavigation, UnavailableBindingsKeepSyntaxAndRemoveLexicalLinks) {
  Inspection I("function f() { return f(); } f();");
  auto B = I.Bindings;
  B.Status = "budget_exceeded";
  const auto A = analyzeSourceNavigation(I.Source, B);
  EXPECT_EQ(A.Status, "partial");
  EXPECT_EQ(A.BindingStatus, "budget_exceeded");
  EXPECT_EQ(A.Functions.size(), 1);
  ASSERT_EQ(A.Calls.size(), 2);
  EXPECT_TRUE(A.References.empty());
  EXPECT_EQ(A.Functions[0].NameBinding, NoSourceIndex);
  EXPECT_EQ(A.Calls[0].Reference, NoSourceIndex);
  EXPECT_NE(A.ID, I.Navigation.ID);
}

TEST(WebSourceNavigation, InvalidModelsCannotPublishMisleadingPartialIndexes) {
  Inspection I("function f() { return f(); } f();");
  auto B = I.Bindings;
  B.SourceID = "other-source";
  auto A = analyzeSourceNavigation(I.Source, B);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Functions.empty());
  B = I.Bindings;
  B.References[0].Node = NoSourceIndex;
  A = analyzeSourceNavigation(I.Source, B);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Calls.empty());
  auto S = I.Source;
  S.Nodes[0].Children[0].Index = 0;
  A = analyzeSourceNavigation(S, I.Bindings);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.References.empty());
}
} // namespace
