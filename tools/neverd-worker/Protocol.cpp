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

#include "ResourceLimits.h"

#include <charconv>

namespace neverd::worker {

std::string hexAddress(std::uint64_t value) {
  char buffer[18]; // 16 hexadecimal digits and the prefix.
  buffer[0] = '0';
  buffer[1] = 'x';
  const auto result =
      std::to_chars(buffer + 2, buffer + sizeof buffer, value, 16);
  return std::string(buffer, result.ptr);
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
  if (stringField(
          request, "request_id", {},
          ::neverd::limits::worker::kProtocolValidateRequestIdStringFieldMax)
          .empty())
    throw Error("invalid_request", "A nonempty request_id string is required");
  if (stringField(request, "operation", {},
                  ::neverd::limits::worker::
                      kProtocolValidateRequestOperationStringFieldMax)
          .empty())
    throw Error("invalid_request", "An operation string is required");
  if (request.contains("payload") && !request["payload"].is_object())
    throw Error("invalid_request", "payload must be an object");
  if (request.contains("expected_revision"))
    (void)stringField(
        request, "expected_revision", {},
        ::neverd::limits::worker::
            kProtocolValidateRequestExpectedRevisionStringFieldMax);
  if (request.contains("project_id"))
    (void)stringField(request, "project_id", {},
                      ::neverd::limits::worker::
                          kProtocolValidateRequestProjectIdStringFieldMax);
}

std::uint32_t frameSize(const unsigned char *header) {
  const std::uint32_t size = (std::uint32_t(header[0]) << 24) |
                             (std::uint32_t(header[1]) << 16) |
                             (std::uint32_t(header[2]) << 8) | header[3];
  if (size == 0 || size > MaxFrameBytes)
    throw Error("invalid_frame", "Frame size must be between 1 byte and 8 MiB");
  return size;
}

namespace {
/// Whether the serializer, which escapes no non-ASCII character, copies \p
/// text byte for byte: it holds no control character, quote or backslash, and
/// only well-formed UTF-8 (which the serializer would otherwise replace).
bool copiedVerbatim(std::string_view text) {
  const auto *at = reinterpret_cast<const unsigned char *>(text.data());
  const auto *const end = at + text.size();
  while (at != end) {
    const unsigned char lead = *at;
    if (lead < 0x80) {
      if (lead < 0x20 || lead == '"' || lead == '\\')
        return false;
      ++at;
      continue;
    }
    // A continuation byte at \p offset, within [low, high].
    const auto follows = [&](std::ptrdiff_t offset, unsigned char low = 0x80,
                             unsigned char high = 0xbf) {
      return end - at > offset && at[offset] >= low && at[offset] <= high;
    };
    std::ptrdiff_t length = 0;
    if (lead >= 0xc2 && lead <= 0xdf)
      length = follows(1) ? 2 : 0;
    else if (lead == 0xe0)
      length = follows(1, 0xa0) && follows(2) ? 3 : 0;
    else if ((lead >= 0xe1 && lead <= 0xec) || lead == 0xee || lead == 0xef)
      length = follows(1) && follows(2) ? 3 : 0;
    else if (lead == 0xed)
      length = follows(1, 0x80, 0x9f) && follows(2) ? 3 : 0;
    else if (lead == 0xf0)
      length = follows(1, 0x90) && follows(2) && follows(3) ? 4 : 0;
    else if (lead >= 0xf1 && lead <= 0xf3)
      length = follows(1) && follows(2) && follows(3) ? 4 : 0;
    else if (lead == 0xf4)
      length = follows(1, 0x80, 0x8f) && follows(2) && follows(3) ? 4 : 0;
    if (!length)
      return false;
    at += length;
  }
  return true;
}
} // namespace

void JsonWriter::escaped(std::string_view text) {
  if (copiedVerbatim(text)) {
    text_ += '"';
    text_.append(text);
    text_ += '"';
    return;
  }
  text_ += Json(std::string(text))
               .dump(-1, ' ', false, Json::error_handler_t::replace);
}

void JsonWriter::key(std::string_view name) {
  separate();
  escaped(name);
  text_ += ':';
  first_ = true;
}

void JsonWriter::string(std::string_view text) {
  separate();
  escaped(text);
}

void JsonWriter::address(std::uint64_t value) {
  separate();
  // The quotes, the prefix and 16 hexadecimal digits.
  char buffer[20] = {'"', '0', 'x'};
  auto *end =
      std::to_chars(buffer + 3, buffer + sizeof buffer - 1, value, 16).ptr;
  *end++ = '"';
  text_.append(buffer, end);
}

void JsonWriter::number(std::uint64_t value) {
  separate();
  char buffer[24];
  text_.append(buffer,
               std::to_chars(buffer, buffer + sizeof buffer, value).ptr);
}

void JsonWriter::number(std::int64_t value) {
  separate();
  char buffer[24];
  text_.append(buffer,
               std::to_chars(buffer, buffer + sizeof buffer, value).ptr);
}

void JsonWriter::value(const Json &value) {
  separate();
  text_ += value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void JsonWriter::fragment(const EncodedJson &piece) {
  separate();
  text_ += piece.text();
}

EncodedJson::EncodedJson(const Json &value)
    : text_(value.dump(-1, ' ', false, Json::error_handler_t::replace)) {}

EncodedJson::EncodedJson(JsonWriter &&writer) : text_(std::move(writer.text_)) {
  if (text_.size() > MaxFrameBytes)
    throw Error("budget_exceeded", "Encoded JSON exceeds the output budget");
}

EncodedJson::EncodedJson(const Json::object_t &fields,
                         const std::string &arrayKey, std::size_t count,
                         const std::function<const Json &(std::size_t)> &item)
    : text_(transport::serializeJsonArray(fields, arrayKey, count, item,
                                          MaxFrameBytes,
                                          Json::error_handler_t::replace)) {}

std::string sidecarText(const Json &value) {
  std::string dumped =
      value.dump(2, ' ', false, Json::error_handler_t::replace);
  // LLVM's JSON writes a backspace and a form feed as \u0008 and \u000c,
  // where nlohmann writes \b and \f.
  auto escape = dumped.find('\\');
  if (escape == std::string::npos) {
    dumped += '\n';
    return dumped;
  }
  std::string text;
  text.reserve(dumped.size() + 1);
  std::size_t start = 0;
  // Serializer output has backslashes only inside strings. Consume each
  // complete escape so a literal backslash followed by b/f stays literal.
  for (; escape != std::string::npos; escape = dumped.find('\\', start)) {
    text.append(dumped, start, escape - start);
    const char escaped = dumped[escape + 1];
    if (escaped == 'b' || escaped == 'f')
      text += escaped == 'b' ? "\\u0008" : "\\u000c";
    else
      text.append(dumped, escape, 2);
    start = escape + 2;
  }
  text.append(dumped, start, dumped.size() - start);
  text += '\n';
  return text;
}

std::string formatSidecarText(std::string_view encoded) {
  std::string text;
  text.reserve(encoded.size() + encoded.size() / 2);
  std::size_t indent = 0;
  const auto newline = [&] {
    text += '\n';
    text.append(indent, ' ');
  };
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    const char c = encoded[i];
    switch (c) {
    case '"': {
      // Copy string spans together. Bound each escape search by the next
      // quote so many short strings never rescan the remaining document.
      auto start = i, cursor = i + 1;
      auto quote = encoded.find('"', cursor);
      for (;;) {
        auto escape = encoded.substr(cursor, quote - cursor).find('\\');
        if (escape == std::string_view::npos) {
          text.append(encoded.substr(start, quote + 1 - start));
          i = quote;
          break;
        }
        escape += cursor;
        const char escaped = encoded[escape + 1];
        // LLVM spells these controls with Unicode escapes. Consume whole
        // escapes so a literal backslash followed by b/f stays literal.
        if (escaped == 'b' || escaped == 'f') {
          text.append(encoded.substr(start, escape - start));
          text += escaped == 'b' ? "\\u0008" : "\\u000c";
          start = escape + 2;
        }
        cursor = escape + 2;
        if (cursor > quote)
          quote = encoded.find('"', cursor);
      }
      break;
    }
    case '[':
    case '{':
      text += c;
      if (encoded[i + 1] == (c == '[' ? ']' : '}'))
        text += encoded[++i];
      else {
        indent += 2;
        newline();
      }
      break;
    case ']':
    case '}':
      indent -= 2;
      newline();
      text += c;
      break;
    case ',':
      text += c;
      newline();
      break;
    case ':':
      text += ": ";
      break;
    default:
      text += c;
      break;
    }
  }
  text += '\n';
  return text;
}

namespace {
// Callers admit the complete body before reserving its final framed size.
std::string frameHeader(std::size_t bodySize) {
  const auto size = static_cast<std::uint32_t>(bodySize);
  std::string result(4, '\0');
  result.reserve(4 + bodySize);
  for (unsigned i = 0; i < 4; ++i)
    result[i] = static_cast<char>(size >> (24 - 8 * i));
  return result;
}

std::string frameBody(const std::string &body) {
  if (body.size() > MaxFrameBytes)
    throw Error("budget_exceeded",
                "Response exceeds the 8 MiB transport budget");
  auto result = frameHeader(body.size());
  result += body;
  return result;
}
} // namespace

std::string frame(const Json &message) {
  return frameBody(
      message.dump(-1, ' ', false, Json::error_handler_t::replace));
}

std::string frame(const Json &message, const EncodedJson &payload) {
  return frame(Json(message), payload);
}

std::string frame(Json &&message, const EncodedJson &payload) {
  if (!message.is_object())
    throw Error("invalid_request", "A response envelope must be an object");
  message.erase("payload");
  std::string body =
      message.dump(-1, ' ', false, Json::error_handler_t::replace);
  body.pop_back();
  if (!message.empty())
    body += ',';
  body += "\"payload\":";
  if (body.size() >= MaxFrameBytes ||
      payload.text().size() >= MaxFrameBytes - body.size())
    throw Error("budget_exceeded",
                "Response exceeds the 8 MiB transport budget");
  auto result = frameHeader(body.size() + payload.text().size() + 1);
  result += body;
  result += payload.text();
  result += '}';
  return result;
}
} // namespace neverd::worker
