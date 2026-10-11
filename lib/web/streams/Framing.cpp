//===- Framing.cpp - Bounded transcript and SSE framing
//--------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Byte-preserving line/block admission with explicit unfinished SSE records.
///
//===----------------------------------------------------------------------===//

#include "StreamInternal.h"

#include "neverd/web/Error.h"

#include <algorithm>

namespace neverd::web {
namespace {
constexpr std::string_view Profiles[] = {"jsonl-metadata-v1",
                                         "sse-utf8-metadata-v1",
                                         "jsonrpc-2.0-jsonl-v1",
                                         "mcp-2025-06-18-stdio-jsonl-v1",
                                         "recorded-jsonrpc-2.0-jsonl-v1",
                                         "diagnostic-lines-v1"};

void sse(stream::Reader &Reader, std::string_view Bytes, size_t Start) {
  StreamRecord Record;
  Record.Offset = Start;
  Record.FirstLine = 1;
  bool EffectiveID = false;
  size_t Cursor = Start;
  uint64_t Line = 1;
  auto Publish = [&](bool Complete) {
    Record.Length = Cursor - Record.Offset;
    Record.Terminated = Complete;
    Record.Dispatched = Complete && Record.DataLines != 0;
    Record.Kind = !Complete           ? "sse_incomplete"
                  : Record.Dispatched ? "sse_event"
                                      : "sse_control";
    Record.EventIDPresent = EffectiveID;
    if (Record.Dispatched && Record.Event == "absent")
      Record.Event = "message";
    Reader.add(std::move(Record));
    Record = {};
    Record.Offset = Cursor;
    Record.FirstLine = Line;
  };
  while (Cursor < Bytes.size()) {
    const auto Begin = Cursor;
    auto End = Bytes.find_first_of("\r\n", Cursor);
    if (End == std::string_view::npos)
      End = Bytes.size();
    if (End - Begin > MaxStreamFragmentBytes)
      throw Error("stream_fragment_budget_exceeded");
    const auto Text = Bytes.substr(Begin, End - Begin);
    Cursor = End;
    if (Cursor < Bytes.size()) {
      const auto Delimiter = Bytes[Cursor++];
      if (Delimiter == '\r' && Cursor < Bytes.size() && Bytes[Cursor] == '\n')
        ++Cursor;
    }
    if (Cursor - Record.Offset > MaxStreamFragmentBytes)
      throw Error("stream_fragment_budget_exceeded");
    ++Record.LineCount;
    ++Line;
    if (Text.empty()) {
      Publish(true);
      continue;
    }
    if (Text.front() == ':') {
      ++Record.Comments;
      continue;
    }
    const auto Colon = Text.find(':');
    const auto Name = Text.substr(0, Colon);
    auto Value = Colon == std::string_view::npos ? std::string_view()
                                                 : Text.substr(Colon + 1);
    if (!Value.empty() && Value.front() == ' ')
      Value.remove_prefix(1);
    if (Name == "data") {
      Record.DataBytes += Value.size() + (Record.DataLines != 0);
      ++Record.DataLines;
    } else if (Name == "event")
      Record.Event =
          Value.empty() || Value == "message" ? "message" : "custom_redacted";
    else if (Name == "id") {
      ++Record.IDFields;
      if (Value.find('\0') != std::string_view::npos)
        ++Record.InvalidIDFields;
      else {
        EffectiveID = !Value.empty();
        Record.EventIDReset = Value.empty();
      }
    } else if (Name == "retry") {
      ++Record.RetryFields;
      if (Value.empty() || !std::all_of(Value.begin(), Value.end(), [](char C) {
            return C >= '0' && C <= '9';
          }))
        ++Record.IgnoredFields;
    } else
      ++Record.IgnoredFields;
  }
  if (Cursor != Record.Offset)
    Publish(false);
}
} // namespace

std::vector<std::string_view> streamProfiles() {
  return {std::begin(Profiles), std::end(Profiles)};
}

void stream::Reader::add(StreamRecord Record, RecordedIdentity Identity) {
  if (Capture.Records.size() >= MaxStreamRecords)
    throw Error("stream_record_budget_exceeded");
  const auto Bytes = Identity.Session.size() + Identity.ID.size();
  if (Bytes > MaxStreamPrivateBytes - PrivateBytes)
    throw Error("stream_private_budget_exceeded");
  PrivateBytes += Bytes;
  Record.ID = identity("stream-record",
                       {Capture.ID, std::to_string(Capture.Records.size())});
  Capture.Records.push_back(std::move(Record));
  Identities.push_back(std::move(Identity));
}

StreamCapture inspectStream(std::string_view ArtifactID, std::string_view Bytes,
                            std::string_view Profile) {
  if (std::find(std::begin(Profiles), std::end(Profiles), Profile) ==
      std::end(Profiles))
    throw Error("unsupported_stream_profile");
  if (Bytes.size() > MaxStreamBytes)
    throw Error("stream_byte_budget_exceeded");
  if (!validUtf8(Bytes))
    throw Error("invalid_stream_encoding");
  stream::Reader R;
  R.Capture.ArtifactID = ArtifactID;
  R.Capture.BlobHash = sha256(Bytes);
  R.Capture.Profile = Profile;
  R.Capture.ID = identity("stream-capture", {ArtifactID, R.Capture.BlobHash,
                                             Profile, StreamRedactionPolicy});
  const bool SSE = Profile == "sse-utf8-metadata-v1";
  size_t Cursor = 0;
  if (Bytes.substr(0, 3) == "\xef\xbb\xbf") {
    if (!SSE)
      throw Error("unsupported_stream_bom");
    R.Capture.BOM = true;
    Cursor = 3;
  }
  if (SSE) {
    sse(R, Bytes, Cursor);
    return std::move(R.Capture);
  }
  uint64_t Line = 1;
  while (Cursor < Bytes.size()) {
    const auto Begin = Cursor;
    auto End = Bytes.find('\n', Cursor);
    const bool Terminated = End != std::string_view::npos;
    if (!Terminated)
      End = Bytes.size();
    Cursor = End + Terminated;
    if (Cursor - Begin > MaxStreamFragmentBytes)
      throw Error("stream_fragment_budget_exceeded");
    if (End > Begin && Bytes[End - 1] == '\r' && Terminated)
      --End;
    R.record(Bytes.substr(Begin, End - Begin), Begin, Cursor - Begin, Line++,
             Terminated);
  }
  R.relate();
  return std::move(R.Capture);
}

} // namespace neverd::web
