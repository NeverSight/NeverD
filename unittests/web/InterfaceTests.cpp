//===- InterfaceTests.cpp - Passive HAR and endpoint qualification -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inert captures, malformed inputs, private URL keys and hard budgets.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/Interfaces.h"

using namespace neverd::web;

namespace {
std::string har(std::string Entries) {
  return R"({"log":{"version":"1.2","entries":[)" + Entries + "]}}";
}
} // namespace

TEST(WebInterfaces, HARPreservesOccurrencesAndAvailableFields) {
  const auto Bytes = har(R"({
    "startedDateTime":"2024-02-29T23:59:59.123+08:00",
    "_requestId":"PRIVATE_VENDOR_ID",
    "request":{"method":"POST",
      "url":"https://private.example/v1/users?token=PRIVATE_QUERY#PRIVATE_FRAGMENT",
      "headers":[{"name":"Authorization","value":"PRIVATE_AUTH"},
                 {"name":"Content-Type","value":"PRIVATE_TYPE"},
                 {"name":"PRIVATE_HEADER","value":"PRIVATE_VALUE"}],
      "cookies":[{"name":"PRIVATE_COOKIE","value":"PRIVATE_COOKIE_VALUE"}],
      "queryString":[{"name":"PRIVATE_PARAM","value":"PRIVATE_PARAM_VALUE"}],
      "postData":{"text":"PRIVATE_REQUEST_BODY"}},
    "response":{"status":201,
      "headers":[{"name":"Set-Cookie","value":"PRIVATE_RESPONSE_COOKIE"}],
      "cookies":[],
      "content":{"text":"PRIVATE_RESPONSE_BODY","encoding":"base64"}},
    "_webSocketMessages":[{"data":"PRIVATE_FRAME"}]
  },{"request":{"method":"GET","url":"https://private.example/v1/users"}})");
  const auto A = inspectHAR("artifact", Bytes);
  ASSERT_EQ(A.Observations.size(), 2u);
  EXPECT_EQ(A.BlobHash, sha256(Bytes));
  EXPECT_EQ(A.MissingResponses, 1u);
  EXPECT_EQ(A.MissingTimestamps, 1u);
  EXPECT_EQ(A.ExtensionFields, 2u);
  EXPECT_FALSE(A.CreatorPresent);
  const auto &O = A.Observations[0];
  EXPECT_NE(O.ID, A.Observations[1].ID);
  EXPECT_NE(O.RequestID, O.ResponseID);
  EXPECT_EQ(O.Method, "POST");
  EXPECT_EQ(O.Status, 201);
  EXPECT_EQ(O.Endpoint.Origin, "https://private.example");
  EXPECT_EQ(O.Endpoint.Path, "/v1/users");
  EXPECT_EQ(O.Endpoint.QueryParameters, 1u);
  EXPECT_EQ(O.Endpoint.PathSegments, 2u);
  EXPECT_TRUE(O.Endpoint.FragmentPresent);
  EXPECT_EQ(O.RequestHeaders.HeaderKinds.at("sensitive"), 1u);
  EXPECT_EQ(O.RequestHeaders.HeaderKinds.at("other"), 1u);
  EXPECT_TRUE(O.RequestTextPresent);
  EXPECT_TRUE(O.ResponseTextPresent);
  EXPECT_EQ(O.ResponseEncoding, "base64_not_decoded");
  EXPECT_TRUE(O.WebSocketExtensionPresent);
  EXPECT_EQ(inspectHAR("artifact", Bytes).ID, A.ID);
  const auto BOM = inspectHAR("artifact", "\xef\xbb\xbf" + Bytes);
  EXPECT_NE(BOM.ID, A.ID);
  EXPECT_EQ(BOM.Observations[0].Method, O.Method);
}

TEST(WebInterfaces, PartialCaptureIsNotCompleteOrAuthenticatedTraffic) {
  const auto A = inspectHAR(
      "artifact", har(R"({},{"startedDateTime":"PRIVATE_TIME","request":{
        "headers":[{"name":"PRIVATE_NAME"}],"method":"PRIVATE_METHOD",
        "url":"/relative/private"},"response":{"status":0,"content":{}}})"));
  ASSERT_EQ(A.Observations.size(), 2u);
  EXPECT_EQ(A.MissingRequests, 1u);
  EXPECT_EQ(A.MissingResponses, 1u);
  EXPECT_EQ(A.InvalidTimestamps, 1u);
  const auto &O = A.Observations[1];
  EXPECT_EQ(O.Method, "other");
  EXPECT_EQ(O.Endpoint.Status, "relative_unresolved");
  EXPECT_TRUE(O.Timestamp.empty());
  EXPECT_EQ(O.RequestHeaders.Incomplete, 1u);
  EXPECT_TRUE(O.ResponseContentPresent);
  EXPECT_FALSE(O.ResponseTextPresent);
  EXPECT_EQ(O.Status, 0);
}

TEST(WebInterfaces, TimestampsUseAValidatedNarrowCalendarProfile) {
  for (const auto *Time :
       {"2024-02-29T01:02:03Z", "2000-02-29T01:02:03.123456789-07:30"}) {
    const auto A = inspectHAR(
        "a", har(std::string("{\"startedDateTime\":\"") + Time + "\"}"));
    EXPECT_EQ(A.Observations[0].Timestamp, Time);
  }
  for (const auto *Time :
       {"1900-02-29T01:02:03Z", "2024-02-30T01:02:03Z", "2024-01-01T24:00:00Z",
        "2024-01-01T01:00:60Z", "2024-01-01T01:00:00.Z",
        "2024-01-01T01:00:00+24:00", "2024-01-01T01:00:00.1234567890Z",
        "PRIVATE_CANARY"}) {
    const auto A = inspectHAR(
        "a", har(std::string("{\"startedDateTime\":\"") + Time + "\"}"));
    EXPECT_TRUE(A.Observations[0].Timestamp.empty());
    EXPECT_EQ(A.InvalidTimestamps, 1u);
  }
}

TEST(WebInterfaces, MalformedAndContradictoryHARFailAtomically) {
  for (
      const auto *Bytes :
      {R"({"log":{"version":"1.1","entries":[]}})",
       R"({"log":{"version":"1.2","entries":{}}})",
       R"({"log":{"version":"1.2","entries":[{},null]}})",
       R"({"log":{"version":"1.2","entries":[{"request":null}]}})",
       R"({"log":{"version":"1.2","entries":[{"request":{"method":1}}]}})",
       R"({"log":{"version":"1.2","entries":[{"response":{"status":999}}]}})",
       R"({"log":{"version":"1.2","entries":[{"response":{"status":200.5}}]}})",
       R"({"log":{"version":"1.2","entries":[{"request":{"headers":[1]}}]}})",
       R"({"log":{"version":"1.2","entries":[{"request":{"postData":{"text":"","params":[]}}}]}})",
       R"({"log":{"version":"1.2","entries":[],"entri\u0065s":[]}})",
       R"({"log":{"version":"1.2","entries":[{})"}) {
    EXPECT_THROW(inspectHAR("a", Bytes), Error);
  }
}

TEST(WebInterfaces, URLNormalizationRetainsOnlyPrivateComparisonMaterial) {
  const auto A = inspectInterfaceEndpoint(
      "HTTPS://Example.COM:443/a/../b/%2f?token=PRIVATE#PRIVATE_FRAGMENT");
  EXPECT_TRUE(A.comparable());
  EXPECT_EQ(A.Origin, "https://example.com");
  EXPECT_EQ(A.Path, "/b/%2f");
  EXPECT_EQ(A.QueryParameters, 1u);
  const auto B =
      inspectInterfaceEndpoint("https://USER:PASS@example.com/b/%2f");
  EXPECT_TRUE(B.CredentialsPresent);
  EXPECT_FALSE(B.comparable());
  EXPECT_EQ(B.Origin, A.Origin);
  EXPECT_EQ(B.Path, A.Path);
  EXPECT_FALSE(inspectInterfaceEndpoint("wss://example.com/b").comparable());
  EXPECT_EQ(inspectInterfaceEndpoint("javascript:PRIVATE").Status,
            "unsupported_scheme");
  EXPECT_EQ(inspectInterfaceEndpoint("https://example.com/a\nb").Status,
            "invalid_url");
  EXPECT_EQ(inspectInterfaceEndpoint("//example.com/path").Status,
            "relative_unresolved");
  for (const auto *URL :
       {"https:example.com/path", "https:/example.com/path",
        "https:///example.com/path", "https:\\example.com/path"})
    EXPECT_FALSE(inspectInterfaceEndpoint(URL).comparable());
  EXPECT_EQ(interfaceMethod("post", true), "POST");
  EXPECT_EQ(interfaceMethod("post"), "other");
  EXPECT_EQ(interfaceMethod("patch", true), "other");
  EXPECT_EQ(interfaceMethod("PATCH", true), "PATCH");
  EXPECT_EQ(interfaceHeaderKind("PRIVATE_HEADER"), "other");
}

TEST(WebInterfaces, AdmissionBudgetsBoundEntriesFieldsBytesAndURLs) {
  EXPECT_THROW(inspectHAR("a", std::string(MaxHARBytes + 1, 'x')), Error);
  std::string Entries;
  for (uint64_t I = 0; I <= MaxInterfaceRecords; ++I) {
    if (I)
      Entries += ',';
    Entries += "{}";
  }
  EXPECT_THROW(inspectHAR("a", har(Entries)), Error);
  std::string Fields;
  for (uint64_t I = 0; I <= MaxInterfaceFields; ++I) {
    if (I)
      Fields += ',';
    Fields += "{}";
  }
  EXPECT_THROW(
      inspectHAR("a", har("{\"request\":{\"headers\":[" + Fields + "]}}")),
      Error);
  EXPECT_THROW(
      inspectInterfaceEndpoint(std::string(MaxInterfaceURLBytes + 1, 'x')),
      Error);
  std::string Expanded = "https://example.com/";
  for (unsigned I = 0; I < 3000; ++I)
    Expanded += "\xe4\xb8\xad";
  EXPECT_LT(Expanded.size(), MaxInterfaceURLBytes);
  EXPECT_THROW(inspectInterfaceEndpoint(Expanded), Error);
}
