//===- StreamTests.cpp - Passive framing and protocol regressions
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inert C++ fixtures for exact framing, refused joins and whole-input budgets.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/Streams.h"

namespace {
using namespace neverd::web;
constexpr auto Recorded = "recorded-jsonrpc-2.0-jsonl-v1";
StreamCapture read(std::string_view Bytes,
                   std::string_view Profile = "jsonl-metadata-v1") {
  return inspectStream("artifact", Bytes, Profile);
}
std::string envelope(std::string_view Message,
                     std::string_view Direction = "client_to_server",
                     std::string_view Session = "CANARY_SESSION") {
  return "{\"session\":\"" + std::string(Session) + "\",\"direction\":\"" +
         std::string(Direction) +
         "\",\"timestamp\":\"CANARY_TIME\",\"message\":" +
         std::string(Message) + "}\n";
}
std::string request(std::string_view ID = "1") {
  return "{\"jsonrpc\":\"2.0\",\"method\":\"CANARY_METHOD\",\"id\":" +
         std::string(ID) + ",\"params\":{\"CANARY_KEY\":\"CANARY_VALUE\"}}";
}
std::string response(std::string_view ID = "1") {
  return "{\"jsonrpc\":\"2.0\",\"result\":\"CANARY_RESULT\",\"id\":" +
         std::string(ID) + "}";
}
std::string pair(std::string_view ID = "1") {
  return envelope(request(ID)) + envelope(response(ID), "server_to_client");
}
template <class F> void error(F Fn, std::string_view Expected) {
  try {
    Fn();
    FAIL() << "expected failure";
  } catch (const Error &E) {
    EXPECT_EQ(std::string_view(E.what()), Expected);
  }
}

TEST(WebStreams, JSONLFramingDoesNotInventProtocolOrDirection) {
  const std::string Text = request() + "\r\ntrue\n[]\n\"CANARY\"";
  const auto C = read(Text);
  ASSERT_EQ(C.Records.size(), 4u);
  EXPECT_EQ(C.BlobHash, sha256(Text));
  EXPECT_EQ(C.Records[0].RPC, "not_selected");
  EXPECT_EQ(C.Records[0].Direction, "absent");
  EXPECT_EQ(C.Records[1].JSONKind, "boolean");
  EXPECT_EQ(C.Records[2].JSONKind, "array");
  EXPECT_EQ(C.Records[3].JSONKind, "string");
  EXPECT_FALSE(C.Records[3].Terminated);
  uint64_t End = 0;
  for (size_t I = 0; I < C.Records.size(); ++I) {
    const auto &R = C.Records[I];
    EXPECT_EQ(R.Offset, End);
    EXPECT_EQ(R.FirstLine, I + 1);
    End += R.Length;
  }
  EXPECT_EQ(End, Text.size());
  EXPECT_NE(C.ID, read(Text, "jsonrpc-2.0-jsonl-v1").ID);
  EXPECT_NE(C.ID, inspectStream("other", Text, "jsonl-metadata-v1").ID);
}

TEST(WebStreams, MalformedAndUnterminatedJSONAreLocatedLimitations) {
  const auto C = read("\n{\"a\":1,\"a\":2}\n{\"CANARY\":\n{\"incomplete\":");
  ASSERT_EQ(C.Records.size(), 4u);
  EXPECT_EQ(C.Records[0].Kind, "blank_line");
  EXPECT_EQ(C.Records[1].Kind, "malformed_json");
  EXPECT_EQ(C.Records[2].Kind, "malformed_json");
  EXPECT_EQ(C.Records[3].Kind, "possibly_truncated_json");
  EXPECT_GT(C.JSONWork, 0u);
}

TEST(WebStreams, SSEBlankLinesDispatchAndEOFPreservesUnfinishedBlocks) {
  for (const std::string EOL : {"\n", "\r", "\r\n"}) {
    const std::string Text = "\xef\xbb\xbf" + std::string("id: CANARY_ID") +
                             EOL + "data: CANARY_A" + EOL + "data: CANARY_B" +
                             EOL + EOL + "data:" + EOL + EOL + "id:" + EOL +
                             "data: C" + EOL + EOL + "data: unfinished" + EOL;
    const auto C = read(Text, "sse-utf8-metadata-v1");
    ASSERT_EQ(C.Records.size(), 4u);
    EXPECT_TRUE(C.BOM);
    EXPECT_EQ(C.Records[0].Offset, 3u);
    EXPECT_EQ(C.Records[0].DataLines, 2u);
    EXPECT_EQ(C.Records[0].DataBytes, 17u);
    EXPECT_TRUE(C.Records[0].Dispatched);
    EXPECT_TRUE(C.Records[1].Dispatched);
    EXPECT_EQ(C.Records[1].DataBytes, 0u);
    EXPECT_TRUE(C.Records[1].EventIDPresent);
    EXPECT_TRUE(C.Records[2].EventIDReset);
    EXPECT_FALSE(C.Records[2].EventIDPresent);
    EXPECT_FALSE(C.Records[3].Dispatched);
    EXPECT_EQ(C.Records[3].Kind, "sse_incomplete");
    EXPECT_EQ(C.Records[3].Offset + C.Records[3].Length, Text.size());
  }
}

TEST(WebStreams, SSECommentsIDsAndUnknownEventsStayRedactedMetadata) {
  std::string Text = "id: CANARY\n\n: CANARY_COMMENT\nevent: CANARY_EVENT\n"
                     "unknown: CANARY_FIELD\nretry: CANARY_RETRY\nid: ";
  Text.push_back('\0');
  Text += "\ndata: CANARY_DATA\n\n";
  const auto C = read(Text, "sse-utf8-metadata-v1");
  ASSERT_EQ(C.Records.size(), 2u);
  EXPECT_EQ(C.Records[0].Kind, "sse_control");
  EXPECT_EQ(C.Records[1].Comments, 1u);
  EXPECT_EQ(C.Records[1].IgnoredFields, 2u);
  EXPECT_EQ(C.Records[1].InvalidIDFields, 1u);
  EXPECT_EQ(C.Records[1].Event, "custom_redacted");
  EXPECT_TRUE(C.Records[1].EventIDPresent);
  EXPECT_EQ(C.Records[1].RPC, "not_selected");
  EXPECT_EQ(C.PairCount, 0u);
}

TEST(WebStreams, RPCShapeChecksKeepBatchesAndInvalidMessagesExplicit) {
  const auto C =
      read(request() + "\n" + response() +
               "\n{\"jsonrpc\":\"2.0\",\"method\":\"CANARY\"}\n"
               "[]\n{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":0,\"error\":{}}\n"
               "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
               "{\"code\":-32600,\"message\":\"CANARY\"}}\n",
           "jsonrpc-2.0-jsonl-v1");
  ASSERT_EQ(C.Records.size(), 6u);
  EXPECT_EQ(C.Records[0].RPC, "request");
  EXPECT_EQ(C.Records[1].RPC, "response");
  EXPECT_EQ(C.Records[2].RPC, "notification");
  EXPECT_EQ(C.Records[3].RPC, "unsupported_batch");
  EXPECT_EQ(C.Records[4].RPC, "invalid_message");
  EXPECT_EQ(C.Records[5].RPC, "response");
  EXPECT_EQ(C.Records[5].IDKind, "null");
  EXPECT_EQ(C.Records[0].Method, "method_redacted");
  EXPECT_EQ(C.PairCount, 0u);
  EXPECT_EQ(C.RelationStatus, "missing_recorded_context");
}

TEST(WebStreams, MCPSelectionDoesNotProveNegotiationOrValidateMethodSchemas) {
  const auto C = read(
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\","
      "\"params\":{\"protocolVersion\":\"CANARY_OTHER_VERSION\"}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"CANARY\",\"params\":[]}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":0}\n",
      "mcp-2025-06-18-stdio-jsonl-v1");
  ASSERT_EQ(C.Records.size(), 5u);
  EXPECT_EQ(C.Records[0].Method, "tools/call");
  EXPECT_EQ(C.Records[1].Method, "initialize");
  for (unsigned I : {2, 3, 4})
    EXPECT_EQ(C.Records[I].RPC, "invalid_message");
  EXPECT_EQ(C.RelationStatus, "missing_recorded_context");
}

TEST(WebStreams, ErrorCodesUseExactIntegerShapeWithoutTheIDJoinLimit) {
  for (auto Code :
       {"9007199254740992", "-32600.0", "-32600e0", "1.2e1", "0.000e-10"}) {
    const auto Response = std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":"
                                      "{\"message\":\"CANARY\",\"code\":") +
                          Code + "}}";
    const auto C = read(
        envelope(request()) + envelope(Response, "server_to_client"), Recorded);
    ASSERT_EQ(C.Records.size(), 2u);
    EXPECT_EQ(C.Records[1].RPC, "response") << Code;
    EXPECT_EQ(C.PairCount, 1u);
  }
  const auto C =
      read("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"message\":\"CANARY\","
           "\"code\":1.0000000000000001}}\n",
           "jsonrpc-2.0-jsonl-v1");
  EXPECT_EQ(C.Records[0].RPC, "invalid_message");
}

TEST(WebStreams, RecordedRelationsUseTypedIDsDirectionSessionAndOrder) {
  const auto C = read(pair("1") + pair("\"1\""), Recorded);
  ASSERT_EQ(C.Records.size(), 4u);
  EXPECT_EQ(C.PairCount, 2u);
  EXPECT_EQ(C.Records[0].Peer, 1u);
  EXPECT_EQ(C.Records[2].Peer, 3u);
  EXPECT_EQ(C.Records[0].Timestamp, "string_withheld");
  EXPECT_EQ(read(pair() + envelope(request()), Recorded).PairCount, 0u);
  const auto Reverse = read(
      envelope(response(), "server_to_client") + envelope(request()), Recorded);
  EXPECT_EQ(Reverse.PairCount, 0u);
  EXPECT_EQ(Reverse.Records[0].Relation, "response_before_request");
  EXPECT_EQ(read(envelope(request()) + envelope(response(), "client_to_server"),
                 Recorded)
                .PairCount,
            0u);
  EXPECT_EQ(read(envelope(request()) +
                     envelope(response(), "server_to_client", "other"),
                 Recorded)
                .PairCount,
            0u);
}

TEST(WebStreams, NumericIdentityUsesOriginalTokensBeforeDOMRounding) {
  for (auto ID : {"1.0000000000000001", "9007199254740990.5", "1e0",
                  "9007199254740992"}) {
    const auto C = read(pair() + envelope(request(ID)), Recorded);
    EXPECT_EQ(C.PairCount, 0u) << ID;
    EXPECT_EQ(C.RelationStatus, "coverage_gap");
    EXPECT_EQ(C.Records.back().IDKind, "number_unlinkable");
  }
  const auto Escaped =
      read(envelope(R"({"jsonrpc":"2.0","method":"CANARY","\u0069d":1})") +
               envelope(response(), "server_to_client"),
           Recorded);
  EXPECT_EQ(Escaped.PairCount, 1u);
}

TEST(WebStreams, CoverageGapsCannotHideDuplicateRequests) {
  for (const auto &Gap :
       {std::string("CANARY_MALFORMED\n"), envelope("[]"),
        std::string("{\"session\":\"CANARY_SESSION\",\"message\":") +
            request() + "}\n",
        envelope(request(), "CANARY_DIRECTION"),
        envelope(request(), "client_to_server", ""),
        std::string("{\"message\":") + request() + "}\n"}) {
    const auto C = read(pair() + Gap, Recorded);
    EXPECT_EQ(C.PairCount, 0u);
    EXPECT_EQ(C.RelationStatus, "coverage_gap");
    EXPECT_EQ(C.Records[0].Relation, "coverage_gap");
  }
}

TEST(WebStreams, DiagnosticNoiseRemainsOpaqueAndDoesNotBecomeMCP) {
  const auto C = read("CANARY_LOG\n{\"CANARY_KEY\":\"CANARY_VALUE\"}\n" +
                          request() + "\n{\"CANARY_BROKEN\":",
                      "diagnostic-lines-v1");
  ASSERT_EQ(C.Records.size(), 4u);
  EXPECT_EQ(C.Records[0].Kind, "opaque_line");
  EXPECT_EQ(C.Records[1].Kind, "json_value");
  EXPECT_EQ(C.Records[2].RPC, "not_selected");
  EXPECT_EQ(C.Records[3].Kind, "opaque_line");
}

TEST(WebStreams, AdmissionAndAggregateJSONBudgetsFailAtomically) {
  error([] { read("", "CANARY_PROFILE"); }, "unsupported_stream_profile");
  error([] { read("\xff"); }, "invalid_stream_encoding");
  error([] { read("\xef\xbb\xbf{}\n"); }, "unsupported_stream_bom");
  error([] { read(std::string(MaxStreamBytes + 1, ' ')); },
        "stream_byte_budget_exceeded");
  error([] { read(std::string(MaxStreamFragmentBytes + 1, ' ')); },
        "stream_fragment_budget_exceeded");
  error([] { read(std::string(MaxStreamRecords + 1, '\n')); },
        "stream_record_budget_exceeded");
  std::string Array = "[";
  for (unsigned I = 0; I < 60000; ++I)
    Array += "0,";
  Array.back() = ']';
  const auto Valid = Array + "\n";
  error([&] { read(Valid + Valid + Valid + Valid); },
        "stream_json_budget_exceeded");
  Array.back() = ','; // Failure after most nodes still consumes shared budget.
  const auto Bad = Array + "\n";
  error([&] { read(Bad + Bad + Bad + Bad); }, "stream_json_budget_exceeded");
  error([] { read(std::string(33, '[') + "0" + std::string(33, ']')); },
        "stream_json_budget_exceeded");
  const auto LargeIdentity =
      envelope(request(), "client_to_server", std::string(1024, 'x'));
  std::string ManyIdentities;
  for (unsigned I = 0; I < 1100; ++I)
    ManyIdentities += LargeIdentity;
  error([&] { read(ManyIdentities, Recorded); },
        "stream_private_budget_exceeded");
}
} // namespace
