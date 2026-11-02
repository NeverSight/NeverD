//===- X87Control.h - x87 control-word domain ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared architectural control domain for concrete x87 semantic owners.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X87CONTROL_H
#define NEVERD_IR_X86_X87CONTROL_H

#include <cstdint>

namespace neverd {

inline constexpr uint16_t X87ControlReservedMask = 0xe080;
inline constexpr uint16_t X87PrecisionControlMask = 0x300;
inline constexpr uint16_t X87ReservedPrecisionControl = 0x100;
inline constexpr uint16_t X87ExceptionMaskBits = 0x3f;

constexpr bool x87ControlWordIsValid(uint16_t Control) {
  // PC=01 and reserved control bits have no admitted architectural meaning.
  return (Control & X87ControlReservedMask) == 0 &&
         (Control & X87PrecisionControlMask) != X87ReservedPrecisionControl;
}

/// Waiting instructions observe the pending exception before operand access.
/// Callers decide whether an instruction waits from the shared encoding owner.
constexpr bool x87HasPendingException(uint16_t Control, uint16_t Status) {
  return (Status & 0x80) || (Status & ~Control & X87ExceptionMaskBits);
}

/// Requires x87ControlWordIsValid. PC affects precision, not exponent range.
constexpr unsigned x87PrecisionBits(uint16_t Control) {
  return (Control & 0x300) == 0 ? 24 : (Control & 0x300) == 0x200 ? 53 : 64;
}

} // namespace neverd

#endif // NEVERD_IR_X86_X87CONTROL_H
