//===- Protocol.cpp - Framed native worker protocol -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native address fields, request envelopes and length-prefixed framing.
///
//===----------------------------------------------------------------------===//

#include "Protocol.h"

#include <charconv>

namespace neverd::worker {

std::string hexAddress(std::uint64_t value) {
  char buffer[16];
  const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, 16);
  return "0x" + std::string(buffer, result.ptr);
}

std::uint64_t parseAddress(std::string_view text) {
  if (text.size() < 3 || text.size() > 18 || text[0] != '0' ||
      (text[1] != 'x' && text[1] != 'X'))
    throw Error("invalid_address",
                "Address must be a 0x-prefixed 64-bit hexadecimal string");
  std::uint64_t result = 0;
  const auto parsed =
      std::from_chars(text.data() + 2, text.data() + text.size(), result, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw Error("invalid_address", "Address is outside the unsigned 64-bit "
                                   "range or contains invalid digits");
  return result;
}

void validateRequest(const Json &request) {
  if (!request.is_object())
    throw Error("invalid_request", "Request must be an object");
  const auto version = request.find("protocol_major");
  if (version == request.end() || !version->is_number_integer() ||
      *version != 1)
    throw Error("incompatible_protocol",
                "This worker requires protocol major version 1");
  if (stringField(request, "request_id", {}, 128).empty())
    throw Error("invalid_request", "A nonempty request_id string is required");
  if (stringField(request, "operation", {}, 64).empty())
    throw Error("invalid_request", "An operation string is required");
  if (request.contains("payload") && !request["payload"].is_object())
    throw Error("invalid_request", "payload must be an object");
  if (request.contains("expected_revision"))
    (void)stringField(request, "expected_revision", {}, 32);
  if (request.contains("project_id"))
    (void)stringField(request, "project_id", {}, 128);
}

std::uint32_t frameSize(const unsigned char *header) {
  const std::uint32_t size = (std::uint32_t(header[0]) << 24) |
                             (std::uint32_t(header[1]) << 16) |
                             (std::uint32_t(header[2]) << 8) | header[3];
  if (size == 0 || size > MaxFrameBytes)
    throw Error("invalid_frame", "Frame size must be between 1 byte and 8 MiB");
  return size;
}

std::string frame(const Json &message) {
  const std::string body =
      message.dump(-1, ' ', false, Json::error_handler_t::replace);
  if (body.size() > MaxFrameBytes)
    throw Error("budget_exceeded",
                "Response exceeds the 8 MiB transport budget");
  const auto size = static_cast<std::uint32_t>(body.size());
  std::string result(4, '\0');
  for (unsigned i = 0; i < 4; ++i)
    result[i] = static_cast<char>(size >> (24 - 8 * i));
  result += body;
  return result;
}
} // namespace neverd::worker
