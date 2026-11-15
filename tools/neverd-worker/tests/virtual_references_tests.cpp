//===- virtual_references_tests.cpp - Virtual reference ownership ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check reference replacement, incoming queries and owner invalidation.
//===----------------------------------------------------------------------===//

#include "VirtualReferences.h"

#include <cstdlib>
#include <iostream>

using namespace neverd::worker;

static void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main() {
  VirtualReferences refs;
  Json call{{"from", "0x1004"},          {"to", "0x3000"},
            {"table", "0x6000"},         {"slot", "0x6020"},
            {"slot_offset", "0x10"},     {"symbol", "_ZN6Device5valueEv"},
            {"receiver_type", "Device"}, {"resolution", "static_type"}};
  const Json calls = Json::array({call, call});
  check(refs.replace(0x1000, calls), "first publication changed nothing");
  check(!refs.replace(0x1000, calls), "identical evidence changed the index");
  std::size_t total = 0;
  auto rows = refs.page(0x3000, true, 0, 10, total);
  check(total == 1 && rows.size() == 1 && rows[0]["kind"] == "vcall" &&
            rows[0]["resolution"] == "static_type",
        "declared method reference lost its dispatch qualification");
  rows = refs.page(0x6020, true, 0, 10, total);
  check(total == 1 && rows[0]["kind"] == "vslot" &&
            rows[0]["declared_target"] == "0x3000",
        "table slot did not retain its declaration");
  rows = refs.page(0x1004, false, 1, 1, total);
  check(total == 2 && rows.size() == 1 && rows[0]["kind"] == "vslot",
        "outgoing reference pagination changed");
  refs.replace(0x2000, Json::array({call}));
  refs.replace(0x1000, Json::array());
  rows = refs.page(0x3000, true, 0, 10, total);
  check(total == 1 && rows[0]["caller_function"] == "0x2000",
        "replacing one owner removed another owner's evidence");
  call["to"] = "0x4000";
  refs.replace(0x2000, Json::array({call}));
  check(refs.page(0x3000, true, 0, 10, total).empty() && total == 0,
        "old target remained after replacement");
  bool rejected = false;
  try {
    auto invalid = call;
    invalid["resolution"] = "guessed";
    refs.replace(0x2000, Json::array({invalid}));
  } catch (const Error &) {
    rejected = true;
  }
  check(rejected && refs.page(0x4000, true, 0, 10, total).size() == 1,
        "bad metadata mutated the last valid result");
  refs.clear();
  check(refs.page(0x1004, false, 0, 10, total).empty() && total == 0,
        "type invalidation retained outgoing references");
  check(!refs.replace(0x2000, Json::array()), "empty evidence changed state");
}
