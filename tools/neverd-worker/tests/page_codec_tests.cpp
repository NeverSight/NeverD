// The compact form of kept engine pages: every input round-trips, including
// matches that overlap their own output, and anything else is refused.
#include "PageCodec.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace neverd::worker;

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void roundTrip(const std::string &raw, const std::string &name) {
  const auto compact = compressPage(raw);
  const auto back = decompressPage(compact);
  check(back.has_value(), name + ": refused its own compact form");
  check(*back == raw, name + ": changed by a round trip");
}

std::string engineLikePage() {
  // Long repeated keys and source text, as an engine page is.
  std::string page = "{\"rows\":[";
  for (int i = 0; i != 2000; ++i)
    page += "{\"line\":" + std::to_string(i) +
            ",\"kind\":\"source\",\"mapping_status\":\"unmapped\","
            "\"addresses\":[\"0x14000" +
            std::to_string(i) + "\"]},";
  return page + "\"text\":\"int main(void) { return 0; }\"}";
}
} // namespace

int main() {
  roundTrip("", "empty");
  for (std::size_t size = 1; size != 64; ++size) {
    roundTrip(std::string(size, 'a'), "one byte repeated");
    std::string varied;
    for (std::size_t i = 0; i != size; ++i)
      varied.push_back(static_cast<char>('a' + i % 7));
    roundTrip(varied, "short period");
  }
  // Every period below the longest match, whose copies overlap their output.
  for (std::size_t period = 1; period != 40; ++period) {
    std::string raw;
    for (std::size_t i = 0; i != 5000; ++i)
      raw.push_back(static_cast<char>('A' + i % period));
    roundTrip(raw, "period " + std::to_string(period));
  }
  std::mt19937 random(7);
  std::string noise;
  for (int i = 0; i != 200000; ++i)
    noise.push_back(static_cast<char>(random()));
  roundTrip(noise, "incompressible");
  // A match reaching back past the hash table's reach of one bucket.
  roundTrip(noise + noise, "a repeat of incompressible bytes");
  std::string zeros(3 * 1024 * 1024, '\0');
  roundTrip(zeros, "zeros");
  const auto page = engineLikePage();
  roundTrip(page, "engine page");
  check(compressPage(page).size() * 3 < page.size(),
        "an engine page should compress well");

  // Anything but the whole of what compressPage wrote is refused.
  const auto compact = compressPage(page);
  check(!decompressPage(compact.substr(0, compact.size() - 1)),
        "accepted a truncated page");
  check(!decompressPage(compact + "x"), "accepted trailing bytes");
  check(!decompressPage(""), "accepted nothing");
  check(!decompressPage("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"),
        "accepted an endless size");
  // Size 4, no literals, a match of 4 from before the start.
  check(!decompressPage(std::string("\x04\x00\x04\x01", 4)),
        "accepted a match before the output");
  // Size 1, a literal, then a match past the size.
  check(!decompressPage(std::string("\x01\x01x\x04\x01", 5)),
        "accepted a match past the size");
  // Size 1 with no bytes written.
  check(!decompressPage(std::string("\x01\x00\x00", 3)),
        "accepted a page shorter than its size");
  for (std::size_t at = 0; at < compact.size(); at += 97) {
    auto damaged = compact;
    damaged[at] = static_cast<char>(damaged[at] ^ 0x5a);
    // Damage need not be detected, but must not run outside the page.
    (void)decompressPage(damaged);
  }
  return 0;
}
