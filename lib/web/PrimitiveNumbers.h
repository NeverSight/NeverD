#pragma once

#include <cstdint>
#include <string>

namespace neverd::web {
// Keep the embedded Hermes formatter and LLVM APFloat in separate TUs.
std::string primitiveNumberToString(uint64_t Binary64Bits);
} // namespace neverd::web
