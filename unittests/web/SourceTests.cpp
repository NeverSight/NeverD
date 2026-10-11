//===- SourceTests.cpp - JavaScript source evidence tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JavaScript source evidence tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/Source.h"

#include <algorithm>
#include <string>

namespace {
using namespace neverd::web;

SourceAnalysis inspect(std::string_view Text,
                       std::string_view Type = "script") {
  return inspectJavaScript(identity("test-source", {Text}), Text, Type);
}

TEST(WebSource, ModernSyntaxIsParsedWithoutRunningTheProgram) {
  const auto Result = inspect(R"JS(
    // This is data. Running it would never return.
    while (true) { process.exit(123); }
    const match = /[a-z]+/giu;
    const result = object?.child ?? 42;
    class Example { #value = 1; read() { return this.#value; } }
    async function f(...args) { return await missing(...args); }
    eval('throw "must not execute"');
    require('/never-read-this-target');
  )JS");
  EXPECT_EQ(Result.ParseStatus, "parsed");
  EXPECT_EQ(Result.CommentCount, 1);
  EXPECT_TRUE(Result.Diagnostics.empty());
  EXPECT_GT(Result.Nodes.size(), 20);
}

TEST(WebSource, ByteSpansAndParentIdentityReferToOriginalUtf8) {
  const std::string Text = "\xEF\xBB\xBF"
                           "const text = \"\xF0\x9F\x98\x80\";\r\ntext;";
  const auto Result = inspect(Text);
  ASSERT_EQ(Result.ParseStatus, "parsed");
  ASSERT_FALSE(Result.Nodes.empty());
  EXPECT_TRUE(Result.Nodes.front().ParentID.empty());
  bool SawLiteral = false;
  for (const auto &Node : Result.Nodes) {
    ASSERT_LE(Node.Start, Node.End);
    ASSERT_LE(Node.End, Text.size());
    if (!Node.ParentID.empty()) {
      const auto Parent =
          std::find_if(Result.Nodes.begin(), Result.Nodes.end(),
                       [&](const auto &P) { return P.ID == Node.ParentID; });
      ASSERT_NE(Parent, Result.Nodes.end());
      EXPECT_LE(Parent->Start, Node.Start);
      EXPECT_GE(Parent->End, Node.End);
    }
    if (Node.Kind == "StringLiteral") {
      SawLiteral = true;
      EXPECT_EQ(Text.substr(Node.Start, Node.End - Node.Start),
                "\"\xF0\x9F\x98\x80\"");
    }
  }
  EXPECT_TRUE(SawLiteral);
  const auto Again = inspect(Text);
  ASSERT_EQ(Result.Nodes.size(), Again.Nodes.size());
  for (size_t I = 0; I < Result.Nodes.size(); ++I)
    EXPECT_EQ(Result.Nodes[I].ID, Again.Nodes[I].ID);
  EXPECT_NE(Result.ID,
            inspectJavaScript("another-occurrence", Text, "script").ID);
}

TEST(WebSource, ExplicitSourceTypeControlsModuleAndReturnSyntax) {
  const auto Module = "import x from 'not-resolved'; export {x};";
  EXPECT_EQ(inspect(Module, "module").ParseStatus, "parsed");
  EXPECT_NE(inspect(Module, "script").ParseStatus, "parsed");
  EXPECT_NE(inspect(Module, "commonjs").ParseStatus, "parsed");
  EXPECT_EQ(inspect("return 1;", "commonjs").ParseStatus, "parsed");
  EXPECT_NE(inspect("return 1;", "script").ParseStatus, "parsed");
  EXPECT_THROW(inspect("let x = 1;", "automatic"), Error);
  EXPECT_NE(inspect("with (obj) {}", "module").ParseStatus, "parsed");
  EXPECT_NE(inspect("'use strict'; with (obj) {}").ParseStatus, "parsed");
  EXPECT_NE(inspect("function f() { 'use strict'; with (obj) {} }").ParseStatus,
            "parsed");
  EXPECT_EQ(inspect("with (obj) {}").ParseStatus, "parsed");
}

TEST(WebSource, AsyncArrowRestParametersKeepParserOwnedLocations) {
  const std::string Text =
      "const f=async (x, ... /* rest */ args)=>async(...more)=>args;";
  const auto Result = inspect(Text, "module");
  ASSERT_EQ(Result.ParseStatus, "parsed");
  size_t RestCount = 0;
  for (const auto &N : Result.Nodes) {
    ASSERT_LE(N.Start, N.End);
    ASSERT_LE(N.End, Text.size());
    if (N.Kind != "RestElement")
      continue;
    const auto Spelling = Text.substr(N.Start, N.End - N.Start);
    EXPECT_TRUE(Spelling == "... /* rest */ args" || Spelling == "...more");
    ++RestCount;
  }
  EXPECT_EQ(RestCount, 2);
  // A source-location fix must not relax the parameter grammar.
  for (const auto Invalid : {"async (...rest, x)=>x", "async (...rest,)=>x",
                             "async (...rest /*comment*/,)=>x"})
    EXPECT_NE(inspect(Invalid, "module").ParseStatus, "parsed") << Invalid;
  EXPECT_EQ(inspect("async(...rest,)", "module").ParseStatus, "parsed");
  EXPECT_EQ(
      inspect("const f=async(... /* , */ rest)=>rest", "module").ParseStatus,
      "parsed");
}

TEST(WebSource, ResourceDeclarationsKeepKindsAndOriginalLocations) {
  const std::string Text =
      "using /*sync*/ first = get(), second = null;"
      "await /*async*/ using third = await get();"
      "async function f(){for(await using item of items){use(item)}"
      "for await(using item of items){use(item)}"
      "for(using once=get();test();step()){use(once)}}";
  const auto S = inspect(Text, "module");
  ASSERT_EQ(S.ParseStatus, "parsed");
  ASSERT_EQ(S.LexemeStatus, "ok");
  size_t Sync = 0, Async = 0;
  for (const auto &N : S.Nodes) {
    ASSERT_LE(N.Start, N.End);
    ASSERT_LE(N.End, Text.size());
    if (N.Kind != "VariableDeclaration")
      continue;
    ASSERT_NE(N.text("kind"), nullptr);
    const auto Spelling = Text.substr(N.Start, N.End - N.Start);
    if (*N.text("kind") == u"using") {
      EXPECT_TRUE(Spelling.starts_with("using"));
      ++Sync;
    } else {
      EXPECT_EQ(*N.text("kind"), u"await using");
      EXPECT_TRUE(Spelling.starts_with("await"));
      ++Async;
    }
  }
  EXPECT_EQ(Sync, 3);
  EXPECT_EQ(Async, 2);
}

TEST(WebSource, ResourceGrammarDistinguishesContextsAndOrdinaryIdentifiers) {
  for (const auto Text :
       {"{using x=null;}", "function f(){using x=null;}",
        "class C {static {using x=null;}}", "for(using x=null;false;){}",
        "for(using x of values){}", "for(using of values){}", "using[x]=value;",
        "let using; using = 1; using(x); using: while(false){break using;}",
        "function f(){using await=null;using yield=null;}", "using\nvalue=1;",
        "using/*\n*/value=1;", "using\xE2\x80\xA8value=1;",
        "async function f(){await using; await using\nvalue=1;}",
        "async function f(){for await(await using x of values){}}"}) {
    const auto S = inspect(Text);
    EXPECT_EQ(S.ParseStatus, "parsed") << Text;
    EXPECT_EQ(S.LexemeStatus, "ok") << Text;
  }
  EXPECT_EQ(inspect("using x=null;", "commonjs").ParseStatus, "parsed");
  EXPECT_EQ(inspect("using x=null;", "module").ParseStatus, "parsed");
  EXPECT_EQ(inspect("await using x=null;", "module").ParseStatus, "parsed");
  EXPECT_EQ(inspect("await value;", "module").ParseStatus, "parsed");
  EXPECT_NE(inspect("using x=null;", "script").ParseStatus, "parsed");
  EXPECT_NE(inspect("await using x=null;", "commonjs").ParseStatus, "parsed");
}

TEST(WebSource, MalformedResourceDeclarationsPublishNoPartialModel) {
  for (const auto Text : {"using x;",
                          "using x=null,y;",
                          "using x=null,x=null;",
                          "using let=null;",
                          "using {x}=value;",
                          "using x=null,[y]=value;",
                          "using x=null,{y}=value;",
                          "using x=null,;",
                          "us\\u0069ng x=null;",
                          "await us\\u0069ng x=null;",
                          "aw\\u0061it using x=null;",
                          "await\nusing x=null;",
                          "await/*\n*/using x=null;",
                          "if(true)using x=null;",
                          "label:using x=null;",
                          "export using x=null;",
                          "export await using x=null;",
                          "function f(){await using x=null;}",
                          "class C {static {await using x=null;}}",
                          "for(using x in values){}",
                          "for(using x=null in values){}",
                          "for(using x=null of values){}",
                          "for(using x,y of values){}",
                          "for(using of of values){}",
                          "for(using x;;){}",
                          "for(await using x in values){}",
                          "for(await using x;;){}",
                          "for(using\nx of values){}",
                          "for(await\nusing x of values){}"}) {
    const auto S = inspect(Text, "module");
    EXPECT_NE(S.ParseStatus, "parsed") << Text;
    EXPECT_TRUE(S.Nodes.empty()) << Text;
    EXPECT_FALSE(S.Diagnostics.empty()) << Text;
  }
}

TEST(WebSource, MalformedAndUnsupportedSyntaxDoesNotPublishPartialNodes) {
  for (const auto Text : {"let = ;", "function {", "const a: number = 1;",
                          "const a = <div/>;", "throw\n1;", "break;"}) {
    const auto Result = inspect(Text);
    EXPECT_NE(Result.ParseStatus, "parsed") << Text;
    EXPECT_TRUE(Result.Nodes.empty()) << Text;
    EXPECT_FALSE(Result.Diagnostics.empty()) << Text;
  }
}

TEST(WebSource, UntrustedMessagesNeverBecomeDiagnosticText) {
  const std::string Canary = "SECRET_DIAGNOSTIC_CANARY_137";
  const auto Result = inspect("const " + Canary + " = ;");
  EXPECT_NE(Result.ParseStatus, "parsed");
  ASSERT_FALSE(Result.Diagnostics.empty());
  for (const auto &Diagnostic : Result.Diagnostics) {
    EXPECT_EQ(Diagnostic.Code.find(Canary), std::string::npos);
    EXPECT_TRUE(Diagnostic.ByteOffset == -1 ||
                uint64_t(Diagnostic.ByteOffset) <= Canary.size() + 10);
  }
}

TEST(WebSource, BoundsEncodingAndRecursionAreExplicit) {
  EXPECT_EQ(inspect(std::string("x\0y", 3)).ParseStatus,
            "unsupported_encoding");
  EXPECT_EQ(inspect(std::string("\xF0\x80\x80\x80", 4)).ParseStatus,
            "unsupported_encoding");
  EXPECT_THROW(inspect(std::string(MaxJavaScriptBytes + 1, ' ')), Error);
  const auto Deep =
      inspect(std::string(1024, '(') + "0" + std::string(1024, ')'));
  EXPECT_NE(Deep.ParseStatus, "parsed");
  EXPECT_TRUE(Deep.Nodes.empty());
  // Empty input is a successfully parsed, empty program, not a missing file.
  const auto Empty = inspect("");
  ASSERT_EQ(Empty.ParseStatus, "parsed");
  ASSERT_EQ(Empty.Nodes.size(), 1);
  EXPECT_EQ(Empty.Nodes[0].Start, 0);
  EXPECT_EQ(Empty.Nodes[0].End, 0);
}

TEST(WebSource, SyntaxInventoryPreservesSparsePatternsAndComputedMembers) {
  const std::string Text = R"JS(
    const [, second, ...rest] = input;
    const sparse = [,, second, ...rest];
    const { [key()]: value = fallback(), ...remaining } = input;
    const object = { second, ['x' + second]: value, get x() { return value; } };
    try { throw sparse; } catch ({message}) { sink(message); }
    outer: for (const item of rest) { if (item) break outer; }
  )JS";
  const auto Result = inspect(Text);
  ASSERT_EQ(Result.ParseStatus, "parsed");
  bool SawPattern = false, SawProperty = false;
  for (const auto &Node : Result.Nodes) {
    EXPECT_LE(Node.Start, Node.End);
    EXPECT_LE(Node.End, Text.size());
    SawPattern |= Node.Kind == "ArrayPattern";
    SawProperty |= Node.Kind == "Property";
  }
  EXPECT_TRUE(SawPattern);
  EXPECT_TRUE(SawProperty);
}

TEST(WebSource, ChildRolesDistinguishShorthandOccurrencesAndSparseOrdinals) {
  const auto Result = inspect("const x = 1; const a = {x}; const b = [,x,,];");
  ASSERT_EQ(Result.ParseStatus, "parsed");
  bool SawProperty = false, SawArray = false;
  for (const auto &N : Result.Nodes) {
    for (const auto &C : N.Children) {
      ASSERT_LT(C.Index, Result.Nodes.size());
      EXPECT_EQ(Result.Nodes[C.Index].ParentID, N.ID);
    }
    if (N.Kind == "Property") {
      ASSERT_EQ(N.Children.size(), 2);
      EXPECT_EQ(N.Children[0].Field, "key");
      EXPECT_EQ(N.Children[1].Field, "value");
      const auto &Key = Result.Nodes[N.Children[0].Index];
      const auto &Value = Result.Nodes[N.Children[1].Index];
      EXPECT_EQ(Key.Start, Value.Start);
      EXPECT_EQ(Key.End, Value.End);
      EXPECT_NE(Key.ID, Value.ID);
      EXPECT_TRUE(N.flag("shorthand"));
      SawProperty = true;
    }
    if (N.Kind == "ArrayExpression") {
      ASSERT_EQ(N.Children.size(), 3);
      for (uint32_t I = 0; I != 3; ++I) {
        EXPECT_EQ(N.Children[I].Field, "elements");
        EXPECT_EQ(N.Children[I].Ordinal, I);
        EXPECT_EQ(Result.Nodes[N.Children[I].Index].Kind,
                  I == 1 ? "Identifier" : "Empty");
      }
      SawArray = true;
    }
  }
  EXPECT_TRUE(SawProperty);
  EXPECT_TRUE(SawArray);
}

TEST(WebSource, PrivateValuesPreserveUtf16IncludingLoneSurrogatesAndNul) {
  const auto Result = inspect(
      R"JS(const 𝒜 = ["😀", "\uD83D\uDE00", "\uD800", "\u0000", true, 1.5]; \u{1D49C};)JS");
  ASSERT_EQ(Result.ParseStatus, "parsed");
  std::vector<std::u16string> Names, Strings;
  bool SawBoolean = false, SawNumber = false;
  for (const auto &N : Result.Nodes) {
    if (N.Kind == "Identifier") {
      ASSERT_NE(N.text("name"), nullptr);
      Names.push_back(*N.text("name"));
    } else if (N.Kind == "StringLiteral") {
      ASSERT_NE(N.text("value"), nullptr);
      Strings.push_back(*N.text("value"));
    } else if (N.Kind == "BooleanLiteral") {
      EXPECT_TRUE(N.flag("value"));
      SawBoolean = true;
    } else if (N.Kind == "NumericLiteral") {
      ASSERT_NE(N.attribute("value"), nullptr);
      EXPECT_EQ(std::get<double>(N.attribute("value")->Value), 1.5);
      SawNumber = true;
    }
  }
  EXPECT_EQ(Names, (std::vector<std::u16string>{u"𝒜", u"𝒜"}));
  EXPECT_EQ(Strings, (std::vector<std::u16string>{
                         u"😀", u"😀", std::u16string(1, char16_t(0xD800)),
                         std::u16string(1, char16_t(0))}));
  EXPECT_TRUE(SawBoolean);
  EXPECT_TRUE(SawNumber);
}
} // namespace
