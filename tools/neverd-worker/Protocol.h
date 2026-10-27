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

#include <cstdint>
#include <string>
#include <string_view>

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
/// Owned JSON produced by the same serializer as ordinary frames. Keeping its
/// constructor typed prevents an unescaped string from entering a response.
class EncodedJson {
public:
  explicit EncodedJson(const Json &value);
  const std::string &text() const { return text_; }
  std::size_t storageBytes() const {
    return sizeof(*this) + text_.capacity() + 1;
  }

private:
  std::string text_;
};
std::string frame(const Json &message, const EncodedJson &payload);
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
