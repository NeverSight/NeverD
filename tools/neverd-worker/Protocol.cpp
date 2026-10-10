#include "Protocol.h"

#include <charconv>
#include <limits>
#include <set>
#include <vector>

namespace neverd::worker {
namespace {
// nlohmann's DOM filter callback rescans the parent array at every object
// end to remove discarded nodes, even when no callback discards anything.
// Use the public SAX interface for decoded-key admission, then its ordinary
// DOM parser. Both passes are bounded by the existing byte/depth ceilings.
class UniqueKeys final : public nlohmann::json_sax<Json> {
  std::vector<std::set<std::string>> keys_;

public:
  bool null() override { return true; }
  bool boolean(bool) override { return true; }
  bool number_integer(Json::number_integer_t) override { return true; }
  bool number_unsigned(Json::number_unsigned_t) override { return true; }
  bool number_float(Json::number_float_t, const Json::string_t &) override {
    return true;
  }
  bool string(Json::string_t &) override { return true; }
  bool binary(Json::binary_t &) override { return true; }
  bool start_object(std::size_t) override {
    keys_.emplace_back();
    return true;
  }
  bool key(Json::string_t &value) override {
    if (!keys_.back().insert(value).second)
      throw Error("invalid_json", "Duplicate object key in JSON frame");
    return true;
  }
  bool end_object() override {
    keys_.pop_back();
    return true;
  }
  bool start_array(std::size_t) override { return true; }
  bool end_array() override { return true; }
  bool parse_error(std::size_t, const std::string &,
                   const Json::exception &) override {
    return false;
  }
};
} // namespace

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

std::string stringField(const Json &object, const char *key,
                        std::string fallback, std::size_t max) {
  auto it = object.find(key);
  if (it == object.end())
    return fallback;
  if (!it->is_string())
    throw Error("invalid_request", std::string(key) + " must be a string");
  const auto &value = it->get_ref<const std::string &>();
  if (value.size() > max || value.find('\0') != std::string::npos)
    throw Error("invalid_request",
                std::string(key) + " is too long or contains NUL");
  return value;
}

std::size_t sizeField(const Json &object, const char *key, std::size_t fallback,
                      std::size_t max) {
  auto it = object.find(key);
  if (it == object.end())
    return fallback;
  if (!it->is_number_integer() ||
      (it->is_number_integer() && !it->is_number_unsigned() &&
       it->get<std::int64_t>() < 0))
    throw Error("invalid_request",
                std::string(key) + " must be a nonnegative integer");
  const auto value = it->get<std::uint64_t>();
  if (value > max)
    throw Error("budget_exceeded",
                std::string(key) + " exceeds the operation budget");
  return static_cast<std::size_t>(value);
}

Json parseJson(std::string_view text, std::size_t maxBytes) {
  if (text.empty() || text.size() > maxBytes)
    throw Error("invalid_frame", "Empty or oversized JSON frame");
  // Reject deeply nested input before the JSON parser can consume its stack.
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (char c : text) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '[' || c == '{') {
      if (++depth > 64)
        throw Error("invalid_frame", "JSON nesting exceeds 64 levels");
    } else if ((c == ']' || c == '}') && depth)
      --depth;
  }
  try {
    UniqueKeys keys;
    if (!Json::sax_parse(text, &keys))
      throw Error("invalid_json", "Malformed UTF-8 JSON frame");
    return Json::parse(text);
  } catch (const Json::exception &) {
    throw Error("invalid_json", "Malformed UTF-8 JSON frame");
  }
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
