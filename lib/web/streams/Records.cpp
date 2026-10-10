//===- Records.cpp - Redacted JSON and protocol record admission
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict bounded JSON shape inspection without exposing payloads or names.
///
//===----------------------------------------------------------------------===//

#include "../JsonReader.h"
#include "StreamInternal.h"

#include "neverd/web/Error.h"

#include <algorithm>
#include <charconv>

namespace neverd::web::stream {
namespace {
using Value = llvm::json::Value;

bool space(char C) { return C == ' ' || C == '\t' || C == '\r' || C == '\n'; }
std::string_view trim(std::string_view S) {
  while (!S.empty() && space(S.front()))
    S.remove_prefix(1);
  while (!S.empty() && space(S.back()))
    S.remove_suffix(1);
  return S;
}

// Input has already passed the shared JSON grammar and duplicate-key checks.
// This scanner locates a field's ORIGINAL token; DOM numeric conversion cannot
// establish exact integer identity (for example 1.0000000000000001).
size_t stringEnd(std::string_view S, size_t At) {
  ++At;
  while (At < S.size()) {
    if (S[At++] == '"')
      return At;
    if (S[At - 1] == '\\')
      ++At;
  }
  return S.size();
}
size_t valueEnd(std::string_view S, size_t At) {
  if (S[At] == '"')
    return stringEnd(S, At);
  if (S[At] != '{' && S[At] != '[') {
    auto End = S.find_first_of(",}] \t\r\n", At);
    return End == std::string_view::npos ? S.size() : End;
  }
  unsigned Depth = 0;
  for (; At < S.size(); ++At) {
    if (S[At] == '"') {
      At = stringEnd(S, At) - 1;
      continue;
    }
    if (S[At] == '{' || S[At] == '[')
      ++Depth;
    else if ((S[At] == '}' || S[At] == ']') && --Depth == 0)
      return At + 1;
  }
  return S.size();
}
std::string_view token(std::string_view S, llvm::StringRef Field) {
  S = trim(S);
  if (S.empty() || S.front() != '{')
    return {};
  size_t At = 1;
  while (At < S.size()) {
    while (At < S.size() && (space(S[At]) || S[At] == ','))
      ++At;
    if (At == S.size() || S[At] == '}')
      return {};
    const auto KeyEnd = stringEnd(S, At);
    auto Key = llvm::json::parse(llvm::StringRef(S.substr(At, KeyEnd - At)));
    if (!Key) {
      llvm::consumeError(Key.takeError());
      throw Error("stream_token_invariant");
    }
    At = KeyEnd;
    while (space(S[At]))
      ++At;
    ++At; // Validated colon.
    while (space(S[At]))
      ++At;
    const auto End = valueEnd(S, At);
    if (Key->getAsString() == Field)
      return S.substr(At, End - At);
    At = End;
  }
  return {};
}
std::optional<int64_t> integer(std::string_view S) {
  if (S.empty() || S.find_first_of(".eE+") != std::string_view::npos)
    return {};
  int64_t N = 0;
  const auto R = std::from_chars(S.data(), S.data() + S.size(), N);
  if (R.ec != std::errc() || R.ptr != S.data() + S.size() ||
      N < -9007199254740991LL || N > 9007199254740991LL)
    return {};
  return N;
}
// Error codes require an integer Number, without the ID join's safe-integer
// restriction. Decide mathematical integrality from the validated decimal
// spelling, so DOM rounding cannot turn a fractional code into an integer.
bool integralNumber(std::string_view S) {
  if (S.empty() || (S.front() != '-' && (S.front() < '0' || S.front() > '9')))
    return false;
  const auto E = S.find_first_of("eE");
  const auto Mantissa = S.substr(0, E);
  int64_t Exponent = 0;
  if (E != std::string_view::npos) {
    auto Exp = S.substr(E + 1);
    const bool Negative = Exp.front() == '-';
    if (Exp.front() == '-' || Exp.front() == '+')
      Exp.remove_prefix(1);
    for (char C : Exp)
      Exponent = std::min<int64_t>(100000, Exponent * 10 + C - '0');
    if (Negative)
      Exponent = -Exponent;
  }
  const auto Dot = Mantissa.find('.');
  const auto Fraction =
      Dot == std::string_view::npos ? 0 : int64_t(Mantissa.size() - Dot - 1);
  if (Fraction <= Exponent)
    return true;
  int64_t Zeros = 0;
  for (auto I = Mantissa.rbegin(); I != Mantissa.rend(); ++I) {
    if (*I == '.' || *I == '-')
      continue;
    if (*I != '0')
      return Zeros >= Fraction - Exponent;
    ++Zeros;
  }
  return true; // Any decimal spelling of zero is integral.
}
std::string kind(const Value &V) {
  if (V.getAsObject())
    return "object";
  if (V.getAsArray())
    return "array";
  if (V.getAsString())
    return "string";
  if (V.getAsBoolean())
    return "boolean";
  if (V.getAsNumber())
    return "number";
  return "null";
}
std::string method(llvm::StringRef Name, bool MCP) {
  if (!MCP)
    return Name.starts_with("rpc.") ? "reserved_rpc_method" : "method_redacted";
  for (const auto *Known : {"initialize",
                            "ping",
                            "tools/list",
                            "tools/call",
                            "resources/list",
                            "resources/templates/list",
                            "resources/read",
                            "resources/subscribe",
                            "resources/unsubscribe",
                            "prompts/list",
                            "prompts/get",
                            "completion/complete",
                            "logging/setLevel",
                            "sampling/createMessage",
                            "roots/list",
                            "elicitation/create",
                            "notifications/initialized",
                            "notifications/cancelled",
                            "notifications/progress",
                            "notifications/resources/updated",
                            "notifications/resources/list_changed",
                            "notifications/prompts/list_changed",
                            "notifications/tools/list_changed",
                            "notifications/message",
                            "notifications/roots/list_changed"})
    if (Name == Known)
      return Known;
  return "other_redacted";
}

bool rpc(const Value &V, std::string_view Bytes, bool MCP, StreamRecord &R,
         RecordedIdentity &Identity) {
  R.RPC = V.getAsArray() ? "unsupported_batch" : "invalid_message";
  const auto *O = V.getAsObject();
  if (!O || O->getString("jsonrpc") != "2.0")
    return false;
  const auto *ID = O->get("id");
  if (ID) {
    if (auto S = ID->getAsString()) {
      R.IDKind = "string";
      Identity.ID = "s" + S->str();
    } else if (ID->kind() == Value::Null)
      R.IDKind = "null";
    else if (ID->getAsNumber()) {
      if (const auto N = integer(token(Bytes, "id"))) {
        R.IDKind = "safe_integer";
        Identity.ID = "n" + std::to_string(*N);
      } else
        R.IDKind = "number_unlinkable";
    } else {
      R.IDKind = "invalid";
      return false;
    }
  }
  R.ParamsPresent = O->get("params") != nullptr;
  R.ResultPresent = O->get("result") != nullptr;
  R.ErrorPresent = O->get("error") != nullptr;
  if (MCP && ID && R.IDKind != "string" && R.IDKind != "safe_integer") {
    R.RPC = R.IDKind == "number_unlinkable" ? "unsupported_numeric_id"
                                            : "invalid_message";
    return false;
  }
  if (const auto *Method = O->get("method")) {
    auto Name = Method->getAsString();
    const auto *Params = O->get("params");
    if (!Name || R.ResultPresent || R.ErrorPresent ||
        (Params && !Params->getAsObject() && (MCP || !Params->getAsArray())))
      return false;
    R.Method = method(*Name, MCP);
    R.RPC = ID ? "request" : "notification";
    Identity.Request = ID != nullptr;
    return true;
  }
  if (!ID || R.ParamsPresent || R.ResultPresent == R.ErrorPresent)
    return false;
  if (R.ErrorPresent) {
    const auto *E = O->getObject("error");
    if (!E || !E->getString("message") ||
        !integralNumber(token(token(Bytes, "error"), "code")))
      return false;
  }
  if (MCP && R.ResultPresent && !O->getObject("result"))
    return false;
  R.RPC = "response";
  Identity.Response = true;
  return true;
}
} // namespace

void Reader::record(std::string_view Bytes, uint64_t Offset, uint64_t Length,
                    uint64_t Line, bool Terminated) {
  StreamRecord R;
  R.Offset = Offset;
  R.Length = Length;
  R.FirstLine = Line;
  R.LineCount = 1;
  R.Terminated = Terminated;
  const bool Log = Capture.Profile == "diagnostic-lines-v1";
  const bool Plain = Capture.Profile == "jsonl-metadata-v1" || Log;
  const bool Recorded = Capture.Profile == "recorded-jsonrpc-2.0-jsonl-v1";
  const bool MCP = Capture.Profile == "mcp-2025-06-18-stdio-jsonl-v1";
  const auto Text = trim(Bytes);
  if (Text.empty()) {
    R.Kind = "blank_line";
    CoverageGap |= !Plain;
    add(std::move(R));
    return;
  }
  // A diagnostic line is never assumed to be a protocol record.
  if (Log && Text.front() != '{' && Text.front() != '[') {
    R.Kind = "opaque_line";
    add(std::move(R));
    return;
  }
  std::optional<Value> Parsed;
  uint64_t Nodes = 0;
  try {
    Parsed = parseBoundedJSON(Bytes,
                              {MaxStreamFragmentBytes, 32,
                               MaxStreamJSONWork - Capture.JSONWork,
                               MaxStreamFragmentBytes},
                              &Nodes);
  } catch (const Error &E) {
    Capture.JSONWork += Nodes;
    if (std::string_view(E.what()).find("budget_exceeded") !=
        std::string_view::npos)
      throw Error("stream_json_budget_exceeded");
    R.Kind = Log          ? "opaque_line"
             : Terminated ? "malformed_json"
                          : "possibly_truncated_json";
    R.JSONKind = "invalid";
    CoverageGap |= !Plain;
    add(std::move(R));
    return;
  }
  Capture.JSONWork += Nodes;
  R.Kind = "json_value";
  R.JSONKind = kind(*Parsed);
  if (Plain) {
    add(std::move(R));
    return;
  }
  R.Kind = "protocol_record";
  RecordedIdentity I;
  const Value *Message = &*Parsed;
  if (Recorded) {
    const auto *Envelope = Parsed->getAsObject();
    Message = Envelope ? Envelope->get("message") : nullptr;
    if (!Message) {
      R.RPC = "invalid_envelope";
      CoverageGap = true;
      add(std::move(R));
      return;
    }
    if (const auto *D = Envelope->get("direction")) {
      const auto S = D->getAsString();
      R.Direction = S && (*S == "client_to_server" || *S == "server_to_client")
                        ? S->str()
                        : "invalid";
    }
    if (const auto *S = Envelope->get("session")) {
      if (auto Name = S->getAsString(); Name && !Name->empty()) {
        R.SessionPresent = true;
        I.Session = Name->str();
      }
    }
    if (const auto *T = Envelope->get("timestamp"))
      R.Timestamp =
          T->getAsString() ? "string_withheld" : "unsupported_withheld";
    Bytes = token(Bytes, "message");
  }
  const auto Valid = rpc(*Message, Bytes, MCP, R, I);
  CoverageGap |= !Valid;
  if (Recorded && (I.Request || I.Response) &&
      (!R.SessionPresent || R.Direction == "absent" ||
       R.Direction == "invalid"))
    CoverageGap = true;
  // A noncanonical number may equal another recorded integer after exact
  // decimal interpretation; it must not hide a duplicate in a successful group.
  if (Recorded && R.IDKind == "number_unlinkable")
    CoverageGap = true;
  if (!Valid)
    I = {};
  if (R.RPC == "request" || R.RPC == "response")
    R.Relation = Recorded ? "pending" : "missing_recorded_context";
  add(std::move(R), std::move(I));
}
} // namespace neverd::web::stream
