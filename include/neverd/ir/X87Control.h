//===- X87Control.h - x87 control-word domain ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared architectural control domain for concrete x87 semantic owners.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X87CONTROL_H
#define NEVERD_IR_X87CONTROL_H

#include <cstdint>

namespace neverd {

constexpr bool x87ControlWordIsValid(uint16_t Control) {
  // PC=01 and reserved control bits have no admitted architectural meaning.
  return (Control & 0xe080) == 0 && (Control & 0x300) != 0x100;
}

/// Requires x87ControlWordIsValid. PC affects precision, not exponent range.
constexpr unsigned x87PrecisionBits(uint16_t Control) {
  return (Control & 0x300) == 0 ? 24 : (Control & 0x300) == 0x200 ? 53 : 64;
}

} // namespace neverd

#endif // NEVERD_IR_X87CONTROL_H
