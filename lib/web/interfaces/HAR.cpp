//===- HAR.cpp - Bounded HAR observation admission ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HAR 1.2 metadata admission without publishing target-derived values.
///
//===----------------------------------------------------------------------===//

#include "../JsonReader.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/Interfaces.h"

namespace neverd::web {
namespace {
using Object = llvm::json::Object;

const Object *object(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V)
    return nullptr;
  const auto *R = V->getAsObject();
  if (!R)
    throw Error("invalid_har_object");
  return R;
}
const llvm::json::Array *array(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V)
    return nullptr;
  const auto *R = V->getAsArray();
  if (!R)
    throw Error("invalid_har_array");
  return R;
}
std::optional<llvm::StringRef> string(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V)
    return {};
  auto R = V->getAsString();
  if (!R)
    throw Error("invalid_har_string");
  return R;
}

// A deliberately narrow RFC3339 subset: Gregorian date, seconds 00..59,
// optional 1..9 fractional digits, and Z or a numeric offset. Invalid values
// remain an explicit limitation and never enter public output.
bool timestamp(std::string_view S) {
  if (S.size() < 20 || S.size() > 35 || S[4] != '-' || S[7] != '-' ||
      S[10] != 'T' || S[13] != ':' || S[16] != ':')
    return false;
  auto Number = [&](size_t At, size_t Length) -> int {
    int N = 0;
    for (size_t I = At; I < At + Length; ++I) {
      if (S[I] < '0' || S[I] > '9')
        return -1;
      N = 10 * N + S[I] - '0';
    }
    return N;
  };
  const auto Year = Number(0, 4), Month = Number(5, 2), Day = Number(8, 2);
  const auto Hour = Number(11, 2), Minute = Number(14, 2),
             Second = Number(17, 2);
  if (Year < 1 || Month < 1 || Month > 12 || Day < 1 || Hour < 0 || Hour > 23 ||
      Minute < 0 || Minute > 59 || Second < 0 || Second > 59)
    return false;
  static constexpr int Days[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
  const bool Leap = Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
  if (Day > Days[Month - 1] + (Month == 2 && Leap))
    return false;
  size_t At = 19;
  if (S[At] == '.') {
    const auto First = ++At;
    while (At < S.size() && S[At] >= '0' && S[At] <= '9')
      ++At;
    if (At == First || At - First > 9)
      return false;
  }
  if (At + 1 == S.size() && S[At] == 'Z')
    return true;
  return At + 6 == S.size() && (S[At] == '+' || S[At] == '-') &&
         S[At + 3] == ':' && Number(At + 1, 2) >= 0 &&
         Number(At + 1, 2) <= 23 && Number(At + 4, 2) >= 0 &&
         Number(At + 4, 2) <= 59;
}

struct Reader {
  HARCapture Result;

  void step(uint64_t N = 1) {
    if (N > MaxInterfaceSteps - Result.Steps)
      throw Error("har_work_budget_exceeded");
    Result.Steps += N;
  }
  void extensions(const llvm::json::Value &V) {
    step();
    if (const auto *O = V.getAsObject())
      for (const auto &[Key, Child] : *O) {
        if (Key.str().starts_with('_'))
          ++Result.ExtensionFields;
        extensions(Child);
      }
    else if (const auto *A = V.getAsArray())
      for (const auto &Child : *A)
        extensions(Child);
  }
  HARFields fields(const Object &O, llvm::StringRef Key, bool Headers = false) {
    HARFields R;
    const auto *A = array(O, Key);
    if (!A)
      return R;
    R.Present = true;
    R.Count = A->size();
    if (R.Count > MaxInterfaceFields - Result.Fields)
      throw Error("har_field_budget_exceeded");
    Result.Fields += R.Count;
    for (const auto &V : *A) {
      step();
      const auto *Field = V.getAsObject();
      if (!Field)
        throw Error("invalid_har_field");
      const auto Name = string(*Field, "name");
      const auto Value = string(*Field, "value");
      if (!Name || !Value)
        ++R.Incomplete;
      if (Headers)
        ++R.HeaderKinds[Name ? interfaceHeaderKind(*Name) : "absent"];
    }
    return R;
  }
  void request(const Object &O, HARObservation &R) {
    R.RequestPresent = true;
    if (auto Method = string(O, "method")) {
      R.Method = interfaceMethod(*Method);
      R.MethodStatus =
          R.Method == "other" ? "unrecognized_redacted" : "capture_declared";
    }
    if (auto URL = string(O, "url")) {
      R.Endpoint = inspectInterfaceEndpoint(*URL);
      const auto Size = R.Endpoint.Origin.size() + R.Endpoint.Path.size();
      if (Size > MaxInterfacePrivateBytes - Result.PrivateBytes)
        throw Error("har_private_budget_exceeded");
      Result.PrivateBytes += Size;
    }
    R.RequestHeaders = fields(O, "headers", true);
    R.RequestCookies = fields(O, "cookies");
    R.Query = fields(O, "queryString");
    if (const auto *Post = object(O, "postData")) {
      R.PostDataPresent = true;
      R.RequestTextPresent = string(*Post, "text").has_value();
      R.PostParameters = fields(*Post, "params");
      // HAR declares text and params mutually exclusive. Preserve an explicit
      // refusal instead of choosing between inconsistent body declarations.
      if (R.RequestTextPresent && R.PostParameters.Present)
        throw Error("conflicting_har_post_data");
    }
  }
  void response(const Object &O, HARObservation &R) {
    R.ResponsePresent = true;
    if (const auto *Status = O.get("status")) {
      const auto N = Status->getAsInteger();
      if (!N || (*N != 0 && (*N < 100 || *N > 599)))
        throw Error("invalid_har_status");
      R.Status = *N;
    }
    R.ResponseHeaders = fields(O, "headers", true);
    R.ResponseCookies = fields(O, "cookies");
    if (const auto *Content = object(O, "content")) {
      R.ResponseContentPresent = true;
      R.ResponseTextPresent = string(*Content, "text").has_value();
      if (auto Encoding = string(*Content, "encoding"))
        R.ResponseEncoding = *Encoding == "base64" ? "base64_not_decoded"
                                                   : "unsupported_redacted";
    }
  }
  void entry(const Object &O) {
    step();
    const auto Ordinal = std::to_string(Result.Observations.size());
    HARObservation R;
    R.ID = identity("har-observation", {Result.ID, Ordinal});
    R.RequestID = identity("har-request", {R.ID});
    R.ResponseID = identity("har-response", {R.ID});
    if (auto Time = string(O, "startedDateTime")) {
      if (timestamp(*Time)) {
        R.Timestamp = Time->str();
        R.TimestampStatus = "capture_declared_rfc3339_subset";
      } else {
        R.TimestampStatus = "invalid_or_unsupported_redacted";
        ++Result.InvalidTimestamps;
      }
    } else {
      ++Result.MissingTimestamps;
    }
    if (const auto *Request = object(O, "request"))
      request(*Request, R);
    else
      ++Result.MissingRequests;
    if (const auto *Response = object(O, "response"))
      response(*Response, R);
    else
      ++Result.MissingResponses;
    R.WebSocketExtensionPresent = O.get("_webSocketMessages") != nullptr;
    Result.Observations.push_back(std::move(R));
  }
};
} // namespace

HARCapture inspectHAR(std::string_view ArtifactID, std::string_view Bytes) {
  if (Bytes.size() > MaxHARBytes)
    throw Error("har_byte_budget_exceeded");
  Reader R;
  R.Result.ArtifactID = ArtifactID;
  R.Result.BlobHash = sha256(Bytes);
  R.Result.ID =
      identity("har-capture", {ArtifactID, R.Result.BlobHash, HARProfile});
  // The evidence identity always covers the original bytes, including BOM.
  if (Bytes.starts_with("\xef\xbb\xbf"))
    Bytes.remove_prefix(3);
  const auto Document =
      parseBoundedJSON(Bytes, {MaxHARBytes, 64, 200000, 4 * 1024 * 1024});
  R.extensions(Document);
  const auto *Root = Document.getAsObject();
  if (!Root)
    throw Error("invalid_har_root");
  const auto *Log = object(*Root, "log");
  if (!Log || string(*Log, "version") != "1.2")
    throw Error("unsupported_har_version");
  R.Result.CreatorPresent = object(*Log, "creator") != nullptr;
  const auto *Entries = array(*Log, "entries");
  if (!Entries)
    throw Error("missing_har_entries");
  if (Entries->size() > MaxInterfaceRecords)
    throw Error("har_entry_budget_exceeded");
  for (const auto &V : *Entries) {
    const auto *O = V.getAsObject();
    if (!O)
      throw Error("invalid_har_entry");
    R.entry(*O);
  }
  return std::move(R.Result);
}
} // namespace neverd::web
