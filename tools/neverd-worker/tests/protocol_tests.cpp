#include "Protocol.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

using namespace neverd::worker;
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
template <class Fn> void rejects(Fn call, const char *message) {
  try {
    call();
  } catch (const Error &) {
    return;
  }
  check(false, message);
}
void backendJson() {
  const auto compare = [](const std::string &text) {
    Json expected;
    int error = 0;
    try {
      expected = Json::parse(text);
    } catch (const Json::exception &e) {
      error = e.id;
    }
    try {
      const auto actual = neverd::transport::parseBackendJson(text);
      check(!error && actual.type() == expected.type() &&
                actual.dump() == expected.dump(),
            "Backend JSON changed its native value or number spelling");
    } catch (const Json::exception &e) {
      check(error == e.id, "Backend JSON changed its native rejection");
    }
  };
  for (const auto *text :
       {"null",
        "true",
        "0",
        "-0",
        "-0.0",
        "1.25",
        "1e-9999",
        "1e9999",
        "18446744073709551615",
        "18446744073709551616",
        "-9223372036854775808",
        "-9223372036854775809",
        R"({"a":1,"\u0061":2,"nested":{"x":false,"x":[]}})",
        R"({"a":1.25,"a":2})",
        R"({"a":{},"a":false})",
        R"([{"a":1},{"a":2},"quote\" slash\\\n\u0000\u4e2d"])",
        "\xef\xbb\xbf{}",
        R"("\ud800")",
        "[1,]",
        "{",
        "",
        "true false"})
    compare(text);
  compare(std::string("\"bad") + '\xff' + '"');
  compare(std::string(70, '[') + "0" + std::string(70, ']'));
  compare("\"" + std::string(MaxFrameBytes, 'x') + "\"");
  // Exercise temporary parser storage, repeated calls and decoded duplicates
  // in a large native inventory, independently of strict frame admission.
  std::string inventory = "[";
  for (unsigned i = 0; i < 2048; ++i) {
    if (i)
      inventory += ',';
    inventory += "{\"value\":null,\"value\":" + std::to_string(i) +
                 ",\"text\":\"unchanged inventory row\"}";
  }
  inventory += ']';
  compare(inventory);
  compare(inventory);
  const auto retained = neverd::transport::parseBackendJson(inventory);
  inventory.assign("[]");
  (void)neverd::transport::parseBackendJson(inventory);
  check(retained.size() == 2048 && retained.back().at("value") == 2047 &&
            retained.front().at("text") == "unchanged inventory row",
        "Backend values borrowed parser or input storage");
}
void sidecarLayout() {
  // The original generic pretty serializer is independent of the compact
  // formatter. Controls with LLVM-specific spelling have literal oracles
  // below; every other byte, scalar and nested layout must match dump(2).
  const auto compare = [](const Json &value) {
    const auto expected = value.dump(2) + '\n';
    check(sidecarText(value) == expected &&
              formatSidecarText(value.dump()) == expected,
          "Sidecar layout differs from the original JSON serializer");
  };
  for (const Json &value :
       {Json(nullptr), Json(true), Json(false), Json::array(), Json::object(),
        Json(std::numeric_limits<std::uint64_t>::max()),
        Json(std::numeric_limits<std::int64_t>::min()), Json(-0.0), Json(0.0),
        Json(std::numeric_limits<double>::max()),
        Json(std::numeric_limits<double>::min()),
        Json(std::numeric_limits<double>::denorm_min())})
    compare(value);
  for (unsigned c = 0; c < 128; ++c) {
    if (c == '\b' || c == '\f')
      continue;
    const auto value = std::string("\\\"{}[],: 中文") + char(c) + "\\";
    compare(value);
    compare(Json{{value, Json::array({value, Json::array(), Json::object(),
                                      Json{{"number", -0.0}}})}});
  }
  Json nested = Json::array({"\\\"", "\\\\\"", "[,]{}:", "中文🙂"});
  for (unsigned i = 0; i < 24; ++i) {
    compare(nested);
    nested = i % 2
                 ? Json{{"child", std::move(nested)}, {"empty", Json::array()}}
                 : Json::array({std::move(nested), nullptr, i, true});
  }
  compare(Json{{"large", std::string(32768, 'a')}});
  const Json invalid = std::string("invalid") + char(0xff) + "\"\\";
  check(sidecarText(invalid) ==
            invalid.dump(2, ' ', false, Json::error_handler_t::replace) + '\n',
        "Sidecar replacement handling changed");
}
int main() {
  backendJson();
  sidecarLayout();
  const Json escapedSidecar = Json::array(
      {"plain", "\\b", "\\f", "\\", "\\\b", "\\\f", "\"", "\b", "\f", "中文"});
  check(sidecarText(escapedSidecar) == R"([
  "plain",
  "\\b",
  "\\f",
  "\\",
  "\\\u0008",
  "\\\u000c",
  "\"",
  "\u0008",
  "\u000c",
  "中文"
]
)",
        "Sidecar escapes differ from LLVM JSON spelling");
  check(formatSidecarText(escapedSidecar.dump()) == sidecarText(escapedSidecar),
        "Preencoded sidecars changed LLVM control escapes");
  check(sidecarText(Json{{"\b", "\f"}}) == "{\n  \"\\u0008\": \"\\u000c\"\n}\n",
        "Sidecar object keys lost control escapes");
  check(formatSidecarText(Json{{"\b", "\f"}}.dump()) ==
            "{\n  \"\\u0008\": \"\\u000c\"\n}\n",
        "Preencoded object keys lost control escapes");
  check(sidecarText(Json{{"addr", "0x123"}, {"size", 1}}) ==
            "{\n  \"addr\": \"0x123\",\n  \"size\": 1\n}\n",
        "Unescaped sidecar text changed");
  for (auto value : {std::uint64_t(0), std::uint64_t(0x20000000000001),
                     std::uint64_t(0xffff800012345678),
                     std::numeric_limits<std::uint64_t>::max()})
    check(parseAddress(hexAddress(value)) == value,
          "Address round trip lost precision");
  for (const auto *value :
       {"", "123", "0x", "0x-1", "0x10000000000000000", "0x123g", "0x1 "})
    rejects([&] { parseAddress(value); }, "Malformed address accepted");
  rejects([] { sizeField(Json{{"limit", -1}}, "limit", 1, 512); },
          "Negative limit accepted");
  rejects([] { sizeField(Json{{"limit", 1.5}}, "limit", 1, 512); },
          "Fractional limit accepted");
  rejects([] { sizeField(Json{{"limit", 513}}, "limit", 1, 512); },
          "Over-budget limit accepted");
  rejects([] { sizeField(Json{{"limit", "2"}}, "limit", 1, 512); },
          "String limit accepted");
  const Json request{{"protocol_major", 1},
                     {"request_id", "1"},
                     {"operation", "bytes"},
                     {"payload", {{"address", "0xffffffffffffffff"}}}};
  validateRequest(request);
  const auto encoded = frame(request);
  check(frameSize(reinterpret_cast<const unsigned char *>(encoded.data())) ==
            encoded.size() - 4,
        "Incorrect network byte order");
  check(parseJson(std::string_view(encoded).substr(4)) == request,
        "Frame round trip failed");
  const Json payload{
      {"text", std::string("a\0b", 3) + "\"\\\n中文"},
      {"items", {nullptr, true, std::numeric_limits<std::uint64_t>::max()}}};
  const EncodedJson saved(payload);
  for (const auto *id : {"first", "second\"\\\n"}) {
    Json envelope{{"request_id", id},
                  {"payload", nullptr},
                  {"cancellation_requested", true}};
    const auto wire = frame(envelope, saved);
    envelope["payload"] = payload;
    check(parseJson(std::string_view(wire).substr(4)) == envelope,
          "Encoded payload changed data or reused an old response envelope");
    check(frameSize(reinterpret_cast<const unsigned char *>(wire.data())) ==
              wire.size() - 4,
          "Encoded payload frame has an incorrect length");
  }
  const Json invalidText{{"text", std::string(1, '\xff')}};
  const auto ordinary = frame(Json{{"payload", invalidText}});
  const auto reused = frame(Json::object(), EncodedJson(invalidText));
  check(parseJson(std::string_view(ordinary).substr(4)) ==
            parseJson(std::string_view(reused).substr(4)),
        "Encoded payload changed invalid UTF-8 replacement");
  const auto overhead = frame(Json::object(), EncodedJson(Json(""))).size() - 4;
  const auto full =
      frame(Json::object(),
            EncodedJson(Json(std::string(MaxFrameBytes - overhead, 'x'))));
  check(full.size() - 4 == MaxFrameBytes,
        "Exact-limit encoded payload was not framed");
  rejects(
      [&] {
        frame(
            Json::object(),
            EncodedJson(Json(std::string(MaxFrameBytes - overhead + 1, 'x'))));
      },
      "Over-budget encoded payload was accepted");
  rejects([&] { frame(Json::array(), saved); },
          "Encoded payload accepted a non-object envelope");
  rejects(
      [] {
        validateRequest(Json{
            {"protocol_major", 2}, {"request_id", "1"}, {"operation", "open"}});
      },
      "Incompatible protocol accepted");
  rejects(
      [] {
        validateRequest(Json{
            {"protocol_major", 1}, {"request_id", 3}, {"operation", "open"}});
      },
      "Numeric request ID accepted");
  rejects([] { parseJson(std::string(65, '[') + std::string(65, ']')); },
          "Excessive nesting accepted");
  rejects([] { parseJson("{broken"); }, "Malformed JSON accepted");
  rejects([] { parseJson(R"({"path":"first","p\u0061th":"second"})"); },
          "Decoded duplicate path key accepted");
  rejects([] { parseJson(R"({"options":{"limit":1,"limit":2}})"); },
          "Nested duplicate options key accepted");
  rejects([] { parseJson(R"({"number":1.5,"key":1,"key":2})"); },
          "Number fallback skipped later duplicate admission");
  rejects([] { parseJson(R"({"number":-0,"key":1,"key":2})"); },
          "Signed-zero fallback skipped later duplicate admission");
  check(parseJson(R"([{"key":1},{"key":2}])").size() == 2,
        "Separate object keys incorrectly merged");
  // Native history and source inventories can contain large arrays. The
  // duplicate-key admission must not run a DOM filter that rescans every
  // preceding sibling whenever an object ends.
  std::string large = "[";
  for (unsigned i = 0; i < 29000; ++i) {
    if (i)
      large += ',';
    large += "{\"id\":" + std::to_string(i) + ",\"text\":\"" +
             std::string(220, 'x') + "\"}";
  }
  large += ']';
  const auto inventory = parseJson(large);
  check(inventory.size() == 29000 && inventory.back()["id"] == 28999 &&
            inventory.front()["text"] == std::string(220, 'x'),
        "Large object array was not preserved");
  large.pop_back();
  large += R"(,{"id":1,"\u0069d":2}])";
  rejects([&] { parseJson(large); },
          "Duplicate key late in a large array accepted");
  rejects([] { parseJson(R"({"outer":[{"nested":{"x":1,"x":2}}]})"); },
          "Object key scopes drifted through nested arrays");
  rejects(
      [] {
        const unsigned char bytes[]{0, 0, 0, 0};
        frameSize(bytes);
      },
      "Zero frame accepted");
  rejects(
      [] {
        const unsigned char bytes[]{0, 128, 0, 1};
        frameSize(bytes);
      },
      "Oversized frame accepted");
  check(parseJson("{\"text\":\"[[[\\\"{{{\"}").is_object(),
        "Quoted braces count as nesting");
  std::cout << "protocol validation passed\n";
}
