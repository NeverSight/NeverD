//===- Protocol.h - Framed worker protocol ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Worker framing and request schema over the shared bounded JSON layer.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "ResourceLimits.h"
#include "common/transport/Json.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker {
using transport::Error;
using transport::Json;
using transport::parseJson;
using transport::sizeField;
using transport::stringField;
inline constexpr std::size_t MaxFrameBytes = transport::MaxJsonBytes;

std::string hexAddress(std::uint64_t value);
std::uint64_t parseAddress(std::string_view text);
void validateRequest(const Json &request);
std::uint32_t frameSize(const unsigned char *header);
std::string frame(const Json &message);
/// Writes a JSON value piece by piece, byte for byte as the serializer prints
/// the document with the same members, for replies whose rows cost more built
/// as a document than written as text.  An object's keys must come in the
/// order a document keeps them (sorted), and every string goes through the
/// serializer's escaping: copied where the serializer copies it, printed by
/// the serializer otherwise.  Only EncodedJson takes what it wrote.
class EncodedJson;
class JsonWriter {
public:
  void beginObject() {
    separate();
    text_ += '{';
    first_ = true;
  }
  void endObject() {
    text_ += '}';
    first_ = false;
  }
  void beginArray() {
    separate();
    text_ += '[';
    first_ = true;
  }
  void endArray() {
    text_ += ']';
    first_ = false;
  }
  void key(std::string_view name);
  void string(std::string_view text);
  /// An address, as hexAddress spells it.
  void address(std::uint64_t value);
  void number(std::uint64_t value);
  void number(std::int64_t value);
  void boolean(bool value) {
    separate();
    text_ += value ? "true" : "false";
  }
  /// A value the serializer prints whole.
  void value(const Json &value);
  /// A value written before, as \p piece holds it.
  void fragment(const EncodedJson &piece);

private:
  friend class EncodedJson;
  void separate() {
    if (!first_)
      text_ += ',';
    first_ = false;
  }
  void escaped(std::string_view text);
  std::string text_;
  bool first_ = true;
};

/// Owned JSON produced by the same serializer as ordinary frames. Keeping its
/// constructor typed prevents an unescaped string from entering a response.
class EncodedJson {
public:
  explicit EncodedJson(const Json &value);
  /// What \p writer wrote, within the payload budget a frame allows.
  explicit EncodedJson(JsonWriter &&writer);
  /// Encode generated rows without retaining a complete array DOM. The row
  /// producer is synchronous and never escapes this construction.
  EncodedJson(const Json::object_t &fields, const std::string &arrayKey,
              std::size_t count,
              const std::function<const Json &(std::size_t)> &item);
  const std::string &text() const { return text_; }
  std::size_t storageBytes() const {
    return sizeof(*this) + text_.capacity() + 1;
  }

private:
  std::string text_;
};
/// A listing page as `listing` writes it, for a client that keeps pages: its
/// fields, and each line as written (Listing::pageLines).
struct ListingPage {
  std::uint64_t anchor = 0;
  bool atStart = true, atEnd = true;
  std::string generation;
  Json index;
  std::vector<std::shared_ptr<const EncodedJson>> lines;
};
std::string frame(const Json &message, const EncodedJson &payload);
/// Avoid copying a disposable envelope. Only its payload field is removed;
/// other metadata remains available to construct an error response on failure.
std::string frame(Json &&message, const EncodedJson &payload);
/// The text of a user state file beside the input: JSON indented by two
/// spaces with sorted keys and a final newline, byte for byte as the engine's
/// writer (SessionSidecar.cpp) prints the same files. The CLI and the
/// workbench then write identical files for identical state, and a project
/// diffs and merges line by line under version control. Size budgets measure
/// this text, the bytes a later read checks.
std::string sidecarText(const Json &value);
/// Apply the same file layout to compact, serializer-produced JSON. Callers
/// must supply a complete encoded value, not unvalidated file or IPC text.
std::string formatSidecarText(std::string_view encoded);
} // namespace neverd::worker

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::worker {
inline constexpr std::size_t MaxBackendBytes =
    ::neverd::limits::worker::kProtocolMaxBackendBytes;
inline constexpr std::size_t MaxQueueBytes =
    ::neverd::limits::worker::kProtocolMaxQueueBytes;
inline constexpr std::size_t MaxQueueRequests =
    ::neverd::limits::worker::kProtocolMaxQueueRequests;
} // namespace neverd::worker
