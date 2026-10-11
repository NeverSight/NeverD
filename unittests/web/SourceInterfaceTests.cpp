//===- SourceInterfaceTests.cpp - Request inference and join boundaries ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source-only, HAR correlation, uncertain dispatch and fanout regressions.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Bun.h"
#include "neverd/web/Error.h"
#include "neverd/web/Interfaces.h"

#include <cstdlib>
using namespace neverd::web;

namespace {
SourceInterfaces analyze(std::string_view Bytes) {
  const auto S = inspectJavaScript("a", Bytes, "module");
  EXPECT_EQ(S.ParseStatus, "parsed");
  return analyzeSourceInterfaces(S, analyzeSourceBindings(S),
                                 analyzeSourceValues(S));
}
HARCapture capture(std::string Entries) {
  return inspectHAR("h",
                    R"({"log":{"version":"1.2","entries":[)" + Entries + "]}}");
}
} // namespace

TEST(WebSourceInterfaces, LiteralAndTemplateRequestsKeepExactSourceAnchors) {
  const std::string Code = R"(
fetch('https://example.com/' + 'a', {method:'post', headers:{}, body:payload});
fetch(`https://example.com/${unknown}`);
fetch(`https://example.com/${'b'}`);
new WebSocket('wss://example.com/socket');
fetch(dynamic);
)";
  const auto S = inspectJavaScript("a", Code, "module");
  const auto A = analyzeSourceInterfaces(S, analyzeSourceBindings(S),
                                         analyzeSourceValues(S));
  ASSERT_EQ(A.Status, "partial");
  ASSERT_EQ(A.Records.size(), 5u);
  EXPECT_EQ(A.Records[0].Method, "POST");
  EXPECT_EQ(A.Records[0].Endpoint.Path, "/a");
  EXPECT_NE(A.Records[0].BodyNode, NoSourceIndex);
  EXPECT_EQ(A.Records[1].URLStatus, "unresolved_template");
  EXPECT_EQ(A.Records[1].TemplateHoles, 1u);
  EXPECT_EQ(A.Records[2].Endpoint.Path, "/b");
  EXPECT_EQ(A.Records[3].Kind, "websocket");
  EXPECT_EQ(A.Records[4].URLStatus, "dynamic_or_unavailable");
  for (const auto &R : A.Records) {
    ASSERT_LT(R.Node, S.Nodes.size());
    ASSERT_LT(R.URLNode, S.Nodes.size());
    const auto &Call = S.Nodes[R.Node];
    EXPECT_GT(Call.End, Call.Start);
    EXPECT_LT(Call.Start, Code.size());
    EXPECT_GE(S.Nodes[R.URLNode].Start, Call.Start);
    EXPECT_LE(S.Nodes[R.URLNode].End, Call.End);
  }
}

TEST(WebSourceInterfaces, ShadowedWrittenAndDynamicCalleesAreNotGlobals) {
  EXPECT_TRUE(
      analyze("function f(fetch) { fetch('https://x/a'); }").Records.empty());
  EXPECT_TRUE(
      analyze("fetch = replacement; fetch('https://x/a');").Records.empty());
  EXPECT_TRUE(analyze("function f(WebSocket) { new WebSocket('wss://x'); }")
                  .Records.empty());
  const auto Eval = analyze("eval(code); fetch('https://x/a');");
  EXPECT_EQ(Eval.Status, "unavailable");
  EXPECT_EQ(Eval.Reason, "interface_source_dynamic_eval");
  EXPECT_TRUE(Eval.Records.empty());
  EXPECT_TRUE(
      analyze("client.fetch('https://x/a'); new client.WebSocket('wss://x');")
          .Records.empty());
}

TEST(WebSourceInterfaces, OptionsDoNotGuessThroughInheritanceOrEffects) {
  const auto A = analyze(R"(
fetch('https://x/a', {});
fetch('https://x/a', {__proto__:{method:'POST'}});
fetch('https://x/a', {...options,method:'POST'});
fetch('https://x/a', {get method(){return 'POST'}});
fetch('https://x/a', {method:'POST',method:'GET'});
fetch('https://x/a', {['method']:'POST'});
fetch('https://x/a', {method:'patch'});
fetch('https://x/a', {method:'TRACE'});
fetch('https://x/a');
)");
  ASSERT_EQ(A.Records.size(), 9u);
  EXPECT_EQ(A.Records[0].MethodStatus, "inherited_method_unknown");
  for (size_t I = 0; I < 6; ++I)
    EXPECT_EQ(A.Records[I].Method, "unknown");
  EXPECT_EQ(A.Records[6].Method, "other");
  EXPECT_EQ(A.Records[7].MethodStatus, "fetch_forbidden_method");
  EXPECT_EQ(A.Records[8].Method, "GET");
  const auto H = capture(
      R"({"request":{"method":"GET","url":"https://x/a"}},{"request":{"method":"TRACE","url":"https://x/a"}})");
  const auto C = correlateInterfaces(A, H);
  ASSERT_EQ(C.Pairs.size(), 1u);
  EXPECT_EQ(C.Pairs[0].Source, 8u);
}

TEST(WebSourceInterfaces, CredentialsAndWebSocketsNeverBecomeHTTPObservations) {
  const auto A = analyze(R"(
fetch('https://USER:PASS@x/a');
new WebSocket('wss://x/a#');
new WebSocket('https://x/a');
fetch('/a');
)");
  ASSERT_EQ(A.Records.size(), 4u);
  EXPECT_EQ(A.Records[0].Endpoint.Status, "fetch_credentials_rejected");
  EXPECT_EQ(A.Records[1].Endpoint.Status, "websocket_fragment_rejected");
  const auto H = capture(R"({"request":{"method":"GET","url":"https://x/a"}})");
  EXPECT_TRUE(correlateInterfaces(A, H).Pairs.empty());
}

TEST(WebSourceInterfaces,
     CorrelationPreservesMultiplicityAndIgnoredQueryValues) {
  const auto A = analyze(R"(
fetch('https://x/a?token=SOURCE');
fetch('https://x/a?other=SOURCE');
fetch('http://x/a');
fetch('https://y/a');
fetch('https://x/A');
fetch('https://x/%61');
fetch('https://x/a',{method:'POST'});
)");
  const auto H = capture(R"(
{"request":{"method":"GET","url":"https://x:443/a?token=CAPTURE"}},
{"request":{"method":"GET","url":"https://x/a?different=CAPTURE"}},
{"request":{"method":"GET","url":"https://USER:PASS@x/a"}},
{"request":{"method":"GET","url":"wss://x/a"}}
)");
  const auto C = correlateInterfaces(A, H);
  ASSERT_EQ(C.Pairs.size(), 4u);
  EXPECT_EQ(C.EligibleObservations, 2u);
  EXPECT_EQ(C.Pairs[0].Source, 0u);
  EXPECT_EQ(C.Pairs[1].Source, 0u);
  EXPECT_EQ(C.Pairs[2].Source, 1u);
  EXPECT_NE(C.Pairs[0].ID, C.Pairs[1].ID);
  EXPECT_EQ(correlateInterfaces(A, H).ID, C.ID);
}

TEST(WebSourceInterfaces, CorrelationFanoutFailsInsteadOfTruncatingResults) {
  std::string Source, Entries;
  for (unsigned I = 0; I < 257; ++I)
    Source += "fetch('https://x/a');";
  for (unsigned I = 0; I < 128; ++I) {
    if (I)
      Entries += ',';
    Entries += R"({"request":{"method":"GET","url":"https://x/a"}})";
  }
  const auto A = analyze(Source);
  ASSERT_EQ(A.Records.size(), 257u);
  const auto H = capture(Entries);
  EXPECT_THROW(correlateInterfaces(A, H), Error);
}

TEST(WebSourceInterfaces, SchemeWithoutAuthorityRequiresAnEnvironmentBase) {
  const auto A = analyze(R"(
fetch('https:api.example/v1');
fetch('https:/api.example/v1');
fetch('https://api.example/v1');
)");
  const auto H =
      capture(R"({"request":{"method":"GET","url":"https://api.example/v1"}})");
  const auto C = correlateInterfaces(A, H);
  ASSERT_EQ(C.Pairs.size(), 1u);
  EXPECT_EQ(C.Pairs[0].Source, 2u);
}

TEST(WebSourceInterfaces,
     ClaudeCode21296KeepsDynamicRequestConstructionUnknown) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path)
    GTEST_SKIP() << "Optional hash-pinned official original not supplied";
  const auto Snapshot = neverd::web::capture(Path, Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
  const auto &Original = Snapshot.Artifacts.front();
  ASSERT_EQ(Original.BlobHash,
            "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
  const auto E = extractBun(Original);
  ASSERT_GT(E.Modules.size(), 1897u);
  const auto &M = E.Modules[1897];
  const auto Bytes = bunSourceBytes(E, M, MaxJavaScriptBytes);
  const auto S = inspectJavaScript(M.SourceArtifactID, Bytes,
                                   M.Format == 1 ? "module" : "commonjs");
  const auto A = analyzeSourceInterfaces(S, analyzeSourceBindings(S),
                                         analyzeSourceValues(S));
  ASSERT_EQ(A.Status, "partial");
  ASSERT_EQ(A.Records.size(), 1u);
  const auto &R = A.Records[0];
  EXPECT_EQ(R.Kind, "fetch");
  EXPECT_EQ(R.URLStatus, "dynamic_or_unavailable");
  EXPECT_EQ(R.Method, "unknown");
  EXPECT_EQ(R.OptionsStatus, "unresolved");
  EXPECT_EQ(S.Nodes[R.Node].Start, 65473u);
  EXPECT_EQ(S.Nodes[R.Node].End - S.Nodes[R.Node].Start, 52u);
  // No capture was supplied and no runtime URL is invented.
  EXPECT_FALSE(R.Endpoint.comparable());
}
