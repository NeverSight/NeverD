//===- MinGWHeaderFunctions.cpp - Functions mingw-w64 declares ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/libc/MinGWHeaderFunctions.h"

#include <algorithm>
#include <iterator>

namespace neverd::libc {
namespace {

constexpr std::string_view kMinGWHeaderFunctions[] = {
#define NEVERD_MINGW_HEADER_FUNCTION(NAME) NAME,
#include "neverd/libc/MinGWHeaderFunctions.inc"
#undef NEVERD_MINGW_HEADER_FUNCTION
};
static_assert(std::is_sorted(std::begin(kMinGWHeaderFunctions),
                             std::end(kMinGWHeaderFunctions)),
              "MinGWHeaderFunctions.inc lists its names in order");

} // namespace

bool mingwHeadersDeclare(std::string_view Name) {
  return std::binary_search(std::begin(kMinGWHeaderFunctions),
                            std::end(kMinGWHeaderFunctions), Name);
}

} // namespace neverd::libc
