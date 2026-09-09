#include "Protocol.h"

#include <cstdlib>
#include <iostream>
#include <limits>

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
int main() {
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
