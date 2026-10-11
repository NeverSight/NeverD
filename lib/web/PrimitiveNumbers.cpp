//===- PrimitiveNumbers.cpp - JavaScript primitive formatting ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JavaScript primitive formatting.
///
//===----------------------------------------------------------------------===//

#include "PrimitiveNumbers.h"

#include "hermes/Support/Conversions.h"

#include "neverd/web/Error.h"

#include <cfenv>
#include <cstring>

namespace neverd::web {
std::string primitiveNumberToString(uint64_t Binary64Bits) {
  if (std::fegetround() != FE_TONEAREST)
    throw Error("unsupported_host_rounding_mode");
  double Value;
  static_assert(sizeof(Value) == sizeof(Binary64Bits));
  std::memcpy(&Value, &Binary64Bits, sizeof(Value));
  char Buffer[hermes::NUMBER_TO_STRING_BUF_SIZE];
  const auto Size = hermes::numberToString(Value, Buffer, sizeof(Buffer));
  return {Buffer, Size};
}
} // namespace neverd::web
