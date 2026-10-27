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

EncodedJson::EncodedJson(const Json &value)
    : text_(value.dump(-1, ' ', false, Json::error_handler_t::replace)) {}

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
std::string frameBody(const std::string &body) {
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
} // namespace

std::string frame(const Json &message) {
  return frameBody(
      message.dump(-1, ' ', false, Json::error_handler_t::replace));
}

std::string frame(const Json &message, const EncodedJson &payload) {
  if (!message.is_object())
    throw Error("invalid_request", "A response envelope must be an object");
  Json envelope = message;
  envelope.erase("payload");
  std::string body =
      envelope.dump(-1, ' ', false, Json::error_handler_t::replace);
  body.pop_back();
  if (!envelope.empty())
    body += ',';
  body += "\"payload\":";
  if (body.size() >= MaxFrameBytes ||
      payload.text().size() >= MaxFrameBytes - body.size())
    throw Error("budget_exceeded",
                "Response exceeds the 8 MiB transport budget");
  body += payload.text();
  body += '}';
  return frameBody(body);
}
} // namespace neverd::worker
